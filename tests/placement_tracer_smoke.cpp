#include "tracer/placement_tracer.hpp"
#include "utl/Logger.h"

#include <boost/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <array>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F action) {
  bool threw = false;
  try { action(); } catch (const std::exception &) { threw = true; }
  require(threw, "expected rejection");
}
} // namespace

int main(int argc, char **argv) {
  namespace trace = gordian::tracer;
  namespace json = boost::json;
  try {
    const auto directory = argc == 2 ? std::filesystem::path(argv[1]) :
        std::filesystem::temp_directory_path() / ("placement-trace-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    utl::Logger logger;
    std::unique_ptr<odb::dbDatabase, decltype(&odb::dbDatabase::destroy)> db(
        odb::dbDatabase::create(), &odb::dbDatabase::destroy);
    db->setLogger(&logger);
    db->setDbuPerMicron(2000);
    auto *tech = odb::dbTech::create(db.get(), "tech");
    auto *layer = odb::dbTechLayer::create(tech, "m1", odb::dbTechLayerType::ROUTING);
    auto *lib = odb::dbLib::create(db.get(), "lib", tech);
    auto *chip = odb::dbChip::create(db.get(), tech);
    auto *block = odb::dbBlock::create(chip, "trace-test");
    block->setDieArea(odb::Rect(0, 0, 1000, 1000));
    auto *master = odb::dbMaster::create(lib, "cell");
    master->setWidth(20);
    master->setHeight(10);
    master->setType(odb::dbMasterType::CORE);
    master->setFrozen();
    auto *a = odb::dbInst::create(block, master, "cell\"\\\nA");
    a->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    a->setOrient(odb::dbOrientType::R90);
    a->setLocation(100, 100);
    auto *b = odb::dbInst::create(block, master, "B");
    b->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    auto *fixed = odb::dbInst::create(block, master, "fixed");
    fixed->setLocation(500, 500);
    fixed->setPlacementStatus(odb::dbPlacementStatus::LOCKED);
    auto *net = odb::dbNet::create(block, "signal");
    auto *term = odb::dbBTerm::create(net, "IO");
    auto *pin = odb::dbBPin::create(term);
    odb::dbBox::create(pin, layer, 0, 300, 4, 304);
    pin->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    gordian::ModuleRegistry registry;
    registry.collect(block, gordian::IoMode::FIXED);
    require(registry.size() == 4 && registry.movable_count() == 2, "fixture counts");
    const auto original_centre = registry.at(0)->get_xy();
    const auto original_box = a->getBBox()->getBox();
    std::vector<gordian::Region> regions(2);
    regions[0].x_min = 0; regions[0].y_min = 0;
    regions[0].x_max = 500; regions[0].y_max = 1000;
    regions[1].x_min = 500; regions[1].y_min = 0;
    regions[1].x_max = 1000; regions[1].y_max = 1000;
    regions[0].movable_modules.push_back(registry.at(0));
    regions[1].movable_modules.push_back(registry.at(1));
    // Debug identities: the root (ID 0) is split vertically at x = 500.
    gordian::Region root{{registry.at(0), registry.at(1)}, 0, 0, 1000, 1000};
    trace::Tracer stamper; // GP_TRACE_DIR unset: stamps IDs, records nothing
    unsetenv("GP_TRACE_DIR");
    stamper.record_split(root, regions[0], regions[1], true);
    require(regions[0].debug.id == 1 && regions[1].debug.id == 2 &&
                regions[1].debug.parent_id == 0 && regions[1].debug.depth == 1,
            "region debug identities");
    const auto record = trace::SnapshotSampler::partition(root, true, &regions[0], &regions[1]);
    require(record.split && record.split->cut == 500 &&
                record.split->child_ids == std::array<std::size_t, 2>{1, 2} &&
                record.movable_count == 2 &&
                record.movable_area == record.split->movable_areas[0] +
                                           record.split->movable_areas[1],
            "partition record");
    require(!trace::SnapshotSampler::partition(root, false, nullptr, nullptr).split,
            "unsplit record");
    Eigen::VectorXd x(2), y(2);
    x << 200.25, 800.5;
    y << 400.5, 600.25;
    const auto design = trace::SnapshotSampler::geometry(block, registry);
    require(design.modules[0].bbox_offsets == trace::Bounds{-5, -10, 5, 10},
            "oriented bbox dimensions");
    require(design.modules[2].kind == "fixed" && design.modules[3].kind == "io",
            "fixed and IO geometry");
    auto first = trace::SnapshotSampler::iteration(0, registry, regions, x, y);
    require(first.modules[0].x == 200.25 && first.modules[1].region_id == 2 &&
                first.regions[1].parent_id == 0u,
            "solved coordinates and membership");
    require(registry.at(0)->get_xy() == original_centre &&
                a->getBBox()->getBox() == original_box, "sampling mutated placement");
    x(0) = 700.75; // Deliberately drift outside region 0 in second frame.
    require(first.modules[0].x == 200.25, "snapshot retained input references");
    {
      trace::JsonTraceWriter writer(directory, design);
      writer.append(first);
      writer.append(trace::SnapshotSampler::iteration(1, registry, regions, x, y));
      rejects([&] { writer.append(first); });
    }
    rejects([&] { trace::JsonTraceWriter duplicate(directory, design); });
    auto invalid = x;
    invalid(0) = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { trace::SnapshotSampler::iteration(2, registry, regions, invalid, y); });
    rejects([&] { trace::SnapshotSampler::iteration(2, registry, regions, Eigen::VectorXd(1), y); });
    regions[1].movable_modules.push_back(registry.at(0));
    rejects([&] { trace::SnapshotSampler::iteration(2, registry, regions, x, y); });
    regions[1].movable_modules.clear();
    rejects([&] { trace::SnapshotSampler::iteration(2, registry, regions, x, y); });
    regions[1].movable_modules.push_back(registry.at(1));
    std::ifstream input(directory / "iterations.jsonl");
    std::string line;
    for (int i = 0; i < 2; ++i) {
      require(bool(std::getline(input, line)), "missing frame");
      require(json::parse(line).at("iteration").as_int64() == i, "iteration JSON");
    }
    require(!std::getline(input, line), "unexpected frame");

    // Disabled collector must not even validate invalid sampling input.
    unsetenv("GP_TRACE_DIR");
    trace::Tracer disabled;
    disabled.sample(nullptr, registry, regions, invalid, y);
    // Enabled wrapper writes one static file and two completed frames.
    const auto collected = directory / "collector";
    setenv("GP_TRACE_DIR", collected.c_str(), 1);
    {
      trace::Tracer enabled;
      auto low = regions[0], high = regions[1];
      enabled.record_split(root, low, high, true);
      enabled.record_unsplit(root, false);
      enabled.sample(block, registry, regions, x, y);
      enabled.sample(block, registry, regions, x, y);
    }
    std::ifstream collected_frames(collected / "iterations.jsonl");
    require(bool(std::getline(collected_frames, line)), "collector missing first frame");
    {
      const auto frame = json::parse(line).as_object();
      const auto &parts = frame.at("partitions").as_array();
      require(parts.size() == 2 && parts[0].at("split").as_bool() &&
                  parts[0].at("cut").as_double() == 500 &&
                  parts[0].at("direction").as_string() == "vertical" &&
                  !parts[1].at("split").as_bool(),
              "partition JSON");
      require(frame.at("regions").as_array()[0].at("parent_id").as_int64() == 0,
              "region parent JSON");
    }
    require(bool(std::getline(collected_frames, line)), "collector missing second frame");
    require(json::parse(line).at("iteration").as_int64() == 1, "collector numbering");
    require(json::parse(line).at("partitions").as_array().empty(),
            "partitions belong only to the frame after their level");
    require(!std::getline(collected_frames, line), "collector extra frame");
    // Version 3: a partition frame repeats the latest solved positions with
    // the new regions and carries this level's records.
    const auto phased = directory / "phased";
    setenv("GP_TRACE_DIR", phased.c_str(), 1);
    {
      trace::Tracer enabled;
      enabled.sample(block, registry, {root}, x, y);
      auto low = regions[0], high = regions[1];
      enabled.record_split(root, low, high, true);
      enabled.sample_partition(block, registry, {low, high});
    }
    std::ifstream phased_frames(phased / "iterations.jsonl");
    require(bool(std::getline(phased_frames, line)), "phased solve frame");
    {
      const auto frame = json::parse(line).as_object();
      require(frame.at("phase").as_string() == "post_solve" &&
                  frame.at("frame").as_int64() == 0 &&
                  frame.at("iteration").as_int64() == 0 &&
                  frame.at("partitions").as_array().empty(),
              "post_solve frame JSON");
    }
    require(bool(std::getline(phased_frames, line)), "phased partition frame");
    {
      const auto frame = json::parse(line).as_object();
      require(frame.at("phase").as_string() == "post_partition" &&
                  frame.at("frame").as_int64() == 1 &&
                  frame.at("iteration").as_int64() == 0 &&
                  frame.at("partitions").as_array().size() == 1 &&
                  frame.at("regions").as_array().size() == 2,
              "post_partition frame JSON");
      // Registry positions (not the solve vector) are shown: nothing was placed.
      const auto [rx, ry] = registry.at(0)->get_xy();
      require(frame.at("modules").as_array()[0].at("x").as_double() == rx,
              "post_partition positions come from the registry");
    }
    require(!std::getline(phased_frames, line), "phased extra frame");
    // Benchmark metrics: input baseline header, then one line per frame.
    std::ifstream metrics(phased / "metrics.jsonl");
    require(bool(std::getline(metrics, line)), "metrics header");
    {
      const auto header = json::parse(line).as_object();
      require(header.at("schema").as_string() == "global-placement.gordian-metrics" &&
                  header.at("phase").as_string() == "input" &&
                  header.at("frame").is_null(),
              "metrics header JSON");
    }
    for (int frame = 0; frame < 2; ++frame) {
      require(bool(std::getline(metrics, line)), "metrics frame");
      const auto record = json::parse(line).as_object();
      require(record.at("frame").as_int64() == frame &&
                  record.at("phase").as_string() ==
                      (frame == 0 ? "post_solve" : "post_partition") &&
                  record.at("density").at("overflow").is_number() &&
                  record.at("cog_error_um").is_null() == (frame == 1),
              "metrics frame JSON");
    }
    require(!std::getline(metrics, line), "metrics extra line");
    // Existing-directory failure is observational: no throw, no overwrite.
    setenv("GP_TRACE_DIR", directory.c_str(), 1);
    trace::Tracer failed;
    failed.sample(block, registry, regions, x, y);
    failed.sample(block, registry, regions, x, y);
    unsetenv("GP_TRACE_DIR");
    require(registry.at(0)->get_xy() == original_centre, "collector mutated registry");
    if (argc != 2) std::filesystem::remove_all(directory);
    std::cout << "Placement trace checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
