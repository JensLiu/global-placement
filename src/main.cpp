#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "odb/db.h"
#include "utl/Logger.h"

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: " << argv[0] << " <design.odb>\n";
    return 0;
  }
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <design.odb>\n";
    return 1;
  }

  try {
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
      throw std::runtime_error("Cannot open ODB file");
    }
    input.exceptions(std::ios::failbit | std::ios::badbit);

    utl::Logger logger;
    std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)>
        database(odb::dbDatabase::create(), &odb::dbDatabase::destroy);
    database->setLogger(&logger);
    database->read(input);

    auto *chip = database->getChip();
    auto *block = chip ? chip->getBlock() : nullptr;
    if (!block) {
      throw std::runtime_error("ODB has no top-level design block");
    }
    const int dbu_per_micron = block->getDbUnitsPerMicron();
    if (dbu_per_micron <= 0) {
      throw std::runtime_error("ODB has an invalid DBU-per-micron scale");
    }
    const double units = dbu_per_micron;
    const auto die = block->getDieArea();
    const auto core = block->getCoreArea();

    std::cout << "Hello from OpenDB!\n"
              << "Design: " << block->getName() << '\n'
              << "DBU per micron: " << dbu_per_micron << '\n'
              << std::fixed << std::setprecision(3) << "Die (um): ("
              << die.xMin() / units << ", " << die.yMin() / units << ") - ("
              << die.xMax() / units << ", " << die.yMax() / units << ")\n"
              << "Core (um): (" << core.xMin() / units << ", "
              << core.yMin() / units << ") - (" << core.xMax() / units << ", "
              << core.yMax() / units << ")\n"
              << "Instances: " << block->getInsts().size() << '\n'
              << "Nets: " << block->getNets().size() << '\n'
              << "IO terminals: " << block->getBTerms().size() << '\n'
              << "Rows: " << block->getRows().size() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "Error reading " << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
