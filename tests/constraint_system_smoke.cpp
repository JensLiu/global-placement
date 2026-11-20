#include "gordian/constraint_system_builder.hpp"
#include "gordian/constraint_system_solver.hpp"
#include "utl/Logger.h"

#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void near(double error, double scale, const char *message) {
  require(std::isfinite(error) && error < 1e-7 * (1 + scale), message);
}

void verify(const gordian::ModuleRegistry &registry,
            const std::vector<gordian::Region> &regions,
            const gordian::MovableSystem &parameters) {
  const auto m = static_cast<Eigen::Index>(registry.movable_count());
  const auto q = static_cast<Eigen::Index>(regions.size());
  const auto system = gordian::ConstraintSystemBuilder(registry, regions, parameters).build();
  require(system.Z.rows() == m && system.Z.cols() == m-q, "wrong null-space dimensions");
  const auto guess = Eigen::VectorXd::Zero(m).eval();
  const auto [x, y] = gordian::ConstraintSystemSolver::solve(system, guess, guess);
  Eigen::MatrixXd A = Eigen::MatrixXd::Zero(q, m);
  Eigen::VectorXd ux(q), uy(q);
  for (Eigen::Index r = 0; r < q; ++r) {
    double total = 0;
    for (const auto *module : regions[r].movable_modules) total += module->get_area();
    for (const auto *module : regions[r].movable_modules)
      A(r, module->get_id()) = module->get_area()/total;
    const auto centre = regions[r].get_centre_xy();
    ux(r) = centre.first; uy(r) = centre.second;
  }
  near((A*system.Z).norm(), 0, "A Z != 0");
  near((A*system.x0-ux).norm(), ux.norm(), "infeasible x0");
  near((A*system.y0-uy).norm(), uy.norm(), "infeasible y0");
  near((A*x-ux).norm(), ux.norm(), "infeasible x");
  near((A*y-uy).norm(), uy.norm(), "infeasible y");

  // Independent dense saddle-point solve: no null-space elimination or CG.
  Eigen::MatrixXd K = Eigen::MatrixXd::Zero(m+q, m+q);
  K.topLeftCorner(m,m) = Eigen::MatrixXd(parameters.C);
  K.topRightCorner(m,q) = A.transpose();
  K.bottomLeftCorner(q,m) = A;
  if (m+q > 0) {
    Eigen::FullPivLU<Eigen::MatrixXd> factor(K);
    require(factor.rank() == m+q, "singular reference KKT system");
    for (bool axis : {false, true}) {
      Eigen::VectorXd rhs(m+q);
      rhs.head(m) = -(axis ? parameters.by : parameters.bx);
      rhs.tail(q) = axis ? uy : ux;
      const Eigen::VectorXd expected = factor.solve(rhs);
      const auto &actual = axis ? y : x;
      near((actual-expected.head(m)).norm(), expected.head(m).norm(), "CG differs from dense KKT");
      const Eigen::VectorXd residual = system.Z.transpose() *
          (parameters.C*actual+(axis ? parameters.by : parameters.bx));
      near(residual.norm(), (axis ? system.dy : system.dx).norm(), "nonstationary solution");
    }
  }
  if (system.n_free_vars() > 0) {
    gordian::ReducedHessian H(parameters.C, system.Z);
    Eigen::VectorXd v = Eigen::VectorXd::LinSpaced(system.Z.cols(), 1, 2);
    const Eigen::MatrixXd dense_h = system.Z.transpose()*parameters.C*system.Z;
    const Eigen::VectorXd actual = H*v;
    near((actual-dense_h*v).norm(), actual.norm(), "matrix-free product wrong");
    Eigen::VectorXd scaled = Eigen::VectorXd::Ones(v.size());
    H.scale_add_to(scaled, v, -2.5);
    near((scaled-(Eigen::VectorXd::Ones(v.size())-2.5*dense_h*v)).norm(),
         scaled.norm(), "scaled matrix-free product wrong");
  }
}
} // namespace

int main() {
  utl::Logger logger;
  std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
      odb::dbDatabase::create(), &odb::dbDatabase::destroy);
  db->setLogger(&logger);
  auto *tech = odb::dbTech::create(db.get(), "tech");
  auto *lib = odb::dbLib::create(db.get(), "lib", tech);
  auto *chip = odb::dbChip::create(db.get(), tech);
  auto *block = odb::dbBlock::create(chip, "test");
  gordian::ModuleRegistry registry;
  registry.collect(block, gordian::IoMode::SKIP);
  gordian::MovableSystem empty;
  empty.C.resize(0,0); empty.bx.resize(0); empty.by.resize(0);
  verify(registry, {}, empty);
  std::vector<odb::dbInst *> instances;
  for (int i = 0; i < 5; ++i) {
    auto *master = odb::dbMaster::create(lib, ("cell"+std::to_string(i)).c_str());
    master->setWidth(2+i); master->setHeight(3);
    master->setType(odb::dbMasterType::CORE); master->setFrozen();
    auto *inst = odb::dbInst::create(block, master, ("u"+std::to_string(i)).c_str());
    inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    instances.push_back(inst);
  }
  registry.collect(block, gordian::IoMode::SKIP);
  auto region = [&](std::initializer_list<int> ids, double x, double y) {
    gordian::Region r;
    for (int id : ids) r.movable_modules.push_back(registry.from_inst(instances[id]));
    r.x_min=x-1; r.x_max=x+1; r.y_min=y-2; r.y_max=y+2;
    return r;
  };
  std::mt19937 rng(20261120);
  std::normal_distribution<double> random;
  gordian::MovableSystem parameters;
  for (int trial = 0; trial < 24; ++trial) {
    Eigen::MatrixXd R(5,5);
    for (Eigen::Index i = 0; i < R.size(); ++i) R.data()[i] = random(rng);
    parameters.C = (R.transpose()*R+Eigen::MatrixXd::Identity(5,5)).eval().sparseView();
    parameters.bx.resize(5); parameters.by.resize(5);
    for (int i = 0; i < 5; ++i) { parameters.bx(i)=random(rng); parameters.by(i)=random(rng); }
    verify(registry, {region({3,0,2},10,20), region({4,1},30,40)}, parameters);
    verify(registry, {region({0,1,2,3,4},10,20)}, parameters);
    verify(registry, {region({0},1,2),region({1},3,4),region({2},5,6),
                      region({3},7,8),region({4},9,10)}, parameters);
  }
  // A connected graph Laplacian is PSD; a single centroid removes its null mode.
  parameters.C = (5*Eigen::MatrixXd::Identity(5,5)-Eigen::MatrixXd::Ones(5,5)).eval().sparseView();
  parameters.bx << 1,-1,2,-2,0; parameters.by << -1,1,3,-3,0;
  verify(registry, {region({3,0,1,2,4},10,20)}, parameters);
}
