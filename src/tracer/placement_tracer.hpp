#pragma once

#include "database/module_registry.hpp"
#include "gordian/region.hpp"

#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gordian::tracer {

class PlacementMetrics; // placement_metrics.hpp
class MetricsWriter;

using Bounds = std::array<double, 4>; // xmin, ymin, xmax, ymax, in DBU

struct ModuleGeometry {
  std::size_t id;
  std::string name;
  std::string kind; // movable, fixed, io
  Bounds bbox_offsets; // oriented footprint relative to its centre
  std::array<double, 2> initial_centre;
};

struct DesignSnapshot {
  std::string name;
  int dbu_per_micron;
  Bounds core_bounds;
  Bounds die_bounds;
  std::vector<ModuleGeometry> modules;
};

struct RegionSnapshot {
  std::size_t id; // persistent for the run (Region::debug.id)
  std::optional<std::size_t> parent_id;
  std::size_t depth;
  Bounds bounds;
};

// One parent region at one partition level. Built only from Region contents
// and module areas; it reads no solver internals.
struct PartitionRecord {
  std::size_t parent_id;
  Bounds parent_bounds;
  bool is_vertical;
  std::size_t movable_count;
  double movable_area;
  // Present only when the region was split; index 0 is the low (left/bottom) son.
  struct Split {
    double cut; // x for a vertical cut, y for a horizontal cut
    std::array<std::size_t, 2> child_ids;
    std::array<std::size_t, 2> movable_counts;
    std::array<double, 2> movable_areas;
  };
  std::optional<Split> split;
};

struct ModulePosition {
  std::size_t id;
  std::size_t region_id;
  double x;
  double y;
};

enum class FramePhase {
  PostSolve,     // positions from a solve, with the regions that solve used
  PostPartition, // the same positions, with the regions just partitioned
};

struct IterationSnapshot {
  // Index of the solve whose positions are shown; absent for a partition that
  // precedes the first solve.
  std::optional<std::size_t> iteration;
  std::vector<RegionSnapshot> regions;
  std::vector<PartitionRecord> partitions; // the level that produced `regions`
  std::vector<ModulePosition> modules;
  FramePhase phase = FramePhase::PostSolve;
};

// Read-only copying boundary. Snapshots own their data and retain no DB pointers.
class SnapshotSampler {
public:
  static DesignSnapshot geometry(odb::dbBlock *block,
                                 const ModuleRegistry &registry);
  static IterationSnapshot iteration(std::optional<std::size_t> number,
                                     const ModuleRegistry &registry,
                                     const std::vector<Region> &regions,
                                     const Eigen::VectorXd &x,
                                     const Eigen::VectorXd &y);
  static PartitionRecord partition(const Region &parent, bool is_vertical,
                                   const Region *low, const Region *high);
};

// Each writer owns a NEW directory and refuses to overwrite an existing run.
class JsonTraceWriter {
public:
  JsonTraceWriter(const std::filesystem::path &directory,
                  const DesignSnapshot &design);
  void append(const IterationSnapshot &snapshot);
  auto run_id() const -> const std::string & { return run_id_; }

private:
  std::string run_id_;
  std::ofstream iterations_;
  std::size_t next_frame_ = 0;
  std::size_t solves_written_ = 0;
};

// GP_TRACE_DIR enables collection; unset/empty means no sampling or file I/O.
// A per-solver instance owns the run. Errors warn once and disable tracing.
//
// Each level produces two frames (schema version 3):
//   sample() after a solve: the solved positions with the regions that solve
//     used ("post_solve").
//   record_split/record_unsplit for every parent region, then
//     sample_partition(): the same positions with the new regions and this
//     level's partition records ("post_partition").
// Any records still pending when sample() runs are attached to that frame.
//
// Alongside iterations.jsonl, metrics.jsonl records benchmark metrics (HPWL,
// quadratic objective, density overflow, containment, movement, timing) for
// the input placement and for every frame. A metrics failure warns once and
// disables only the metrics; the trace continues.
class Tracer {
public:
  Tracer();
  ~Tracer();
  Tracer(const Tracer &) = delete;
  auto operator=(const Tracer &) -> Tracer & = delete;

  // DEBUG ONLY: stamps Region::debug on both sons (always, as it is cheap) and
  // records the split when tracing is enabled. Never alters placement data.
  void record_split(const Region &parent, Region &low, Region &high,
                    bool is_vertical) noexcept;
  void record_unsplit(const Region &region, bool is_vertical) noexcept;

  void sample(odb::dbBlock *block, const ModuleRegistry &registry,
              const std::vector<Region> &regions, const Eigen::VectorXd &x,
              const Eigen::VectorXd &y) noexcept;
  // Uses the registry's current movable positions (the latest solve).
  void sample_partition(odb::dbBlock *block, const ModuleRegistry &registry,
                        const std::vector<Region> &regions) noexcept;

private:
  void emit(odb::dbBlock *block, const ModuleRegistry &registry,
            IterationSnapshot snapshot) noexcept;
  void record_metrics(odb::dbBlock *block, const ModuleRegistry &registry,
                      const IterationSnapshot &snapshot) noexcept;
  bool active() const noexcept;
  void disable(const char *reason) noexcept;

  bool disabled_ = false;
  std::unique_ptr<JsonTraceWriter> writer_;
  std::size_t solves_ = 0; // completed post_solve frames
  std::size_t next_region_id_ = 1; // the root keeps the default ID 0
  std::vector<PartitionRecord> pending_;
  std::unique_ptr<PlacementMetrics> metrics_;
  std::unique_ptr<MetricsWriter> metrics_writer_;
  bool metrics_disabled_ = false;
  std::size_t frames_emitted_ = 0;
};

} // namespace gordian::tracer
