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
#include "tracer/placement_tracer.hpp"

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
    const auto core_area = block__->getCoreArea();
    region.x_min = core_area.xMin();
    region.y_min = core_area.yMin();
    region.x_max = core_area.xMax();
    region.y_max = core_area.yMax();
    regions__ = {region};
  }

  // TODO: remove this interface
  auto parameters() const -> const MovableSystem & { return parameters__; }
  // Read-only: final positions after run(), e.g. for writeback.
  auto registry() const -> const ModuleRegistry & { return module_registry__; }

  void run() {
    bool is_vertical = true;
    size_t iteration = 0;
    while (true) {
      solve_partition__();
      if (termination_condition__()) {
        std::cout << "Termination due to region size condition" << std::endl;
        break;
      } else if (iteration >= max_iterations__) {
        std::cout << "Termination due to iteration limit" << std::endl;
        break;
      }
      region_partition_advance__(is_vertical);
      is_vertical = !is_vertical;
      iteration++;
    }
  }

  auto termination_condition__() -> bool {
    for (const auto &region : regions__) {
      if (region.movable_modules.size() > max_modules_per_region__) {
        return false;
      }
    }
    return true;
  }

  void region_partition_advance__(const bool is_vertical) {
    Vector<Region> new_regions;
    new_regions.reserve(regions__.size() * 2);
    for (const auto &region : regions__) {
      RegionPartitioner<RegionPartitionPolicy::Naive> partitioner(region,
                                                                  is_vertical);
      partitioner.skip_when_n_modules_below(max_modules_per_region__);
      const auto partition = partitioner.partition();
      if (partition) {
        auto [left, right] = *partition;
        tracer__.record_split(region, left, right, is_vertical); // debug only
        new_regions.push_back(left);
        new_regions.push_back(right);
      } else {
        tracer__.record_unsplit(region, is_vertical); // debug only
        new_regions.push_back(region);
      }
    }
    regions__ = std::move(new_regions);
    tracer__.sample_partition(block__, module_registry__,
                              regions__); // debug only
  }

  void solve_partition__() {
    const auto constraint_system =
        ConstraintSystemBuilder(module_registry__, regions__, parameters__)
            .build();
    // Warm start from the current positions (the previous solve, or the
    // input placement before the first solve).
    const auto n =
        static_cast<Eigen::Index>(module_registry__.n_movable_modules());
    Eigen::VectorXd x_guess(n), y_guess(n);
    for (const auto &module : module_registry__.get_movable_modules()) {
      const auto [mx, my] = module.get_xy();
      x_guess(static_cast<Eigen::Index>(module.get_id())) = mx;
      y_guess(static_cast<Eigen::Index>(module.get_id())) = my;
    }
    const auto [x, y] =
        ConstraintSystemSolver::solve(constraint_system, x_guess, y_guess);
    tracer__.sample(block__, module_registry__, regions__, x, y);
    for (size_t i = 0; i < module_registry__.n_movable_modules(); ++i) {
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

  tracer::Tracer tracer__; // debug only; inactive unless GP_TRACE_DIR is set
  // -- Solver Parameters --
  size_t max_iterations__ = 20;
  size_t max_modules_per_region__ = 5;
};
} /* namespace gordian */
