/**
 * @file      sddp_enums.hpp
 * @brief     Named enum types for SDDP and boundary-cut options
 * @date      2026-03-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by SddpOptions and MonolithicOptions
 * (boundary cuts).  Extracted from sddp_options.hpp so that the
 * struct definition stays focused on the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── BoundaryCutsMode ───────────────────────────────────────────────────────

/**
 * @brief How boundary (future-cost) cuts are loaded for the last phase.
 *
 * - `noload`:    Do not load boundary cuts even if a file is given.
 * - `separated`: Load cuts per scene (scene UID matching; default).
 * - `combined`:  Broadcast all cuts into all scenes.
 */
enum class BoundaryCutsMode : uint8_t
{
  noload = 0,  ///< Skip loading boundary cuts
  separated = 1,  ///< Per-scene cut assignment (default)
  combined = 2,  ///< Broadcast all cuts to all scenes
};

inline constexpr auto boundary_cuts_mode_entries =
    std::to_array<EnumEntry<BoundaryCutsMode>>({
        {.name = "noload", .value = BoundaryCutsMode::noload},
        {.name = "separated", .value = BoundaryCutsMode::separated},
        {.name = "combined", .value = BoundaryCutsMode::combined},
    });

[[nodiscard]] constexpr auto enum_entries(BoundaryCutsMode /*tag*/) noexcept
{
  return std::span {boundary_cuts_mode_entries};
}

// ─── CutSharingMode ────────────────────────────────────────────────────────

/**
 * @brief SDDP cut sharing mode across scenes in the backward pass.
 *
 * When a sharing mode other than `none` is selected, the backward pass is
 * synchronized per-phase: all scenes complete a phase before cuts are shared
 * and the next phase is processed.
 *
 * @warning Only `none` is mathematically valid for production multi-scenario
 * runs.  gtopt implements multi-cut SDDP (one α^k_s column per scene).  The
 * `expected` / `accumulate` / `max` modes broadcast a cut from scene S to
 * every other scene's α LP, which is valid only when scenes share the
 * IDENTICAL sample-path realization (same inflows, demands, capacities at
 * every (phase, block)).  Distinct-sample-path runs (the typical case for
 * Monte Carlo SDDP, e.g. juan/gtopt_iplp with 16 historical hydrology
 * samples) violate that condition and produce LB > UB that compounds
 * across iterations.  A runtime WARN is emitted at SDDP setup when
 * `cut_sharing != none && num_scenes > 1`.  See
 * `docs/analysis/investigations/sddp/sddp_cut_sharing_fix_plan_2026-04-30.md`
 * and the regression test `test/source/test_sddp_bounds_sanity.cpp`.
 */
enum class CutSharingMode : uint8_t
{
  none = 0,  ///< No sharing; scenes solved independently (default; only
             ///< mathematically valid choice for production multi-scenario)
  expected = 1,  ///< Average cuts within each scene, then sum across scenes
                 ///< (KNOWN INVALID for distinct sample paths — see warning)
  accumulate = 2,  ///< Sum all cuts directly (KNOWN INVALID for distinct
                   ///< sample paths — see warning)
  max = 3,  ///< All cuts from all scenes added to all scenes (KNOWN INVALID
            ///< for distinct sample paths — see warning)
};

inline constexpr auto cut_sharing_mode_entries =
    std::to_array<EnumEntry<CutSharingMode>>({
        {.name = "none", .value = CutSharingMode::none},
        {.name = "expected", .value = CutSharingMode::expected},
        {.name = "accumulate", .value = CutSharingMode::accumulate},
        {.name = "max", .value = CutSharingMode::max},
    });

[[nodiscard]] constexpr auto enum_entries(CutSharingMode /*tag*/) noexcept
{
  return std::span {cut_sharing_mode_entries};
}

// ─── CutDrainMode ──────────────────────────────────────────────────────────

/**
 * @brief How the SDDP async path drains in-flight cuts after the
 * aggregate-convergence signal fires.
 *
 * When `m_stop_requested_` flips in `SDDPMethod::solve_async` (because the
 * aggregate tracker certified an iter as converged), some scenes can still
 * have an in-flight forward+backward pass for the NEXT iter — under
 * `max_async_spread > 0`, a fast scene may be up to `max_async_spread`
 * iters ahead of the slowest one.  Cuts added by those still-running
 * tasks AFTER the stop signal are non-deterministic (their inclusion
 * depends on pool drain timing); we must drop them so the cut count
 * handed off to the next cascade level is reproducible.
 *
 * Three drain strategies are available:
 *
 *  - `count` — snapshot each scene's cut count at the boundary
 *    (per-scene, recorded the moment `m_stop_requested_` flips).
 *    Any cuts pushed past that snapshot get truncated.  Cuts already in
 *    the store at the boundary (including a fast scene's iter-(N+1)
 *    head-start cuts) are kept.  Asymmetric across scenes: faster
 *    scenes carry more cuts into the next level.
 *
 *  - `iteration` — drop every cut whose `iteration_index` is greater
 *    than the last aggregate-certified iter (i.e.
 *    `results.back().iteration_index`).  Symmetric: every scene retains
 *    cuts up to the SAME iter; faster scenes lose their head-start cuts.
 *    Bit-for-bit reproducible irrespective of pool timing.  Recommended
 *    default.
 *
 *  - `all` — no truncation at all; every cut currently in the store
 *    (including any race cuts that landed after the stop signal) is
 *    kept.  Maximises learned-cut retention at the cost of run-to-run
 *    determinism — the inherited cut count for the next cascade level
 *    will vary depending on how many in-flight tasks happened to
 *    finish before the orchestration loop exited.
 *
 * Default `iteration` (introduced 2026-05).  Switch to `count` to
 * preserve the legacy asymmetric behaviour, or to `all` if convergence
 * at the next cascade level is more important than reproducibility.
 */
enum class CutDrainMode : uint8_t
{
  count = 0,  ///< Snapshot per-scene cut count at the stop boundary.
              ///< Legacy behaviour; asymmetric across scenes.
  iteration = 1,  ///< Filter by `cut.iteration_index <= last_certified_iter`.
                  ///< Default; symmetric and run-to-run deterministic.
  all = 2,  ///< Keep every cut currently in the store — no truncation.
            ///< Trades determinism for maximum cut retention.
};

inline constexpr auto cut_drain_mode_entries =
    std::to_array<EnumEntry<CutDrainMode>>({
        {.name = "count", .value = CutDrainMode::count},
        {.name = "iteration", .value = CutDrainMode::iteration},
        {.name = "all", .value = CutDrainMode::all},
    });

[[nodiscard]] constexpr auto enum_entries(CutDrainMode /*tag*/) noexcept
{
  return std::span {cut_drain_mode_entries};
}

// ─── ElasticFilterMode ──────────────────────────────────────────────────────

/**
 * @brief How the elastic filter handles feasibility issues in the backward
 * pass.
 *
 * - `single_cut`: Build a single Benders feasibility cut.
 * - `multi_cut`: Build a Benders cut + per-slack bound cuts.
 * - `chinneck` (default): Run a Chinneck-style elastic IIS filter —
 *   identify the irreducible infeasible subset of fixed state-variable
 *   bounds, then emit per-IIS-bound multi-cuts plus a tightened
 *   Benders cut whose reduced costs come from the IIS-restricted
 *   clone.  More LP solves per fcut event than `multi_cut`, but the
 *   cuts forbid only the true infeasibility-causing region (Chinneck,
 *   *Feasibility and Infeasibility in Optimization*, 2008, §3.5; PLP
 *   `osi_lp_get_feasible_cut`).  Falls back to the full elastic
 *   result when the IIS re-fix step cannot confirm a smaller subset,
 *   so behaviour is no worse than `multi_cut` in the worst case.
 */
// NOTE: `backpropagate` (numeric value 2) was a historical fourth
// mode for PLP-style source-bound updates; it was deleted from the
// production code in the forward-pass-installs-fcuts refactor and
// the enum value removed when the parser stopped recognising it.
// Legacy JSON/CLI strings of "backpropagate" now fall through to
// the default (chinneck) via parse_elastic_filter_mode's value_or.
enum class ElasticFilterMode : uint8_t
{
  single_cut = 0,  ///< Build a single Benders feasibility cut
  multi_cut = 1,  ///< Build a Benders cut + per-slack bound cuts
  chinneck = 3,  ///< Build cuts only on the Chinneck IIS (default)
};

/// Includes "cut" as a backward-compatible alias for "single_cut",
/// and "iis" as an alias for "chinneck".
inline constexpr auto elastic_filter_mode_entries =
    std::to_array<EnumEntry<ElasticFilterMode>>({
        {.name = "single_cut", .value = ElasticFilterMode::single_cut},
        {
            .name = "cut",
            .value = ElasticFilterMode::single_cut,
            .is_alias = true,
        },
        {.name = "multi_cut", .value = ElasticFilterMode::multi_cut},
        {.name = "chinneck", .value = ElasticFilterMode::chinneck},
        {.name = "iis", .value = ElasticFilterMode::chinneck, .is_alias = true},
    });

[[nodiscard]] constexpr auto enum_entries(ElasticFilterMode /*tag*/) noexcept
{
  return std::span {elastic_filter_mode_entries};
}

// ─── ApertureSelectionMode ─────────────────────────────────────────────────

/**
 * @brief How `SddpOptions::num_apertures` selects a subset from each
 * phase's `Phase::apertures` list.
 *
 * `Phase::apertures` is emitted by `plp2gtopt` sorted **wettest →
 * driest**, so the choice of selection rule controls which N entries
 * survive truncation:
 *
 * - `head` (default): pick the **first N** entries — i.e. the N
 *   wettest apertures per phase.  Concentrates effort on the
 *   high-water tail of the distribution.  Best for cascade L0 where
 *   the uninodal relaxation needs only the worst-case wet tail.
 *
 * - `stride`: pick N entries **evenly spaced** across the full
 *   ordered list (indices `i × total / N` for `i = 0..N-1`).  Samples
 *   the whole wetness spectrum — first index is still the wettest,
 *   last index is near the driest.  Best when the level wants a
 *   representative cross-section rather than the tail.
 *
 * When `num_apertures` is `nullopt`, the full per-phase list is used
 * regardless of mode.  When `num_apertures >= len(Phase::apertures)`,
 * the full list survives in either mode.
 */
enum class ApertureSelectionMode : uint8_t
{
  head = 0,  ///< Pick first N (wettest-N when list is wettest-first).
  stride = 1,  ///< Pick N entries evenly spaced across the full list.
  tail = 2,  ///< Pick last N (driest-N when list is wettest-first).
};

inline constexpr auto aperture_selection_mode_entries =
    std::to_array<EnumEntry<ApertureSelectionMode>>({
        {.name = "head", .value = ApertureSelectionMode::head},
        {
            .name = "first",
            .value = ApertureSelectionMode::head,
            .is_alias = true,
        },
        {.name = "stride", .value = ApertureSelectionMode::stride},
        {
            .name = "interleave",
            .value = ApertureSelectionMode::stride,
            .is_alias = true,
        },
        {
            .name = "spread",
            .value = ApertureSelectionMode::stride,
            .is_alias = true,
        },
        {.name = "tail", .value = ApertureSelectionMode::tail},
        {
            .name = "last",
            .value = ApertureSelectionMode::tail,
            .is_alias = true,
        },
    });

[[nodiscard]] constexpr auto enum_entries(
    ApertureSelectionMode /*tag*/) noexcept
{
  return std::span {aperture_selection_mode_entries};
}

// ─── ApertureSolveMode ─────────────────────────────────────────────────────

/**
 * @brief How each backward-pass aperture subproblem is solved and how its
 *        optimality cut's coefficients are recovered.
 *
 * Apertures differ only in column (flow) bounds, so the three modes trade
 * off per-solve cost against the quality / determinism of the reduced costs
 * that become the cut coefficients:
 *
 * - `cold`: each aperture is an independent **cold barrier solve with
 *   crossover**.  Crossover lands a vertex basis, so the reduced costs
 *   feeding the cut are the exact vertex duals.  Byte-for-byte the legacy
 *   behaviour.
 *
 * - `warm`: only meaningful with `aperture_chunk_size > 1`.  The first
 *   aperture in a chunk seeds a basis (cold barrier + crossover); every
 *   subsequent aperture re-optimizes that resident basis with a **warm
 *   simplex** solve (a few pivots off the previous optimum) instead of a
 *   fresh barrier.  Fastest on small LPs; on large cut-laden LPs the stale
 *   basis after large bound changes makes it net-slower than `cold` (see
 *   `docs/analysis/sddp-aperture-warmstart-fullnetwork.md`).
 *
 * - `reduced_cost` (default): each aperture is a **cold barrier solve
 *   WITHOUT crossover**.  No vertex basis is formed; the cut coefficients
 *   are taken directly from the interior-point (analytic-center) reduced
 *   costs, whose tolerance-level noise is filtered by `cut_coeff_eps`.
 *   ~35% faster per aperture than `cold` on big cut-laden LPs by skipping
 *   the crossover, at the price of approximate (to barrier tolerance)
 *   duals — see the cut-validity caveat in
 *   `docs/analysis/sddp-aperture-warmstart-fullnetwork.md`.
 */
enum class ApertureSolveMode : uint8_t
{
  cold = 0,  ///< Cold barrier + crossover; cut from vertex reduced costs.
  warm = 1,  ///< Warm simplex off the resident chunk basis (chunk_size > 1).
  reduced_cost = 2,  ///< Cold barrier, NO crossover; cut from interior-point
                     ///< reduced costs (filtered by cut_coeff_eps).  Default.
};

/// Includes "warm_start" / "barrier" as back-compatible aliases.
inline constexpr auto aperture_solve_mode_entries =
    std::to_array<EnumEntry<ApertureSolveMode>>({
        {.name = "cold", .value = ApertureSolveMode::cold},
        {.name = "warm", .value = ApertureSolveMode::warm},
        {
            .name = "warm_start",
            .value = ApertureSolveMode::warm,
            .is_alias = true,
        },
        {.name = "reduced_cost", .value = ApertureSolveMode::reduced_cost},
        {
            .name = "barrier",
            .value = ApertureSolveMode::reduced_cost,
            .is_alias = true,
        },
    });

[[nodiscard]] constexpr auto enum_entries(ApertureSolveMode /*tag*/) noexcept
{
  return std::span {aperture_solve_mode_entries};
}

// ─── HotStartMode ──────────────────────────────────────────────────────────

/**
 * @brief How the SDDP solver handles hot-start and the output cut file.
 *
 * Controls both whether to load cuts from a previous run and how to
 * handle the combined output file (`sddp_cuts.csv`) at the end of
 * the solve.
 */
enum class HotStartMode : uint8_t
{
  none = 0,  ///< Cold start — no cuts loaded (default)
  keep = 1,  ///< Hot-start; keep original output file unchanged
  append = 2,  ///< Hot-start; append new cuts to original file
  replace = 3,  ///< Hot-start; replace original file with all cuts
};

inline constexpr auto hot_start_mode_entries =
    std::to_array<EnumEntry<HotStartMode>>({
        {.name = "none", .value = HotStartMode::none},
        {.name = "keep", .value = HotStartMode::keep},
        {.name = "append", .value = HotStartMode::append},
        {.name = "replace", .value = HotStartMode::replace},
    });

[[nodiscard]] constexpr auto enum_entries(HotStartMode /*tag*/) noexcept
{
  return std::span {hot_start_mode_entries};
}

// ─── RecoveryMode ───────────────────────────────────────────────────────────

/**
 * @brief Controls what is recovered from a previous SDDP run.
 *
 * - `none`:  No recovery — cold start.
 * - `cuts`:  Recover only Benders cuts from previous run.
 * - `full`:  Recover Benders cuts AND state variable solutions (default).
 */
enum class RecoveryMode : uint8_t
{
  none = 0,  ///< No recovery — cold start
  cuts = 1,  ///< Recover only Benders cuts
  full = 2,  ///< Recover cuts + state variable solutions (default)
};

inline constexpr auto recovery_mode_entries =
    std::to_array<EnumEntry<RecoveryMode>>({
        {.name = "none", .value = RecoveryMode::none},
        {.name = "cuts", .value = RecoveryMode::cuts},
        {.name = "full", .value = RecoveryMode::full},
    });

[[nodiscard]] constexpr auto enum_entries(RecoveryMode /*tag*/) noexcept
{
  return std::span {recovery_mode_entries};
}

// ─── MissingCutVarMode ─────────────────────────────────────────────────────

/**
 * @brief How to handle boundary/named cut rows that reference state variables
 *        not present in the current model.
 *
 * - `skip_coeff`: Drop the missing variable's coefficient from the cut but
 *                 still load the cut (default).
 * - `skip_cut`:   Skip the entire cut if any missing variable has a non-zero
 *                 coefficient.
 */
enum class MissingCutVarMode : uint8_t
{
  skip_coeff = 0,  ///< Drop the coefficient, load the cut
  skip_cut = 1,  ///< Skip the entire cut
};

inline constexpr auto missing_cut_var_mode_entries =
    std::to_array<EnumEntry<MissingCutVarMode>>({
        {.name = "skip_coeff", .value = MissingCutVarMode::skip_coeff},
        {.name = "skip_cut", .value = MissingCutVarMode::skip_cut},
    });

[[nodiscard]] constexpr auto enum_entries(MissingCutVarMode /*tag*/) noexcept
{
  return std::span {missing_cut_var_mode_entries};
}

// ─── ConvergenceMode ───────────────────────────────────────────────────────

/**
 * @brief SDDP convergence criterion selection.
 *
 * All modes respect `min_iterations` and `max_iterations`.
 *
 * - `gap_only`:        Converge only when the deterministic gap closes.
 * - `gap_stationary`:  Also declare convergence when the gap stops
 *                      improving.
 * - `statistical`:     Full PLP-style criterion (default).  Adds a
 *                      confidence-interval test for multi-scene problems.
 */
enum class ConvergenceMode : uint8_t
{
  gap_only = 0,  ///< Deterministic gap test only
  gap_stationary = 1,  ///< Gap + stationary gap detection
  statistical = 2,  ///< Gap + stationary + CI (default, PLP-style)
};

inline constexpr auto convergence_mode_entries =
    std::to_array<EnumEntry<ConvergenceMode>>({
        {.name = "gap_only", .value = ConvergenceMode::gap_only},
        {.name = "gap_stationary", .value = ConvergenceMode::gap_stationary},
        {.name = "statistical", .value = ConvergenceMode::statistical},
    });

[[nodiscard]] constexpr auto enum_entries(ConvergenceMode /*tag*/) noexcept
{
  return std::span {convergence_mode_entries};
}

// ─── StateVariableLookupMode ───────────────────────────────────────────────

/**
 * @brief How update_lp elements obtain reservoir/battery volume between phases.
 *
 * - `warm_start` (default): volume comes from the previous iteration's
 *   warm-start solution, a recovered state file, or the element's vini.
 * - `cross_phase`: volume is taken from the previous phase's efin within
 *   the same forward pass.
 */
enum class StateVariableLookupMode : uint8_t
{
  warm_start =
      0,  ///< Warm solution / recovery / vini (default, no cross-phase)
  cross_phase = 1,  ///< Previous phase's efin within the same forward pass
};

inline constexpr auto state_variable_lookup_mode_entries =
    std::to_array<EnumEntry<StateVariableLookupMode>>({
        {.name = "warm_start", .value = StateVariableLookupMode::warm_start},
        {.name = "cross_phase", .value = StateVariableLookupMode::cross_phase},
    });

[[nodiscard]] constexpr auto enum_entries(
    StateVariableLookupMode /*tag*/) noexcept
{
  return std::span {state_variable_lookup_mode_entries};
}

// ─── LowMemoryMode ──────────────────────────────────────────────────────

/**
 * @brief Low-memory mode for SDDP solver.
 *
 * Controls whether and how the solver releases backend memory between solves.
 *
 * - `off`:      Disabled — keep solver backend loaded (default).
 * - `compress`: Release solver backend after each solve; keep a (optionally
 *               compressed) FlatLinearProblem snapshot + dynamic columns +
 *               accumulated cuts.  Reconstructed on demand.  Set
 *               `memory_codec = uncompressed` to retain the flat LP raw
 *               (previously the dedicated `snapshot` mode).
 *
 * The previous `rebuild` mode (re-flatten from collections on every solve,
 * no snapshot) was removed 2026-05-13: it delivered no measurable benefit
 * over `compress` on production workloads, while doubling the surface area
 * of LP-lifecycle bugs we had to keep alive.
 */
enum class LowMemoryMode : uint8_t
{
  off = 0,  ///< Disabled — keep solver backend loaded (default)
  compress = 2,  ///< Release solver, keep (optionally compressed) flat LP
};

inline constexpr auto low_memory_mode_entries =
    std::to_array<EnumEntry<LowMemoryMode>>({
        {.name = "off", .value = LowMemoryMode::off},
        {.name = "compress", .value = LowMemoryMode::compress},
        // Back-compat alias: "snapshot" parses to `compress`.  Callers
        // that want the old snapshot semantics (uncompressed flat LP)
        // set `memory_codec = uncompressed` explicitly.
        {
            .name = "snapshot",
            .value = LowMemoryMode::compress,
            .is_alias = true,
        },
        // Back-compat alias: "rebuild" was removed 2026-05-13; route to
        // `compress` so existing configs / CLI invocations don't error.
        // Drop this entry after one release cycle.
        {
            .name = "rebuild",
            .value = LowMemoryMode::compress,
            .is_alias = true,
        },
    });

[[nodiscard]] constexpr auto enum_entries(LowMemoryMode /*tag*/) noexcept
{
  return std::span {low_memory_mode_entries};
}

}  // namespace gtopt
