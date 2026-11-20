#pragma once

#include "database/io_mode.hpp"
#include "database/module_ref.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace gordian {

// Owns registered wrapper values, not OpenDB objects. Recollecting invalidates
// pointers/references into this registry; rebuild regions and extracted
// pins/nets afterward. Lookup pointers are non-owning and non-null on success;
// invalid lookups throw rather than returning nullptr.
class ModuleRegistry {
public:
  void collect(odb::dbBlock *block, IoMode io_mode) {
    if (block == nullptr) {
      throw std::invalid_argument("ModuleRegistry::collect: null block");
    }

    // Build separately so a failed collection preserves the previous registry.
    ModuleRegistry collected;
    std::vector<odb::dbInst *> fixed_instances;
    for (auto *inst : block->getInsts()) {
      switch (inst->getPlacementStatus()) {
      case odb::dbPlacementStatus::FIRM:
      case odb::dbPlacementStatus::COVER:
      case odb::dbPlacementStatus::LOCKED:
        fixed_instances.push_back(inst);
        break;
      case odb::dbPlacementStatus::SUGGESTED:
      case odb::dbPlacementStatus::PLACED:
      case odb::dbPlacementStatus::UNPLACED:
      case odb::dbPlacementStatus::NONE:
        collected.register_inst__(inst);
        break;
      default:
        throw std::runtime_error("Unsupported instance placement status");
      }
    }

    collected.fixed_module_id_start__ = collected.modules__.size();
    for (auto *inst : fixed_instances) {
      collected.register_inst__(inst);
    }
    if (io_mode == IoMode::FIXED) {
      for (auto *bterm : block->getBTerms()) {
        auto *net = bterm->getNet();
        if (net == nullptr || net->getSigType().isSupply()) {
          continue;
        }
        // Included IOs must have valid placed geometry; never invent an anchor.
        collected.register_bterm__(bterm);
      }
    }
    collected.collected__ = true;
    modules__.swap(collected.modules__);
    inst_to_id__.swap(collected.inst_to_id__);
    bterm_to_id__.swap(collected.bterm_to_id__);
    fixed_module_id_start__ = collected.fixed_module_id_start__;
    collected__ = true;
  }

  auto from_inst(odb::dbInst *inst) const -> const ModuleRef * {
    require_collected__();
    if (inst == nullptr) {
      throw std::invalid_argument("ModuleRegistry::from_inst: null instance");
    }
    return &modules__.at(inst_to_id__.at(inst));
  }

  auto from_bterm(odb::dbBTerm *bterm) const -> const ModuleRef * {
    require_collected__();
    if (bterm == nullptr) {
      throw std::invalid_argument("ModuleRegistry::from_bterm: null BTerm");
    }
    return &modules__.at(bterm_to_id__.at(bterm));
  }

  auto at(std::size_t id) const -> const ModuleRef * {
    require_collected__();
    return &modules__.at(id);
  }

  auto size() const -> std::size_t { return modules__.size(); }

  auto n_movable_modules() const -> std::size_t {
    return fixed_module_id_start__;
  }

  auto n_fixed_modules() const -> std::size_t {
    return modules__.size() - fixed_module_id_start__;
  }

  auto movable_count() const -> std::size_t { return fixed_module_id_start__; }

  // Read-only, non-owning views into the contiguous ID partitions. Successful
  // recollection, assignment, or destruction invalidates outstanding views.
  // Module IDs remain global registry IDs, not indices within these spans.
  auto movable_modules() const -> std::span<const ModuleRef> {
    require_collected__();
    return std::span<const ModuleRef>(modules__).first(fixed_module_id_start__);
  }

  auto fixed_modules() const -> std::span<const ModuleRef> {
    require_collected__();
    return std::span<const ModuleRef>(modules__).subspan(
        fixed_module_id_start__);
  }

  // Solver mobility is fixed at collection time, not read live from OpenDB.
  // IDs [0, movable_count()) are movable; all remaining registered IDs are
  // fixed.
  auto is_movable(std::size_t id) const -> bool {
    // Validate the ID rather than treating arbitrary IDs as fixed.
    (void)at(id);
    return id < fixed_module_id_start__;
  }

  auto is_fixed(std::size_t id) const -> bool { return !is_movable(id); }

  auto get_modules() const -> std::span<const ModuleRef> {
    require_collected__();
    return std::span(modules__);
  }

  auto get_movable_modules() const -> std::span<const ModuleRef> {
    require_collected__();
    return std::span(modules__).first(fixed_module_id_start__);
  }

  auto get_fixed_modules() const -> std::span<const ModuleRef> {
    require_collected__();
    return std::span(modules__).subspan(fixed_module_id_start__);
  }

  void place_module(const std::size_t id, const double x, const double y) {
    auto &module = modules__.at(id);
    module.set_xy__(x, y);
  }

  auto get_module(const size_t &id) const -> const ModuleRef * {
    return at(id);
  }

private:
  void require_collected__() const {
    if (!collected__) {
      throw std::logic_error("ModuleRegistry: collect must be called first");
    }
  }

  void register_inst__(odb::dbInst *inst) {
    auto module = ModuleRef::from_inst__(inst);
    const auto id = modules__.size();
    module.register_id__(id);
    inst_to_id__.emplace(inst, id);
    modules__.push_back(module);
  }

  void register_bterm__(odb::dbBTerm *bterm) {
    if (bterm->getBPins().size() != 1) {
      // std::cout << "ModuleRegistry::register_bterm__: skipped "
      //           << bterm->getName()
      //           << " with bp size =" << bterm->getBPins().size() <<
      //           std::endl;
      return;
    }
    auto module = ModuleRef::from_bterm__(bterm);
    const auto id = modules__.size();
    module.register_id__(id);
    bterm_to_id__.emplace(bterm, id);
    modules__.push_back(module);
  }

  std::vector<ModuleRef> modules__;
  std::unordered_map<odb::dbInst *, std::size_t> inst_to_id__;
  std::unordered_map<odb::dbBTerm *, std::size_t> bterm_to_id__;
  std::size_t fixed_module_id_start__ = 0;
  bool collected__ = false;
};

} // namespace gordian
