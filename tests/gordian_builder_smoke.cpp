#include "gordian/solver.hpp"
#include "utl/Logger.h"

#include <cmath>
#include <memory>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<gordian::MovableSystem>);
static_assert(!std::is_copy_assignable_v<gordian::MovableSystem>);
static_assert(std::is_nothrow_move_constructible_v<gordian::MovableSystem>);

#include "gordian_objective_check.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
void near(double actual, double expected) {
  require(std::abs(actual - expected) < 1e-10, "unexpected coefficient");
}
} // namespace

int main() {
  gordian_test::randomized_objective_checks();

  utl::Logger logger;
  std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
      odb::dbDatabase::create(), &odb::dbDatabase::destroy);
  db->setLogger(&logger);
  auto *tech = odb::dbTech::create(db.get(), "tech");
  auto *layer = odb::dbTechLayer::create(tech, "metal1", odb::dbTechLayerType::ROUTING);
  auto *lib = odb::dbLib::create(db.get(), "lib", tech);
  auto *site = odb::dbSite::create(lib, "site");
  site->setWidth(1);
  site->setHeight(9);
  auto *chip = odb::dbChip::create(db.get(), tech);
  auto *block = odb::dbBlock::create(chip, "test");
  odb::dbRow::create(block, "row", site, 0, 0, odb::dbOrientType::R0,
                     odb::dbRowDir::HORIZONTAL, 100, 1);
  auto *master = odb::dbMaster::create(lib, "cell");
  master->setWidth(11);
  master->setHeight(9);
  master->setType(odb::dbMasterType::CORE);
  for (const char *name : {"A", "B"}) {
    auto *term = odb::dbMTerm::create(master, name, odb::dbIoType::INPUT,
                                     odb::dbSigType::SIGNAL);
    auto *pin = odb::dbMPin::create(term);
    odb::dbBox::create(pin, layer, 1, 1, 3, 3);
  }
  master->setFrozen();
  auto *moving = odb::dbInst::create(block, master, "moving");
  auto *fixed = odb::dbInst::create(block, master, "fixed");
  moving->setLocation(10, 20);
  moving->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  fixed->setLocation(30, 40);
  fixed->setPlacementStatus(odb::dbPlacementStatus::COVER);
  auto *net = odb::dbNet::create(block, "signal");
  net->setWeight(1);
  moving->findITerm("A")->connect(net);
  moving->findITerm("B")->connect(net);
  fixed->findITerm("A")->connect(net);
  auto *io = odb::dbBTerm::create(net, "input");
  auto *bpin = odb::dbBPin::create(io);
  odb::dbBox::create(bpin, layer, 100, 200, 102, 202);
  bpin->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  auto *empty = odb::dbNet::create(block, "empty");
  empty->setWeight(1);

  using gordian::ModuleRef;
  using gordian::Solver;
  using gordian::GlobalParameterBuilder;
  gordian::ModuleRegistry registry;
  bool before_collect_rejected = false;
  try {
    registry.from_inst(moving);
  } catch (const std::logic_error &) {
    before_collect_rejected = true;
  }
  require(before_collect_rejected, "lookup before collect was accepted");
  registry.collect(block, gordian::IoMode::FIXED);
  const auto moving_ref = registry.from_inst(moving);
  const auto fixed_ref = registry.from_inst(fixed);
  const auto io_ref = registry.from_bterm(io);
  require(moving_ref == registry.from_inst(moving), "wrapper equality failed");
  require(moving_ref != fixed_ref && fixed_ref != io_ref, "identity collision");
  require(registry.size() == 3 && registry.movable_count() == 1,
          "incorrect registry size/range");
  require(moving_ref->get_id() == 0 && fixed_ref->get_id() == 1 && io_ref->get_id() == 2,
          "noncontiguous module IDs");
  require(registry.is_movable(0) && registry.is_fixed(1) && registry.is_fixed(2),
          "incorrect mobility range");
  // Registry mobility remains consistent with its ID partition until recollect.
  moving->setPlacementStatus(odb::dbPlacementStatus::LOCKED);
  require(registry.is_movable(moving_ref->get_id()), "mobility read live DB status");
  moving->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  bool invalid_id_rejected = false;
  try {
    registry.is_fixed(registry.size());
  } catch (const std::out_of_range &) {
    invalid_id_rejected = true;
  }
  require(invalid_id_rejected, "invalid ID was classified as fixed");
  for (std::size_t id = 0; id < registry.size(); ++id) {
    require(registry.at(id)->get_id() == id, "ID differs from registry index");
  }
  const auto pin = gordian::PinRef::from_iterm(moving->findITerm("A"), *moving_ref);
  require(pin.get_module().get_id() == moving_ref->get_id(), "pin lost owner ID");
  bool wrong_owner_rejected = false;
  try {
    gordian::PinRef::from_iterm(moving->findITerm("A"), *fixed_ref);
  } catch (const std::invalid_argument &) {
    wrong_owner_rejected = true;
  }
  require(wrong_owner_rejected, "mismatched pin owner was accepted");
  // Multiplicity and grouping are checked through the public build result below.
  const auto io_pin = gordian::PinRef::from_bterm(io, *io_ref);
  require(io_pin.get_offset() == std::pair<double, double>{0, 0},
          "IO offset is not zero");
  near(pin.get_offset().first, -3.5);
  near(pin.get_offset().second, -2.5);

  // Region membership borrows the canonical wrapper, not its coordinate cache.
  gordian::Region region;
  region.movable_modules.push_back(moving_ref);
  require(region.representative() == moving_ref, "region lost module identity");
  const auto original_xy = moving_ref->get_xy();
  registry.place_module(moving_ref->get_id(), 123.5, 456.5);
  require(region.representative()->get_xy() ==
              std::pair<double, double>{123.5, 456.5},
          "region did not observe registry placement");
  registry.place_module(moving_ref->get_id(), original_xy.first, original_xy.second);

  // The OpenMP builder must reproduce the serial system.
  const auto same_as_parallel = [&](const gordian::MovableSystem &serial,
                                    gordian::IoMode mode) {
    const auto parallel = GlobalParameterBuilder::build_parallel(block, registry, mode);
    near((Eigen::MatrixXd(serial.C) - Eigen::MatrixXd(parallel.C)).norm(), 0);
    near((serial.bx - parallel.bx).norm(), 0);
    near((serial.by - parallel.by).norm(), 0);
  };
  auto parameters = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::FIXED);
  require(parameters.C.rows() == 1 && parameters.C.cols() == 1,
          "returned system is not movable-only");
  near(parameters.C.coeff(0, 0), 1.0);
  near(parameters.bx(0), -70.0);
  near(parameters.by(0), -124.0);
  same_as_parallel(parameters, gordian::IoMode::FIXED);

  // Solver exposes the same owned reduced result, not full-system storage.
  Solver with_io(block);
  near(with_io.parameters().C.coeff(0, 0), 1.0);
  near(with_io.parameters().bx(0), -70.0);
  near(with_io.parameters().by(0), -124.0);

  // Results outlive temporary builders and do not borrow registry storage.
  registry.collect(block, gordian::IoMode::SKIP);
  near(parameters.bx(0), -70.0);
  auto skip = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::SKIP);
  require(skip.C.rows() == 1, "skip result is not movable-only");
  near(skip.C.coeff(0, 0), 2.0 / 3.0);
  // Reacquire after recollection: pointers into the old collection are invalid.
  near(skip.bx(0), -(2.0 / 3.0) * registry.from_inst(fixed)->get_xy().first);
  near(skip.by(0), -(2.0 / 3.0) * registry.from_inst(fixed)->get_xy().second);
  same_as_parallel(skip, gordian::IoMode::SKIP);
  const auto before = Eigen::MatrixXd(skip.C);
  skip = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::SKIP);
  near((Eigen::MatrixXd(skip.C) - before).norm(), 0);

  // Move ownership of Eigen buffers rather than copying their coefficients.
  const double* bx_storage = parameters.bx.data();
  const double* c_storage = parameters.C.valuePtr();
  auto transferred = std::move(parameters);
  require(transferred.bx.data() == bx_storage &&
              transferred.C.valuePtr() == c_storage,
          "moving result did not transfer storage");
  near(transferred.bx(0), -70.0);
  skip = std::move(transferred); // Also test assignment into existing storage.
  require(skip.bx.data() == bx_storage && skip.C.valuePtr() == c_storage,
          "move assignment did not transfer storage");
  near(skip.bx(0), -70.0);

  // No-fixed fast path.
  fixed->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  registry.collect(block, gordian::IoMode::SKIP);
  auto all_movable = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::SKIP);
  require(all_movable.C.rows() == 2, "all-movable dimensions");
  near(all_movable.C.coeff(0, 0), 2.0 / 3.0);
  near(all_movable.C.coeff(0, 1), -2.0 / 3.0);
  near(all_movable.bx.norm(), 0);
  same_as_parallel(all_movable, gordian::IoMode::SKIP);
  near(all_movable.by.norm(), 0);

  fixed->findITerm("A")->disconnect();
  auto same_module = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::SKIP);
  near(Eigen::MatrixXd(same_module.C).norm(), 0);
  near(same_module.bx.norm(), 0);

  // All fixed: no unknowns remain.
  moving->setPlacementStatus(odb::dbPlacementStatus::LOCKED);
  fixed->setPlacementStatus(odb::dbPlacementStatus::LOCKED);
  registry.collect(block, gordian::IoMode::FIXED);
  auto all_fixed = GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::FIXED);
  require(all_fixed.C.rows() == 0 && all_fixed.C.cols() == 0 &&
              all_fixed.bx.size() == 0 && all_fixed.by.size() == 0,
          "all-fixed result is not empty");

  // Empty but collected registry is also valid.
  auto* empty_block = odb::dbBlock::create(block, "empty_block");
  gordian::ModuleRegistry empty_registry;
  empty_registry.collect(empty_block, gordian::IoMode::SKIP);
  auto empty_system = GlobalParameterBuilder{}.build(
      empty_block, empty_registry, gordian::IoMode::SKIP);
  require(empty_system.C.rows() == 0 && empty_system.bx.size() == 0,
          "empty registry failed");

  bool null_rejected = false;
  try {
    (void)GlobalParameterBuilder{}.build(nullptr, registry, gordian::IoMode::FIXED);
  } catch (const std::invalid_argument&) {
    null_rejected = true;
  }
  require(null_rejected, "null block accepted");
  bool uncollected_rejected = false;
  try {
    gordian::ModuleRegistry uncollected;
    (void)GlobalParameterBuilder{}.build(block, uncollected, gordian::IoMode::SKIP);
  } catch (const std::logic_error&) {
    uncollected_rejected = true;
  }
  require(uncollected_rejected, "uncollected registry accepted");

  // A missing registration must not fabricate an IO anchor. The existing
  // registry skips zero-BPin IOs; the builder must reject their later lookup.
  odb::dbBTerm::create(net, "unplaced");
  registry.collect(block, gordian::IoMode::FIXED);
  bool missing_rejected = false;
  try {
    (void)GlobalParameterBuilder{}.build(block, registry, gordian::IoMode::FIXED);
  } catch (const std::out_of_range&) {
    missing_rejected = true;
  }
  require(missing_rejected, "missing IO registration accepted");
  return 0;
}
