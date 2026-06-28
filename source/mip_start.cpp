/**
 * @file      mip_start.cpp
 * @brief     Initial-MIP-solution (warm-start) pipeline implementation
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>

#include <gtopt/linear_interface.hpp>
#include <gtopt/mip_start.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Round a relaxed value to its nearest feasible integer, biased by
/// `threshold`: a relaxed binary at `v` rounds up iff `v >= threshold`
/// (`floor(v + 1 - threshold)`), then clamp to `[lb, ub]`.
[[nodiscard]] double round_with_threshold(double v,
                                          double threshold,
                                          double lb,
                                          double ub) noexcept
{
  const double r = std::floor(v + (1.0 - threshold));
  return std::clamp(r, lb, ub);
}

/// In-place min-up/min-down repair of the rounded status columns in `values`
/// (a dense, raw-column-indexed vector).  Greedy forward sweep per unit:
/// suppress any transition that would end a run shorter than the unit's
/// minimum on/off time (accumulated block durations, hours) by extending the
/// current run.  Makes the rounded commitment respect the run-length rules so
/// the fixed dispatch LP is feasible and the start passes CHECKFEAS.
/// @return Number of column values flipped (for logging).
[[nodiscard]] int repair_run_lengths(
    std::vector<double>& values, std::span<const CommitmentRunInfo> commitments)
{
  constexpr double eps = 1e-9;
  int flipped = 0;
  for (const auto& c : commitments) {
    const std::size_t n = c.status_cols.size();
    if (n < 2 || (c.min_up_hours <= 0.0 && c.min_down_hours <= 0.0)) {
      continue;
    }
    const auto val = [&](std::size_t t) -> double&
    { return values[static_cast<std::size_t>(c.status_cols[t])]; };

    int state = (val(0) >= 0.5) ? 1 : 0;
    double run = c.durations[0];
    for (std::size_t t = 1; t < n; ++t) {
      const int u = (val(t) >= 0.5) ? 1 : 0;
      if (u == state) {
        run += c.durations[t];
        continue;
      }
      const double min_run = (state == 1) ? c.min_up_hours : c.min_down_hours;
      if (run + eps < min_run) {
        // Premature transition — extend the current run by suppressing it.
        val(t) = static_cast<double>(state);
        run += c.durations[t];
        ++flipped;
      } else {
        state = u;
        run = c.durations[t];
      }
    }
  }
  return flipped;
}

/// P1 generator: round the integer columns of the solved LP relaxation,
/// leaving the continuous (dispatch) columns at their relaxation value.
/// Storage-safe — operates on the whole-horizon relaxation, no time chunking.
class LpRoundMipStart final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "lp_round";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    auto& li = ctx.li;
    const auto sol = li.get_col_sol_raw();  // RAW relaxation primal
    if (sol.empty()) {
      return std::nullopt;
    }
    const auto lb = li.get_col_low_raw();
    const auto ub = li.get_col_upp_raw();
    const double threshold = ctx.opts.round_threshold.value_or(0.5);

    std::vector<double> start(sol.begin(), sol.end());
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      const double l = (u < lb.size()) ? lb[u] : 0.0;
      const double h = (u < ub.size()) ? ub[u] : 1.0;
      start[u] = round_with_threshold(sol[u], threshold, l, h);
    }
    // Repair min-up/down run-length violations on the rounded commitment.
    const int fixed = repair_run_lengths(start, ctx.commitments);
    if (fixed > 0) {
      spdlog::info(
          "MIP-start[lp_round]: min-up/down repair flipped {} status "
          "values",
          fixed);
    }
    return start;
  }
};

/// P2 generator: round the integer columns, PIN them to the rounded values,
/// and re-solve the full-horizon LP so the dependent continuous columns
/// (startup/shutdown logic, dispatch) are recomputed CONSISTENTLY with the
/// fixed commitment — yielding a genuinely feasible start (unlike lp_round,
/// whose startup/shutdown columns keep their fractional relaxation values and
/// violate the u−u₋₁ = v−w logic, so CHECKFEAS rejects it).  Storage-safe: the
/// whole horizon is fixed at once and one economic-dispatch LP keeps the
/// reservoir balance intact (no time chunking).  Returns std::nullopt when the
/// rounded commitment makes the ED-LP infeasible (rounded binaries violate hard
/// constraints) — the signal that a feasibility repair is needed.
class RelaxFixMipStart final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "relax_fix";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    auto& li = ctx.li;
    const auto sol = li.get_col_sol_raw();  // RAW relaxation primal
    if (sol.empty()) {
      return std::nullopt;
    }
    const auto lb = li.get_col_low_raw();
    const auto ub = li.get_col_upp_raw();
    const double threshold = ctx.opts.round_threshold.value_or(0.5);

    // Round every integer column into a working vector, then repair min-up/down
    // run-length violations so the fixed commitment is feasible.
    std::vector<double> rounded(sol.begin(), sol.end());
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      const double l = (u < lb.size()) ? lb[u] : 0.0;
      const double h = (u < ub.size()) ? ub[u] : 1.0;
      rounded[u] = round_with_threshold(sol[u], threshold, l, h);
    }
    const int fixed = repair_run_lengths(rounded, ctx.commitments);
    if (fixed > 0) {
      spdlog::info(
          "MIP-start[relax_fix]: min-up/down repair flipped {} status "
          "values",
          fixed);
    }

    // Pin the repaired commitment to bounds; record originals to restore.
    std::vector<ColIndex> cols;
    std::vector<double> pinned;
    std::vector<double> orig_lb;
    std::vector<double> orig_ub;
    cols.reserve(ctx.int_cols.size());
    pinned.reserve(ctx.int_cols.size());
    orig_lb.reserve(ctx.int_cols.size());
    orig_ub.reserve(ctx.int_cols.size());
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      cols.emplace_back(i);
      pinned.push_back(rounded[u]);
      orig_lb.push_back((u < lb.size()) ? lb[u] : 0.0);
      orig_ub.push_back((u < ub.size()) ? ub[u] : 1.0);
    }
    if (cols.empty()) {
      return std::nullopt;
    }

    // Pin the rounded commitment ('B' = both bounds) and re-solve the LP.
    const std::vector<char> lu_pin(cols.size(), 'B');
    li.set_col_bounds_raw(cols, lu_pin, pinned);
    const auto ed = li.resolve(ctx.relax_opts);
    const bool feasible = ed.has_value() && li.is_optimal();

    std::optional<std::vector<double>> start;
    if (feasible) {
      const auto fixed_sol = li.get_col_sol_raw();
      start = std::vector<double>(fixed_sol.begin(), fixed_sol.end());
    } else {
      spdlog::warn(
          "MIP-start[relax_fix]: economic-dispatch LP infeasible with the "
          "rounded commitment (rounded binaries violate hard constraints); no "
          "start produced — a feasibility repair pass is needed");
      // Confirm WHICH hard rows the rounded commitment conflicts with (the
      // repair pass must respect them — expected: min-up/down run-lengths).
      if (auto conflicts = li.diagnose_infeasibility(20)) {
        spdlog::warn(
            "MIP-start[relax_fix]: {} conflicting constraints in the fixed "
            "ED-LP (minimal infeasible subsystem):",
            conflicts->size());
        for (const auto& c : *conflicts) {
          spdlog::warn("  conflict: {}", c);
        }
      }
    }

    // Restore the original bounds on the (still-relaxed) integer columns.
    const std::vector<char> lu_lo(cols.size(), 'L');
    li.set_col_bounds_raw(cols, lu_lo, orig_lb);
    const std::vector<char> lu_hi(cols.size(), 'U');
    li.set_col_bounds_raw(cols, lu_hi, orig_ub);

    return start;
  }
};

/// File generator: replay an integer solution dumped by a previous solve
/// (`dump_integer_solution`).  The dumped `<index> <value>` lines carry the
/// integer-column raw values found by another solver; this overlays them onto
/// THIS solver's own LP-relaxation primal (the continuous columns stay at the
/// relaxation value, exactly like `lp_round`, but the integer columns come from
/// the file instead of rounding).  Enables a cross-solver hand-off: e.g. HiGHS
/// (strong MIP heuristics) finds a feasible incumbent and dumps it, then cuOpt
/// replays it as its start.  Both runs build the identical deterministic flat
/// LP, so raw column indices match 1:1 — validated by the `ncols` header and a
/// per-index integer-column membership check (no silent skip on mismatch).
class FileMipStart final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "file";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    const auto& path = ctx.opts.file;
    if (!path.has_value() || path->empty()) {
      spdlog::warn(
          "MIP-start[file]: no mip_start.file configured; nothing to replay");
      return std::nullopt;
    }
    std::ifstream in(*path);
    if (!in) {
      spdlog::error("MIP-start[file]: cannot open '{}' for reading", *path);
      return std::nullopt;
    }

    const auto sol = ctx.li.get_col_sol_raw();  // RAW relaxation primal = base
    if (sol.empty()) {
      return std::nullopt;
    }
    const auto ncols = static_cast<std::size_t>(ctx.li.get_numcols());
    const std::unordered_set<int> int_set(ctx.int_cols.begin(),
                                          ctx.int_cols.end());

    std::vector<double> start(sol.begin(), sol.end());
    std::size_t file_ncols = 0;
    std::size_t placed = 0;
    std::size_t skipped = 0;
    std::string token;
    while (in >> token) {
      if (token == "ncols") {
        in >> file_ncols;
        if (file_ncols != ncols) {
          spdlog::error(
              "MIP-start[file]: column-count mismatch (dump ncols={}, this LP "
              "ncols={}); refusing to replay a start built for a different LP",
              file_ncols,
              ncols);
          return std::nullopt;
        }
        continue;
      }
      if (token == "nint") {
        in >> token;  // value consumed, informational only
        continue;
      }
      if (!token.empty() && token.front() == '#') {
        std::getline(in, token);  // comment line — discard remainder
        continue;
      }
      // Otherwise `token` is a column index; the next token is its value.
      int idx = -1;
      double value = 0.0;
      try {
        idx = std::stoi(token);
      } catch (const std::exception&) {
        continue;  // not an index token; skip
      }
      if (!(in >> value)) {
        break;
      }
      const auto u = static_cast<std::size_t>(idx);
      if (idx < 0 || u >= ncols || !int_set.contains(idx)) {
        ++skipped;
        continue;
      }
      start[u] = value;
      ++placed;
    }

    if (placed == 0) {
      spdlog::error(
          "MIP-start[file]: dump '{}' yielded no usable integer values "
          "(placed=0, skipped={})",
          *path,
          skipped);
      return std::nullopt;
    }
    spdlog::info(
        "MIP-start[file]: replayed {} integer values from '{}' ({} skipped, "
        "{} integer cols in this LP)",
        placed,
        *path,
        skipped,
        ctx.int_cols.size());
    return start;
  }
};

/// Report the saturated / binding constraints (nonzero dual) of the solved LP
/// relaxation — solver-agnostic, read from the row duals.
void report_saturated_rows(LinearInterface& li, int max_items)
{
  const auto duals = li.get_row_dual_raw();
  if (duals.empty()) {
    spdlog::info("MIP-start: no row duals available; cannot report saturation");
    return;
  }
  constexpr double tol = 1e-7;

  struct Hit
  {
    double mag {};
    double dual {};
    int row {};
  };
  std::vector<Hit> hits;
  for (std::size_t i = 0; i < duals.size(); ++i) {
    const double d = duals[i];
    if (std::abs(d) > tol) {
      hits.push_back({
          .mag = std::abs(d),
          .dual = d,
          .row = static_cast<int>(i),
      });
    }
  }
  if (hits.empty()) {
    spdlog::info("MIP-start: LP relaxation has no binding (saturated) rows");
    return;
  }
  std::ranges::sort(hits,
                    [](const Hit& a, const Hit& b) { return a.mag > b.mag; });

  const int n = std::min<int>(static_cast<int>(hits.size()), max_items);
  spdlog::info(
      "MIP-start: {} binding (saturated) rows in LP relaxation (top {}):",
      hits.size(),
      n);
  for (int k = 0; k < n; ++k) {
    const auto& hit = hits[static_cast<std::size_t>(k)];
    const auto* lbl = li.row_label_at(RowIndex {hit.row});
    if (lbl != nullptr) {
      spdlog::info("  [{}] {}:{} dual={:.6g}",
                   hit.row,
                   lbl->class_name,
                   lbl->constraint_name,
                   hit.dual);
    } else {
      spdlog::info("  [{}] row_{} dual={:.6g}", hit.row, hit.row, hit.dual);
    }
  }
}

}  // namespace

std::unique_ptr<MipStartGenerator> make_mip_start_generator(
    const MipStartMethod method)
{
  switch (method) {
    case MipStartMethod::lp_round:
      return std::make_unique<LpRoundMipStart>();
    case MipStartMethod::relax_fix:
      return std::make_unique<RelaxFixMipStart>();
    case MipStartMethod::file:
      return std::make_unique<FileMipStart>();
    case MipStartMethod::none:
      break;
  }
  return nullptr;
}

std::expected<void, Error> dump_integer_solution(const LinearInterface& li,
                                                 const std::string& path)
{
  const auto sol = li.get_col_sol_raw();
  if (sol.empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "MIP-start dump: no primal solution to dump",
    });
  }
  const int ncols = static_cast<int>(li.get_numcols());

  // Collect integer columns + their raw values (integrality must still be
  // intact at the call site — before any dual-recovery relaxation).
  std::vector<std::pair<int, double>> ints;
  ints.reserve((static_cast<std::size_t>(ncols) / 8) + 1);
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      ints.emplace_back(i, sol[static_cast<std::size_t>(i)]);
    }
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("MIP-start dump: cannot open '{}' for writing", path),
    });
  }
  out << "# gtopt mip_start integer solution (index value)\n";
  out << std::format("ncols {}\n", ncols);
  out << std::format("nint {}\n", ints.size());
  for (const auto& [idx, value] : ints) {
    // `{}` renders the shortest round-trippable representation of the double.
    out << std::format("{} {}\n", idx, value);
  }
  if (!out) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start dump: write to '{}' failed", path),
    });
  }
  spdlog::info(
      "MIP-start dump: wrote {} integer columns to '{}'", ints.size(), path);
  return {};
}

std::expected<MipStartReport, Error> apply_mip_start(
    LinearInterface& li,
    const SolverOptions& base_opts,
    const MipStartOptions& opts,
    std::span<const CommitmentRunInfo> commitments)
{
  MipStartReport report;

  const auto method = opts.method.value_or(MipStartMethod::none);
  const bool relax_check = opts.relax_check.value_or(false);
  if (method == MipStartMethod::none && !relax_check) {
    return report;  // feature off
  }

  // Snapshot the integer columns BEFORE relaxing — relaxation hides
  // integrality, so this must run first.
  std::vector<int> int_cols;
  const int ncols = static_cast<int>(li.get_numcols());
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      int_cols.push_back(i);
    }
  }
  if (int_cols.empty()) {
    return report;  // pure LP — nothing to seed or diagnose
  }
  // Restore integrality via the backend's bulk path (one CPXchgprobtype on
  // CPLEX, a per-column loop otherwise).  Done in place (no clone) — cloning
  // the full monolithic LP per scene would re-trigger the known CPXcloneprob
  // global-side-effect crash.
  const auto restore_integrality = [&] { li.restore_integers(int_cols); };

  // Relaxation solve options: base overlaid with the relax-specific overrides
  // so the relaxation can use a different algorithm / params than the MIP.
  SolverOptions relax_opts = base_opts;
  if (opts.relax_solver_options.has_value()) {
    relax_opts.overlay(*opts.relax_solver_options);
  }

  // ── Stage A: solve & analyze the LP relaxation ──────────────────────────
  li.relax_integers();
  const auto rr = li.resolve(relax_opts);
  report.relaxation_solved = true;
  const bool feasible = rr.has_value() && li.is_optimal();
  report.relaxation_feasible = feasible;
  if (feasible) {
    report.relax_obj = li.get_obj_value();
  }

  if (!feasible) {
    const auto action =
        opts.on_infeasible.value_or(RelaxInfeasibleAction::stop);
    if (action == RelaxInfeasibleAction::feasopt) {
      if (auto conflicts = li.diagnose_infeasibility()) {
        spdlog::error(
            "MIP-start: LP relaxation INFEASIBLE — {} conflicting constraints "
            "(minimal infeasible subsystem):",
            conflicts->size());
        for (const auto& c : *conflicts) {
          spdlog::error("  conflict: {}", c);
        }
      } else {
        spdlog::error(
            "MIP-start: LP relaxation INFEASIBLE — backend cannot diagnose the "
            "conflict on this solver");
      }
    }
    restore_integrality();
    if (action == RelaxInfeasibleAction::warn) {
      spdlog::warn(
          "MIP-start: LP relaxation infeasible; proceeding to the MIP solve "
          "anyway (on_infeasible=warn)");
      return report;
    }
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::string {"MIP-start: LP relaxation is infeasible "
                                "(on_infeasible="}
            + std::string {enum_name(action)} + ")",
    });
  }

  // Feasible: optional saturated-row report.
  if (opts.report_saturated.value_or(false)) {
    report_saturated_rows(li, 50);
  }

  // ── Stage B: integer-solution generation & injection ────────────────────
  if (method == MipStartMethod::none) {
    restore_integrality();  // diagnosis-only run
    return report;
  }

  auto gen = make_mip_start_generator(method);
  if (!gen) {
    spdlog::warn("MIP-start: method '{}' not available; skipping injection",
                 enum_name(method));
    restore_integrality();
    return report;
  }

  MipStartContext ctx {
      .li = li,
      .relax_opts = relax_opts,
      .int_cols = int_cols,
      .opts = opts,
      .commitments = commitments,
  };
  auto start = gen->generate(ctx);

  // Re-establish integrality BEFORE injecting: the backend MIP-start API
  // requires the problem to be a MIP (CPXaddmipstarts needs integer columns).
  restore_integrality();

  if (!start) {
    spdlog::warn("MIP-start[{}]: generator produced no start", gen->name());
    return report;
  }

  // Default to `repair`: the rounded + run-length-repaired commitment is
  // near-feasible but typically still nicks a few Pmin / user-constraint /
  // reserve rows, so CHECKFEAS would discard it; `repair` lets the solver mend
  // those residual conflicts and (empirically) lands the optimum at the root.
  const auto effort = opts.effort.value_or(MipStartEffort::repair);
  report.injected = li.set_mip_start(*start, effort);
  report.source = std::string {gen->name()};
  spdlog::info(
      "MIP-start[{}]: {} ({} integer cols, effort={}, relax_obj={:.6g})",
      gen->name(),
      report.injected ? "injected" : "backend declined",
      int_cols.size(),
      enum_name(effort),
      report.relax_obj.value_or(0.0));
  return report;
}

}  // namespace gtopt
