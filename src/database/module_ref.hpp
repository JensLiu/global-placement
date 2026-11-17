#pragma once

#include "odb/db.h"
#include "odb/dbTypes.h"

#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gordian {

class ModuleRef {
public:
  auto get_xy() const -> std::pair<double, double> { return xy__; }

  auto get_id() const -> std::size_t {
    if (!id__.has_value()) {
      throw std::logic_error("ModuleRef: ID has not been registered");
    }
    return *id__;
  }

  auto get_area() const -> double {
    if (is_io_virt__) {
      std::cout << "area for virtual module = 0" << std::endl;
      return 0;
    }
    const auto area = src__.inst__->getBBox()->getBox().area();
    return static_cast<double>(area);
  }

  auto get_name() const -> std::string {
    if (is_io_virt__) {
      return "*" + src__.bterm__->getName();
    }
    return src__.inst__->getName();
  }

  // Identity ignores cached coordinates: wrappers of the same source are equal.
  auto operator==(const ModuleRef &other) const -> bool {
    if (is_io_virt__ != other.is_io_virt__) {
      return false;
    }
    return is_io_virt__ ? src__.bterm__ == other.src__.bterm__
                        : src__.inst__ == other.src__.inst__;
  }

  struct Hash {
    auto operator()(const ModuleRef &module) const -> std::size_t {
      const auto source_hash =
          module.is_io_virt__
              ? std::hash<odb::dbBTerm *>{}(module.src__.bterm__)
              : std::hash<odb::dbInst *>{}(module.src__.inst__);
      return source_hash ^ (module.is_io_virt__ ? std::size_t{0x9e3779b9} : 0);
    }
  };

private:
  friend class ModuleRegistry;
  friend class PinRef;

  // Only the registry creates and registers modules. Public callers look them
  // up.
  static auto from_inst__(odb::dbInst *inst) -> ModuleRef {
    if (inst == nullptr) {
      throw std::invalid_argument("ModuleRef::from_inst__: null instance");
    }
    return ModuleRef(inst);
  }

  static auto from_bterm__(odb::dbBTerm *bterm) -> ModuleRef {
    if (bterm == nullptr) {
      throw std::invalid_argument(
          "ModuleRef::from_bterm__: null boundary terminal");
    }
    return ModuleRef(bterm);
  }

  void register_id__(std::size_t id) {
    if (id__.has_value()) {
      throw std::logic_error("ModuleRef: ID already registered");
    }
    id__ = id;
  }

  void set_xy__(const double x, const double y) { xy__ = {x, y}; }

  ModuleRef() = delete;

  explicit ModuleRef(odb::dbInst *inst) : is_io_virt__(false), src__(inst) {
    compute_xy__();
  }

  explicit ModuleRef(odb::dbBTerm *bterm) : is_io_virt__(true), src__(bterm) {
    compute_xy__();
  }

  static void valid_virt_module_or_else_throw__(odb::dbBTerm *b) {
    const auto bpins = b->getBPins();
    if (bpins.size() != 1) {
      std::cout << "BTerm " << b->getName() << "=[";
      for (const auto bpin : bpins) {
        std::cout << bpin->getName() << ",\t";
      }
      std::cout << "], type=" << b->getSigType().getString() << std::endl;

      throw std::runtime_error("ModuleRef::Source__::constructor: expected "
                               "exactly one BPin per IO terminal");
    }
    const auto bpin = bpins.begin();
    if (!bpin->getPlacementStatus().isPlaced() || bpin->getBoxes().empty()) {
      throw std::runtime_error("ModuleRef::get_xy: IO pin is not positioned");
    }
    const auto &bbox = bpin->getBBox();
    if (bbox.isInverted()) {
      throw std::runtime_error(
          "ModuleRef::get_xy: IO pin has invalid geometry");
    }
  }

  void compute_xy__() {
    if (is_io_virt__) {
      const auto bpin = src__.bterm__->getBPins().begin();
      const auto bbox = bpin->getBBox();
      xy__ = {0.5 * bbox.xMin() + 0.5 * bbox.xMax(),
              0.5 * bbox.yMin() + 0.5 * bbox.yMax()};
      return;
    }

    const auto bbox = src__.inst__->getBBox()->getBox();
    xy__ = {0.5 * bbox.xMin() + 0.5 * bbox.xMax(),
            0.5 * bbox.yMin() + 0.5 * bbox.yMax()};
  }

  bool is_io_virt__;
  std::pair<double, double> xy__;
  std::optional<std::size_t> id__ = std::nullopt;
  union Source__ {
    explicit Source__(odb::dbInst *value) : inst__(value) {}
    explicit Source__(odb::dbBTerm *value) : bterm__(value) {
      valid_virt_module_or_else_throw__(value);
    }

    odb::dbInst *inst__;
    odb::dbBTerm *bterm__;
  } src__;
};

} // namespace gordian

namespace std {
template <> struct hash<gordian::ModuleRef> : gordian::ModuleRef::Hash {};
} // namespace std
