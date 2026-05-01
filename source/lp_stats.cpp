/**
 * @file      lp_stats.cpp
 * @brief     Implementation of LP coefficient static analysis logging
 * @date      2026-03-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <format>
#include <limits>
#include <unordered_map>

#include <gtopt/fmap.hpp>
#include <gtopt/lp_stats.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

void log_stats_line(std::string_view label, const ScenePhaseLPStats& stats)
{
  spdlog::info(
      "  {:<28s} vars={:>7} cons={:>7} "
      "|coeff| [{:.3e}, {:.3e}] ratio={:.2e} ({})",
      label,
      stats.num_vars,
      stats.num_constraints,
      stats.stats_min_abs,
      stats.stats_max_abs,
      stats.coeff_ratio(),
      stats.quality_label());

  // Print per-column details for max/min coefficients when available.
  if (stats.stats_max_col >= 0) {
    if (!stats.stats_max_col_name.empty()) {
      spdlog::info("    max |coeff|={:.3e}  col={}  name={}",
                   stats.stats_max_abs,
                   stats.stats_max_col,
                   stats.stats_max_col_name);
    } else {
      spdlog::info("    max |coeff|={:.3e}  col={}",
                   stats.stats_max_abs,
                   stats.stats_max_col);
    }
  }
  if (stats.stats_min_col >= 0) {
    if (!stats.stats_min_col_name.empty()) {
      spdlog::info("    min |coeff|={:.3e}  col={}  name={}",
                   stats.stats_min_abs,
                   stats.stats_min_col,
                   stats.stats_min_col_name);
    } else {
      spdlog::info("    min |coeff|={:.3e}  col={}",
                   stats.stats_min_abs,
                   stats.stats_min_col);
    }
  }
  if (stats.stats_zeroed > 0) {
    spdlog::info("    zeroed by eps: {} entries", stats.stats_zeroed);
  }
}

}  // namespace

void log_lp_stats_summary(const std::vector<ScenePhaseLPStats>& entries,
                          double ratio_threshold)
{
  if (entries.empty()) {
    return;
  }

  // Compute global aggregate.
  ScenePhaseLPStats global {};
  global.stats_min_abs = std::numeric_limits<double>::max();
  for (const auto& e : entries) {
    global.num_vars += e.num_vars;
    global.num_constraints += e.num_constraints;
    global.stats_nnz += e.stats_nnz;
    global.stats_zeroed += e.stats_zeroed;
    global.stats_max_abs = std::max(global.stats_max_abs, e.stats_max_abs);
    if (e.stats_nnz > 0 && e.stats_min_col >= 0) {
      if (e.stats_min_abs < global.stats_min_abs) {
        global.stats_min_abs = e.stats_min_abs;
        global.stats_min_col = e.stats_min_col;
        global.stats_min_col_name = e.stats_min_col_name;
      }
    }
    if (e.stats_max_abs > global.stats_max_abs && e.stats_max_col >= 0) {
      global.stats_max_col = e.stats_max_col;
      global.stats_max_col_name = e.stats_max_col_name;
    }
  }

  // P0-4 — when nothing was accumulated (e.g. under --low-memory=compress,
  // or any path that bypasses the flatten() stats scan), every entry has
  // `stats_nnz == 0` and the sentinel `stats_min_abs = DBL_MAX` has never
  // been overwritten.  Report "unavailable" instead of printing garbage.
  if (global.stats_nnz == 0) {
    spdlog::info(
        "  LP coefficient analysis: {} LP(s), stats unavailable "
        "(compute_stats disabled or low-memory path)",
        entries.size());
    return;
  }

  // If the global ratio is within the threshold, emit a one-liner.
  if (global.coeff_ratio() <= ratio_threshold) {
    auto line = std::format(
        "  LP coefficient analysis: {} LP(s), "
        "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({})",
        entries.size(),
        global.stats_min_abs,
        global.stats_max_abs,
        global.coeff_ratio(),
        global.quality_label());
    if (global.stats_zeroed > 0) {
      line += std::format(", zeroed={}", global.stats_zeroed);
    }
    spdlog::info(line);
    return;
  }

  // Show global summary + worst-case entry only (not all 51 phases).
  auto header = std::format(
      "  LP coefficient analysis: {} LP(s), "
      "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({}) — "
      "exceeds threshold {:.0e}",
      entries.size(),
      global.stats_min_abs,
      global.stats_max_abs,
      global.coeff_ratio(),
      global.quality_label(),
      ratio_threshold);
  if (global.stats_zeroed > 0) {
    header += std::format(", zeroed={}", global.stats_zeroed);
  }
  spdlog::info(header);

  // Show only the worst-case scene/phase.
  const auto& worst =
      *std::ranges::max_element(entries, {}, &ScenePhaseLPStats::coeff_ratio);
  log_stats_line(
      std::format("worst: scene {} phase {}", worst.scene_uid, worst.phase_uid),
      worst);

  // Per-row-type breakdown: aggregate across all scenes/phases.
  // Collect per-type stats from the first entry that has them (all
  // scene/phase LPs share the same constraint structure, so any entry
  // suffices for type classification).
  //
  // Key is `std::string_view` (not owning) —
  // `entries[i].row_type_stats[j].type` outlives this function, so the views
  // stay valid throughout the aggregate / sort / log walk.  Mirrors the cousin
  // `type_map` in `linear_problem.cpp:680` and eliminates one heap allocation
  // per distinct constraint type.
  std::unordered_map<std::string_view, RowTypeStats> type_map;
  // The LP has typically 20–50 distinct constraint types (Demand,
  // Capacity, Kirchhoff, etc.) so 32 is a tight upper-bound that
  // avoids rehash during accumulation.
  map_reserve(type_map, 32);
  for (const auto& entry : entries) {
    for (const auto& rts : entry.row_type_stats) {
      auto& acc = type_map[rts.type];
      if (acc.type.empty()) {
        acc.type = rts.type;
      }
      acc.count += rts.count;
      acc.nnz += rts.nnz;
      acc.max_abs = std::max(acc.max_abs, rts.max_abs);
      if (rts.nnz > 0 && rts.min_abs < acc.min_abs) {
        acc.min_abs = rts.min_abs;
      }
    }
  }

  if (!type_map.empty()) {
    // Sort by ratio descending.
    std::vector<RowTypeStats> sorted_types;
    sorted_types.reserve(type_map.size());
    for (auto& [_, v] : type_map) {
      sorted_types.push_back(std::move(v));
    }
    std::ranges::sort(sorted_types,
                      [](const RowTypeStats& a, const RowTypeStats& b)
                      { return a.coeff_ratio() > b.coeff_ratio(); });

    spdlog::info("  Per-row-type coefficient breakdown:");
    for (const auto& rts : sorted_types) {
      spdlog::info(
          "    {:<12s} rows={:>6} nnz={:>8} "
          "|coeff| [{:.3e}, {:.3e}] ratio={:.2e}",
          rts.type,
          rts.count,
          rts.nnz,
          rts.min_abs,
          rts.max_abs,
          rts.coeff_ratio());
    }
  }
}

}  // namespace gtopt
