#pragma once

#include "gordian/constraint_system.hpp"
#include "gordian/reduced_hessian.hpp"

#include <iostream>

namespace gordian {

class ConstraintSystemSolver {
public:
  static auto solve(const ConstraintSystem &sys) -> Pair<Eigen::VectorXd> {

    // std::cout << "Solving constraint system with" << std::endl;
    // std::cout << "\tZ=" << sys.Z << std::endl;
    // std::cout << "\tdx=" << sys.dx.transpose() << std::endl;
    // std::cout << "\tdy=" << sys.dy.transpose() << std::endl;

    if (sys.n_free_vars() == 0) {
      return {sys.x0, sys.y0};
    }

    ReducedHessian H(sys.parameters.C, sys.Z);
    Eigen::ConjugateGradient<
        ReducedHessian,
        Eigen::Lower | Eigen::Upper /* full operator multiplication */,
        Eigen::IdentityPreconditioner>
        solver;

    solver.setTolerance(1e-8);
    solver.setMaxIterations(1000);
    solver.compute(H); // attach the operator, no computation performed

    if (solver.info() != Eigen::Success) {
      throw std::runtime_error("Failed to initialise the reduced Hessian");
    }

    const Eigen::VectorXd initial_guess =
        Eigen::VectorXd::Zero(sys.n_free_vars());
    // Materialize each lazy solve before inspecting solver.info() or using it
    // in a sparse product (SolveWithGuess has no coefficient-wise access).
    const Eigen::VectorXd x_free = solver.solveWithGuess(sys.dx, initial_guess);
    if (solver.info() != Eigen::Success) {
      throw std::runtime_error("Failed to solve x");
    }
    const Eigen::VectorXd y_free = solver.solveWithGuess(sys.dy, initial_guess);
    if (solver.info() != Eigen::Success) {
      throw std::runtime_error("Failed to solve y");
    }

    const auto x = sys.Z * x_free + sys.x0;
    const auto y = sys.Z * y_free + sys.y0;

    return {std::move(x), std::move(y)};
  }
};

} // namespace gordian
