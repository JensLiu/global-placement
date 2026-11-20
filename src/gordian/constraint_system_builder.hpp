#pragma once

#include "database/module_registry.hpp"
#include "gordian/constraint_system.hpp"
#include "gordian/region.hpp"

namespace gordian {

class ConstraintSystemBuilder {
public:
  ConstraintSystemBuilder(const ModuleRegistry &registry,
                          const Vector<Region> &regions,
                          const MovableSystem &parameters)
      : registry__(registry), regions__(regions), sys__(parameters) {
    sys__.module_id_to_free_id__.assign(registry.n_movable_modules(),
                                        ConstraintSystem::not_free);

    build_free_id__();
    build_Z_dx_dy__();
  }

  auto build() -> ConstraintSystem { return std::move(sys__); }

private:
  auto to_free_id__(const size_t module_id) -> size_t {
    return sys__.module_id_to_free_id__.at(module_id);
  }

  void build_free_id__() {

    Set<size_t> rep_module_ids;
    for (const auto &region : regions__) {
      rep_module_ids.insert(region.representative()->get_id());
    }

    size_t free_id = 0;
    for (const auto module : registry__.get_movable_modules()) {
      if (rep_module_ids.contains(module.get_id())) {
        continue;
      }
      const auto module_id = module.get_id();
      sys__.module_id_to_free_id__[module_id] = free_id;
      free_id++;
    }
  }

  void build_Z_dx_dy__() {
    const size_t n_modules = registry__.n_movable_modules();
    const size_t n_regions = regions__.size();
    const size_t n_free_vars = n_modules - n_regions;

    sys__.Z.resize(n_modules, n_free_vars);
    sys__.x0.resize(n_modules);
    sys__.y0.resize(n_modules);
    sys__.Z.setZero();
    sys__.x0.setZero();
    sys__.y0.setZero();

    for (auto &region : regions__) {
      const auto reg_rep = region.representative();
      const auto reg_rep_id = reg_rep->get_id();

      double total_area = 0;

      /* independent modules */
      for (auto &module : region.movable_modules) {
        const auto i = module->get_id();
        const auto si = module->get_area();
        total_area += si;
        if (reg_rep_id == i) {
          continue;
        }
        sys__.Z.coeffRef(i, to_free_id__(i)) = 1;
      }

      /* dependent/representative module */
      for (auto &another_module : region.movable_modules) {
        const auto j = another_module->get_id();
        if (reg_rep_id == j) {
          continue;
        }
        const auto sj = another_module->get_area();
        const auto si = reg_rep->get_area();
        sys__.Z.coeffRef(reg_rep_id, to_free_id__(j)) = -sj / si;
      }

      const auto ratio = total_area / reg_rep->get_area();
      const auto [x_bar, y_bar] = region.get_centre_xy();
      sys__.x0(reg_rep_id) = ratio * x_bar;
      sys__.y0(reg_rep_id) = ratio * y_bar;
    }

    // std::cout << "Z=" << sys__.Z << std::endl;
    // std::cout << "x0=" << sys__.x0.transpose() << std::endl;
    // std::cout << "y0=" << sys__.y0.transpose() << std::endl;

    sys__.Z.makeCompressed();

    const auto &param = sys__.parameters;
    sys__.dx = sys__.Z.transpose() * (param.C * sys__.x0 + param.bx);
    sys__.dy = sys__.Z.transpose() * (param.C * sys__.y0 + param.by);

    // Stopping reference only; the system above is unchanged.
    Eigen::VectorXd xc(static_cast<Eigen::Index>(n_modules));
    Eigen::VectorXd yc(static_cast<Eigen::Index>(n_modules));
    for (const auto &region : regions__) {
      const auto [x_bar, y_bar] = region.get_centre_xy();
      for (const auto *module : region.movable_modules) {
        xc(static_cast<Eigen::Index>(module->get_id())) = x_bar;
        yc(static_cast<Eigen::Index>(module->get_id())) = y_bar;
      }
    }
    sys__.dx_reference_norm =
        (sys__.Z.transpose() * (param.C * xc + param.bx)).norm();
    sys__.dy_reference_norm =
        (sys__.Z.transpose() * (param.C * yc + param.by)).norm();
  }

  const ModuleRegistry &registry__;
  const Vector<Region> &regions__;
  ConstraintSystem sys__;
};

} // namespace gordian
