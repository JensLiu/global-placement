#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "gordian/solver.hpp"
#include "odb/db.h"
#include "utl/Logger.h"

int main(int argc, char *argv[]) {
  const auto usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " <design.odb> [--io-mode fixed|skip]\n"
        << "  fixed (default): use positioned IOs as fixed anchors\n"
        << "  skip: omit IO endpoints for preliminary cell placement\n";
  };
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc != 2 && argc != 4) {
    usage(std::cerr);
    return 1;
  }
  auto io_mode = gordian::IoMode::FIXED;
  if (argc == 4) {
    const std::string_view flag(argv[2]);
    const std::string_view value(argv[3]);
    if (flag != "--io-mode" || (value != "fixed" && value != "skip")) {
      usage(std::cerr);
      return 1;
    }
    io_mode = value == "fixed" ? gordian::IoMode::FIXED : gordian::IoMode::SKIP;
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
    // auto *database = odb::dbDatabase::create();
    database->setLogger(&logger);
    database->read(input);

    auto *chip = database->getChip();
    auto *block = chip ? chip->getBlock() : nullptr;
    if (!block) {
      throw std::runtime_error("ODB has no top-level design block");
    }

    gordian::Solver gordian(block, io_mode);
    gordian.run();

  } catch (const std::exception &error) {
    std::cerr << "Error reading " << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
