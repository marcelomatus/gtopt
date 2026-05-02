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
  // ColIndex is `strong::formattable`; format its underlying value via
  // `value_of()` so spdlog gets a plain int for the `col=...` slot
  // without requiring a `std::formatter<std::optional<ColIndex>>`.
  if (stats.stats_max_col) {
    if (!stats.stats_max_col_name.empty()) {
      spdlog::info("    max |coeff|={:.3e}  col={}  name={}",
                   stats.stats_max_abs,
                   stats.stats_max_col->value_of(),
                   stats.stats_max_col_name);
    } else {
      spdlog::info("    max |coeff|={:.3e}  col={}",
                   stats.stats_max_abs,
                   stats.stats_max_col->value_of());
    }
  }
  if (stats.stats_min_col) {
    if (!stats.stats_min_col_name.empty()) {
      spdlog::info("    min |coeff|={:.3e}  col={}  name={}",
                   stats.stats_min_abs,
                   stats.stats_min_col->value_of(),
                   stats.stats_min_col_name);
    } else {
      spdlog::info("    min |coeff|={:.3e}  col={}",
                   stats.stats_min_abs,
                   stats.stats_min_col->value_of());
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
    if (e.stats_nnz > 0 && e.stats_min_col) {
      if (e.stats_min_abs < global.stats_min_abs) {
        global.stats_min_abs = e.stats_min_abs;
        global.stats_min_col = e.stats_min_col;
        global.stats_min_col_name = e.stats_min_col_name;
      }
    }
    if (e.stats_max_abs > global.stats_max_abs && e.stats_max_col) {
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
  // Split into a (with-zeroed / without-zeroed) pair of `spdlog::info`
  // calls so the format pattern + args reach spdlog directly — when
  // info is filtered out (e.g. `--log-level=warn`) the format work is
  // skipped entirely.  Pre-building the message via `std::format` and
  // passing it as a `std::string` defeats spdlog's lazy formatting.
  if (global.coeff_ratio() <= ratio_threshold) {
    if (global.stats_zeroed > 0) {
      spdlog::info(
          "  LP coefficient analysis: {} LP(s), "
          "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({}), zeroed={}",
          entries.size(),
          global.stats_min_abs,
          global.stats_max_abs,
          global.coeff_ratio(),
          global.quality_label(),
          global.stats_zeroed);
    } else {
      spdlog::info(
          "  LP coefficient analysis: {} LP(s), "
          "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({})",
          entries.size(),
          global.stats_min_abs,
          global.stats_max_abs,
          global.coeff_ratio(),
          global.quality_label());
    }
    return;
  }

  // Show global summary + worst-case entry only (not all 51 phases).
  // Same lazy-format split as above.
  if (global.stats_zeroed > 0) {
    spdlog::info(
        "  LP coefficient analysis: {} LP(s), "
        "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({}) — "
        "exceeds threshold {:.0e}, zeroed={}",
        entries.size(),
        global.stats_min_abs,
        global.stats_max_abs,
        global.coeff_ratio(),
        global.quality_label(),
        ratio_threshold,
        global.stats_zeroed);
  } else {
    spdlog::info(
        "  LP coefficient analysis: {} LP(s), "
        "global |coeff| [{:.3e}, {:.3e}], ratio={:.2e} ({}) — "
        "exceeds threshold {:.0e}",
        entries.size(),
        global.stats_min_abs,
        global.stats_max_abs,
        global.coeff_ratio(),
        global.quality_label(),
        ratio_threshold);
  }

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
  // `entries[i].row_type_stats[j].type` outlives this function, so the
  // views stay valid throughout the aggregate / sort / log walk.
  //
  // `gtopt::flat_map` (sorted contiguous storage) is preferred over
  // `std::unordered_map` here: the working set is small (~12-15 keys —
  // see `extract_row_type` in `linear_problem.cpp` for the truncation
  // behavior), iteration is hot (we walk the whole map to sort by
  // ratio), and insertion is cold (called once per `--stats` summary).
  // Cache locality + smaller per-entry overhead beats hash-table
  // indirection at this size.
  flat_map<std::string_view, RowTypeStats> type_map;
  // 32 is a tight upper-bound on distinct constraint types
  // (`extract_row_type` keys: balance/emission/cycle/flow/loss/target/
  // storage/elastic/scut/fcut/bcut/ecut/mcut/aperture/share/cut/...).
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
    // `auto&&` — `gtopt::flat_map` (std::flat_map under GCC 15) yields a
    // proxy `pair<key&, value&>` temporary, so a plain `auto&` lvalue
    // reference would not bind.
    for (auto&& [_, v] : type_map) {
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
