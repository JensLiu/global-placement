// Opt-in real-design audit: independent pin-centroid gradients and KKT checks.
#include "gordian/constraint_system_builder.hpp"
#include "gordian/constraint_system_solver.hpp"
#include "gordian/global_parameter_builder.hpp"
#include "utl/Logger.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <string>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
struct Endpoint { Eigen::Index id; double dx, dy; };
struct Net { std::vector<Endpoint> pins; double weight; };

// No PinRef, module-pair loops, C or Z: differentiate each pin's distance
// from its net's mean. Fixed endpoints participate in that mean as constants.
auto extract(odb::dbBlock *block, const gordian::ModuleRegistry &registry,
             gordian::IoMode mode) -> std::vector<Net> {
  std::vector<Net> nets;
  std::size_t max_pins = 0, pairs = 0;
  for (auto *net : block->getNets()) {
    if (!gordian::GlobalParameterBuilder::include_net__(net)) continue;
    Net out{{}, static_cast<double>(net->getWeight())};
    for (auto *pin : net->getITerms()) {
      int x, y;
      require(pin->getAvgXY(&x, &y), "oracle: missing pin geometry");
      const auto box = pin->getInst()->getBBox()->getBox();
      out.pins.push_back({static_cast<Eigen::Index>(registry.from_inst(pin->getInst())->get_id()),
                         x - 0.5 * box.xMin() - 0.5 * box.xMax(),
                         y - 0.5 * box.yMin() - 0.5 * box.yMax()});
    }
    if (mode == gordian::IoMode::FIXED) {
      for (auto *pin : net->getBTerms())
        out.pins.push_back({static_cast<Eigen::Index>(registry.from_bterm(pin)->get_id()), 0, 0});
    }
    max_pins = std::max(max_pins, out.pins.size());
    pairs += out.pins.size() * out.pins.size();
    nets.push_back(std::move(out));
  }
  std::cout << "ORACLE nets " << nets.size() << " max_pins " << max_pins
            << " pin_pairs_upper_bound " << pairs << '\n';
  return nets;
}

auto gradient(const std::vector<Net> &nets, const gordian::ModuleRegistry &registry,
              const Eigen::VectorXd &moving, bool y_axis) -> Eigen::VectorXd {
  Eigen::VectorXd g = Eigen::VectorXd::Zero(moving.size());
  auto coordinate = [&](const Endpoint &p) {
    const auto fixed = registry.at(p.id)->get_xy();
    return (p.id < moving.size() ? moving(p.id) : (y_axis ? fixed.second : fixed.first))
           + (y_axis ? p.dy : p.dx);
  };
  for (const auto &net : nets) {
    if (net.pins.empty()) continue;
    double mean = 0;
    for (const auto &pin : net.pins) mean += coordinate(pin);
    mean /= net.pins.size();
    for (const auto &pin : net.pins)
      if (pin.id < moving.size()) g(pin.id) += net.weight * (coordinate(pin) - mean);
  }
  return g;
}

void dump(std::ofstream &out, const Eigen::VectorXd &v) {
  const std::int64_t n = v.size();
  out.write(reinterpret_cast<const char *>(&n), sizeof(n));
  out.write(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(double));
}
void dump(std::ofstream &out, const Eigen::SparseMatrix<double> &matrix) {
  const std::int64_t shape[] = {matrix.rows(), matrix.cols(), matrix.nonZeros()};
  out.write(reinterpret_cast<const char *>(shape), sizeof(shape));
  for (Eigen::Index col = 0; col < matrix.outerSize(); ++col)
    for (Eigen::SparseMatrix<double>::InnerIterator it(matrix, col); it; ++it) {
      const std::int64_t indices[] = {it.row(), it.col()};
      const double value = it.value();
      out.write(reinterpret_cast<const char *>(indices), sizeof(indices));
      out.write(reinterpret_cast<const char *>(&value), sizeof(value));
    }
}

// Orthogonal projection onto the tangent of the area-centroid constraints,
// independently of the representative and of the constructed null-space Z.
auto tangent(Eigen::VectorXd g, const std::vector<gordian::Region> &regions)
    -> Eigen::VectorXd {
  for (const auto &region : regions) {
    long double dot = 0, norm = 0;
    for (const auto *m : region.movable_modules) {
      const long double area = m->get_area();
      dot += area * g(m->get_id());
      norm += area * area;
    }
    require(norm > 0, "zero area constraint");
    for (const auto *m : region.movable_modules)
      g(m->get_id()) -= static_cast<double>(dot / norm * m->get_area());
  }
  return g;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc >= 5, "usage: real_design_audit design.odb serial|parallel|compare|parallel-check snapshot levels [fixed|skip] [largest]");
    const bool parallel = std::string(argv[2]) == "parallel" || std::string(argv[2]) == "parallel-check";
    const bool compare = std::string(argv[2]) == "compare" || std::string(argv[2]) == "parallel-check";
    require(parallel || compare || std::string(argv[2]) == "serial", "invalid builder");
    const int levels = std::stoi(argv[4]);
    const auto mode = argc > 5 && std::string(argv[5]) == "skip"
                          ? gordian::IoMode::SKIP : gordian::IoMode::FIXED;
    std::cout << std::setprecision(17);
#ifdef _OPENMP
    std::cout << "THREADS " << omp_get_max_threads() << " EIGEN_THREADS " << Eigen::nbThreads() << '\n';
#endif
    utl::Logger logger;
    std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
        odb::dbDatabase::create(), &odb::dbDatabase::destroy);
    db->setLogger(&logger);
    std::ifstream input(argv[1], std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    db->read(input);
    auto *block = db->getChip()->getBlock();
    gordian::ModuleRegistry registry;
    registry.collect(block, mode);
    const auto n = static_cast<Eigen::Index>(registry.movable_count());
    std::cout << "MODULES movable " << n << " fixed " << registry.n_fixed_modules() << std::endl;
    const auto start = Clock::now();
    auto system = parallel ? gordian::GlobalParameterBuilder::build_parallel(block, registry, mode)
                           : gordian::GlobalParameterBuilder::build(block, registry, mode);
    std::cout << "ASSEMBLY seconds " << std::chrono::duration<double>(Clock::now()-start).count()
              << " nnz " << system.C.nonZeros() << std::endl;
    if (compare) {
      // Same DB and address layout: ModuleRef's pointer hash can change the
      // within-net accumulation order between separate processes.
      for (int threads : {1, 8, 32}) {
#ifdef _OPENMP
        omp_set_num_threads(threads);
#endif
        for (int repeat = 0; repeat < 2; ++repeat) {
          const auto begin = Clock::now();
          const auto other = gordian::GlobalParameterBuilder::build_parallel(block, registry, mode);
          std::size_t different = 0;
          for (Eigen::Index col = 0; col < system.C.outerSize(); ++col)
            for (Eigen::SparseMatrix<double>::InnerIterator it(system.C, col); it; ++it)
              different += it.value() != other.C.coeff(it.row(), it.col());
          require(other.C.nonZeros() == system.C.nonZeros() && different == 0 &&
                  (system.bx.array() == other.bx.array()).all() &&
                  (system.by.array() == other.by.array()).all(), "serial/parallel coefficients differ");
          std::cout << "EXACT_COMPARE threads " << threads << " repeat " << repeat
                    << " seconds " << std::chrono::duration<double>(Clock::now()-begin).count()
                    << " PASS" << std::endl;
        }
      }
    }
    require(system.C.rows() == n && system.bx.allFinite() && system.by.allFinite(), "bad system dimensions/data");
    const Eigen::SparseMatrix<double> transposed = system.C.transpose();
    const Eigen::SparseMatrix<double> asymmetry = system.C - transposed;
    const double symmetry = asymmetry.norm() / std::max(1.0, system.C.norm());
    require(symmetry < 1e-12, "asymmetric C");
    std::ofstream output(argv[3], std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    dump(output, system.C); dump(output, system.bx); dump(output, system.by);
    const auto nets = extract(block, registry, mode);
    std::mt19937 rng(20261120);
    for (int trial = 0; trial < 3; ++trial) {
      Eigen::VectorXd position(n);
      for (Eigen::Index i = 0; i < n; ++i) position(i) = static_cast<int>(rng() % 1000001) - 500000.25;
      for (bool y : {false, true}) {
        const Eigen::VectorXd direct = gradient(nets, registry, position, y);
        const Eigen::VectorXd assembled = system.C * position + (y ? system.by : system.bx);
        const double error = (direct - assembled).norm() / std::max(1.0, direct.norm());
        std::cout << "GRADIENT trial " << trial << " axis " << y << " relative_error " << error << '\n';
        require(error < 1e-11 && direct.allFinite() && assembled.allFinite(), "independent gradient mismatch");
      }
    }
    gordian::Region root;
    const auto core = block->getCoreArea();
    root.x_min = core.xMin(); root.x_max = core.xMax();
    root.y_min = core.yMin(); root.y_max = core.yMax();
    for (const auto &m : registry.movable_modules()) root.movable_modules.push_back(&m);
    std::vector<gordian::Region> regions{root};
    Eigen::VectorXd x(n), y(n);
    for (const auto &m : registry.movable_modules()) {
      const auto xy = m.get_xy(); x(m.get_id()) = xy.first; y(m.get_id()) = xy.second;
    }
    int residual_failures = 0;
    for (int level = 0; level < levels; ++level) {
      // Optional diagnostic only; production representative policy is untouched.
      if (argc > 6 && std::string(argv[6]) == "largest") {
        for (auto &region : regions) {
          auto largest = std::max_element(region.movable_modules.begin(), region.movable_modules.end(),
              [](const auto *a, const auto *b) {
                return a->get_area() != b->get_area() ? a->get_area() < b->get_area() : a->get_id() > b->get_id();
              });
          std::iter_swap(region.movable_modules.begin(), largest);
        }
      }
      auto constraints = gordian::ConstraintSystemBuilder(registry, regions, system).build();
      auto solution = gordian::ConstraintSystemSolver::solve(constraints, x, y);
      x = std::move(solution.first); y = std::move(solution.second);
      require(x.allFinite() && y.allFinite(), "non-finite solution");
      std::vector<int> membership(n, 0);
      double centroid_error = 0;
      Eigen::VectorXd xc(n), yc(n);
      for (const auto &region : regions) {
        long double total = 0, sx = 0, sy = 0;
        const auto centre = region.get_centre_xy();
        for (const auto *m : region.movable_modules) {
          const auto id = m->get_id(); ++membership.at(id);
          const long double area = m->get_area();
          total += area; sx += area*x(id); sy += area*y(id);
          xc(id) = centre.first; yc(id) = centre.second;
        }
        centroid_error = std::max(centroid_error, static_cast<double>(std::max(
            std::abs(sx/total-centre.first), std::abs(sy/total-centre.second))));
      }
      for (int count : membership) require(count == 1, "partition is not an exact cover");
      const double core_scale = std::max(root.x_max-root.x_min, root.y_max-root.y_min);
      require(centroid_error < 1e-9 * std::max(1.0, core_scale), "centroid feasibility failure");
      std::cout << "LEVEL " << level << " regions " << regions.size()
                << " centroid_error_dbu " << centroid_error;
      for (bool axis : {false, true}) {
        const auto &position = axis ? y : x;
        const auto &centre = axis ? yc : xc;
        const auto &b = axis ? system.by : system.bx;
        const Eigen::VectorXd direct = gradient(nets, registry, position, axis);
        const Eigen::VectorXd assembled = system.C * position + b;
        const Eigen::VectorXd projected = constraints.Z.transpose() * direct;
        const double reference = axis ? constraints.dy_reference_norm : constraints.dx_reference_norm;
        const double reduced_relative = projected.norm() / std::max(1.0, reference);
        const double physical_relative = tangent(direct, regions).norm() /
            std::max(1.0, tangent(gradient(nets, registry, centre, axis), regions).norm());
        const Eigen::VectorXd u = constraints.to_free(position, axis ? constraints.y0 : constraints.x0);
        const Eigen::VectorXd algebraic_residual = constraints.Z.transpose() *
            (system.C * (constraints.Z * u)) + (axis ? constraints.dy : constraints.dx);
        const Eigen::VectorXd reconstructed_residual = constraints.Z.transpose() * assembled;
        std::cout << (axis ? " y" : " x") << "_true_reduced_relative " << reduced_relative
                  << " tangent_relative " << physical_relative
                  << " algebraic_relative " << algebraic_residual.norm()/std::max(1.0, reference)
                  << " reconstructed_relative " << reconstructed_residual.norm()/std::max(1.0, reference);
        require((direct-assembled).norm() < 1e-10*std::max(1.0, assembled.norm()), "solved gradient mismatch");
        // Fail on false CG convergence, allowing 10% for reconstruction rounding.
        if (reduced_relative >= 1.1e-8) {
          ++residual_failures;
          std::cout << " [TRUE_RESIDUAL_EXCEEDS_TARGET]";
        }
      }
      std::cout << std::endl;
      dump(output, x); dump(output, y);
      for (Eigen::Index i = 0; i < n; ++i) registry.place_module(i, x(i), y(i));
      bool done = true;
      std::vector<gordian::Region> next;
      for (const auto &region : regions) {
        done = done && region.movable_modules.size() <= 5;
        gordian::RegionPartitioner<gordian::RegionPartitionPolicy::Naive> partitioner(region, level % 2 == 0);
        partitioner.skip_when_n_modules_below(5);
        const auto split = partitioner.partition();
        if (split) { next.push_back(split->first); next.push_back(split->second); }
        else next.push_back(region);
      }
      if (done) break;
      regions = std::move(next);
    }
    std::cout << (residual_failures ? "AUDIT FAIL" : "AUDIT PASS")
              << " symmetry_relative " << symmetry
              << " residual_failures " << residual_failures << std::endl;
    return residual_failures ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << "AUDIT FAIL: " << error.what() << std::endl;
    return 1;
  }
}
