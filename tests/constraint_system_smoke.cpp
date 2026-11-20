#include "gordian/constraint_system_builder.hpp"
#include "gordian/constraint_system_solver.hpp"
#include "utl/Logger.h"

#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void near(double value, double expected, const char* message) {
  require(std::abs(value - expected) < 1e-7 * (1 + std::abs(expected)), message);
}
template <typename F>
void rejects(F&& f) {
  bool rejected = false;
  try { f(); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid input accepted");
}

void verify(const gordian::ModuleRegistry& registry,
            const std::vector<gordian::Region>& regions,
            const gordian::MovableSystem& parameters) {
  const auto m = static_cast<Eigen::Index>(registry.movable_count());
  const auto q = static_cast<Eigen::Index>(regions.size());
  const auto system = gordian::ConstraintSystemBuilder{}.build(registry, regions, parameters);
  require(system.transformation().rows() == m && system.n_free_vars() == m - q,
          "wrong transformation dimensions");
  const auto solution = system.solve(1e-11, 100);
  Eigen::MatrixXd A = Eigen::MatrixXd::Zero(q, m);
  Eigen::VectorXd ux(q), uy(q);
  for (Eigen::Index r = 0; r < q; ++r) {
    double total = 0;
    for (const auto& module : regions[r].modules) total += module.get_area();
    for (const auto& module : regions[r].modules)
      A(r, module.get_id()) = module.get_area() / total;
    const auto [x,y] = regions[r].get_centre_xy();
    ux(r) = x; uy(r) = y;
  }
  near((A * system.transformation()).norm(), 0, "A*Z != 0");
  near((A * system.x_origin() - ux).norm(), 0, "bad x particular solution");
  near((A * system.y_origin() - uy).norm(), 0, "bad y particular solution");
  near((A * solution.x - ux).norm(), 0, "x COG violated");
  near((A * solution.y - uy).norm(), 0, "y COG violated");

  // Independent dense KKT reference, only for these tiny test cases.
  Eigen::MatrixXd K = Eigen::MatrixXd::Zero(m + q, m + q);
  K.topLeftCorner(m,m) = Eigen::MatrixXd(parameters.C);
  K.topRightCorner(m,q) = A.transpose();
  K.bottomLeftCorner(q,m) = A;
  if (m + q > 0) {
    Eigen::VectorXd rhs(m+q);
    rhs.head(m) = -parameters.bx; rhs.tail(q) = ux;
    const Eigen::VectorXd expected_x = K.fullPivLu().solve(rhs);
    near((solution.x - expected_x.head(m)).norm(), 0, "x differs from KKT");
    rhs.head(m) = -parameters.by; rhs.tail(q) = uy;
    const Eigen::VectorXd expected_y = K.fullPivLu().solve(rhs);
    near((solution.y - expected_y.head(m)).norm(), 0, "y differs from KKT");
  }

  if (system.n_free_vars() > 0) {
    const auto& Z = system.transformation();
    gordian::ReducedHessian H(parameters.C, Z);
    Eigen::VectorXd v = Eigen::VectorXd::LinSpaced(Z.cols(), 1, 2);
    const Eigen::MatrixXd dense_h = Z.transpose() * parameters.C * Z;
    Eigen::VectorXd actual = H * v;
    near((actual - dense_h * v).norm(), 0, "matrix-free product wrong");
    actual.setOnes();
    H.scale_add_to(actual, v, -2.5);
    near((actual - (Eigen::VectorXd::Ones(v.size()) - 2.5 * dense_h * v)).norm(),
         0, "scaled product wrong");
  }
}
} // namespace

int main() {
  utl::Logger logger;
  std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
      odb::dbDatabase::create(), &odb::dbDatabase::destroy);
  db->setLogger(&logger);
  auto* tech = odb::dbTech::create(db.get(), "tech");
  auto* lib = odb::dbLib::create(db.get(), "lib", tech);
  auto* chip = odb::dbChip::create(db.get(), tech);
  auto* block = odb::dbBlock::create(chip, "test");
  gordian::ModuleRegistry registry;
  registry.collect(block, gordian::IoMode::SKIP);
  gordian::MovableSystem empty;
  empty.C.resize(0,0); empty.bx.resize(0); empty.by.resize(0);
  verify(registry, {}, empty);

  std::vector<odb::dbInst*> instances;
  for (int i=0; i<5; ++i) {
    auto* master = odb::dbMaster::create(lib, ("cell" + std::to_string(i)).c_str());
    master->setWidth(2 + i); master->setHeight(3);
    master->setType(odb::dbMasterType::CORE); master->setFrozen();
    auto* inst = odb::dbInst::create(block, master, ("u" + std::to_string(i)).c_str());
    inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    instances.push_back(inst);
  }
  registry.collect(block, gordian::IoMode::SKIP);
  auto region = [&](std::initializer_list<int> ids, double x, double y) {
    gordian::Region r;
    for (int id : ids) r.modules.push_back(registry.from_inst(instances[id]));
    r.x_min=x-1; r.x_max=x+1; r.y_min=y-2; r.y_max=y+2;
    return r;
  };
  std::mt19937 rng(20261116);
  std::normal_distribution<double> random;
  gordian::MovableSystem parameters;
  for (int trial=0; trial<12; ++trial) {
    Eigen::MatrixXd R(5,5);
    for (Eigen::Index i=0; i<R.size(); ++i) R.data()[i] = random(rng);
    const Eigen::MatrixXd C = R.transpose()*R + Eigen::MatrixXd::Identity(5,5);
    parameters.C = C.sparseView();
    parameters.bx.resize(5); parameters.by.resize(5);
    for (int i=0; i<5; ++i) { parameters.bx(i)=random(rng); parameters.by(i)=random(rng); }
    verify(registry, {}, parameters);
    verify(registry, {region({3,0,2},10,20), region({4,1},30,40)}, parameters);
    verify(registry, {region({2,4},10,20)}, parameters); // Uncovered free modules.
    verify(registry, {region({0},1,2),region({1},3,4),region({2},5,6),
                      region({3},7,8),region({4},9,10)}, parameters);
  }
  // PSD graph Laplacian becomes SPD on the COG-constrained free space.
  Eigen::MatrixXd lap = 5*Eigen::MatrixXd::Identity(5,5) - Eigen::MatrixXd::Ones(5,5);
  parameters.C = lap.sparseView();
  parameters.bx << 1,-1,2,-2,0;
  parameters.by << -1,1,3,-3,0;
  verify(registry, {region({3,0,1,2,4},10,20)}, parameters);

  parameters.C.setIdentity(); parameters.bx.setZero(); parameters.by.setZero();
  verify(registry, {}, parameters); // Zero RHS bypasses iterations.
  rejects([&]{ (void)gordian::ConstraintSystemBuilder{}.build(registry, {region({},0,0)}, parameters); });
  const auto system = gordian::ConstraintSystemBuilder{}.build(registry, {}, parameters);
  rejects([&]{ (void)system.solve(0,100); });
  rejects([&]{ (void)system.solve(1e-8,0); });

  // Force insufficient iterations on a nontrivial SPD operator.
  for(int i=0;i<5;++i) parameters.C.coeffRef(i,i)=i+1;
  parameters.bx.setOnes();
  const auto hard = gordian::ConstraintSystemBuilder{}.build(registry, {}, parameters);
  bool failed = false;
  try { (void)hard.solve(1e-14,1); } catch(const std::runtime_error&) { failed=true; }
  require(failed, "nonconvergence was not reported");
}
