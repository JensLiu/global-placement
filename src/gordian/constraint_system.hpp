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
  /* CG stopping reference: ||Z^T (C x_c + b)|| at the feasible point x_c that
   * puts every module at its region centre. ||dx|| itself is dominated by the
   * representative's large x0 entry ((D_r/S_k) * centre) and is not a usable
   * scale for a relative tolerance. */
  double dx_reference_norm = 0;
  double dy_reference_norm = 0;

  static constexpr size_t not_free = static_cast<size_t>(-1);

  ConstraintSystem(const MovableSystem &parameters) : parameters(parameters) {}
  auto n_free_vars() const -> size_t { return Z.cols(); }

  // Free coordinates u with Z u + x0 equal to x on every free row (the
  // representative rows are then implied by the constraints). Used as a warm
  // start; any u is a valid guess.
  auto to_free(const Eigen::VectorXd &x, const Eigen::VectorXd &offset) const
      -> Eigen::VectorXd {
    Eigen::VectorXd u(static_cast<Eigen::Index>(n_free_vars()));
    for (size_t i = 0; i < module_id_to_free_id__.size(); ++i) {
      const auto j = module_id_to_free_id__[i];
      if (j != not_free) {
        u(static_cast<Eigen::Index>(j)) = x(static_cast<Eigen::Index>(i)) -
                                          offset(static_cast<Eigen::Index>(i));
      }
    }
    return u;
  }

private:
  friend class ConstraintSystemBuilder;
  Vector<size_t> module_id_to_free_id__;
};

} // namespace gordian
