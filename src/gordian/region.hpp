#pragma once

#include "commmon.h"
#include "database/module_ref.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <odb/db.h>
#include <optional>
#include <span>
#include <stdexcept>

namespace gordian {

struct SlicingTreeNode {};
struct SlicingTree {};

// DEBUG ONLY: region identity for the tracer (src/tracer). The solver never
// reads these values. IDs are plain integers, not pointers, so discarded parent
// regions cannot dangle; the history lives in the trace file, not in memory.
struct RegionDebugInfo {
  static constexpr std::size_t no_parent =
      std::numeric_limits<std::size_t>::max();
  std::size_t id = 0; // root is 0; the tracer assigns child IDs
  std::size_t parent_id = no_parent;
  std::size_t depth = 0;
};

struct Region {
  // we only need to partition moavble modules, fixed modules are NOT used to
  // build constraint system
  Vector<const ModuleRef *> movable_modules;

  double x_min, y_min, x_max, y_max;

  RegionDebugInfo
      debug{}; // DEBUG ONLY; keep last so aggregate init is unchanged
  // The dependent (eliminated) module of this region's centre-of-gravity
  // constraint. Choosing the largest area S_k keeps every entry -S_m/S_k of Z
  // in [-1, 0) and the representative's x0 entry (D_r/S_k) * centre as small as
  // possible, which bounds the conditioning of Z^T C Z. Ties go to the smallest
  // module ID, so the choice is deterministic and independent of member order.
  // Stateless (no cache) so Region stays an aggregate; the builder calls this
  // twice per region and both calls must agree, which determinism guarantees.
  auto representative() const -> const ModuleRef * {
    if (movable_modules.empty()) {
      throw std::logic_error("Region::representative: region has no modules");
    }
    // const auto larger = [](const ModuleRef *a, const ModuleRef *b) {
    //   const double area_a = a->get_area();
    //   const double area_b = b->get_area();
    //   return area_a != area_b ? area_a > area_b : a->get_id() < b->get_id();
    // };
    // return *std::ranges::min_element(movable_modules, larger);
    return movable_modules.front();
  }
  auto get_centre_xy() const -> Pair<double> {
    return {0.5 * x_min + 0.5 * x_max, 0.5 * y_min + 0.5 * y_max};
  }
};

enum class RegionPartitionPolicy { Naive };

template <RegionPartitionPolicy Policy> struct RegionPartitioner;

template <> struct RegionPartitioner<RegionPartitionPolicy::Naive> {
  RegionPartitioner(const Region &region, bool is_vertical)
      : p_region__(region),
        p_movable_modules_sorted__(region.movable_modules.begin(),
                                   region.movable_modules.end()),
        area_prefix__(region.movable_modules.size()),
        is_vertical__(is_vertical), p_x_min__(region.x_min),
        p_x_max__(region.x_max), p_y_min__(region.y_min),
        p_y_max__(region.y_max) {
    auto coord = [is_vertical](const ModuleRef *m) -> double {
      const auto [x, y] = m->get_xy();
      return is_vertical ? x : y;
    };
    std::ranges::sort(p_movable_modules_sorted__, {}, coord);
  }

  void skip_when_n_modules_below(const size_t n_modules) {
    skip_partition_when_n_modules_below__ = n_modules;
  }

  auto partition() -> std::optional<std::pair<Region, Region>> {
    if (p_movable_modules_sorted__.size() <=
        skip_partition_when_n_modules_below__) {
      return std::nullopt;
    }
    const auto split = naive_find_split__();
    if (!split) {
      return std::nullopt;
    }
    return partition__(split->first, split->second);
  }

private:
  auto partition__(const size_t &module_split_idx,
                   const double region_split_coord)
      -> std::pair<Region, Region> {
    const auto &is_vertical = is_vertical__;
    const auto &sorted_movables = p_movable_modules_sorted__;
    Vector<const ModuleRef *> left_modules(
        sorted_movables.begin(), sorted_movables.begin() + module_split_idx);
    Vector<const ModuleRef *> right_modules(
        sorted_movables.begin() + module_split_idx, sorted_movables.end());
    Region left_region{left_modules, p_x_min__, p_y_min__,
                       is_vertical ? region_split_coord : p_x_max__,
                       is_vertical ? p_y_max__ : region_split_coord};
    Region right_region{
        right_modules, is_vertical ? region_split_coord : p_x_min__,
        is_vertical ? p_y_min__ : region_split_coord, p_x_max__, p_y_max__};
    return std::make_pair(left_region, right_region);
  }
  auto naive_find_split__() -> std::optional<std::pair<size_t, double>> {
    auto &sorted = p_movable_modules_sorted__;
    auto &area_prefix = area_prefix__;

    if (sorted.size() < 3) {
      return std::nullopt;
    }

    std::transform_inclusive_scan(
        sorted.begin(), sorted.end(), area_prefix.begin(), std::plus<>(),
        [](const ModuleRef *m) { return m->get_area(); });

    const double total_area = area_prefix.back();
    if (total_area == 0.0) {
      return std::nullopt;
    }

    const auto candidates =
        std::span(area_prefix).first(area_prefix.size() - 1);

    const auto best_left_it = std::ranges::min_element(
        candidates, {}, [total_area](double area_prefix) {
          return std::abs(area_prefix - 0.5 * total_area);
        });

    // area_prefix[k] includes module k, so the left side holds k + 1 modules.
    // Candidates exclude the last prefix, so both sides are non-empty.
    const auto module_split_idx = static_cast<size_t>(
        std::ranges::distance(candidates.begin(), best_left_it) + 1);

    const auto &lo = is_vertical__ ? p_x_min__ : p_y_min__;
    const auto &hi = is_vertical__ ? p_x_max__ : p_y_max__;
    const auto alpha = *best_left_it / total_area;
    const auto region_split_coord = lo + alpha * (hi - lo);

    return std::make_pair(module_split_idx, region_split_coord);
  }

private:
  const Region &p_region__;
  Vector<const ModuleRef *> p_movable_modules_sorted__;
  Vector<double> area_prefix__;
  bool is_vertical__;
  double p_x_min__, p_x_max__, p_y_min__, p_y_max__;
  double skip_partition_when_n_modules_below__ = 3;
};

} // namespace gordian
