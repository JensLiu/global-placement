#include <cmath>
#include <iostream>
#include <vector>

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

static_assert(EIGEN_VERSION_AT_LEAST(3, 4, 0),
              "Eigen 3.4 or newer is required");

int main() {
  // Positive-definite toy system, not a placement algorithm.
  // A * [1, 2, 3]^T = [2, 4, 7]^T.
  Eigen::SparseMatrix<double> matrix(3, 3);
  const std::vector<Eigen::Triplet<double>> entries{
      {0, 0, 4.0},  {0, 1, -1.0}, {1, 0, -1.0}, {1, 1, 4.0},
      {1, 2, -1.0}, {2, 1, -1.0}, {2, 2, 3.0}};
  matrix.setFromTriplets(entries.begin(), entries.end());
  matrix.makeCompressed();
  const Eigen::Vector3d rhs(2.0, 4.0, 7.0);
  const Eigen::Vector3d expected(1.0, 2.0, 3.0);

  const auto valid = [&](const Eigen::VectorXd &solution) {
    return solution.allFinite() && (solution - expected).norm() < 1e-10 &&
           (matrix * solution - rhs).norm() / rhs.norm() < 1e-10;
  };

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> direct;
  direct.compute(matrix);
  if (direct.info() != Eigen::Success) {
    std::cerr << "Sparse LDLT factorization failed\n";
    return 1;
  }
  const Eigen::VectorXd direct_solution = direct.solve(rhs);
  if (direct.info() != Eigen::Success || !valid(direct_solution)) {
    std::cerr << "Sparse LDLT solution/residual check failed\n";
    return 1;
  }

  Eigen::ConjugateGradient<Eigen::SparseMatrix<double>,
                           Eigen::Lower | Eigen::Upper>
      iterative;
  iterative.setTolerance(1e-12);
  iterative.setMaxIterations(20);
  iterative.compute(matrix);
  if (iterative.info() != Eigen::Success) {
    std::cerr << "Conjugate-gradient setup failed\n";
    return 1;
  }
  const Eigen::VectorXd iterative_solution = iterative.solve(rhs);
  if (iterative.info() != Eigen::Success || !valid(iterative_solution)) {
    std::cerr << "Conjugate-gradient solution/residual check failed\n";
    return 1;
  }

  std::cout << "Eigen " << EIGEN_WORLD_VERSION << '.' << EIGEN_MAJOR_VERSION
            << '.' << EIGEN_MINOR_VERSION << ": sparse solver checks passed\n";
  return 0;
}
