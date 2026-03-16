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

#include <gtopt/lp_stats.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

void log_stats_line(std::string_view label, const ScenePhaseLPStats& stats)
{
  spdlog::info(
      std::format("  {:<28s} vars={:>7} cons={:>7} "
                  "|coeff| [{:.3e}, {:.3e}] ratio={:.2e} ({})",
                  label,
                  stats.num_vars,
                  stats.num_constraints,
                  stats.stats_min_abs,
                  stats.stats_max_abs,
                  stats.coeff_ratio(),
                  stats.quality_label()));

  // Print per-column details for max/min coefficients when available.
  if (stats.stats_max_col >= 0) {
    if (!stats.stats_max_col_name.empty()) {
      spdlog::info(std::format("    max |coeff|={:.3e}  col={}  name={}",
                               stats.stats_max_abs,
                               stats.stats_max_col,
                               stats.stats_max_col_name));
    } else {
      spdlog::info(std::format("    max |coeff|={:.3e}  col={}",
                               stats.stats_max_abs,
                               stats.stats_max_col));
    }
  }
  if (stats.stats_min_col >= 0) {
    if (!stats.stats_min_col_name.empty()) {
      spdlog::info(std::format("    min |coeff|={:.3e}  col={}  name={}",
                               stats.stats_min_abs,
                               stats.stats_min_col,
                               stats.stats_min_col_name));
    } else {
      spdlog::info(std::format("    min |coeff|={:.3e}  col={}",
                               stats.stats_min_abs,
                               stats.stats_min_col));
    }
  }
  if (stats.stats_zeroed > 0) {
    spdlog::info(
        std::format("    zeroed by eps: {} entries", stats.stats_zeroed));
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

  // Detailed per-scene/phase table.
  spdlog::info(std::format(
      "  LP coefficient analysis: {} LP(s) — "
      "global ratio {:.2e} exceeds threshold {:.0e}, showing details:",
      entries.size(),
      global.coeff_ratio(),
      ratio_threshold));

  for (const auto& entry : entries) {
    log_stats_line(
        std::format("scene {} phase {}", entry.scene_uid, entry.phase_uid),
        entry);
  }

  log_stats_line("GLOBAL", global);
}

}  // namespace gtopt
