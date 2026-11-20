#pragma once

#include "gordian/constraint_system.hpp"
#include "gordian/reduced_hessian.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace gordian {

class ConstraintSystemSolver {
public:
  // Target relative residual, measured against the reduced gradient at the
  // region-centre feasible point (ConstraintSystem::d*_reference_norm), not
  // against ||d||, which the representative's large x0 entry inflates by up
  // to ~1e5 at the root.
  static constexpr double relative_tolerance = 1e-8;

  // x_guess/y_guess: current movable positions, used only as a warm start.
  static auto solve(const ConstraintSystem &sys,
                    const Eigen::VectorXd &x_guess,
                    const Eigen::VectorXd &y_guess) -> Pair<Eigen::VectorXd> {
    if (sys.n_free_vars() == 0) {
      return {sys.x0, sys.y0};
    }

    ReducedHessian H(sys.parameters.C, sys.Z);
    Eigen::ConjugateGradient<ReducedHessian,
                             Eigen::Lower | Eigen::Upper /* full operator */,
                             Eigen::IdentityPreconditioner>
        solver;
    // Exact-arithmetic CG needs at most n iterations; allow head-room for
    // rounding on ill-conditioned levels.
    const auto n = static_cast<Eigen::Index>(sys.n_free_vars());
    solver.setMaxIterations(std::max<Eigen::Index>(1000, 2 * n));
    solver.compute(H); // attach the operator, no computation performed
    if (solver.info() != Eigen::Success) {
      throw std::runtime_error("Failed to initialise the reduced Hessian");
    }

    const auto solve_axis = [&](const Eigen::VectorXd &d, double reference,
                                const Eigen::VectorXd &guess,
                                const Eigen::VectorXd &x0, const char *axis) {
      const double d_norm = d.norm();
      // Eigen stops at ||r|| <= tol * ||rhs||; express the target absolute
      // residual relative_tolerance * reference in those terms.
      const double tolerance =
          d_norm > 0 ? std::max(relative_tolerance * reference / d_norm,
                                std::numeric_limits<double>::epsilon())
                     : relative_tolerance;
      solver.setTolerance(tolerance);
      // Materialise the lazy solve before reading solver.info().
      const Eigen::VectorXd u = solver.solveWithGuess(-d, sys.to_free(guess, x0));
      std::cout << "CG " << axis << ": n_free " << n << ", iterations "
                << solver.iterations() << ", residual " << solver.error() * d_norm
                << " (target " << tolerance * d_norm << ")" << std::endl;
      if (solver.info() != Eigen::Success) {
        throw std::runtime_error(std::string("Failed to solve ") + axis);
      }
      return u;
    };

    const Eigen::VectorXd x_free =
        solve_axis(sys.dx, sys.dx_reference_norm, x_guess, sys.x0, "x");
    const Eigen::VectorXd y_free =
        solve_axis(sys.dy, sys.dy_reference_norm, y_guess, sys.y0, "y");

    Eigen::VectorXd x = sys.Z * x_free + sys.x0;
    Eigen::VectorXd y = sys.Z * y_free + sys.y0;
    return {std::move(x), std::move(y)};
  }
};

} // namespace gordian
