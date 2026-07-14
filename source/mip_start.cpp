/**
 * @file      mip_start.cpp
 * @brief     Initial-MIP-solution (warm-start) pipeline implementation
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>

#include <arrow/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/domain_rules.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/mip_start.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace detail
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

/// Stage 2 — overlay the external commitment seed in place on `start`
/// (`SeedCommitmentRule` via `make_default_domain_rules`; a no-op without a
/// `seed_solution_file`).  Commitment-feasibility REPAIR is the seed
/// producer's job (`gtopt_warmstart.build_full_seed`) — the in-tree repair
/// rules were removed 2026-07-14 (blind to the constraint families that
/// actually reject a start; see domain_rules.hpp).  Shared by the
/// round-from-relaxation generator and the seed-only (no-relaxation) path.
namespace
{
void apply_seed_overlay_impl(const MipStartContext& ctx,
                             std::vector<double>& start,
                             std::string_view generator_name)
{
  SeedCommitmentMap seed;
  if (const auto& sf = ctx.opts.seed_solution_file;
      sf.has_value() && !sf->empty())
  {
    if (auto loaded = load_seed_commitment(*sf)) {
      seed = std::move(*loaded);
    } else {
      spdlog::warn("MIP-start[{}]: seed_solution_file '{}' not loaded: {}",
                   generator_name,
                   *sf,
                   loaded.error().message);
    }
  }
  const DomainRulePipeline pipeline =
      make_default_domain_rules(std::move(seed));
  const int flipped = pipeline.apply(start,
                                     DomainRuleContext {
                                         .commitments = ctx.commitments,
                                     });
  if (flipped > 0) {
    spdlog::info(
        "MIP-start[{}]: seed set {} status values", generator_name, flipped);
  }
}

}  // namespace

std::vector<double> seed_only_start(const MipStartContext& ctx,
                                    std::string_view generator_name)
{
  const auto ncols = static_cast<std::size_t>(ctx.li.get_numcols());
  std::vector<double> start(ncols, 0.0);
  apply_seed_overlay_impl(ctx, start, generator_name);
  return start;
}

[[nodiscard]] std::vector<double> rounded_start_with_rules(
    const MipStartContext& ctx, std::string_view generator_name)
{
  auto& li = ctx.li;
  const auto sol = li.get_col_sol_raw();
  std::vector<double> start(sol.begin(), sol.end());
  if (sol.empty()) {
    return start;
  }
  // Stage 1 — generic rounding of the integer columns.
  const auto lb = li.get_col_low_raw();
  const auto ub = li.get_col_upp_raw();
  const double threshold = ctx.opts.round.threshold.value_or(0.5);
  for (const int i : ctx.int_cols) {
    const auto u = static_cast<std::size_t>(i);
    const double l = (u < lb.size()) ? lb[u] : 0.0;
    const double h = (u < ub.size()) ? ub[u] : 1.0;
    start[u] = round_with_threshold(sol[u], threshold, l, h);
  }
  // Stage 2 — overlay the external commitment seed (if any).
  apply_seed_overlay_impl(ctx, start, generator_name);
  return start;
}

}  // namespace detail

namespace
{

/// P1 generator: round the integer columns of the solved LP relaxation,
/// leaving the continuous (dispatch) columns at their relaxation value.
/// Storage-safe — operates on the whole-horizon relaxation, no time chunking.
class WarmStartGenerator final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "warmstart";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    // Stage 1 (round) + stage 2 (seed overlay); nothing else.
    auto start = detail::rounded_start_with_rules(ctx, "warmstart");
    if (start.empty()) {
      return std::nullopt;
    }
    return start;
  }
};

/// File generator: replay a solution dumped by a previous solve
/// (`dump_integer_solution`).  The dumped `<index> <value>` lines carry the
/// COMPLETE raw solution (integer AND continuous columns) found by another
/// run; every dumped column overlays the start, so replaying a complete
/// optimal dump reconstructs the exact optimum — a CONSISTENT start that a
/// strict backend feasibility check (CPLEX CHECKFEAS) accepts.  Legacy
/// integer-only dumps still work: columns absent from the file keep this
/// solver's own LP-relaxation primal when one is available (the pre-2026-07
/// behaviour), and a zero base otherwise.  Overlaying optimal integers onto
/// the RELAXATION continuous produced a mutually inconsistent start that
/// CPLEX rejected with "No solution found from 1 MIP starts" even when the
/// integers were the exact optimum — the CEN UC round-trip bug.  Both runs
/// build the identical deterministic flat LP, so raw column indices match
/// 1:1 — validated by the `ncols` header (a mismatched dump is refused).
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
    const auto& path = ctx.opts.from_file;
    if (!path.has_value() || path->empty()) {
      spdlog::warn(
          "MIP-start[file]: no mip_start.from_file configured; nothing to "
          "replay");
      return std::nullopt;
    }
    std::ifstream in(*path);
    if (!in) {
      spdlog::error("MIP-start[file]: cannot open '{}' for reading", *path);
      return std::nullopt;
    }

    const auto ncols = static_cast<std::size_t>(ctx.li.get_numcols());
    const std::unordered_set<int> int_set(ctx.int_cols.begin(),
                                          ctx.int_cols.end());

    // Base for columns the dump does not cover (legacy integer-only dumps):
    // the destination's own relaxation primal when available, zeros
    // otherwise.  MUST NOT bail on an empty primal — the dump itself is the
    // start; bailing here silently dropped the file replay (and the pipeline
    // fell back to the round candidate).  A complete dump overlays every
    // column, so the base then only pads structural gaps.
    const auto sol = ctx.li.get_col_sol_raw();
    std::vector<double> start;
    if (sol.size() == ncols) {
      start.assign(sol.begin(), sol.end());
    } else {
      if (!sol.empty()) {
        spdlog::warn(
            "MIP-start[file]: relaxation primal size {} != ncols {}; using a "
            "zero base under the dumped values",
            sol.size(),
            ncols);
      }
      start.assign(ncols, 0.0);
    }

    std::size_t file_ncols = 0;
    std::size_t placed_int = 0;
    std::size_t placed_cont = 0;
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
      in >> value;
      if (in.fail()) {
        break;
      }
      const auto u = static_cast<std::size_t>(idx);
      if (idx < 0 || u >= ncols) {
        ++skipped;
        continue;
      }
      start[u] = value;
      if (int_set.contains(idx)) {
        ++placed_int;
      } else {
        ++placed_cont;
      }
    }

    if (placed_int == 0) {
      spdlog::error(
          "MIP-start[file]: dump '{}' yielded no usable integer values "
          "(continuous placed={}, out-of-range skipped={})",
          *path,
          placed_cont,
          skipped);
      return std::nullopt;
    }
    spdlog::info(
        "MIP-start[file]: replayed {} integer + {} continuous values from "
        "'{}' ({} skipped, {} integer cols in this LP)",
        placed_int,
        placed_cont,
        *path,
        skipped,
        ctx.int_cols.size());
    return start;
  }
};

// ── Elastic in-process seed completion (`mip_start.elastic`) ────────────────
//
// The general fix so warm-starts NEVER depend on a perfectly feasible seed:
// real seeds keep failing constraint families that are NOT derivable offline
// (hydro water coupling, network evacuation limits), the backends' own repair
// effort never engages at scale, and the producer-side repair
// (scripts/gtopt_warmstart) hit its ceiling.  gtopt repairs in-process, where
// it has the FULL LP.  Precondition: integers relaxed and the Stage-A
// relaxation solved OPTIMAL.  Mechanism (documented trade-offs in
// `MipStartOptions::elastic`):
//
//   (a) solve ONE elastic-bias LP directly — each seeded binary's objective
//       coefficient is shifted by ∓M (−M pulls toward seed=1, +M toward
//       seed=0; M = 1e4 × max|c|, O(1) memory, restored right after).  The
//       bias LP shares the plain relaxation's feasible region, so it is
//       feasible whenever Stage A was.  A FEASIBLE seed just comes out
//       unchanged (M dominates, so every seeded column lands at its seed
//       value ⇒ deviation = 0) — the former dedicated u-fixed probe LP was a
//       full-size throwaway on every infeasible seed and is GONE (measured
//       5–20 min/LP on CEN-size models).  Threshold u* at 0.5 → the
//       completed pattern.  The bias LP defaults to barrier with crossover
//       DISABLED (interior primal is all the thresholding needs; crossover
//       is pure overhead here) unless the user pinned an algorithm/crossover
//       via `relax.solver_options`.  BIAS CAVEATS: the repair is soft — it
//       maximises seed agreement subject to LP feasibility and residual cost
//       trade-offs below M; deviation = 0 detection is likewise soft (a
//       cost saving beyond M could in principle flip a feasible seed); and M
//       widens the objective range by ~4 orders of magnitude for that one
//       LP, which can degrade numerics on badly scaled models.
//   (b) fix every integer column at u* via bounds and re-solve for a
//       consistent dispatch (this final re-fix is KEPT — it guarantees the
//       injected start is self-consistent).  It runs on the SAME
//       LinearInterface, so a basis-keeping backend (CPLEX) re-solves warm
//       from the resident Stage-A state: bound fixing preserves dual
//       feasibility, making the re-fix a handful of dual-simplex pivots
//       rather than a cold LP.
//   (c) the returned start is COMPLETE and self-consistent (integral u* +
//       the u-fixed LP's continuous dispatch): a genuinely MIP-feasible
//       point, so the caller injects it under `check_feasibility` — the
//       accepted-by-contract path (see test_mip_start_roundtrip.cpp).
//
// Net effort: exactly TWO internal LP solves (bias + re-fix), of which only
// the bias LP is a full solve — ~1 effective LP vs the former worst case of
// three (u-fixed probe → bias → re-fix).  Every exit path restores the
// original column bounds and objective coefficients EXACTLY (asserted); on
// failure the plain relaxation is re-solved so the standard Stage-B fallback
// still sees a valid primal.

/// A seeded integer column: raw column index + target value (integral).
struct ElasticSeed
{
  int col {};
  double value {};
};

/// Outcome of `elastic_seed_completion`: the complete start to inject plus
/// the report fields (`elastic_repaired`, `seed_deviation`,
/// `elastic_lp_solves`).
struct ElasticOutcome
{
  std::vector<double> start {};
  bool repaired {false};
  int seed_deviation {0};
  int lp_solves {0};
};

/// Collect the (column, value) seed pattern the elastic completion pulls
/// toward.  `from_file` (a complete dump) seeds EVERY integer column with the
/// dump's value; otherwise `seed_solution_file` seeds the commitment status
/// columns matched by (generator, block) identity — the same matching as
/// `SeedCommitmentRule`.  Empty ⇒ nothing to complete (caller falls back).
[[nodiscard]] std::vector<ElasticSeed> collect_elastic_seed(
    MipStartContext& ctx)
{
  std::vector<ElasticSeed> seed;
  const auto& opts = ctx.opts;
  if (opts.from_file.has_value() && !opts.from_file->empty()) {
    auto gen = make_mip_start_generator(opts);  // → FileMipStart
    if (const auto start = gen->generate(ctx); start.has_value()) {
      const auto& vals = *start;
      seed.reserve(ctx.int_cols.size());
      for (const int i : ctx.int_cols) {
        const double v = vals[static_cast<std::size_t>(i)];
        seed.push_back({.col = i, .value = std::round(v)});
      }
    }
    return seed;
  }
  const auto& sf = opts.seed_solution_file;
  if (!sf.has_value() || sf->empty()) {
    return seed;
  }
  auto loaded = load_seed_commitment(*sf);
  if (!loaded) {
    spdlog::warn("MIP-start[elastic]: seed_solution_file '{}' not loaded: {}",
                 *sf,
                 loaded.error().message);
    return seed;
  }
  const auto& map = *loaded;
  for (const auto& c : ctx.commitments) {
    if (c.uid == unknown_uid) {
      continue;  // no generator identity → cannot match a semantic seed
    }
    const std::size_t n = std::min(c.status_cols.size(), c.block_uids.size());
    for (std::size_t t = 0; t < n; ++t) {
      const auto it = map.find(seed_commitment_key(c.uid, c.block_uids[t]));
      if (it == map.end()) {
        continue;
      }
      seed.push_back({
          .col = c.status_cols[t],
          .value = (it->second >= 0.5) ? 1.0 : 0.0,
      });
    }
  }
  return seed;
}

/// Run the elastic completion (see the block comment above).  Returns the
/// complete start on success, or `std::nullopt` when there is nothing to
/// complete / the repair could not reach a u-fixed-feasible pattern — the
/// caller then warns and falls back to the standard Stage-B candidate
/// (never aborting the solve).
[[nodiscard]] std::optional<ElasticOutcome> elastic_seed_completion(
    MipStartContext& ctx)
{
  auto& li = ctx.li;
  const auto seed = collect_elastic_seed(ctx);
  if (seed.empty()) {
    spdlog::warn(
        "MIP-start[elastic]: no seed values matched this LP; skipping the "
        "elastic completion");
    return std::nullopt;
  }

  const auto ncols = static_cast<std::size_t>(li.get_numcols());
  // COPIES, not spans: these are the restore targets and the assert oracle
  // (the spans alias live backend memory that the loops below mutate).
  const auto lb_span = li.get_col_low_raw();
  const auto ub_span = li.get_col_upp_raw();
  const auto obj_span = li.get_obj_coeff();
  const std::vector<double> lb0(lb_span.begin(), lb_span.end());
  const std::vector<double> ub0(ub_span.begin(), ub_span.end());
  const std::vector<double> obj0(obj_span.begin(), obj_span.end());
  const auto relax_span = li.get_col_sol_raw();
  const std::vector<double> relax_sol(relax_span.begin(), relax_span.end());
  if (relax_sol.size() != ncols || lb0.size() != ncols || ub0.size() != ncols) {
    spdlog::warn(
        "MIP-start[elastic]: relaxation primal/bounds unavailable; skipping "
        "the elastic completion");
    return std::nullopt;
  }

  // Target integer pattern: seed value where seeded (clamped to the column's
  // own bounds), rounded relaxation elsewhere.
  const double threshold = ctx.opts.round.threshold.value_or(0.5);
  std::vector<double> u_fix(ncols, 0.0);
  std::vector<char> is_seeded(ncols, 0);
  for (const int i : ctx.int_cols) {
    const auto u = static_cast<std::size_t>(i);
    u_fix[u] =
        detail::round_with_threshold(relax_sol[u], threshold, lb0[u], ub0[u]);
  }
  for (const auto& s : seed) {
    const auto u = static_cast<std::size_t>(s.col);
    u_fix[u] = std::clamp(s.value, lb0[u], ub0[u]);
    is_seeded[u] = 1;
  }

  const auto fix_int_cols = [&li, &ctx](const std::vector<double>& vals)
  {
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      li.set_col_low_raw(ColIndex {i}, vals[u]);
      li.set_col_upp_raw(ColIndex {i}, vals[u]);
    }
  };
  const auto unfix_int_cols = [&li, &ctx, &lb0, &ub0]
  {
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      li.set_col_low_raw(ColIndex {i}, lb0[u]);
      li.set_col_upp_raw(ColIndex {i}, ub0[u]);
    }
  };
  int lp_solves = 0;
  const auto solve_ok = [&li, &lp_solves](const SolverOptions& opts)
  {
    ++lp_solves;
    const auto rr = li.resolve(opts);
    return rr.has_value() && li.is_optimal();
  };
  // Internal elastic solve options.  Both LPs are COUSINS of the Stage-A
  // relaxation the pipeline just solved, so both WARM-START off Stage A's
  // basis on the same LinearInterface rather than discarding it for a cold
  // barrier (the measured slow path).  The warm METHOD is dictated by which
  // feasibility the transition preserves — NOT a free choice:
  //  - bias LP shifts the OBJECTIVE.  Stage A's optimal basis stays
  //    primal-feasible but becomes dual-INfeasible for the new objective,
  //    so only PRIMAL simplex can warm-start it.  (Forcing dual is futile:
  //    CPLEX detects the dual-infeasible start and falls back to primal
  //    anyway — verified in the CPLEX log as "Starting dual … Starting
  //    primal … Primal simplex solved" — at the cost of a wasted dual pass.)
  //  - re-fix LP fixes u's BOUNDS.  The bias basis stays dual-feasible, so
  //    DUAL simplex warm-starts it exactly.
  // Crossover is moot (simplex yields a basis natively).  Overridable via
  // `relax.solver_options` (sentinels `automatic`/`default_algo` = "unset").
  SolverOptions elastic_opts = ctx.relax_opts;
  if (elastic_opts.crossover == CrossoverMode::automatic) {
    elastic_opts.crossover = CrossoverMode::none;
  }
  if (elastic_opts.algorithm == LPAlgo::default_algo) {
    elastic_opts.algorithm = LPAlgo::primal;  // objective change → primal warm
  }
  SolverOptions refix_opts = ctx.relax_opts;
  if (refix_opts.crossover == CrossoverMode::automatic) {
    refix_opts.crossover = CrossoverMode::none;
  }
  if (refix_opts.algorithm == LPAlgo::default_algo) {
    refix_opts.algorithm = LPAlgo::dual;  // bound change → dual warm
  }
  // The complete start: the just-solved u-fixed LP's primal with the integer
  // columns snapped EXACTLY onto the fixed pattern (the LP reports them at
  // the pinned bounds up to tolerance; the backend integrality check wants
  // them integral).
  const auto snap_start = [&li, &ctx](const std::vector<double>& vals)
  {
    const auto sol = li.get_col_sol_raw();
    std::vector<double> start(sol.begin(), sol.end());
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      start[u] = vals[u];
    }
    return start;
  };
  // Restoration oracle — bounds and objective must round-trip EXACTLY.
  // (Default capture: the explicit list would trip -Wunused-lambda-capture
  // under NDEBUG, where assert() compiles the body away.)
  const auto assert_restored = [&]() noexcept
  {
    assert(std::ranges::equal(li.get_col_low_raw(), lb0));
    assert(std::ranges::equal(li.get_col_upp_raw(), ub0));
    assert(std::ranges::equal(li.get_obj_coeff(), obj0));
  };

  // ── (a) elastic-bias LP: pull u toward the seed, feasibility decides ────
  // Straight to the bias LP — no u-fixed probe first: a feasible seed comes
  // out of the bias solve unchanged (deviation = 0), so the former probe LP
  // only ever confirmed what the bias solution already tells us, at the
  // price of one full-size LP solve.
  spdlog::info(
      "MIP-start[elastic]: solving the elastic-bias LP over {} seeded "
      "columns (single-LP completion; no u-fixed probe)",
      seed.size());
  double max_abs_obj = 0.0;
  for (const double c : obj0) {
    max_abs_obj = std::max(max_abs_obj, std::abs(c));
  }
  const double big_m = 1e4 * std::max(1.0, max_abs_obj);
  for (const auto& s : seed) {
    const auto u = static_cast<std::size_t>(s.col);
    const double bias = (u_fix[u] >= 0.5) ? -big_m : big_m;
    li.set_obj_coeff_raw(ColIndex {s.col}, obj0[u] + bias);
  }
  const bool bias_ok = solve_ok(elastic_opts);
  std::vector<double> u_star = u_fix;
  int deviation = 0;
  if (bias_ok) {
    const auto esol = li.get_col_sol_raw();
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      u_star[u] = detail::round_with_threshold(esol[u], 0.5, lb0[u], ub0[u]);
      if (is_seeded[u] != 0 && ((u_star[u] >= 0.5) != (u_fix[u] >= 0.5))) {
        ++deviation;
      }
    }
  }
  // Restore the objective BEFORE the final dispatch solve so the completed
  // start carries the TRUE dispatch (and the MIP solve sees the true costs).
  for (const auto& s : seed) {
    li.set_obj_coeff_raw(ColIndex {s.col},
                         obj0[static_cast<std::size_t>(s.col)]);
  }

  if (!bias_ok) {
    // The bias LP shares Stage A's feasible region, so this is a numerical
    // failure (M too large for this model's scaling) — restore + fall back.
    spdlog::warn(
        "MIP-start[elastic]: elastic-bias LP failed to solve (M={:.3g}); "
        "falling back",
        big_m);
    (void)solve_ok(ctx.relax_opts);  // best effort: re-establish the plain
                                     // relaxation
    assert_restored();
    return std::nullopt;
  }

  // ── (b) final u-fixed dispatch LP at the completed pattern ──────────────
  // Same LinearInterface, plain relax opts: a basis-keeping backend (CPLEX)
  // still holds the Stage-A relaxation basis, and bound fixing keeps it
  // dual-feasible — so this re-fix is a warm dual-simplex re-solve, not a
  // second full LP.
  fix_int_cols(u_star);
  if (solve_ok(refix_opts)) {
    const double fixed_obj = li.get_obj_value();
    ElasticOutcome out {
        .start = snap_start(u_star),
        .repaired = deviation > 0,
        .seed_deviation = deviation,
        .lp_solves = lp_solves,
    };
    unfix_int_cols();
    assert_restored();
    if (deviation == 0) {
      spdlog::info(
          "MIP-start[elastic]: seed survived the bias LP unchanged and is "
          "u-fixed feasible (obj={:.6g}, {} LP solves); no repair needed",
          fixed_obj,
          lp_solves);
    } else {
      spdlog::info(
          "MIP-start[elastic]: repaired pattern is u-fixed feasible "
          "(obj={:.6g}, {} of {} seeded columns flipped, {} LP solves)",
          fixed_obj,
          deviation,
          seed.size(),
          lp_solves);
    }
    return out;
  }

  // STILL infeasible: the 0.5 threshold landed on a knife edge the bias LP
  // could not disambiguate.  Restore everything, re-establish the plain
  // relaxation primal, and let the caller fall back — never abort the solve.
  unfix_int_cols();
  spdlog::warn(
      "MIP-start[elastic]: completed pattern is u-fixed infeasible "
      "({} of {} seeded columns flipped); falling back to the un-fixed "
      "relaxation candidate",
      deviation,
      seed.size());
  (void)solve_ok(ctx.relax_opts);  // best effort: re-establish the plain
                                   // relaxation
  assert_restored();
  return std::nullopt;
}

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

/// Opt-in `mip_start.branch_priorities`: derive per-integer-column branching
/// priorities from DECISIVENESS and hand them to the backend —
/// `priority = round(100 × |2·frac − 1|)`, with `frac` the column's
/// fractional part in `frac_source` (the Stage-A relaxation primal when one
/// was solved; the injected seed/candidate on the `skip_relaxation` path).
/// A column the source already pins at an integer (frac ≈ 0 ⇒ priority 100)
/// is DECISIVE and branched first, deferring the genuinely fractional
/// columns deeper into the tree.  Returns the number of columns the backend
/// accepted (0 = no priority-order support — a benign skip).
///
/// WARN on success by design: per IBM, supplying a priority order
/// (`CPXcopyorder`) switches CPLEX from dynamic search to traditional
/// branch-and-cut — the reason `branch_priorities` is strictly opt-in.
[[nodiscard]] int seed_branch_priorities(LinearInterface& li,
                                         std::span<const int> int_cols,
                                         std::span<const double> frac_source)
{
  std::vector<int> cols;
  std::vector<int> prios;
  cols.reserve(int_cols.size());
  prios.reserve(int_cols.size());
  for (const int i : int_cols) {
    const auto u = static_cast<std::size_t>(i);
    if (u >= frac_source.size()) {
      continue;
    }
    // Fractional part generalises the binary |2v−1| decisiveness to general
    // integer columns (an integral value ⇒ frac 0 ⇒ priority 100; the
    // knife-edge frac 0.5 ⇒ priority 0).
    const double v = frac_source[u];
    const double frac = v - std::floor(v);
    cols.push_back(i);
    prios.push_back(
        static_cast<int>(std::lround(100.0 * std::abs((2.0 * frac) - 1.0))));
  }
  if (cols.empty() || !li.set_branch_priorities(cols, prios)) {
    spdlog::info(
        "MIP-start: branch priorities requested but the backend declined "
        "(no priority-order support); proceeding without");
    return 0;
  }
  spdlog::info("gtopt branch priorities: {} cols", cols.size());
  spdlog::warn(
      "MIP-start: branch priority order installed on {} integer columns — "
      "on CPLEX this disables dynamic search (traditional branch-and-cut); "
      "mip_start.branch_priorities is opt-in for exactly this reason",
      cols.size());
  return static_cast<int>(cols.size());
}

}  // namespace

std::unique_ptr<MipStartGenerator> make_mip_start_generator(
    const MipStartOptions& opts)
{
  // `from_file` set → replay a dumped integer solution; otherwise the default
  // round + domain-rules candidate.
  if (opts.from_file.has_value() && !opts.from_file->empty()) {
    return std::make_unique<FileMipStart>();
  }
  return std::make_unique<WarmStartGenerator>();
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

  // Count the integer columns for the informational `nint` header
  // (integrality must still be intact at the call site — before any
  // dual-recovery relaxation).
  std::size_t nint = 0;
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      ++nint;
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
  // The dump carries the COMPLETE solution — every column, integer AND
  // continuous — so a replay (`FileMipStart`) reconstructs a CONSISTENT
  // start.  Dumping only the integers forced the replay to complete the
  // start with the RELAXATION continuous, a mutually inconsistent vector
  // that strict backend feasibility checks (CPLEX CHECKFEAS) reject even
  // when the integers are the exact optimum.
  out << "# gtopt mip_start complete solution (index value)\n";
  out << std::format("ncols {}\n", ncols);
  out << std::format("nint {}\n", nint);
  for (int i = 0; i < ncols; ++i) {
    // `{}` renders the shortest round-trippable representation of the double.
    out << std::format("{} {}\n", i, sol[static_cast<std::size_t>(i)]);
  }
  if (!out) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start dump: write to '{}' failed", path),
    });
  }
  spdlog::info("MIP-start dump: wrote {} columns ({} integer) to '{}'",
               ncols,
               nint,
               path);
  return {};
}

std::expected<SeedCommitmentMap, Error> load_seed_commitment(
    const std::string& path)
{
  // Reuse the Arrow CSV reader — the same path boundary_cuts.csv takes
  // (`sddp_cut_io.cpp`) — so header detection, CRLF, quoting, blank-line
  // skipping and type inference are handled by a battle-tested reader instead
  // of a bespoke parser.  Required header + columns: `generator_uid` (int),
  // `block_uid` (int), `u` (0/1 status).
  auto maybe_infile = arrow::io::ReadableFile::Open(path);
  if (!maybe_infile.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: cannot open '{}': {}",
                               path,
                               maybe_infile.status().ToString()),
    });
  }

  auto read_options = arrow::csv::ReadOptions::Defaults();
  auto parse_options = arrow::csv::ParseOptions::Defaults();
  auto convert_options = arrow::csv::ConvertOptions::Defaults();
  convert_options.column_types["generator_uid"] = arrow::int32();
  convert_options.column_types["block_uid"] = arrow::int32();
  convert_options.column_types["u"] = arrow::float64();
  convert_options.include_missing_columns = false;

  auto maybe_reader =
      arrow::csv::TableReader::Make(arrow::io::default_io_context(),
                                    *maybe_infile,
                                    read_options,
                                    parse_options,
                                    convert_options);
  if (!maybe_reader.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: unreadable CSV '{}': {}",
                               path,
                               maybe_reader.status().ToString()),
    });
  }
  const auto& table_reader = *maybe_reader;
  auto maybe_table = table_reader->Read();
  if (!maybe_table.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: parse failed on '{}': {}",
                               path,
                               maybe_table.status().ToString()),
    });
  }
  // Combine chunks so the three columns are single, row-aligned arrays.
  const auto& raw_table = *maybe_table;
  auto maybe_combined = raw_table->CombineChunks();
  if (!maybe_combined.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: combine failed on '{}': {}",
                               path,
                               maybe_combined.status().ToString()),
    });
  }
  const auto& table = *maybe_combined;

  const auto gcol = table->GetColumnByName("generator_uid");
  const auto bcol = table->GetColumnByName("block_uid");
  const auto ucol = table->GetColumnByName("u");
  if (gcol == nullptr || bcol == nullptr || ucol == nullptr) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "MIP-start seed '{}': need columns generator_uid, block_uid, u",
            path),
    });
  }

  SeedCommitmentMap seed;
  seed.reserve(static_cast<std::size_t>(table->num_rows()));
  for (int ci = 0; ci < gcol->num_chunks(); ++ci) {
    const auto ga =
        std::dynamic_pointer_cast<arrow::Int32Array>(gcol->chunk(ci));
    const auto ba =
        std::dynamic_pointer_cast<arrow::Int32Array>(bcol->chunk(ci));
    const auto ua =
        std::dynamic_pointer_cast<arrow::DoubleArray>(ucol->chunk(ci));
    if (!ga || !ba || !ua) {
      continue;
    }
    for (int64_t i = 0; i < ga->length(); ++i) {
      if (ga->IsValid(i) && ba->IsValid(i) && ua->IsValid(i)) {
        seed[seed_commitment_key(ga->Value(i), ba->Value(i))] = ua->Value(i);
      }
    }
  }
  spdlog::info("MIP-start seed: loaded {} (generator,block) statuses from '{}'",
               seed.size(),
               path);
  return seed;
}

std::expected<MipStartReport, Error> apply_mip_start(
    LinearInterface& li,
    const SolverOptions& base_opts,
    const MipStartOptions& opts,
    std::span<const CommitmentRunInfo> commitments,
    const FlatLinearProblem* flat_lp)
{
  MipStartReport report;

  const bool enabled = opts.enabled.value_or(false);
  const bool relax_check = opts.relax.check.value_or(false);
  if (!enabled && !relax_check) {
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

  // ── Fast path: seed / file replay, no throwaway relaxation ──────────────
  // When `skip_relaxation` is set and either a seed CSV fully specifies the
  // integer commitment or a dumped solution is being replayed (`from_file`),
  // skip Stage 0 entirely: the relaxation's only surviving output is the
  // rounded integers, which the seed/dump overrides anyway — so solving it
  // is a throwaway LP the MIP root then repeats.  Build the start directly
  // and inject; a complete dump already carries a consistent continuous
  // dispatch, and a seed-only start lets the backend complete it as its
  // single warm root LP (effort defaults to `solve_fixed` → dual simplex off
  // the fixed integers, no crossover).
  const bool has_from_file =
      opts.from_file.has_value() && !opts.from_file->empty();
  const bool has_seed_file =
      opts.seed_solution_file.has_value() && !opts.seed_solution_file->empty();
  const bool elastic =
      opts.elastic.value_or(false) && (has_from_file || has_seed_file);
  if (elastic && opts.skip_relaxation.value_or(false)) {
    spdlog::info(
        "MIP-start[elastic]: elastic completion needs the relaxed LP — "
        "skip_relaxation is overridden");
  }
  if (enabled && !elastic && opts.skip_relaxation.value_or(false)
      && (has_from_file || has_seed_file))
  {
    MipStartContext ctx {
        .li = li,
        .relax_opts = base_opts,
        .int_cols = int_cols,
        .opts = opts,
        .commitments = commitments,
        .flat_lp = flat_lp,
    };
    std::optional<std::vector<double>> start;
    std::string source;
    if (has_from_file) {
      auto gen = make_mip_start_generator(opts);  // → FileMipStart
      start = gen->generate(ctx);
      source = std::string {gen->name()};
    } else {
      start = detail::seed_only_start(ctx, "seed");
      source = "seed";
    }
    if (!start.has_value()) {
      spdlog::warn(
          "MIP-start[{}]: produced no start under skip_relaxation; skipping "
          "injection",
          source);
      return report;
    }
    const auto effort =
        opts.inject.effort.value_or(MipStartEffort::solve_fixed);
    report.injected = li.set_mip_start(*start, effort);
    report.source = source;
    if (opts.branch_priorities.value_or(false)) {
      // No relaxation on this path: decisiveness comes from the seed/dump
      // itself (integral values ⇒ every seeded column is fully decisive).
      report.branch_priorities_cols =
          seed_branch_priorities(li, int_cols, *start);
    }
    spdlog::info(
        "MIP-start[{}]: {} ({} integer cols, effort={}, skip_relaxation — "
        "no relaxation solve)",
        source,
        report.injected ? "injected" : "backend declined",
        int_cols.size(),
        enum_name(effort));
    return report;
  }

  // Relaxation solve options: base overlaid with the relax-specific overrides
  // so the relaxation can use a different algorithm / params than the MIP.
  SolverOptions relax_opts = base_opts;
  if (opts.relax.solver_options.has_value()) {
    relax_opts.overlay(*opts.relax.solver_options);
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
        opts.relax.on_infeasible.value_or(RelaxInfeasibleAction::stop);
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
  if (opts.relax.report_saturated.value_or(false)) {
    report_saturated_rows(li, 50);
  }

  // ── Stage B: integer-solution generation & injection ────────────────────
  if (!enabled) {
    restore_integrality();  // diagnosis-only run (relax.check)
    return report;
  }

  // Snapshot the Stage-A relaxation primal for the opt-in branch-priority
  // derivation BEFORE any elastic/candidate re-solve overwrites the live
  // solution (only copied when the option is actually on).
  std::vector<double> relax_frac;
  if (opts.branch_priorities.value_or(false)) {
    const auto s = li.get_col_sol_raw();
    relax_frac.assign(s.begin(), s.end());
  }

  MipStartContext ctx {
      .li = li,
      .relax_opts = relax_opts,
      .int_cols = int_cols,
      .opts = opts,
      .commitments = commitments,
      .flat_lp = flat_lp,
  };

  // ── Elastic in-process seed completion (`mip_start.elastic`) ────────────
  // Repair the seed against the FULL LP (one elastic-bias LP → threshold →
  // one u-fixed re-fix; see `elastic_seed_completion`) and inject the
  // resulting COMPLETE, genuinely MIP-feasible start under
  // `check_feasibility` — the accepted-by-contract path.  On failure the
  // pipeline WARNS and falls through to the standard Stage-B candidate
  // below: elastic never aborts the solve.
  if (elastic) {
    if (auto out = elastic_seed_completion(ctx)) {
      restore_integrality();
      const auto effort =
          opts.inject.effort.value_or(MipStartEffort::check_feasibility);
      report.injected = li.set_mip_start(out->start, effort);
      report.source = has_from_file ? "elastic+file" : "elastic+seed";
      report.elastic_repaired = out->repaired;
      report.seed_deviation = out->seed_deviation;
      report.elastic_lp_solves = out->lp_solves;
      if (opts.branch_priorities.value_or(false)) {
        report.branch_priorities_cols =
            seed_branch_priorities(li, int_cols, relax_frac);
      }
      spdlog::info(
          "MIP-start[{}]: {} ({} integer cols, effort={}, repaired={}, "
          "seed_deviation={}, lp_solves={}, relax_obj={:.6g})",
          report.source,
          report.injected ? "injected" : "backend declined",
          int_cols.size(),
          enum_name(effort),
          report.elastic_repaired,
          report.seed_deviation,
          report.elastic_lp_solves,
          report.relax_obj.value_or(0.0));
      return report;
    }
    spdlog::warn(
        "MIP-start[elastic]: completion produced no start; falling back to "
        "the standard round+seed candidate");
  }

  auto gen = make_mip_start_generator(opts);
  if (!gen) {
    spdlog::warn("MIP-start: no generator available; skipping injection");
    restore_integrality();
    return report;
  }
  auto start = gen->generate(ctx);  // stage 1 (round) + stage 2 (seed)
  std::string source {gen->name()};

  // Stage 4 (optional): SCIP repair.  Composable with any base candidate and
  // any active solver — turns the round+rules candidate into a genuinely
  // feasible integer solution via SCIP's completesol/repair.  On failure (no
  // SCIP plugin / no flat LP / nothing feasible) keep the pre-SCIP candidate.
  if (start && opts.scip_repair.enabled.value_or(false)) {
    if (auto repaired = scip_repair_candidate(ctx, *start)) {
      start = std::move(repaired);
      source += "+scip";
    } else {
      spdlog::warn(
          "MIP-start[scip]: repair stage produced no result; keeping the "
          "round+rules candidate from '{}'",
          gen->name());
    }
  }

  // Re-establish integrality BEFORE injecting: the backend MIP-start API
  // requires the problem to be a MIP (CPXaddmipstarts needs integer columns).
  restore_integrality();

  if (!start) {
    spdlog::warn("MIP-start[{}]: produced no start", source);
    return report;
  }

  // Stage 5 — inject with `effort` ("solver repair").  Default `repair`: the
  // candidate is near-feasible but typically still nicks a few Pmin /
  // user-constraint / reserve rows, so CHECKFEAS would discard it; `repair`
  // lets the active backend mend those residuals (and on CPLEX this composes
  // with the SCIP stage as structural-then-numerical repair).  SystemLP then
  // runs the MIP solve (the "resolve").
  const auto effort = opts.inject.effort.value_or(MipStartEffort::repair);
  report.injected = li.set_mip_start(*start, effort);
  report.source = source;
  if (opts.branch_priorities.value_or(false)) {
    report.branch_priorities_cols =
        seed_branch_priorities(li, int_cols, relax_frac);
  }
  spdlog::info(
      "MIP-start[{}]: {} ({} integer cols, effort={}, relax_obj={:.6g})",
      source,
      report.injected ? "injected" : "backend declined",
      int_cols.size(),
      enum_name(effort),
      report.relax_obj.value_or(0.0));
  return report;
}

}  // namespace gtopt
