#pragma once

#include "commmon.h"

namespace gordian {

struct MovableSystem {
  Eigen::SparseMatrix<double> C;
  Eigen::VectorXd bx;
  Eigen::VectorXd by;

  MovableSystem() = default;
  MovableSystem(const MovableSystem &) = delete;
  auto operator=(const MovableSystem &) -> MovableSystem & = delete;

  MovableSystem(MovableSystem &&other) noexcept { swap__(other); }
  auto operator=(MovableSystem &&other) noexcept -> MovableSystem & {
    swap__(other);
    return *this;
  }

private:
  void swap__(MovableSystem &other) noexcept {
    C.swap(other.C);
    bx.swap(other.bx);
    by.swap(other.by);
  }
};

} // namespace gordian
