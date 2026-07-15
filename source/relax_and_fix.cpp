/**
 * @file      relax_and_fix.cpp
 * @brief     Relax-and-fix rolling-window integrality implementation
 * @date      2026-07-14
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * See relax_and_fix.hpp for the algorithm.  Everything here operates IN
 * PLACE on the live LinearInterface — the LP is built once and never
 * cloned; only column integrality flags and column bounds are mutated, and
 * only through the solver-agnostic LinearInterface raw setters.  Integer
 * columns NOT in the window partition (e.g. discrete investment decisions
 * that belong to no block) simply keep their integrality through every
 * window solve — correct, just not accelerated.
 *
 * Backend-residue note: CBC's branchAndBound() can leave node bound
 * fixings behind on the solved OSI model (see the fresh-interface remark
 * in test_mip_start_roundtrip.cpp).  The rolling loop therefore RE-ASSERTS
 * the bounds of every originally-integer column after each window solve —
 * pinned columns to their pinned 0/1 value, everything else to the
 * original bounds — through one bulk `set_col_bounds_raw` dispatch, so a
 * window MIP always starts from a well-defined bound state on every
 * backend.  The FINAL window's solve is never followed by any mutation:
 * its backend state (incumbent, integrality, bounds) is the returned MIP
 * solution.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <gtopt/linear_interface.hpp>
#include <gtopt/relax_and_fix.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Deduped windowed column: raw index, earliest start hour, rolling state.
struct WinCol
{
  int col {};
  double hour {};
  int window {};
  bool pinned {false};
  double pin_value {};
};

/// Plain (non-rolling) solve wrapped into the report shape.
[[nodiscard]] std::expected<RelaxAndFixReport, Error> plain_solve(
    LinearInterface& li, const SolverOptions& solve_opts, int window_cols)
{
  auto r = li.resolve(solve_opts);
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  return RelaxAndFixReport {
      .status = *r,
      .windows = 1,
      .window_cols = window_cols,
  };
}

}  // namespace

std::vector<RelaxAndFixCol> make_relax_and_fix_cols(
    std::span<const CommitmentRunInfo> commitments,
    const std::unordered_map<Uid, double>& block_start_hours)
{
  // A commitment period's u column appears once per member block in
  // `status_cols`; keep the EARLIEST hour per column (the period starts
  // there), deduping through a map keyed by raw column index.
  std::unordered_map<int, double> col_hour;
  for (const auto& info : commitments) {
    const auto n = std::min(info.status_cols.size(), info.block_uids.size());
    for (std::size_t t = 0; t < n; ++t) {
      const auto it = block_start_hours.find(info.block_uids[t]);
      if (it == block_start_hours.end()) {
        continue;
      }
      const int col = info.status_cols[t];
      const auto [pos, inserted] = col_hour.try_emplace(col, it->second);
      if (!inserted) {
        pos->second = std::min(pos->second, it->second);
      }
    }
  }

  std::vector<RelaxAndFixCol> cols;
  cols.reserve(col_hour.size());
  for (const auto& [col, hour] : col_hour) {
    cols.push_back({.col = col, .start_hour = hour});
  }
  // Deterministic order (unordered_map iteration is not): by hour, then col.
  std::ranges::sort(cols,
                    [](const RelaxAndFixCol& a, const RelaxAndFixCol& b)
                    {
                      return std::tie(a.start_hour, a.col)
                          < std::tie(b.start_hour, b.col);
                    });
  return cols;
}

std::expected<RelaxAndFixReport, Error> solve_relax_and_fix(
    LinearInterface& li,
    const SolverOptions& solve_opts,
    std::span<const RelaxAndFixCol> cols,
    const RelaxAndFixWindowOptions& opts)
{
  const double window = opts.window_hours;
  if (window <= 0.0 || cols.empty()) {
    return plain_solve(li, solve_opts, 0);
  }

  // CBC's OsiCbc backend cannot correctly re-solve a mutated MIP in place: a
  // later window reports the LP relaxation infeasible and even the fallback
  // returns a corrupted (non-integer) solution.  Gate it to the plain full
  // MIP — correct, just not accelerated.  cplex / scip / highs / mindopt
  // drive the in-place rolling loop cleanly.
  if (li.solver_name() == "cbc") {
    spdlog::warn(
        "relax-and-fix: backend 'cbc' does not support in-place rolling "
        "re-solve — using the plain full MIP instead");
    return plain_solve(li, solve_opts, 0);
  }

  // ── Dedupe, filter to genuinely-integer columns, partition ─────────────
  //
  // Only columns the LP actually declared integer participate: under a
  // relaxed (--no-mip) build the commitment u columns are continuous and
  // relax-and-fix must not manufacture a MIP out of a pure LP.  Under
  // compress / rebuild low-memory modes the integrality flags live on the
  // backend — rehydrate it before the `is_integer` scan.
  li.ensure_backend();
  std::unordered_map<int, double> col_hour;
  for (const auto& c : cols) {
    if (!li.is_integer(ColIndex {c.col})) {
      continue;
    }
    const auto [pos, inserted] = col_hour.try_emplace(c.col, c.start_hour);
    if (!inserted) {
      pos->second = std::min(pos->second, c.start_hour);
    }
  }
  if (col_hour.empty()) {
    return plain_solve(li, solve_opts, 0);
  }
  double t0 = std::numeric_limits<double>::infinity();
  double t_last = -std::numeric_limits<double>::infinity();
  for (const auto& [col, hour] : col_hour) {
    t0 = std::min(t0, hour);
    t_last = std::max(t_last, hour);
  }
  const int num_windows =
      1 + static_cast<int>(std::floor((t_last - t0) / window));
  if (num_windows <= 1) {
    // One window spans the whole horizon — identical to the plain MIP.
    return plain_solve(li, solve_opts, static_cast<int>(col_hour.size()));
  }

  std::vector<WinCol> wcols;
  wcols.reserve(col_hour.size());
  for (const auto& [col, hour] : col_hour) {
    const int w = std::min(num_windows - 1,
                           static_cast<int>(std::floor((hour - t0) / window)));
    wcols.push_back({.col = col, .hour = hour, .window = w});
  }
  std::ranges::sort(
      wcols,
      [](const WinCol& a, const WinCol& b)
      { return std::tie(a.hour, a.col) < std::tie(b.hour, b.col); });

  const double overlap = std::clamp(opts.overlap_hours, 0.0, window);
  if (opts.overlap_hours > window) {
    spdlog::warn(
        "relax-and-fix: overlap {:.3g} h exceeds the window ({:.3g} h); "
        "clamped to one full window",
        opts.overlap_hours,
        window);
  }

  // Snapshot the ORIGINAL bounds (copies, not spans: the spans alias live
  // backend memory) and every originally-integer column — the re-assert /
  // fallback targets.
  const auto lb_span = li.get_col_low_raw();
  const auto ub_span = li.get_col_upp_raw();
  const std::vector<double> lb0(lb_span.begin(), lb_span.end());
  const std::vector<double> ub0(ub_span.begin(), ub_span.end());
  const int ncols = li.get_numcols();
  std::vector<int> other_int_cols;  // integer cols outside the partition
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i}) && !col_hour.contains(i)) {
      other_int_cols.push_back(i);
    }
  }

  // One bulk `set_col_bounds_raw` dispatch that (re-)establishes the bound
  // state every window MIP must start from: pinned windowed columns at
  // their pinned 0/1 value ('B'), every other originally-integer column at
  // its original bounds ('L' + 'U').  Also scrubs any node-bound residue a
  // backend's branch-and-bound left behind (CBC).
  std::vector<ColIndex> bidx;
  std::vector<char> blu;
  std::vector<double> bval;
  const auto reassert_bounds = [&]()
  {
    bidx.clear();
    blu.clear();
    bval.clear();
    const auto restore = [&](int col)
    {
      const auto u = static_cast<std::size_t>(col);
      bidx.emplace_back(col);
      blu.push_back('L');
      bval.push_back(lb0[u]);
      bidx.emplace_back(col);
      blu.push_back('U');
      bval.push_back(ub0[u]);
    };
    for (const auto& wc : wcols) {
      if (wc.pinned) {
        bidx.emplace_back(wc.col);
        blu.push_back('B');
        bval.push_back(wc.pin_value);
      } else {
        restore(wc.col);
      }
    }
    for (const int col : other_int_cols) {
      restore(col);
    }
    li.set_col_bounds_raw(bidx, blu, bval);
  };

  // Fallback: unpin everything, restore every integrality flag, and solve
  // the plain full MIP — relax-and-fix may lose time, never a solution.
  const auto fallback =
      [&](int windows_attempted) -> std::expected<RelaxAndFixReport, Error>
  {
    for (auto& wc : wcols) {
      wc.pinned = false;
      li.set_integer(ColIndex {wc.col});
    }
    reassert_bounds();
    spdlog::warn(
        "relax-and-fix: window {}/{} MIP failed — restored bounds and "
        "integrality, falling back to the plain full MIP",
        windows_attempted,
        num_windows);
    auto r = li.resolve(solve_opts);
    if (!r) {
      return std::unexpected(std::move(r.error()));
    }
    return RelaxAndFixReport {
        .status = *r,
        .windows = windows_attempted,
        .window_cols = static_cast<int>(wcols.size()),
        .fallback = true,
    };
  };

  // ── Rolling loop ───────────────────────────────────────────────────────
  //
  // Invariants at solve k:
  //   * window(c) == k          → just flipped INTEGER (the active window);
  //   * window(c) <  k, !pinned → still integer (the re-optimized overlap
  //                               tail of window k−1);
  //   * window(c) <  k, pinned  → bounds pinned to the solved 0/1 value —
  //                               integer-flagged but never a branching
  //                               candidate;
  //   * window(c) >  k          → CONTINUOUS (LP-relaxed future).
  // A column is pinned exactly once — when it drops `overlap` hours behind
  // the advancing window edge — and unpinned only by the fallback.
  int relaxed_now = 0;
  for (const auto& wc : wcols) {
    if (wc.window > 0) {
      li.set_continuous(ColIndex {wc.col});
      ++relaxed_now;
    }
  }
  spdlog::info(
      "relax-and-fix: {} windowed binaries over [{:.6g}, {:.6g}] h → {} "
      "windows of {:.6g} h (overlap {:.6g} h); {} relaxed for window 0, {} "
      "non-windowed integer column(s) kept integer throughout",
      wcols.size(),
      t0,
      t_last,
      num_windows,
      window,
      overlap,
      relaxed_now,
      other_int_cols.size());

  const auto t_start = std::chrono::steady_clock::now();
  int last_status = 0;
  int pinned_total = 0;
  for (int k = 0; k < num_windows; ++k) {
    // Activate window k's binaries (window 0's were never relaxed).
    int activated = 0;
    int free_int = 0;
    int relaxed = 0;
    for (const auto& wc : wcols) {
      if (wc.window == k && k > 0) {
        li.set_integer(ColIndex {wc.col});
        ++activated;
      }
      if (wc.window > k) {
        ++relaxed;
      } else if (!wc.pinned) {
        ++free_int;
      }
    }

    const auto t_win = std::chrono::steady_clock::now();
    auto r = li.resolve(solve_opts);
    const bool ok = r.has_value() && li.is_optimal();
    const auto win_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_win)
            .count();
    if (!ok) {
      if (!r) {
        spdlog::warn("relax-and-fix: window {}/{} solver error: {}",
                     k + 1,
                     num_windows,
                     r.error().message);
      }
      return fallback(k + 1);
    }
    last_status = *r;
    const double win_obj = li.get_obj_value();

    // Pin the binaries that leave the active region (all active binaries
    // more than `overlap` hours behind the next window edge), then
    // re-assert the bound state for the next window.  NEVER after the
    // final window — the last solve's backend state (incumbent,
    // integrality, bounds) is the returned MIP solution and must stay
    // untouched for the output / dual-recovery / dump paths.
    int pinned_now = 0;
    if (k + 1 < num_windows) {
      const double fix_before = t0 + ((k + 1) * window) - overlap;
      const auto sol = li.get_col_sol_raw();
      for (auto& wc : wcols) {
        if (wc.pinned || wc.hour >= fix_before) {
          continue;
        }
        const auto u = static_cast<std::size_t>(wc.col);
        wc.pin_value = std::clamp(std::round(sol[u]), lb0[u], ub0[u]);
        wc.pinned = true;
        ++pinned_now;
      }
      pinned_total += pinned_now;
      reassert_bounds();
    }

    spdlog::info(
        "relax-and-fix: window {}/{} (+{} activated) — {} integer free, {} "
        "pinned, {} relaxed; obj={:.8g}, {:.3f} s{}",
        k + 1,
        num_windows,
        activated,
        free_int,
        pinned_total - pinned_now,
        relaxed,
        win_obj,
        win_s,
        pinned_now > 0 ? std::format(" → pinned {}", pinned_now) : "");
  }

  const auto total_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start)
          .count();
  spdlog::info(
      "relax-and-fix: {} windows solved in {:.3f} s — final obj={:.8g} "
      "({} of {} binaries pinned en route, {} integer in the final MIP)",
      num_windows,
      total_s,
      li.get_obj_value(),
      pinned_total,
      wcols.size(),
      wcols.size() - static_cast<std::size_t>(pinned_total));

  return RelaxAndFixReport {
      .status = last_status,
      .windows = num_windows,
      .window_cols = static_cast<int>(wcols.size()),
  };
}

}  // namespace gtopt
