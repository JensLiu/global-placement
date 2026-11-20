#include "tracer/placement_tracer.hpp"
#include "tracer/placement_metrics.hpp"

#include <boost/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <stdexcept>

namespace gordian::tracer {
namespace {
namespace json = boost::json;

Bounds bounds(const odb::Rect &rect) {
  return {double(rect.xMin()), double(rect.yMin()), double(rect.xMax()),
          double(rect.yMax())};
}
void check_bounds(const Bounds &box) {
  for (const double value : box) {
    if (!std::isfinite(value)) {
      throw std::runtime_error("nonfinite trace bounds");
    }
  }
  if (box[0] > box[2] || box[1] > box[3]) {
    throw std::runtime_error("inverted trace bounds");
  }
}
json::array array(const Bounds &box) {
  check_bounds(box);
  return {box[0], box[1], box[2], box[3]};
}
} // namespace

DesignSnapshot SnapshotSampler::geometry(odb::dbBlock *block,
                                         const ModuleRegistry &registry) {
  if (!block) {
    throw std::invalid_argument("null trace block");
  }
  DesignSnapshot result{block->getName(), block->getDbUnitsPerMicron(),
                        bounds(block->getCoreArea()), bounds(block->getDieArea()), {}};
  result.modules.resize(registry.size());
  std::vector<bool> seen(registry.size(), false);
  const auto add = [&](const ModuleRef &module, const odb::Rect &box,
                       const std::string &kind) {
    const auto id = module.get_id();
    const auto [x, y] = module.get_xy();
    // The footprint is centred independently of the cached analytical position.
    const double half_width = (double(box.xMax()) - box.xMin()) / 2.0;
    const double half_height = (double(box.yMax()) - box.yMin()) / 2.0;
    result.modules.at(id) = {id, module.get_name(), kind,
                            {-half_width, -half_height, half_width, half_height},
                            {x, y}};
    seen.at(id) = true;
  };
  for (auto *inst : block->getInsts()) {
    const auto *module = registry.from_inst(inst);
    add(*module, inst->getBBox()->getBox(),
        registry.is_movable(module->get_id()) ? "movable" : "fixed");
  }
  for (auto *term : block->getBTerms()) {
    const ModuleRef *module;
    try {
      module = registry.from_bterm(term);
    } catch (const std::out_of_range &) {
      continue; // IO mode/filter excluded this terminal from the registry.
    }
    if (term->getBPins().size() != 1) {
      throw std::runtime_error("registered IO no longer has one BPin");
    }
    add(*module, (*term->getBPins().begin())->getBBox(), "io");
  }
  for (const bool found : seen) {
    if (!found) {
      throw std::runtime_error("trace geometry missing registered module");
    }
  }
  return result;
}

IterationSnapshot SnapshotSampler::iteration(
    std::optional<std::size_t> number, const ModuleRegistry &registry,
    const std::vector<Region> &regions, const Eigen::VectorXd &x,
    const Eigen::VectorXd &y) {
  const auto count = registry.movable_count();
  if (x.size() != static_cast<Eigen::Index>(count) || y.size() != x.size() ||
      !x.allFinite() || !y.allFinite()) {
    throw std::runtime_error("trace requires finite, complete movable coordinates");
  }
  const auto unassigned = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> membership(count, unassigned);
  IterationSnapshot result{number, {}, {}, {}};
  std::unordered_set<std::size_t> region_ids;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto &region = regions[i];
    const Bounds box{region.x_min, region.y_min, region.x_max, region.y_max};
    check_bounds(box);
    const auto &debug = region.debug;
    if (!region_ids.insert(debug.id).second) {
      throw std::runtime_error("duplicate region debug ID in trace");
    }
    std::optional<std::size_t> parent;
    if (debug.parent_id != RegionDebugInfo::no_parent) {
      parent = debug.parent_id;
    }
    result.regions.push_back({debug.id, parent, debug.depth, box});
    for (const auto *module : region.movable_modules) {
      if (!module || !registry.is_movable(module->get_id()) ||
          registry.at(module->get_id()) != module) {
        throw std::runtime_error("invalid region member in trace");
      }
      auto &assigned = membership.at(module->get_id());
      if (assigned != unassigned) {
        throw std::runtime_error("duplicate region membership in trace");
      }
      assigned = region.debug.id;
    }
  }
  for (const auto &module : registry.movable_modules()) {
    const auto id = module.get_id();
    if (membership.at(id) == unassigned) {
      throw std::runtime_error("missing region membership in trace");
    }
    // Current registry guarantees movable IDs occupy [0, movable_count()).
    result.modules.push_back({id, membership[id], x(id), y(id)});
  }
  return result;
}

JsonTraceWriter::JsonTraceWriter(const std::filesystem::path &directory,
                                 const DesignSnapshot &design) {
  json::array modules;
  for (const auto &module : design.modules) {
    if (!std::isfinite(module.initial_centre[0]) ||
        !std::isfinite(module.initial_centre[1])) {
      throw std::runtime_error("nonfinite static module centre");
    }
    modules.push_back(json::object{
        {"id", module.id}, {"name", module.name}, {"kind", module.kind},
        {"bbox_offsets", array(module.bbox_offsets)},
        {"initial_centre", json::array{module.initial_centre[0], module.initial_centre[1]}}});
  }
  if (design.dbu_per_micron <= 0) {
    throw std::runtime_error("invalid DBU scale");
  }
  run_id_ = directory.filename().string() + "-" + std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
  const json::object document{
      {"schema", "global-placement.gordian-trace"}, {"version", 3},
      {"run_id", run_id_}, {"design", design.name},
      {"coordinates", json::object{{"units", "dbu"},
          {"dbu_per_micron", design.dbu_per_micron}, {"cell_reference", "bbox_centre"}}},
      {"region_id_scope", "run"},
      {"core_bounds", array(design.core_bounds)},
      {"die_bounds", array(design.die_bounds)}, {"modules", std::move(modules)}};
  const auto encoded = json::serialize(document);
  // Claim a fresh leaf directory. Never overwrite an earlier trace.
  if (directory.empty() || !std::filesystem::create_directories(directory)) {
    throw std::runtime_error("trace directory must not already exist");
  }
  std::ofstream geometry;
  geometry.exceptions(std::ios::failbit | std::ios::badbit);
  geometry.open(directory / "design.json");
  geometry << encoded << '\n';
  geometry.close();
  iterations_.exceptions(std::ios::failbit | std::ios::badbit);
  iterations_.open(directory / "iterations.jsonl");
}

void JsonTraceWriter::append(const IterationSnapshot &snapshot) {
  // post_solve frames number solves consecutively from zero; a post_partition
  // frame shows the latest solve (none before the first solve).
  if (snapshot.phase == FramePhase::PostSolve
          ? snapshot.iteration != solves_written_
          : (solves_written_ == 0 ? snapshot.iteration.has_value()
                                  : snapshot.iteration != solves_written_ - 1)) {
    throw std::runtime_error("trace iterations out of order");
  }
  json::array regions;
  for (const auto &region : snapshot.regions) {
    json::value parent = nullptr;
    if (region.parent_id) {
      parent = *region.parent_id;
    }
    regions.push_back(json::object{{"id", region.id}, {"parent_id", parent},
                                   {"depth", region.depth},
                                   {"bounds", array(region.bounds)}});
  }
  json::array partitions;
  for (const auto &record : snapshot.partitions) {
    json::object entry{{"parent_id", record.parent_id},
                       {"parent_bounds", array(record.parent_bounds)},
                       {"direction", record.is_vertical ? "vertical" : "horizontal"},
                       {"movable_count", record.movable_count},
                       {"movable_area", record.movable_area},
                       {"split", record.split.has_value()}};
    if (const auto &split = record.split) {
      if (!std::isfinite(split->cut)) {
        throw std::runtime_error("nonfinite cut coordinate");
      }
      entry["cut"] = split->cut;
      entry["child_ids"] = json::array{split->child_ids[0], split->child_ids[1]};
      entry["child_movable_counts"] =
          json::array{split->movable_counts[0], split->movable_counts[1]};
      entry["child_movable_areas"] =
          json::array{split->movable_areas[0], split->movable_areas[1]};
    }
    partitions.push_back(std::move(entry));
  }
  json::array modules;
  for (const auto &module : snapshot.modules) {
    if (!std::isfinite(module.x) || !std::isfinite(module.y)) {
      throw std::runtime_error("nonfinite trace position");
    }
    modules.push_back(json::object{{"id", module.id}, {"region_id", module.region_id},
                                   {"x", module.x}, {"y", module.y}});
  }
  json::value iteration = nullptr;
  if (snapshot.iteration) {
    iteration = *snapshot.iteration;
  }
  const auto *phase = snapshot.phase == FramePhase::PostSolve
                          ? "post_solve"
                          : "post_partition";
  const auto text = json::serialize(json::object{
      {"run_id", run_id_}, {"frame", next_frame_}, {"iteration", iteration},
      {"phase", phase}, {"regions", std::move(regions)},
      {"partitions", std::move(partitions)},
      {"modules", std::move(modules)}});
  iterations_ << text << '\n';
  iterations_.flush();
  ++next_frame_;
  if (snapshot.phase == FramePhase::PostSolve) {
    ++solves_written_;
  }
}

PartitionRecord SnapshotSampler::partition(const Region &parent,
                                           bool is_vertical, const Region *low,
                                           const Region *high) {
  const auto area = [](const Region &region) {
    double total = 0.0;
    for (const auto *module : region.movable_modules) {
      total += module->get_area();
    }
    return total;
  };
  PartitionRecord record{parent.debug.id,
                         {parent.x_min, parent.y_min, parent.x_max, parent.y_max},
                         is_vertical,
                         parent.movable_modules.size(),
                         area(parent),
                         std::nullopt};
  check_bounds(record.parent_bounds);
  if (low && high) {
    // The high son's lower edge is the cut line.
    record.split = PartitionRecord::Split{
        is_vertical ? high->x_min : high->y_min,
        {low->debug.id, high->debug.id},
        {low->movable_modules.size(), high->movable_modules.size()},
        {area(*low), area(*high)}};
  }
  return record;
}

Tracer::Tracer() = default;
Tracer::~Tracer() = default;

bool Tracer::active() const noexcept {
  if (disabled_) {
    return false;
  }
  if (writer_) {
    return true;
  }
  const char *directory = std::getenv("GP_TRACE_DIR");
  return directory && *directory;
}

void Tracer::disable(const char *reason) noexcept {
  disabled_ = true;
  writer_.reset();
  pending_.clear();
  std::cerr << "Warning: placement tracing disabled: " << reason << '\n';
}

void Tracer::record_split(const Region &parent, Region &low, Region &high,
                          bool is_vertical) noexcept {
  for (auto *son : {&low, &high}) {
    son->debug = {next_region_id_++, parent.debug.id, parent.debug.depth + 1};
  }
  if (!active()) {
    return;
  }
  try {
    pending_.push_back(SnapshotSampler::partition(parent, is_vertical, &low, &high));
  } catch (const std::exception &error) {
    disable(error.what());
  } catch (...) {
    disable("unknown error");
  }
}

void Tracer::record_unsplit(const Region &region, bool is_vertical) noexcept {
  if (!active()) {
    return;
  }
  try {
    pending_.push_back(SnapshotSampler::partition(region, is_vertical, nullptr, nullptr));
  } catch (const std::exception &error) {
    disable(error.what());
  } catch (...) {
    disable("unknown error");
  }
}

void Tracer::sample(odb::dbBlock *block, const ModuleRegistry &registry,
                    const std::vector<Region> &regions,
                    const Eigen::VectorXd &x,
                    const Eigen::VectorXd &y) noexcept {
  if (!active()) {
    return;
  }
  try {
    auto snapshot = SnapshotSampler::iteration(solves_, registry, regions, x, y);
    snapshot.phase = FramePhase::PostSolve;
    emit(block, registry, std::move(snapshot));
    ++solves_;
  } catch (const std::exception &error) {
    disable(error.what());
  } catch (...) {
    disable("unknown error");
  }
}

void Tracer::sample_partition(odb::dbBlock *block,
                              const ModuleRegistry &registry,
                              const std::vector<Region> &regions) noexcept {
  if (!active()) {
    return;
  }
  try {
    const auto count = static_cast<Eigen::Index>(registry.movable_count());
    Eigen::VectorXd x(count);
    Eigen::VectorXd y(count);
    for (const auto &module : registry.movable_modules()) {
      const auto [mx, my] = module.get_xy();
      x(static_cast<Eigen::Index>(module.get_id())) = mx;
      y(static_cast<Eigen::Index>(module.get_id())) = my;
    }
    std::optional<std::size_t> shown;
    if (solves_ > 0) {
      shown = solves_ - 1;
    }
    auto snapshot = SnapshotSampler::iteration(shown, registry, regions, x, y);
    snapshot.phase = FramePhase::PostPartition;
    emit(block, registry, std::move(snapshot));
  } catch (const std::exception &error) {
    disable(error.what());
  } catch (...) {
    disable("unknown error");
  }
}

void Tracer::emit(odb::dbBlock *block, const ModuleRegistry &registry,
                  IterationSnapshot snapshot) noexcept {
  try {
    snapshot.partitions = std::move(pending_);
    pending_.clear();
    if (!writer_) {
      writer_ = std::make_unique<JsonTraceWriter>(
          std::getenv("GP_TRACE_DIR"), SnapshotSampler::geometry(block, registry));
    }
    writer_->append(snapshot);
    record_metrics(block, registry, snapshot);
  } catch (const std::exception &error) {
    disable(error.what());
  } catch (...) {
    disable("unknown error");
  }
}
void Tracer::record_metrics(odb::dbBlock *block, const ModuleRegistry &registry,
                            const IterationSnapshot &snapshot) noexcept {
  const auto frame = frames_emitted_++;
  if (metrics_disabled_) {
    return;
  }
  try {
    if (!metrics_) {
      metrics_ = std::make_unique<PlacementMetrics>(block, registry);
      metrics_writer_ = std::make_unique<MetricsWriter>(
          std::getenv("GP_TRACE_DIR"), writer_->run_id(), metrics_->baseline());
    }
    metrics_writer_->append(metrics_->evaluate(frame, snapshot));
  } catch (const std::exception &error) {
    metrics_disabled_ = true;
    metrics_writer_.reset();
    metrics_.reset();
    std::cerr << "Warning: placement metrics disabled: " << error.what() << '\n';
  } catch (...) {
    metrics_disabled_ = true;
    metrics_writer_.reset();
    metrics_.reset();
    std::cerr << "Warning: placement metrics disabled: unknown error\n";
  }
}

} // namespace gordian::tracer
