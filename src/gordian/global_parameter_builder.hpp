#pragma once

#include "commmon.h"
#include "database/module_registry.hpp"
#include "database/pin_ref.hpp"
#include "gordian/movable_system.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <tuple>

namespace gordian {

class GlobalParameterBuilder {
private:
public:
  static auto build(odb::dbBlock *block, const ModuleRegistry &registry,
                    IoMode io_mode) -> MovableSystem {
    if (block == nullptr) {
      throw std::invalid_argument("GlobalParameterBuilder::build: null block");
    }
    // Validate collection even when the block contains no nets.
    (void)registry.movable_modules();
    auto full = build_full_C_b__(block, registry, io_mode);
    return build_movable_C_b__(std::move(full), registry);
  }

  // PARALLEL: OpenMP version of build(). Nets are processed in parallel,
  // each into its own slot; the slots are then merged serially in net order,
  // so every coefficient is summed in exactly the order build() uses. The
  // result is bit-identical to build() for any thread count. Without -fopenmp
  // the pragmas are ignored and it runs on one thread.
  static auto build_parallel(odb::dbBlock *block,
                             const ModuleRegistry &registry, IoMode io_mode)
      -> MovableSystem {
    if (block == nullptr) {
      throw std::invalid_argument(
          "GlobalParameterBuilder::build_parallel: null block");
    }
    (void)registry.movable_modules();
    auto full = build_full_C_b_parallel__(block, registry, io_mode);
    return build_movable_C_b__(std::move(full), registry);
  }

private:
  static auto extract_net_modules__(const odb::dbNet *net, IoMode io_mode,
                                    const ModuleRegistry &module_registry)
      -> Map<ModuleRef, Vector<PinRef>> {
    if (net == nullptr) {
      throw std::invalid_argument("extract_net_modules__: null net");
    }

    Map<ModuleRef, Vector<PinRef>> net_modules;
    auto add_pin = [&](PinRef pin) {
      net_modules[pin.get_module()].push_back(pin);
    };
    for (auto *iterm : net->getITerms()) {
      add_pin(PinRef::from_iterm(iterm,
                                 *module_registry.from_inst(iterm->getInst())));
    }
    if (io_mode == IoMode::FIXED) {
      for (auto *bterm : net->getBTerms()) {
        add_pin(PinRef::from_bterm(bterm, *module_registry.from_bterm(bterm)));
      }
    }
    return net_modules;
  }

  static auto get_pin_sum_of_offsets__(const std::vector<PinRef> &pins)
      -> Pair<double> {
    double x_acc = 0;
    double y_acc = 0;
    for (const auto &pin : pins) {
      const auto [x_off, y_off] = pin.get_offset();
      x_acc += x_off;
      y_acc += y_off;
    }
    return {x_acc, y_acc};
  }

  static auto build_full_C_b__(odb::dbBlock *block__,
                               const ModuleRegistry &module_registry__,
                               IoMode io_mode__) -> MovableSystem {
    MovableSystem full;
    auto &C_all__ = full.C;
    auto &bx_all__ = full.bx;
    auto &by_all__ = full.by;

    const size_t n_modules = module_registry__.size();
    C_all__.resize(n_modules, n_modules);
    C_all__.setZero();
    bx_all__.setZero(n_modules);
    by_all__.setZero(n_modules);
#ifdef DEBUG
    size_t net_cnt = 0, iter_net = 0;
#endif

    for (const auto *net : block__->getNets()) {
#ifdef DEBUG
      net_cnt++;
      std::cout << net_cnt << "/" << block__->getNets().size()
                << " name: " << net->getName()
                << "\ttype: " << net->getSigType().getString() << std::endl;
#endif
      if (!include_net__(net)) {
        std::cout << "Skipped " << net->getName() << std::endl;
        continue;
      }

      const double net_weight = net->getWeight();
      assert(net_weight == 1);

      const auto net_modules =
          extract_net_modules__(net, io_mode__, module_registry__);
      size_t p_n = 0;
      for (const auto &[module, pins] : net_modules) {
        p_n += pins.size();
      }
      if (p_n < 2) {
        continue;
      }
      const double e = net_weight / static_cast<double>(p_n);

      Map<size_t, Pair<double>> offset_sums;
      for (const auto &[module, pins] : net_modules) {
        offset_sums.emplace(module.get_id(), get_pin_sum_of_offsets__(pins));
      }

#ifdef DEBUG
      std::cout << "\tpins: " << p_n << "\tmodules: " << net_modules.size()
                << std::endl;
#endif

      for (const auto &[module, pins] : net_modules) {
        const auto i = module.get_id();
        const int p_ni = pins.size();
        const auto poff_sum_i = offset_sums.at(module.get_id());
        for (const auto &[another_module, another_pins] : net_modules) {
#ifdef DEBUG
          iter_net++;
#endif
          const auto j = another_module.get_id();
          if (i == j) {
            continue;
          }

          const int p_nj = another_pins.size();
          const double W_ij_partial = e * p_ni * p_nj;
          const auto poff_sum_j = offset_sums.at(another_module.get_id());
          /* Build *partial* W_{ij}, B^x_{ij} and B^y_{ij} */
          const double Bx_ij_partial =
              e * (p_nj * poff_sum_i.first - p_ni * poff_sum_j.first);
          const double By_ij_partial =
              e * (p_nj * poff_sum_i.second - p_ni * poff_sum_j.second);
          /* C_{ij} = -W_{ij} if i != j */
          C_all__.coeffRef(i, j) -= W_ij_partial;
          /* C_{ii} = \sum_{j \in M \land j \neq i} W_{ij} */
          C_all__.coeffRef(i, i) += W_ij_partial;
          /* B^x_{i} = \sum_{j \in M \land j \neq i} B^x_{ij} */
          bx_all__(i) += Bx_ij_partial;
          /* B^y_{i} = \sum_{j \in M \land j \neq i} B^y_{ij} */
          by_all__(i) += By_ij_partial;
        }
      }
#ifdef DEBUG
      std::cout << "\titers: " << iter_net << std::endl;
      iter_net = 0;
      std::cout << "C non-zeros: " << C_all__.nonZeros() << "\n";
#endif
    }
    return full;
  }

  // PARALLEL: one net's contribution, in the order build_full_C_b__ applies it.
  struct NetContribution__ {
    Vector<Eigen::Triplet<double>> C; // (i, j, -W_ij) then (i, i, +W_ij)
    Vector<std::tuple<Eigen::Index, double, double>> b; // (i, Bx_ij, By_ij)
  };

  // PARALLEL: OpenMP counterpart of build_full_C_b__.
  static auto build_full_C_b_parallel__(odb::dbBlock *block__,
                                        const ModuleRegistry &module_registry__,
                                        IoMode io_mode__) -> MovableSystem {
    const auto n_modules = static_cast<Eigen::Index>(module_registry__.size());

    // Filter serially: ODB set iteration is sequential, and the "Skipped"
    // messages keep the serial builder's order.
    Vector<const odb::dbNet *> nets;
    for (const auto *net : block__->getNets()) {
      if (!include_net__(net)) {
        std::cout << "Skipped " << net->getName() << std::endl;
        continue;
      }
      assert(net->getWeight() == 1);
      nets.push_back(net);
    }

    Vector<NetContribution__> contributions(nets.size());
    std::exception_ptr error;
    std::mutex error_mutex;
    const auto n_nets = static_cast<std::int64_t>(nets.size());

    // Read-only ODB/registry access; each iteration writes only its own slot.
#pragma omp parallel for schedule(dynamic, 64)
    for (std::int64_t k = 0; k < n_nets; ++k) {
      const auto slot = static_cast<std::size_t>(k);
      try {
        contributions[slot] = net_contribution_parallel__(nets[slot], io_mode__,
                                                          module_registry__);
      } catch (...) {
        // Exceptions must not leave an OpenMP region; rethrow afterwards.
        const std::lock_guard lock(error_mutex);
        if (!error) {
          error = std::current_exception();
        }
      }
    }
    if (error) {
      std::rethrow_exception(error);
    }

    // Serial merge in net order. setFromTriplets sums duplicates in insertion
    // order, matching build_full_C_b__'s coeffRef accumulation.
    std::size_t n_triplets = 0;
    for (const auto &contribution : contributions) {
      n_triplets += contribution.C.size();
    }
    Vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(n_triplets);
    MovableSystem full;
    full.bx.setZero(n_modules);
    full.by.setZero(n_modules);
    for (auto &contribution : contributions) {
      triplets.insert(triplets.end(), contribution.C.begin(),
                      contribution.C.end());
      for (const auto &[i, bx, by] : contribution.b) {
        full.bx(i) += bx;
        full.by(i) += by;
      }
      contribution = {}; // release memory as we go
    }
    full.C.resize(n_modules, n_modules);
    full.C.setFromTriplets(triplets.begin(), triplets.end());
    return full;
  }

  // PARALLEL helper: mirrors one iteration of build_full_C_b__'s net loop.
  static auto net_contribution_parallel__(const odb::dbNet *net, IoMode io_mode,
                                          const ModuleRegistry &module_registry)
      -> NetContribution__ {
    NetContribution__ out;
    const auto net_modules =
        extract_net_modules__(net, io_mode, module_registry);
    size_t p_n = 0;
    for (const auto &[module, pins] : net_modules) {
      p_n += pins.size();
    }
    if (p_n < 2) {
      return out;
    }
    const double e = net->getWeight() / static_cast<double>(p_n);

    Map<size_t, Pair<double>> offset_sums;
    for (const auto &[module, pins] : net_modules) {
      offset_sums.emplace(module.get_id(), get_pin_sum_of_offsets__(pins));
    }

    for (const auto &[module, pins] : net_modules) {
      const auto i = module.get_id();
      const int p_ni = pins.size();
      const auto poff_sum_i = offset_sums.at(i);
      for (const auto &[another_module, another_pins] : net_modules) {
        const auto j = another_module.get_id();
        if (i == j) {
          continue;
        }
        const int p_nj = another_pins.size();
        const double W_ij_partial = e * p_ni * p_nj;
        const auto poff_sum_j = offset_sums.at(j);
        const double Bx_ij_partial =
            e * (p_nj * poff_sum_i.first - p_ni * poff_sum_j.first);
        const double By_ij_partial =
            e * (p_nj * poff_sum_i.second - p_ni * poff_sum_j.second);
        out.C.emplace_back(static_cast<int>(i), static_cast<int>(j),
                           -W_ij_partial);
        out.C.emplace_back(static_cast<int>(i), static_cast<int>(i),
                           W_ij_partial);
        out.b.emplace_back(static_cast<Eigen::Index>(i), Bx_ij_partial,
                           By_ij_partial);
      }
    }
    return out;
  }

  static auto build_movable_C_b__(MovableSystem full,
                                  const ModuleRegistry &registry)
      -> MovableSystem {
    const auto m = static_cast<Eigen::Index>(registry.n_movable_modules());
    const auto f = static_cast<Eigen::Index>(registry.n_fixed_modules());
    if (f == 0) {
      return full;
    }

    MovableSystem result;
    result.C.resize(m, m);
    result.bx.setZero(m);
    result.by.setZero(m);
    if (m == 0) {
      return result;
    }

    Eigen::VectorXd xf(f), yf(f);
    Eigen::Index i = 0;
    for (const auto &module : registry.fixed_modules()) {
      const auto [x, y] = module.get_xy();
      xf(i) = x;
      yf(i) = y;
      ++i;
    }

    // C_MF has movable rows and fixed columns, not the bottom-left C_FM.
    const auto C_mf = full.C.topRightCorner(m, f);
    result.C = full.C.topLeftCorner(m, m);
    result.bx = full.bx.head(m) + C_mf * xf;
    result.by = full.by.head(m) + C_mf * yf;
    result.C.makeCompressed();
    return result;
  }

public:
  // The solver's net filter. Public so that evaluation code (the tracer's
  // metrics) can report on exactly the nets the solver models.
  static auto include_net__(const odb::dbNet *net) -> bool {
    switch (net->getSigType().getValue()) {
    case odb::dbSigType::POWER:
    case odb::dbSigType::GROUND:
    case odb::dbSigType::CLOCK:
    case odb::dbSigType::RESET:
      return false;
    default:
      break;
    }
    const std::string name = net->getName();
    return name != "clk_i" && name != "rst_i" &&
           net->getITerms().size() <= 1000;
  }
};

} // namespace gordian
