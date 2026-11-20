#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "database/module_registry.hpp"
#include "utl/Logger.h"

namespace {
void require(bool value) {
  if (!value) {
    throw std::runtime_error("registry view check failed");
  }
}
template <typename Exception, typename F>
void require_throws(F &&action) {
  bool rejected = false;
  try {
    action();
  } catch (const Exception &) {
    rejected = true;
  }
  require(rejected);
}
void check(const gordian::ModuleRegistry &registry, std::size_t movable,
           std::size_t fixed) {
  const auto m = registry.movable_modules();
  const auto f = registry.fixed_modules();
  static_assert(std::is_same_v<decltype(m)::element_type,
                               const gordian::ModuleRef>);
  require(m.size() == movable && f.size() == fixed);
  std::size_t id = 0;
  for (const auto &module : m) {
    require(module.get_id() == id && &module == registry.at(id));
    require(registry.get_module(id) == registry.at(id));
    require(registry.is_movable(id++));
  }
  for (const auto &module : f) {
    require(module.get_id() == id && &module == registry.at(id));
    require(registry.get_module(id) == registry.at(id));
    require(registry.is_fixed(id++));
  }
}
} // namespace

int main() {
  gordian::ModuleRegistry registry;
  static_assert(std::is_same_v<decltype(registry.at(0)),
                               const gordian::ModuleRef *>);
  static_assert(std::is_same_v<decltype(registry.get_module(0)),
                               const gordian::ModuleRef *>);
  static_assert(std::is_same_v<decltype(registry.from_inst(nullptr)),
                               const gordian::ModuleRef *>);
  static_assert(std::is_same_v<decltype(registry.from_bterm(nullptr)),
                               const gordian::ModuleRef *>);
  require_throws<std::logic_error>([&] { (void)registry.at(0); });
  require_throws<std::logic_error>([&] { (void)registry.get_module(0); });
  require_throws<std::logic_error>([&] { (void)registry.from_inst(nullptr); });
  require_throws<std::logic_error>([&] { (void)registry.from_bterm(nullptr); });
  for (bool fixed : {false, true}) {
    bool rejected = false;
    try {
      (void)(fixed ? registry.fixed_modules() : registry.movable_modules());
    } catch (const std::logic_error &) {
      rejected = true;
    }
    require(rejected);
  }

  utl::Logger logger;
  std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
      odb::dbDatabase::create(), &odb::dbDatabase::destroy);
  db->setLogger(&logger);
  auto *tech = odb::dbTech::create(db.get(), "tech");
  auto *layer = odb::dbTechLayer::create(tech, "metal1", odb::dbTechLayerType::ROUTING);
  auto *lib = odb::dbLib::create(db.get(), "lib", tech);
  auto *chip = odb::dbChip::create(db.get(), tech);
  auto *block = odb::dbBlock::create(chip, "test");
  registry.collect(block, gordian::IoMode::SKIP);
  check(registry, 0, 0);
  require_throws<std::out_of_range>([&] { (void)registry.at(0); });
  require_throws<std::out_of_range>([&] { (void)registry.get_module(0); });
  require_throws<std::invalid_argument>([&] { (void)registry.from_inst(nullptr); });
  require_throws<std::invalid_argument>([&] { (void)registry.from_bterm(nullptr); });

  auto *master = odb::dbMaster::create(lib, "cell");
  master->setWidth(10);
  master->setHeight(10);
  master->setType(odb::dbMasterType::CORE);
  master->setFrozen();
  auto *inst = odb::dbInst::create(block, master, "instance");
  inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  require_throws<std::out_of_range>([&] { (void)registry.from_inst(inst); });
  registry.collect(block, gordian::IoMode::SKIP);
  check(registry, 1, 0);
  const auto *module = registry.from_inst(inst);
  require(module == registry.at(0) && module == registry.get_module(0));
  registry.place_module(0, 12.5, 34.5);
  require(module->get_xy() == std::pair<double, double>{12.5, 34.5});

  auto *net = odb::dbNet::create(block, "signal");
  auto *term = odb::dbBTerm::create(net, "io");
  auto *pin = odb::dbBPin::create(term);
  odb::dbBox::create(pin, layer, 100, 100, 102, 102);
  pin->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  registry.collect(block, gordian::IoMode::FIXED);
  check(registry, 1, 1);
  require(&registry.fixed_modules().front() == registry.from_bterm(term));

  inst->setPlacementStatus(odb::dbPlacementStatus::LOCKED);
  check(registry, 1, 1); // Views follow collected mobility, not live DB status.
  registry.collect(block, gordian::IoMode::FIXED);
  check(registry, 0, 2);
  require(&registry.fixed_modules().front() == registry.from_inst(inst));
  registry.collect(block, gordian::IoMode::SKIP);
  check(registry, 0, 1);
}
