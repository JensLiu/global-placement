#pragma once

#include "commmon.h"
#include "gordian/movable_system.hpp"

namespace gordian {

struct ConstraintSystem {
  const MovableSystem &parameters;
  /* system of equations */
  Eigen::SparseMatrix<double> Z;
  Eigen::VectorXd dx;
  Eigen::VectorXd dy;
  /* needed for reconstruction */
  // x_{mov} = Z x_{free} + x0, y_{mov} = Z y_{free} + y0
  Eigen::VectorXd x0;
  Eigen::VectorXd y0;

  ConstraintSystem(const MovableSystem &parameters) : parameters(parameters) {}
  auto n_free_vars() const -> size_t { return Z.cols(); }

private:
  friend class ConstraintSystemBuilder;
  Vector<size_t> module_id_to_free_id__;
};

} // namespace gordian
