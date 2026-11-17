#pragma once

#include "commmon.h"
#include "odb/db.h"

#include "database/module_ref.hpp"
#include "database/module_registry.hpp"
#include "gordian/constraint_system.hpp"
#include "gordian/constraint_system_builder.hpp"
#include "gordian/constraint_system_solver.hpp"
#include "gordian/global_parameter_builder.hpp"
#include "gordian/region.hpp"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <cassert>
#include <iostream>
#include <map>
#include <odb/dbSet.h>
#include <stdexcept>
#include <string>

namespace gordian {

class Solver {

public:
  using IoMode = gordian::IoMode;

  Solver(odb::dbBlock *block, IoMode io_mode = IoMode::FIXED)
      : io_mode__(io_mode), block__(block) {
    if (!input_validity_checker__(block)) {
      throw std::runtime_error("Unsupported input");
    }
    construct_module_data__();
    initial_partition__();
    parameters__ =
        GlobalParameterBuilder::build(block__, module_registry__, io_mode__);
    std::cout << "Finished building" << std::endl;
    // std::cout << "C=" << parameters__.C << std::endl;
    // std::cout << "bx=" << parameters__.bx.transpose() << std::endl;
    // std::cout << "by=" << parameters__.by.transpose() << std::endl;
  }

  static auto input_validity_checker__(odb::dbBlock *block) -> bool {
    return disallow_multiple_site_template(block);
  }

  static auto disallow_multiple_site_template(odb::dbBlock *block) -> bool {
    std::map<std::string, int> sites;
    for (const auto row : block->getRows()) {
      ++sites[row->getSite()->getName()];
    }
    const bool single_template = sites.size() == 1;
    if (!single_template) {
      std::cout << "Input is not single template\n";
    }
    return single_template;
  }

  void construct_module_data__() {
    blockages__.clear();
    for (auto *blockage : block__->getBlockages()) {
      assert(blockage->getObjectType() == odb::dbObjectType::dbBlockageObj);
      blockages__.insert(blockage);
    }
    module_registry__.collect(block__, io_mode__);
  }

  void initial_partition__() {
    Region region;
    for (const auto &module : module_registry__.get_movable_modules()) {
      region.movable_modules.push_back(&module);
    }
    for (const auto &module : module_registry__.get_fixed_modules()) {
      region.fixed_modules.push_back(&module);
    }
    region.x_min = block__->getBBox()->xMin();
    region.y_min = block__->getBBox()->yMin();
    region.x_max = block__->getBBox()->xMax();
    region.y_max = block__->getBBox()->yMax();
    regions__ = {region};
  }

  // TODO: remove this interface
  auto parameters() const -> const MovableSystem & { return parameters__; }

  void run() {
    const auto constraint_system =
        ConstraintSystemBuilder(module_registry__, regions__, parameters__)
            .build();
    const auto [x, y] = ConstraintSystemSolver::solve(constraint_system);
    for (size_t i = 0; i < module_registry__.size(); ++i) {
      module_registry__.place_module(i, x(i), y(i));
    }
  }

private:
  // --- OpenROAD stage ---
  IoMode io_mode__;

  // --- database ---
  odb::dbBlock *block__;

  // --- modules ---
  ModuleRegistry module_registry__;
  Set<odb::dbBlockage *> blockages__;

  // --- algorithmic structures ---
  MovableSystem parameters__;

  Vector<Region> regions__;
};
} /* namespace gordian */
