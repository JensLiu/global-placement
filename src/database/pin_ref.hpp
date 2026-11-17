#pragma once

#include "database/module_ref.hpp"

namespace gordian {

class PinRef {
public:
  static auto from_iterm(odb::dbITerm *iterm, const ModuleRef &module)
      -> PinRef {
    if (iterm == nullptr) {
      throw std::invalid_argument("PinRef::from_iterm: null ITerm");
    }
    if (module.is_io_virt__ || module.src__.inst__ != iterm->getInst()) {
      throw std::invalid_argument(
          "PinRef::from_iterm: module does not own ITerm");
    }
    (void)module.get_id();

    int pin_x = 0;
    int pin_y = 0;
    if (!iterm->getAvgXY(&pin_x, &pin_y)) {
      throw std::runtime_error("PinRef::from_iterm: missing pin geometry");
    }
    const auto [module_x, module_y] = module.get_xy();
    return PinRef(module, {static_cast<double>(pin_x) - module_x,
                           static_cast<double>(pin_y) - module_y});
  }

  static auto from_bterm(odb::dbBTerm *bterm, const ModuleRef &module)
      -> PinRef {
    if (bterm == nullptr || !module.is_io_virt__ ||
        module.src__.bterm__ != bterm) {
      throw std::invalid_argument(
          "PinRef::from_bterm: module does not own BTerm");
    }
    (void)module.get_id();
    return PinRef(module, {0.0, 0.0});
  }

  auto get_module() const -> const ModuleRef & { return module__; }

  auto get_offset() const -> std::pair<double, double> { return offset__; }

  auto get_xy() const -> std::pair<double, double> {
    const auto module_xy = module__.get_xy();
    return {module_xy.first + offset__.first, module_xy.second + offset__.second};
  }

private:
  PinRef() = delete;
  explicit PinRef(ModuleRef module, std::pair<double, double> offset)
      : module__(module), offset__(offset) {}

  ModuleRef module__;
  std::pair<double, double> offset__;
};

} // namespace gordian
