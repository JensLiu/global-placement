#pragma once

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>

#include <stdexcept>

namespace gordian {
class ReducedHessian;
}

namespace Eigen::internal {
/* EigenBase<ReducedHessian> will lookup this trait for metadata:
 * same tarit as SparseMatrix<double> */
template <>
struct traits<gordian::ReducedHessian> : traits<Eigen::SparseMatrix<double>> {};
} // namespace Eigen::internal

namespace gordian {

// Non-owning matrix replacement: H*v = Z^T * (C * (Z*v)).
// C and Z must outlive the operator and remain unchanged during a solve.
class ReducedHessian : public Eigen::EigenBase<ReducedHessian> {
public:
  using Scalar = double;
  using RealScalar = double;
  using StorageIndex = int;
  enum {
    ColsAtCompileTime = Eigen::Dynamic,
    MaxColsAtCompileTime = Eigen::Dynamic,
    IsRowMajor = false
  };

  ReducedHessian(const Eigen::SparseMatrix<double> &C,
                 const Eigen::SparseMatrix<double> &Z)
      : C__(C), Z__(Z) {
    if (C.rows() != C.cols() || Z.rows() != C.rows()) {
      throw std::invalid_argument(
          "ReducedHessian: incompatible C/Z dimensions");
    }
  }

  auto rows() const -> Eigen::Index { return Z__.cols(); }
  auto cols() const -> Eigen::Index { return Z__.cols(); }

  template <typename Rhs>
  auto operator*(const Eigen::MatrixBase<Rhs> &rhs) const
      -> Eigen::Product<ReducedHessian, Rhs, Eigen::AliasFreeProduct> {
    // this returns an object describing the product
    return {*this, rhs.derived()};
  }

  // Public product hook used by Eigen's generic_product_impl.
  template <typename Dest, typename Rhs>
  void scale_add_to(Dest &dst, const Rhs &rhs, Scalar alpha) const {
    Eigen::VectorXd t = Z__ * rhs;
    Eigen::VectorXd u = C__ * t;
    dst.noalias() += alpha * (Z__.transpose() * u);
  }

private:
  const Eigen::SparseMatrix<double> &C__;
  const Eigen::SparseMatrix<double> &Z__;
};
} // namespace gordian

namespace Eigen::internal {
/* partial specialisation */
template <typename Rhs>
struct generic_product_impl<gordian::ReducedHessian, Rhs, SparseShape,
                            DenseShape, GemvProduct>
    : generic_product_impl_base<
          gordian::ReducedHessian, Rhs,
          generic_product_impl<gordian::ReducedHessian, Rhs>> {

  using Scalar = typename Eigen::Product<gordian::ReducedHessian, Rhs>::Scalar;

  template <typename Dest>
  static void scaleAndAddTo(Dest &dst, const gordian::ReducedHessian &lhs,
                            const Rhs &rhs, const Scalar &alpha) {
    lhs.scale_add_to(dst, rhs, alpha);
  }
};
} // namespace Eigen::internal
