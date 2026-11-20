#pragma once

// Independent test oracle: pin-to-centroid incidence matrices, not module pairs.
#include "gordian/global_parameter_builder.hpp"
#include "utl/Logger.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace gordian_test {

inline void check_close(double actual, double expected, const char *what) {
  if (std::abs(actual - expected) > 1e-8 * (1.0 + std::abs(expected))) {
    throw std::runtime_error(what);
  }
}

struct CentroidNet {
  Eigen::MatrixXd incidence;
  Eigen::VectorXd dx;
  Eigen::VectorXd dy;
  double weight;
};

// Independent full pin-centroid model followed by fixed substitution.
inline void verify_objective(const gordian::MovableSystem& system,
                             const gordian::ModuleRegistry& registry,
                             const std::vector<odb::dbNet*>& included_nets,
                             gordian::IoMode io_mode, std::mt19937& rng) {
  const auto n = static_cast<Eigen::Index>(registry.size());
  Eigen::MatrixXd expected_c = Eigen::MatrixXd::Zero(n, n);
  Eigen::VectorXd expected_bx = Eigen::VectorXd::Zero(n);
  Eigen::VectorXd expected_by = Eigen::VectorXd::Zero(n);
  std::vector<CentroidNet> nets;

  for (auto *net : included_nets) {
    struct Endpoint { std::size_t owner; double dx; double dy; };
    std::vector<Endpoint> endpoints;
    // Read geometry directly, without PinRef or builder extraction.
    for (auto *iterm : net->getITerms()) {
      int x, y;
      if (!iterm->getAvgXY(&x, &y)) {
        throw std::runtime_error("oracle: missing ITerm geometry");
      }
      const auto bbox = iterm->getInst()->getBBox()->getBox();
      endpoints.push_back({registry.from_inst(iterm->getInst())->get_id(),
                           x - 0.5 * bbox.xMin() - 0.5 * bbox.xMax(),
                           y - 0.5 * bbox.yMin() - 0.5 * bbox.yMax()});
    }
    if (io_mode == gordian::IoMode::FIXED) {
      for (auto *bterm : net->getBTerms()) {
        endpoints.push_back({registry.from_bterm(bterm)->get_id(), 0, 0});
      }
    }
    const auto q = static_cast<Eigen::Index>(endpoints.size());
    if (q == 0) {
      continue;
    }
    CentroidNet model{Eigen::MatrixXd::Zero(q, n), Eigen::VectorXd(q),
                      Eigen::VectorXd(q), static_cast<double>(net->getWeight())};
    for (Eigen::Index p = 0; p < q; ++p) {
      model.incidence(p, endpoints[p].owner) = 1;
      model.dx(p) = endpoints[p].dx;
      model.dy(p) = endpoints[p].dy;
    }
    const Eigen::MatrixXd projection =
        Eigen::MatrixXd::Identity(q, q) - Eigen::MatrixXd::Ones(q, q) / q;
    expected_c += model.weight * model.incidence.transpose() * projection * model.incidence;
    expected_bx += model.weight * model.incidence.transpose() * projection * model.dx;
    expected_by += model.weight * model.incidence.transpose() * projection * model.dy;
    nets.push_back(std::move(model));
  }


  const auto m = static_cast<Eigen::Index>(registry.movable_count());
  const Eigen::MatrixXd c = system.C;
  const auto& bx = system.bx;
  const auto& by = system.by;
  if (c.rows() != m || c.cols() != m || bx.size() != m || by.size() != m) {
    throw std::runtime_error("incorrect reduced dimensions");
  }
  check_close((c - expected_c.topLeftCorner(m, m)).norm(), 0,
              "reduced C differs from centroid Hessian");
  check_close((c - c.transpose()).norm(), 0, "C is not symmetric");
  if (m > 0) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(c);
    if (eig.info() != Eigen::Success || eig.eigenvalues().minCoeff() < -1e-9) {
      throw std::runtime_error("C is not positive semidefinite");
    }
  }
  for (bool y_axis : {false, true}) {
    const auto& b = y_axis ? by : bx;
    const auto& expected_b = y_axis ? expected_by : expected_bx;
    Eigen::VectorXd baseline = Eigen::VectorXd::Zero(n);
    for (Eigen::Index id = m; id < n; ++id) {
      const auto xy = registry.at(id)->get_xy();
      baseline(id) = y_axis ? xy.second : xy.first;
    }
    const Eigen::VectorXd reduced_b =
        expected_b.head(m) +
        expected_c.topRightCorner(m, n - m) * baseline.tail(n - m);
    check_close((b - reduced_b).norm(), 0, "incorrect reduced linear term");
    auto evaluate = [&](const Eigen::VectorXd& z) {
      double energy = 0;
      for (const auto& net : nets) {
        const Eigen::VectorXd pins = net.incidence * z + (y_axis ? net.dy : net.dx);
        energy += 0.5 * net.weight *
                  (pins.array() - pins.mean()).matrix().squaredNorm();
      }
      return energy;
    };
    const double constant = evaluate(baseline);
    for (int trial = 0; trial < 4; ++trial) {
      Eigen::VectorXd z = baseline;
      for (Eigen::Index i = 0; i < m; ++i) {
        z(i) = static_cast<int>(rng() % 101) - 50.0 + 0.25;
      }
      const Eigen::VectorXd moving = z.head(m);
      check_close(evaluate(z) - constant,
                  0.5 * moving.dot(c * moving) + b.dot(moving),
                  "reduced quadratic differs from centroid energy differences");
      Eigen::VectorXd gradient = Eigen::VectorXd::Zero(n);
      for (const auto& net : nets) {
        const Eigen::VectorXd pins = net.incidence * z + (y_axis ? net.dy : net.dx);
        gradient += net.weight * net.incidence.transpose() *
                    (pins.array() - pins.mean()).matrix();
      }
      check_close((gradient.head(m) - (c * moving + b)).norm(), 0,
                  "reduced gradient differs from centroid gradient");
    }
  }
}

inline void randomized_objective_checks() {
  std::mt19937 rng(20261116);
  for (int case_id = 0; case_id < 24; ++case_id) {
    utl::Logger logger;
    std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
        odb::dbDatabase::create(), &odb::dbDatabase::destroy);
    db->setLogger(&logger);
    auto *tech = odb::dbTech::create(db.get(), "tech");
    auto *layer = odb::dbTechLayer::create(tech, "metal1", odb::dbTechLayerType::ROUTING);
    auto *lib = odb::dbLib::create(db.get(), "lib", tech);
    auto *site = odb::dbSite::create(lib, "site");
    site->setWidth(1);
    site->setHeight(19);
    auto *chip = odb::dbChip::create(db.get(), tech);
    auto *block = odb::dbBlock::create(chip, "random");
    odb::dbRow::create(block, "row", site, 0, 0, odb::dbOrientType::R0,
                       odb::dbRowDir::HORIZONTAL, 100, 1);
    auto *master = odb::dbMaster::create(lib, "cell");
    master->setWidth(31);
    master->setHeight(19);
    master->setType(odb::dbMasterType::CORE);
    for (int p = 0; p < 6; ++p) {
      auto *term = odb::dbMTerm::create(master, ("p" + std::to_string(p)).c_str(),
                                       odb::dbIoType::INPUT, odb::dbSigType::SIGNAL);
      auto *pin = odb::dbMPin::create(term);
      // Different x/y offsets and half-DBU centers; not all pins coincide.
      odb::dbBox::create(pin, layer, 2 + 4 * p, 1 + 2 * p, 4 + 4 * p, 3 + 2 * p);
    }
    master->setFrozen();
    std::vector<odb::dbNet *> included_nets;
    for (int k = 0; k < 7; ++k) {
      auto *net = odb::dbNet::create(block, ("n" + std::to_string(k)).c_str());
      net->setWeight(1);
      included_nets.push_back(net);
    }
    auto *ignored = odb::dbNet::create(block, "ignored_supply");
    ignored->setSigType(odb::dbSigType::POWER);
    ignored->setWeight(1);
    const odb::dbOrientType::Value orientations[] = {
        odb::dbOrientType::R0, odb::dbOrientType::R180,
        odb::dbOrientType::MX, odb::dbOrientType::MY};
    for (int i = 0; i < 6; ++i) {
      auto *inst = odb::dbInst::create(block, master, ("u" + std::to_string(i)).c_str());
      inst->setOrient(orientations[(i + case_id) % 4]);
      inst->setLocation(static_cast<int>(rng() % 100), static_cast<int>(rng() % 100));
      inst->setPlacementStatus(i < case_id % 7 ? odb::dbPlacementStatus::PLACED
                                     : odb::dbPlacementStatus::LOCKED);
      for (int p = 0; p < 6; ++p) {
        auto *iterm = inst->findITerm(("p" + std::to_string(p)).c_str());
        if (p == 5) {
          iterm->connect(ignored);
        } else if (p != 4) {
          iterm->connect(included_nets[rng() % 5]);
        }
      }
    }
    // Include two IOs on one net, an IO-only singleton, and an empty net.
    for (int k = 0; k < 3; ++k) {
      auto *io = odb::dbBTerm::create(included_nets[k < 2 ? 0 : 5],
                                     ("io" + std::to_string(k)).c_str());
      auto *bpin = odb::dbBPin::create(io);
      odb::dbBox::create(bpin, layer, 100 + 10 * k, 150 + 3 * k,
                        103 + 10 * k, 153 + 3 * k);
      bpin->setPlacementStatus(k == 1 ? odb::dbPlacementStatus::LOCKED
                                     : odb::dbPlacementStatus::PLACED);
    }
    for (auto mode : {gordian::IoMode::SKIP, gordian::IoMode::FIXED}) {
      gordian::ModuleRegistry registry;
      registry.collect(block, mode);
      const gordian::GlobalParameterBuilder builder;
      auto system = builder.build(block, registry, mode);
      verify_objective(system, registry, included_nets, mode, rng);
      auto parallel = builder.build_parallel(block, registry, mode);
      if (parallel.C.nonZeros() != system.C.nonZeros() ||
          (parallel.C - system.C).norm() != 0 ||
          !(parallel.bx.array() == system.bx.array()).all() ||
          !(parallel.by.array() == system.by.array()).all()) {
        throw std::runtime_error("parallel builder differs from serial oracle case");
      }
      verify_objective(parallel, registry, included_nets, mode, rng);
    }
  }
}

} // namespace gordian_test
