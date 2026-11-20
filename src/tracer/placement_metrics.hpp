#pragma once

// Benchmark metrics for placement traces (DEBUG/EVALUATION ONLY).
//
// Everything is derived from data the tracer already receives: a read-only
// OpenDB block, the module registry (ID mapping only), and each frame's
// IterationSnapshot (positions, membership, region bounds). Nothing here
// feeds back into placement.

#include "database/module_registry.hpp"
#include "tracer/placement_tracer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace gordian::tracer {

struct MetricsConfig {
  double bin_rows = 4.0;       // density bin side, in row heights
  double target_density = 1.0; // allowed fraction of a bin's free area
};

struct Distribution {
  double median = 0;
  double p95 = 0;
  double max = 0;
};

// One line of metrics.jsonl. Lengths in micrometres, areas in um^2.
struct FrameMetrics {
  std::optional<std::size_t> frame;     // absent for the input baseline
  std::string phase;                    // input | post_solve | post_partition
  std::optional<std::size_t> iteration; // solve shown, as in iterations.jsonl
  double hpwl_all = 0;      // all non-supply nets
  double hpwl_modelled = 0; // nets the solver's net filter keeps
  double objective = 0;     // 0.5 * sum_n w_n * sum_p |pin_p - centroid_n|^2
  double bin_um = 0;
  double target_density = 0;
  double overflow = 0; // (sum max(0, A_bin - t * cap_bin) + A_off_core) / A_mov
  double max_bin_density = 0;
  std::size_t outside_die = 0;
  std::size_t outside_core = 0;
  std::optional<std::size_t> outside_own_region;
  std::optional<double> outside_own_region_area_fraction;
  std::optional<Distribution> move_from_previous_solve; // post_solve only
  Distribution move_from_input;
  std::optional<double> cog_error; // post_solve only: max |COG - centre|
  std::optional<std::size_t> region_count;
  std::optional<double> region_median_size;
  std::optional<std::size_t> singleton_regions;
  double seconds_since_previous = 0;
  double seconds_since_start = 0;
};

// Static design data plus per-frame evaluation. Construct once per run.
class PlacementMetrics {
public:
  PlacementMetrics(odb::dbBlock *block, const ModuleRegistry &registry,
                   MetricsConfig config = {});

  // Metrics of the input placement (OpenDB positions; no regions).
  FrameMetrics baseline();
  // Metrics of one emitted frame. Call in emission order.
  FrameMetrics evaluate(std::size_t frame, const IterationSnapshot &snapshot);

private:
  struct Pin {
    std::int64_t module; // movable registry ID, or -1 for a fixed location
    double x;            // offset from the module centre, or absolute (DBU)
    double y;
    bool solver_visible; // the solver's model includes this pin
  };
  struct Net {
    double weight;
    bool modelled; // passes the solver's net filter
    std::vector<Pin> pins;
  };
  struct Rect {
    double x0, y0, x1, y1;
  };

  FrameMetrics measure(const std::vector<double> &x, const std::vector<double> &y);
  void finish_timing(FrameMetrics &metrics);

  double dbu_ = 1;
  MetricsConfig config_;
  Rect core_{}, die_{};
  std::vector<Net> nets_;
  std::vector<double> area_;                 // movable footprint area (DBU^2)
  std::vector<double> half_w_, half_h_;      // movable footprint half extents
  std::vector<double> input_x_, input_y_;    // input placement centres
  std::vector<Rect> fixed_;                  // fixed instance footprints
  double movable_area_ = 0;
  double bin_ = 0;                           // DBU
  std::size_t nx_ = 0, ny_ = 0;
  std::vector<double> capacity_;             // free area per bin (DBU^2)
  std::vector<double> last_solve_x_, last_solve_y_;
  bool have_last_solve_ = false;
  std::chrono::steady_clock::time_point start_, previous_;
};

// Appends metrics.jsonl next to iterations.jsonl. The first line is the input
// baseline with a schema header; then one line per frame.
class MetricsWriter {
public:
  MetricsWriter(const std::filesystem::path &directory, const std::string &run_id,
                const FrameMetrics &baseline);
  void append(const FrameMetrics &metrics);

private:
  std::ofstream out_;
};

} // namespace gordian::tracer
