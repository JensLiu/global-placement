#include "tracer/placement_metrics.hpp"

#include "gordian/global_parameter_builder.hpp"

#include <algorithm>
#include <boost/json.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace gordian::tracer {
namespace {
namespace json = boost::json;

Distribution distribution(std::vector<double> values) {
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  const auto at = [&](double q) {
    const auto index = static_cast<std::size_t>(q * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
  };
  return {at(0.5), at(0.95), values.back()};
}

double overlap(double a0, double a1, double b0, double b1) {
  return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
}

json::value optional_number(const std::optional<double> &value) {
  return value ? json::value(*value) : json::value(nullptr);
}
json::value optional_count(const std::optional<std::size_t> &value) {
  return value ? json::value(*value) : json::value(nullptr);
}
json::value distribution_json(const Distribution &d) {
  return json::object{{"median", d.median}, {"p95", d.p95}, {"max", d.max}};
}

json::object to_json(const FrameMetrics &m) {
  json::object out{
      {"frame", optional_count(m.frame)},
      {"phase", m.phase},
      {"iteration", optional_count(m.iteration)},
      {"hpwl_um", json::object{{"all", m.hpwl_all}, {"modelled", m.hpwl_modelled}}},
      {"objective_um2", m.objective},
      {"density", json::object{{"bin_um", m.bin_um},
                               {"target", m.target_density},
                               {"overflow", m.overflow},
                               {"max_bin_density", m.max_bin_density}}},
      {"outside", json::object{{"die", m.outside_die},
                               {"core", m.outside_core},
                               {"own_region", optional_count(m.outside_own_region)},
                               {"own_region_area_fraction",
                                optional_number(m.outside_own_region_area_fraction)}}},
      {"movement_um",
       json::object{{"from_previous_solve",
                     m.move_from_previous_solve
                         ? distribution_json(*m.move_from_previous_solve)
                         : json::value(nullptr)},
                    {"from_input", distribution_json(m.move_from_input)}}},
      {"cog_error_um", optional_number(m.cog_error)},
      {"regions", json::object{{"count", optional_count(m.region_count)},
                               {"median_size", optional_number(m.region_median_size)},
                               {"singletons", optional_count(m.singleton_regions)}}},
      {"seconds", json::object{{"since_previous", m.seconds_since_previous},
                               {"since_start", m.seconds_since_start}}}};
  return out;
}
} // namespace

PlacementMetrics::PlacementMetrics(odb::dbBlock *block, const ModuleRegistry &registry,
                                   MetricsConfig config)
    : config_(config) {
  if (!block) {
    throw std::invalid_argument("null metrics block");
  }
  dbu_ = block->getDbUnitsPerMicron();
  const auto core = block->getCoreArea();
  const auto die = block->getDieArea();
  core_ = {double(core.xMin()), double(core.yMin()), double(core.xMax()), double(core.yMax())};
  die_ = {double(die.xMin()), double(die.yMin()), double(die.xMax()), double(die.yMax())};

  // Movable footprints and input centres (the OpenDB placement is never
  // modified by the solver, so it is the input placement).
  const auto m = registry.movable_count();
  area_.assign(m, 0);
  half_w_.assign(m, 0);
  half_h_.assign(m, 0);
  input_x_.assign(m, 0);
  input_y_.assign(m, 0);
  for (auto *inst : block->getInsts()) {
    const auto box = inst->getBBox()->getBox();
    const auto id = registry.from_inst(inst)->get_id();
    if (registry.is_movable(id)) {
      area_[id] = double(box.dx()) * double(box.dy());
      half_w_[id] = box.dx() / 2.0;
      half_h_[id] = box.dy() / 2.0;
      input_x_[id] = (double(box.xMin()) + box.xMax()) / 2.0;
      input_y_[id] = (double(box.yMin()) + box.yMax()) / 2.0;
      movable_area_ += area_[id];
    } else {
      fixed_.push_back({double(box.xMin()), double(box.yMin()), double(box.xMax()),
                        double(box.yMax())});
    }
  }

  // Pin table: movable pins as centre offsets, everything else absolute.
  for (auto *net : block->getNets()) {
    if (net->getSigType().isSupply()) {
      continue;
    }
    Net entry{double(net->getWeight()),
              GlobalParameterBuilder::include_net__(net), {}};
    for (auto *iterm : net->getITerms()) {
      auto *inst = iterm->getInst();
      const auto box = inst->getBBox()->getBox();
      const double cx = (double(box.xMin()) + box.xMax()) / 2.0;
      const double cy = (double(box.yMin()) + box.yMax()) / 2.0;
      int px = 0, py = 0;
      double ax = cx, ay = cy;
      if (iterm->getAvgXY(&px, &py)) {
        ax = px;
        ay = py;
      }
      const auto id = registry.from_inst(inst)->get_id();
      if (registry.is_movable(id)) {
        entry.pins.push_back({std::int64_t(id), ax - cx, ay - cy, true});
      } else {
        entry.pins.push_back({-1, ax, ay, true});
      }
    }
    for (auto *bterm : net->getBTerms()) {
      double sx = 0, sy = 0;
      int count = 0;
      for (auto *bpin : bterm->getBPins()) {
        if (!bpin->getPlacementStatus().isPlaced() || bpin->getBoxes().empty()) {
          continue;
        }
        const auto box = bpin->getBBox();
        sx += (double(box.xMin()) + box.xMax()) / 2.0;
        sy += (double(box.yMin()) + box.yMax()) / 2.0;
        ++count;
      }
      if (count == 0) {
        continue; // unplaced IO: no defined position to measure
      }
      bool visible = true;
      try {
        (void)registry.from_bterm(bterm);
      } catch (const std::out_of_range &) {
        visible = false; // not in the solver's model (IO mode / multi-pin)
      }
      entry.pins.push_back({-1, sx / count, sy / count, visible});
    }
    if (entry.pins.size() >= 2) {
      nets_.push_back(std::move(entry));
    }
  }

  // Density grid over the core: bins of bin_rows row heights.
  double row_height = 0;
  for (auto *row : block->getRows()) {
    row_height = row->getSite()->getHeight();
    break;
  }
  bin_ = row_height > 0 ? config_.bin_rows * row_height : 10.0 * dbu_;
  nx_ = std::max<std::size_t>(1, std::size_t(std::ceil((core_.x1 - core_.x0) / bin_)));
  ny_ = std::max<std::size_t>(1, std::size_t(std::ceil((core_.y1 - core_.y0) / bin_)));
  capacity_.assign(nx_ * ny_, 0);
  for (std::size_t j = 0; j < ny_; ++j) {
    for (std::size_t i = 0; i < nx_; ++i) {
      const double x0 = core_.x0 + i * bin_, y0 = core_.y0 + j * bin_;
      const double x1 = std::min(x0 + bin_, core_.x1), y1 = std::min(y0 + bin_, core_.y1);
      double free = (x1 - x0) * (y1 - y0);
      for (const auto &f : fixed_) {
        free -= overlap(x0, x1, f.x0, f.x1) * overlap(y0, y1, f.y0, f.y1);
      }
      capacity_[j * nx_ + i] = std::max(0.0, free);
    }
  }
  start_ = previous_ = std::chrono::steady_clock::now();
}

FrameMetrics PlacementMetrics::measure(const std::vector<double> &x,
                                       const std::vector<double> &y) {
  FrameMetrics out;
  const double um = 1.0 / dbu_;
  // Wirelength and the quadratic objective.
  for (const auto &net : nets_) {
    double xmin = std::numeric_limits<double>::max(), xmax = -xmin;
    double ymin = xmin, ymax = -xmin;
    double sx = 0, sy = 0, sxx = 0, syy = 0;
    std::size_t visible = 0;
    for (const auto &pin : net.pins) {
      double px = pin.x, py = pin.y;
      if (pin.module >= 0) {
        px += x[std::size_t(pin.module)];
        py += y[std::size_t(pin.module)];
      }
      xmin = std::min(xmin, px);
      xmax = std::max(xmax, px);
      ymin = std::min(ymin, py);
      ymax = std::max(ymax, py);
      if (pin.solver_visible) {
        const double ux = px * um, uy = py * um;
        sx += ux;
        sy += uy;
        sxx += ux * ux;
        syy += uy * uy;
        ++visible;
      }
    }
    const double hpwl = ((xmax - xmin) + (ymax - ymin)) * um;
    out.hpwl_all += hpwl;
    if (net.modelled) {
      out.hpwl_modelled += hpwl;
      if (visible >= 2) {
        // sum_p (a_p - mean)^2 = sum a^2 - (sum a)^2 / p
        const double p = double(visible);
        out.objective += 0.5 * net.weight *
                         ((sxx - sx * sx / p) + (syy - sy * sy / p));
      }
    }
  }
  // Density: footprint overlap with bins; area off the core counts as overflow.
  std::vector<double> used(nx_ * ny_, 0);
  double off_core = 0;
  for (std::size_t k = 0; k < area_.size(); ++k) {
    const double x0 = x[k] - half_w_[k], x1 = x[k] + half_w_[k];
    const double y0 = y[k] - half_h_[k], y1 = y[k] + half_h_[k];
    const double inside = overlap(x0, x1, core_.x0, core_.x1) *
                          overlap(y0, y1, core_.y0, core_.y1);
    off_core += area_[k] - inside;
    if (inside <= 0) {
      continue;
    }
    const auto bin_of = [&](double v, double origin, std::size_t n) {
      const auto i = std::floor((v - origin) / bin_);
      return std::size_t(std::clamp(i, 0.0, double(n - 1)));
    };
    for (auto j = bin_of(std::max(y0, core_.y0), core_.y0, ny_);
         j <= bin_of(std::min(y1, core_.y1), core_.y0, ny_); ++j) {
      for (auto i = bin_of(std::max(x0, core_.x0), core_.x0, nx_);
           i <= bin_of(std::min(x1, core_.x1), core_.x0, nx_); ++i) {
        const double bx0 = core_.x0 + i * bin_, by0 = core_.y0 + j * bin_;
        used[j * nx_ + i] += overlap(x0, x1, bx0, std::min(bx0 + bin_, core_.x1)) *
                             overlap(y0, y1, by0, std::min(by0 + bin_, core_.y1));
      }
    }
  }
  double excess = off_core;
  for (std::size_t b = 0; b < used.size(); ++b) {
    excess += std::max(0.0, used[b] - config_.target_density * capacity_[b]);
    if (capacity_[b] > 0) {
      out.max_bin_density = std::max(out.max_bin_density, used[b] / capacity_[b]);
    }
  }
  out.overflow = movable_area_ > 0 ? excess / movable_area_ : 0;
  out.bin_um = bin_ * um;
  out.target_density = config_.target_density;
  // Containment and movement from the input placement.
  std::vector<double> moved(area_.size());
  for (std::size_t k = 0; k < area_.size(); ++k) {
    out.outside_die += !(die_.x0 <= x[k] && x[k] <= die_.x1 && die_.y0 <= y[k] && y[k] <= die_.y1);
    out.outside_core +=
        !(core_.x0 <= x[k] && x[k] <= core_.x1 && core_.y0 <= y[k] && y[k] <= core_.y1);
    moved[k] = std::hypot(x[k] - input_x_[k], y[k] - input_y_[k]) * um;
  }
  out.move_from_input = distribution(std::move(moved));
  return out;
}

void PlacementMetrics::finish_timing(FrameMetrics &metrics) {
  const auto now = std::chrono::steady_clock::now();
  metrics.seconds_since_previous = std::chrono::duration<double>(now - previous_).count();
  metrics.seconds_since_start = std::chrono::duration<double>(now - start_).count();
  previous_ = now;
}

FrameMetrics PlacementMetrics::baseline() {
  auto metrics = measure(input_x_, input_y_);
  metrics.phase = "input";
  finish_timing(metrics);
  return metrics;
}

FrameMetrics PlacementMetrics::evaluate(std::size_t frame, const IterationSnapshot &snapshot) {
  const auto m = area_.size();
  if (snapshot.modules.size() != m) {
    throw std::runtime_error("metrics: frame does not cover every movable module");
  }
  std::vector<double> x(m), y(m);
  std::vector<std::size_t> region_of(m);
  for (const auto &p : snapshot.modules) {
    x.at(p.id) = p.x;
    y.at(p.id) = p.y;
    region_of.at(p.id) = p.region_id;
  }
  auto metrics = measure(x, y);
  metrics.frame = frame;
  metrics.iteration = snapshot.iteration;
  const bool solve = snapshot.phase == FramePhase::PostSolve;
  metrics.phase = solve ? "post_solve" : "post_partition";

  // Regions: containment, size statistics, and (after a solve) COG error.
  std::unordered_map<std::size_t, std::size_t> index;
  for (std::size_t r = 0; r < snapshot.regions.size(); ++r) {
    index.emplace(snapshot.regions[r].id, r);
  }
  const auto n = snapshot.regions.size();
  std::vector<double> region_area(n, 0), cog_x(n, 0), cog_y(n, 0);
  std::vector<std::size_t> members(n, 0);
  std::size_t outside = 0;
  double outside_area = 0;
  for (std::size_t k = 0; k < m; ++k) {
    const auto r = index.at(region_of[k]);
    const auto &b = snapshot.regions[r].bounds;
    if (!(b[0] <= x[k] && x[k] <= b[2] && b[1] <= y[k] && y[k] <= b[3])) {
      ++outside;
      outside_area += area_[k];
    }
    ++members[r];
    region_area[r] += area_[k];
    cog_x[r] += area_[k] * x[k];
    cog_y[r] += area_[k] * y[k];
  }
  metrics.outside_own_region = outside;
  metrics.outside_own_region_area_fraction =
      movable_area_ > 0 ? outside_area / movable_area_ : 0;
  metrics.region_count = n;
  std::vector<double> sizes(members.begin(), members.end());
  metrics.region_median_size = distribution(sizes).median;
  metrics.singleton_regions = std::size_t(std::count(members.begin(), members.end(), 1));
  if (solve) {
    double worst = 0;
    for (std::size_t r = 0; r < n; ++r) {
      if (region_area[r] <= 0) {
        continue;
      }
      const auto &b = snapshot.regions[r].bounds;
      worst = std::max({worst, std::abs(cog_x[r] / region_area[r] - (b[0] + b[2]) / 2),
                        std::abs(cog_y[r] / region_area[r] - (b[1] + b[3]) / 2)});
    }
    metrics.cog_error = worst / dbu_;
    if (have_last_solve_) {
      std::vector<double> moved(m);
      for (std::size_t k = 0; k < m; ++k) {
        moved[k] = std::hypot(x[k] - last_solve_x_[k], y[k] - last_solve_y_[k]) / dbu_;
      }
      metrics.move_from_previous_solve = distribution(std::move(moved));
    }
    last_solve_x_ = x;
    last_solve_y_ = y;
    have_last_solve_ = true;
  }
  finish_timing(metrics);
  return metrics;
}

MetricsWriter::MetricsWriter(const std::filesystem::path &directory, const std::string &run_id,
                             const FrameMetrics &baseline) {
  out_.exceptions(std::ios::failbit | std::ios::badbit);
  out_.open(directory / "metrics.jsonl");
  auto header = to_json(baseline);
  header["schema"] = "global-placement.gordian-metrics";
  header["version"] = 1;
  header["run_id"] = run_id;
  out_ << json::serialize(header) << '\n';
  out_.flush();
}

void MetricsWriter::append(const FrameMetrics &metrics) {
  out_ << json::serialize(to_json(metrics)) << '\n';
  out_.flush();
}

} // namespace gordian::tracer
