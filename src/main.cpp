#include <cmath>
#include <exception>
#include <string>
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
    out << "Usage: " << argv[0]
        << " <design.odb> [--io-mode fixed|skip] [--write-db <out.odb>]\n"
        << "  fixed (default): use positioned IOs as fixed anchors\n"
        << "  skip: omit IO endpoints for preliminary cell placement\n"
        << "  --write-db: write the final global placement to a NEW ODB file\n"
        << "              (the input file is never modified)\n";
  };
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc < 2 || argc % 2 != 0) {
    usage(std::cerr);
    return 1;
  }
  auto io_mode = gordian::IoMode::FIXED;
  std::string output_db;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string_view flag(argv[i]);
    const std::string_view value(argv[i + 1]);
    if (flag == "--io-mode" && (value == "fixed" || value == "skip")) {
      io_mode = value == "fixed" ? gordian::IoMode::FIXED : gordian::IoMode::SKIP;
    } else if (flag == "--write-db" && !value.empty()) {
      output_db = value;
    } else {
      usage(std::cerr);
      return 1;
    }
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

    if (!output_db.empty()) {
      // Writeback: move each movable instance so its bbox centre lands on the
      // solved centre (orientation unchanged). Fixed instances and IOs stay.
      const auto &registry = gordian.registry();
      for (auto *inst : block->getInsts()) {
        const auto id = registry.from_inst(inst)->get_id();
        if (!registry.is_movable(id)) {
          continue;
        }
        const auto [x, y] = registry.at(id)->get_xy();
        const auto box = inst->getBBox()->getBox();
        const double cx = 0.5 * box.xMin() + 0.5 * box.xMax();
        const double cy = 0.5 * box.yMin() + 0.5 * box.yMax();
        const auto location = inst->getLocation();
        inst->setLocation(location.x() + static_cast<int>(std::lround(x - cx)),
                          location.y() + static_cast<int>(std::lround(y - cy)));
      }
      if (std::ifstream(output_db).good()) {
        throw std::runtime_error("refusing to overwrite existing " + output_db);
      }
      std::ofstream output(output_db, std::ios::binary);
      if (!output) {
        throw std::runtime_error("Cannot create " + output_db);
      }
      output.exceptions(std::ios::failbit | std::ios::badbit);
      database->write(output);
      std::cout << "Wrote global placement to " << output_db << std::endl;
    }

  } catch (const std::exception &error) {
    std::cerr << "Error reading " << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
