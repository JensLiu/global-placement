#pragma once

#include "commmon.h"
#include "database/module_ref.hpp"
#include <odb/db.h>

namespace gordian {

struct Region {
  // Non-owning, non-null pointers into the registry; rebuild after
  // recollection.
  Vector<const ModuleRef *> movable_modules;
  Vector<const ModuleRef *> fixed_modules;
  double x_min, y_min, x_max, y_max;
  auto representative() const -> const ModuleRef * {
    return movable_modules.at(0);
  }
  auto get_centre_xy() const -> Pair<double> {
    return {0.5 * x_min + 0.5 * x_max, 0.5 * y_min + 0.5 * y_max};
  }
};

} // namespace gordian
