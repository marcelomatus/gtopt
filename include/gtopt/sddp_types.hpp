/**
 * @file      sddp_types.hpp
 * @brief     Data types for the SDDP iterative solver
 * @date      2026-04-03
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains the configuration, result, and state types used by the SDDP
 * solver.  Extracted from sddp_method.hpp to reduce header size and
 * improve modularity: consumers that only need the types (e.g. monitors,
 * cut I/O, cascade orchestration) can include this lightweight header
 * instead of the full SDDPMethod class definition.
 *
 * Types provided:
 *   - SDDPOptions            — runtime solver configuration
 *   - SDDPIterationResult    — per-iteration convergence data
 *   - PhaseStateInfo         — per-phase alpha / links / cached solution
 *   - ForwardPassOutcome     — forward-pass summary
 *   - BackwardPassOutcome    — backward-pass summary
 *   - SDDPIterationCallback  — observer callback type
 *   - sddp_file namespace   — file naming constants
 *
 * Free functions:
 *   - parse_cut_sharing_mode()
 *   - parse_elastic_filter_mode()
 *   - compute_scene_weights()
 *   - compute_convergence_gap()
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basis.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/sddp_cut_store_enums.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// Forward declaration for compute_scene_weights()
class SceneLP;
class SimulationLP;
class PlanningLP;

// ─── Cut sharing mode ───────────────────────────────────────────────────────
// CutSharingMode is now defined in <gtopt/sddp_enums.hpp>.
// The generic enum_from_name<CutSharingMode>() replaces the old
// parse_cut_sharing_mode() free function.

/// Parse a cut-sharing mode from a string ("none", "multicut",
/// "markov").
/// Throws `std::invalid_argument` on any other spelling — including
/// the modes REMOVED 2026-07-08 ("accumulate", "broadcast_mean" /
/// "expected", "max"), which get a dedicated removal message plus an
/// ERROR-level log (see `is_removed_cut_sharing_mode_name`).
[[nodiscard]] CutSharingMode parse_cut_sharing_mode(std::string_view name);

// ─── Configuration ──────────────────────────────────────────────────────────

/// File naming patterns for per-scene cut files
namespace sddp_file
{
/// Combined cut file name (Parquet — typed schema with
/// list<struct<key,val>> coefficients; bit-exact float64 storage).
constexpr auto combined_cuts = "sddp_cuts.parquet";
/// Versioned cut file pattern: format with iteration number
constexpr auto versioned_cuts_fmt = "sddp_cuts_{}.parquet";
/// Per-scene cut file pattern: format with scene UID
constexpr auto scene_cuts_fmt = "scene_{}.parquet";
/// Error-prefixed cut file pattern for infeasible scenes (scene UID)
constexpr auto error_scene_cuts_fmt = "error_scene_{}.parquet";
/// Error LP file pattern for unrecoverable-infeasibility dumps.
/// Arguments, in order: scene UID, phase UID, iteration index.  Written
/// under `SDDPOptions::log_directory` when the forward pass can produce
/// no feasibility cut (phase 0, or relaxed clone still infeasible) AND
/// `SDDPOptions::lp_debug` is true.  Without `lp_debug` the dump is
/// suppressed (each LP can be ~10 MB on a CEN-sized case and a stalled
/// run can emit dozens per iteration).
/// `LinearInterface::write_lp` appends the `.lp` extension.
constexpr auto error_lp_fmt = "error_s{}_p{}_i{}";
/// Debug LP file pattern: scene UID, phase UID, iteration index,
/// attempt counter.  The attempt counter distinguishes successive
/// writes of the same `(scene, phase, iter)` cell under the PLP-style
/// backtracking forward pass — every time the loop re-enters a phase
/// (after a backtrack from p → p-1), the LP snapshot carries the
/// accumulated fcuts installed since the last visit, so a separate
/// file is needed to preserve the chronology.  `attempt=1` is the
/// first visit of that phase in this iteration; subsequent values
/// reflect backtrack re-entries.  Uses `s{}_p{}_i{}_a{}` short form
/// matching `error_lp_fmt`.
constexpr auto debug_lp_fmt = "gtopt_s{}_p{}_i{}_a{}";
/// Debug LP file pattern for aperture clones: scene UID, target phase
/// UID, aperture UID, iteration index.  Used by the aperture backward
/// pass when `lp_debug` is enabled and the current (scene, phase)
/// falls inside the `lp_debug_*_min/max` filter window.  The aperture
/// UID disambiguates the N clones built from the same target phase.
constexpr auto debug_aperture_lp_fmt = "gtopt_aperture_s{}_p{}_a{}_i{}";
/// Debug LP file pattern for backward-pass tgt LPs (one .lp file per
/// `(iter, scene, phase)`) immediately before each
/// `tgt_li.resolve(opts)`.  Captures the LP exactly as the solver
/// sees it (post-`update_lp_for_phase`, post-`apply_post_load_replay`
/// under compress).  Active when `lp_debug=true` AND `lp_debug_passes`
/// includes `backward` (or `all`).  No mode tag in the filename —
/// users that want to diff off↔compress dumps configure two
/// separate `log_directory` paths and diff in bulk.
constexpr auto debug_backward_lp_fmt = "gtopt_backward_s{}_p{}_i{}";
/// Sentinel file name: if this file exists in the output directory, the
/// SDDP solver stops gracefully after the current iteration and saves
/// cuts.  Created externally (e.g. by the webservice stop endpoint).
constexpr auto stop_sentinel = "sddp_stop";
// State-variable column file names were removed (2026-05-14) — the
// per-iter `sddp_state.csv` writer (and its retired `_<iter>` /
// `.json` siblings) had no readers anywhere; policy state is
// reconstructed from the versioned cut files at recovery time.
/// Monitoring API stop-request file name: if this file exists, the solver
/// stops gracefully after the current iteration (same behaviour as the
/// sentinel file).  Written by the webservice soft-stop endpoint as part
/// of the bidirectional monitoring API.  Complements rather than replaces
/// the sentinel mechanism so that external scripts using the raw sentinel
/// still work.  The solver checks: sentinel_file exists || stop_request
/// file exists.
constexpr auto stop_request = "sddp_stop_request.json";
}  // namespace sddp_file

// ─── Alpha (future-cost) variable naming ────────────────────────────────────
//
// Alpha is the method-owned cost-to-go variable added to every phase
// except the last.  It is registered in `sim.state_variables()` like any
// other state variable so that state/cut CSV I/O and cross-level
// resolution treat it uniformly.
//
// `sddp_alpha_lp_class` is the canonical `LPClassName` carrying both
// the PascalCase full_name ("Sddp") and precomputed snake_case
// short_name ("sddp").  `StateVariable::Key::class_name` and
// `StateVarLink::class_name` store `const LPClassName*` pointing at
// this constant (or at another LP class's static `ClassName`) so cut
// builders and AMPL lookups hit the right form without any runtime
// string conversion.  `sddp_alpha_class_name` (string_view) is kept
// as a convenience alias for `SparseRow`/`SparseCol` metadata, which
// store only the PascalCase view.
inline constexpr LPClassName sddp_alpha_lp_class {"Sddp"};
constexpr std::string_view sddp_alpha_class_name =
    sddp_alpha_lp_class.full_name();
constexpr std::string_view sddp_alpha_col_name = "alpha";

/// Variable-name tags for the elastic filter's slack columns added
/// by `relax_fixed_state_variable`.  Carried on `SparseCol` metadata
/// so serialised LPs (`CoinLpIO`, `LinearInterface::write_lp`) emit
/// distinct non-empty column labels — a hard requirement for the
/// COIN LP writer, which replaces every row/col name with generic
/// defaults when it encounters even one unnamed column.
constexpr std::string_view sddp_elastic_sup_col_name = "elastic_sup";
constexpr std::string_view sddp_elastic_sdn_col_name = "elastic_sdn";

/// Constraint-name tags carried on LP row metadata for each kind
/// of Benders cut emitted by SDDP.  Paired with
/// `sddp_alpha_class_name` so the eager duplicate detector in
/// `LinearInterface::add_row` keys SDDP cuts on
/// `("Sddp", <tag>, uid, IterationContext)` — the tag distinguishes
/// the pass that produced the cut.  Multi-cuts (`mcut`) are the only
/// SDDP cut family tagged with the *source state-variable's* class
/// name instead of `sddp_alpha_class_name`, since each multi-cut row
/// is a per-link bound constraint on a single state variable.
constexpr std::string_view sddp_scut_constraint_name = "scut";
constexpr std::string_view sddp_aperture_cut_constraint_name = "aperture_cut";
constexpr std::string_view sddp_ecut_constraint_name = "ecut";
constexpr std::string_view sddp_share_cut_constraint_name = "share_cut";
constexpr std::string_view sddp_bcut_constraint_name = "bcut";
constexpr std::string_view sddp_fcut_constraint_name = "fcut";
constexpr std::string_view sddp_mcut_constraint_name = "mcut";

/// Class tags for cuts brought in by the loaders.  Each loader path
/// sets a distinct class_name so mixing loader sources never produces
/// identical metadata:
///   * `sddp_loaded_cut_class_name`   – generic `load_cuts_parquet`
///   * `sddp_boundary_cut_class_name` – `load_boundary_cuts_csv`
/// Both share a single constraint-name constant
/// (`sddp_loaded_cut_constraint_name`) since they describe the
/// same kind of Benders optimality row.  The legacy
/// ``sddp_named_cut_class_name`` ("NamedHs") was retired in 2026-05
/// alongside ``load_named_cuts_csv``.
constexpr std::string_view sddp_loaded_cut_class_name = "Loaded";
constexpr std::string_view sddp_boundary_cut_class_name = "Boundary";
constexpr std::string_view sddp_loaded_cut_constraint_name = "cut";

/// A `(class_name, constraint_name)` pair that identifies the kind
/// of cut a SparseRow represents.  Bundling the two metadata strings
/// makes them move together at every cut-construction site —
/// previously each builder hand-set the two fields independently,
/// which produced subtle mismatches when one rename forgot the
/// other.  All concrete tags are declared as namespace-scope
/// `constexpr CutTag sddp_*_tag` constants below; call sites use
/// `sddp_<x>_tag.apply_to(row)` instead of two assignments.
struct CutTag
{
  std::string_view class_name {};
  std::string_view constraint_name {};

  /// Stamp this tag's identity onto a SparseRow.  Returns the row
  /// reference for fluent chaining at call sites that also set
  /// `variable_uid` / `context` immediately after.
  constexpr auto apply_to(SparseRow& row) const noexcept -> SparseRow&
  {
    row.class_name = class_name;
    row.constraint_name = constraint_name;
    return row;
  }
};

/// SDDP-class cut tags: every Benders cut produced by an SDDP pass
/// is keyed under `(sddp_alpha_class_name, <pass-tag>)` so the eager
/// duplicate detector in `LinearInterface::add_row` distinguishes
/// the seven pass types at the row-metadata level.  These instances
/// replace the `cut.class_name = …; cut.constraint_name = …;` pair
/// at every cut-construction site.
inline constexpr CutTag sddp_scut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_scut_constraint_name,
};
inline constexpr CutTag sddp_fcut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_fcut_constraint_name,
};
inline constexpr CutTag sddp_bcut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_bcut_constraint_name,
};
inline constexpr CutTag sddp_ecut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_ecut_constraint_name,
};
inline constexpr CutTag sddp_aperture_cut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_aperture_cut_constraint_name,
};
inline constexpr CutTag sddp_share_cut_tag {
    .class_name = sddp_alpha_class_name,
    .constraint_name = sddp_share_cut_constraint_name,
};

/// Loader-class cut tags: cuts loaded from disk use a distinct
/// class_name per source so a hot-start that mixes loaders produces
/// unique row-metadata keys for the duplicate detector, while sharing
/// the single `sddp_loaded_cut_constraint_name` constraint name (they
/// describe the same kind of optimality row).
inline constexpr CutTag sddp_loaded_cut_tag {
    .class_name = sddp_loaded_cut_class_name,
    .constraint_name = sddp_loaded_cut_constraint_name,
};
inline constexpr CutTag sddp_boundary_cut_tag {
    .class_name = sddp_boundary_cut_class_name,
    .constraint_name = sddp_loaded_cut_constraint_name,
};

namespace detail
{
/// Compile-time check: returns true iff @p prefix matches the
/// runtime row label produced by `LabelMaker::make_row_label`,
/// which emits `<lowercase(class_name)>_<constraint_name>_…`.  Used
/// in the `static_assert`s below to pin every `sddp_*_row_prefix`
/// to its `(class_short, constraint_name)` pair so a rename of
/// either constant fails the build instead of silently de-syncing
/// the cut-row dispatcher in `extract_iteration_from_name`.
[[nodiscard]] consteval bool prefix_matches(
    std::string_view prefix,
    std::string_view class_short,
    std::string_view constraint) noexcept
{
  if (prefix.size() != class_short.size() + 1 + constraint.size() + 1) {
    return false;
  }
  if (!prefix.starts_with(class_short)) {
    return false;
  }
  if (prefix[class_short.size()] != '_') {
    return false;
  }
  const auto rest = prefix.substr(class_short.size() + 1);
  return rest.starts_with(constraint) && rest[constraint.size()] == '_';
}
}  // namespace detail

/// Row-name prefixes for the four SDDP pass-tagged cut types,
/// consumed by `extract_iteration_from_name` (`sddp_cut_io.cpp`)
/// to dispatch on row-name shape without re-parsing the
/// (class, constraint) pair.  Each prefix is verified at compile
/// time against the corresponding `sddp_alpha_lp_class.short_name()`
/// + `sddp_<x>_constraint_name` pair via the `static_assert`s below.
constexpr std::string_view sddp_scut_row_prefix {"sddp_scut_"};
constexpr std::string_view sddp_fcut_row_prefix {"sddp_fcut_"};
constexpr std::string_view sddp_bcut_row_prefix {"sddp_bcut_"};
constexpr std::string_view sddp_ecut_row_prefix {"sddp_ecut_"};

static_assert(detail::prefix_matches(sddp_scut_row_prefix,
                                     sddp_alpha_lp_class.short_name(),
                                     sddp_scut_constraint_name));
static_assert(detail::prefix_matches(sddp_fcut_row_prefix,
                                     sddp_alpha_lp_class.short_name(),
                                     sddp_fcut_constraint_name));
static_assert(detail::prefix_matches(sddp_bcut_row_prefix,
                                     sddp_alpha_lp_class.short_name(),
                                     sddp_bcut_constraint_name));
static_assert(detail::prefix_matches(sddp_ecut_row_prefix,
                                     sddp_alpha_lp_class.short_name(),
                                     sddp_ecut_constraint_name));
/// Fixed uid used in the alpha `StateVariable::Key`.  The state-variable
/// map is partitioned by `(scene_index, phase_index)` and there is at
/// most one alpha per cell, so any constant uid disambiguates the key.
/// `Uid{0}` keeps the structured cut-key label as `Sddp:alpha:0`.
constexpr Uid sddp_alpha_uid {0};

/// Bootstrap lower bound on α at iter-0 cold start, in **physical ($)**
/// units.  Divided by `scale_alpha` at the call site.
///
/// Value 0.0 matches the project's pre-existing default (proven across
/// all integration tests).  α has a positive objective coefficient, so
/// without a lower bound the cold-start iter-0 LP would be unbounded;
/// a loose bound (e.g. −1e10) admits negative-cost futures but
/// produces very weak LB estimates that trigger premature stationary
/// convergence in tight-tolerance tests.  α ≥ 0 is a mild bias for
/// problems where future cost is positive (almost all dispatch), and
/// users with net-revenue phases can override via a custom cut.
///
/// A cleaner "lazy α" (create α only at first cut install) was tried
/// and abandoned: it interacted badly with the low-memory cached
/// col_sol/col_cost/col_scales vectors, producing out-of-bounds
/// access in the aperture path and a convergence slowdown on the
/// 3-phase hydro test.  Revisit only with proper cached-vector
/// invalidation work.
constexpr double sddp_alpha_bootstrap_min = 0.0;

/// Floating-point noise floor for the convergence gap.
///
/// SDDP theory says `LB ≤ optimum ≤ UB`, so the gap `(UB-LB)/|UB|`
/// is non-negative at the optimum.  A *tiny* negative gap (e.g.
/// `-1e-16` when `UB ≈ LB` exactly) is just IEEE-754 rounding
/// noise and must NOT trigger the "cuts overshoot the optimum"
/// guard.  A *significant* negative gap (`gap < -kSddpGapFpEpsilon`)
/// IS a real SDDP-theory violation that the convergence checks
/// refuse to declare `[CONVERGED]` on.
///
/// Distinct from `SDDPOptions::convergence_tol` (the user-facing
/// gap tolerance for declaring convergence): `convergence_tol`
/// sets the upper bound of the converged band and may be set to a
/// negative sentinel (e.g. `-1.0`) to disable the primary gap test
/// in favour of the stationary criterion.  `kSddpGapFpEpsilon` is
/// a fixed lower bound for the same band, expressed in absolute
/// terms because IEEE-754 rounding noise on `(UB - LB)/|UB|` for
/// values up to ~1e9 stays comfortably below `1e-9`.
constexpr double kSddpGapFpEpsilon = 1.0e-9;

// ─── Elastic filter mode ────────────────────────────────────────────────────
// ElasticFilterMode is now defined in <gtopt/sddp_enums.hpp>.
// The generic enum_from_name<ElasticFilterMode>() replaces the old
// parse_elastic_filter_mode() free function.

/// Parse an elastic filter mode from a string (backward-compatible
/// wrapper).  Accepts "single_cut" / "cut" (= single_cut), "multi_cut",
/// "chinneck" / "iis" (= chinneck).  Unknown strings — including the
/// retired "backpropagate" — fall back to the default mode (chinneck).
[[nodiscard]] ElasticFilterMode parse_elastic_filter_mode(
    std::string_view name);

// ─── Markov-chain SDDP configuration ────────────────────────────────────────

/// Configuration for `CutSharingMode::markov` (Markov-chain SDDP,
/// opt-in / experimental — `docs/formulation/sddp-markov.md`).
///
/// Each scene `s` is statically assigned a Markov state
/// `state_of_scene[s] ∈ [0, num_states)`; `transition` is the
/// row-major `num_states × num_states` row-stochastic matrix `P`.
/// Every non-terminal scene-LP then carries `num_states` future-cost
/// columns `varphi_0..M-1` (uid = `sddp_alpha_uid + m`), scene-S's
/// backward cut lands on `varphi_{m(S)}` in every scene-LP, and
/// `varphi_{m'}` is priced `w_{s,m'} = p_s·P[m(s)][m'] / pi_{m'}`
/// where `pi_{m'}` is the state's total scene-probability mass
/// (theorem MK1).  Validate with `validate_markov_config` before use.
struct MarkovChainConfig
{
  /// Scene → Markov-state assignment (size = number of scenes, each
  /// value in `[0, num_states)`).  Static per scene (v1).
  std::vector<int> state_of_scene {};
  /// Row-major `num_states × num_states` row-stochastic transition
  /// matrix `P` (rows sum to ≈ 1, entries ≥ 0).
  std::vector<double> transition {};
  /// Number of Markov states `M` (the transition matrix dimension).
  std::size_t num_states {0};

  /// True when no Markov configuration was supplied.
  [[nodiscard]] constexpr bool empty() const noexcept
  {
    return num_states == 0;
  }

  /// Transition probability `P[from][to]` (no bounds check — callers
  /// go through `validate_markov_config` first).
  [[nodiscard]] double probability(std::size_t from,
                                   std::size_t to) const noexcept
  {
    return transition[(from * num_states) + to];
  }
};

/// Build a `MarkovChainConfig` from the JSON-facing arrays
/// (`SddpOptions::markov_states` / `markov_transition`).  `num_states`
/// is inferred as the integer square root of the transition length;
/// a non-square length yields the raw length preserved so
/// `validate_markov_config` can report it.  No validation here.
[[nodiscard]] MarkovChainConfig make_markov_config(
    std::vector<int> state_of_scene, std::vector<double> transition);

/// Validate a Markov configuration against the scene count: `M ≥ 1`,
/// square transition of matching dimension with finite non-negative
/// entries and row sums within `1e-6` of 1, one assignment per scene,
/// every assignment in range, and every state non-empty (an empty
/// state has zero mass and the `w = p_s·P/pi` pricing divides by it).
/// Returns the error message, or `nullopt` when valid.
[[nodiscard]] std::optional<std::string> validate_markov_config(
    const MarkovChainConfig& markov, std::size_t num_scenes);

/// Per-column pricing weights of scene-@p scene_index's LP under
/// `CutSharingMode::markov`: `w_{s,m'} = p_s·P[m(s)][m'] / pi_{m'}`
/// for `m' = 0..M-1` (theorem MK1 in `docs/formulation/sddp-markov.md`
/// §2).  Scene probabilities are the per-scene sums of scenario
/// `probability_factor`s (with the same `≤ 0 → 1.0` fallback as
/// `compute_scene_weights`); the weights are scale-invariant in the
/// total mass, so no normalization is applied.  Consumed via the
/// mode-aware `alpha_col_weights` accessor, which
/// `register_alpha_variables` (objective pricing) and the forward-pass
/// UB strip both call so the two always agree.
[[nodiscard]] std::vector<double> markov_alpha_weights(
    const SimulationLP& sim,
    const MarkovChainConfig& markov,
    SceneIndex scene_index);

/// Configuration options for the SDDP iterative solver
struct SDDPOptions  // NOLINT(clang-analyzer-optin.performance.Padding)
{
  // ── Convergence policy (single coherent story) ────────────────────────
  //
  // Aim for a 1 % relative gap (``convergence_tol``).  If the gap stops
  // moving (gap_change < ``stationary_tol`` = 0.5 %) AND the gap is
  // already inside ``stationary_gap_ceiling`` (5 %), accept it as
  // converged.  Otherwise keep iterating up to ``max_iterations``.
  // The statistical CI test (``convergence_confidence``) is opt-in
  // (default 0) because it is too easily fooled by σ scatter when scene
  // UBs are heterogeneous (e.g. Maule/juan).  ``min_iterations`` = 1
  // is the minimum that still lets the convergence check fire — set
  // higher (e.g. 3) only at noisy bootstrap levels (cascade L0) where
  // the very-first iter's gap is structurally meaningless; cascade L1+
  // inherit a converged envelope and may legitimately exit on their
  // first qualifying iter.
  //
  // Override individually only when you know what you're doing — the
  // defaults are deliberately consistent so most users never set them.
  int max_iterations {100};  ///< Maximum forward/backward iterations
  int min_iterations {1};  ///< Minimum iterations before convergence
  double convergence_tol {0.01};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e3};  ///< Penalty for elastic slack variables
  /// Scale for α (0 = auto: max state var_scale).  α itself is a free
  /// variable (no explicit bounds): per-row row-max equilibration on
  /// every Benders cut already controls LP conditioning, and α can
  /// legitimately go negative (net-revenue phases, mid-convergence
  /// cuts that haven't yet tightened LB above zero).
  double scale_alpha {0};
  /// Cut sharing mode across scenes.  Two modes remain
  /// (`docs/formulation/sddp-cut-validity.md` §7–§8):
  ///
  ///  * `none` (default): each scene's single α is bounded only by its
  ///    own cuts — the per-scene persistent-path (wait-and-see) lower
  ///    bound, **unconditionally valid** (theorem N1).
  ///  * `multicut`: PLP-faithful per-scenario varphi columns priced at
  ///    the M4 weight `w_r = p_s` (`alpha_unit_cost`; = 1/N under
  ///    uniform probabilities); the LB is the lower bound of the
  ///    stagewise-RESAMPLED process with measure `q_r = p_r`, valid for
  ///    any probability vector (theorems M1/M4 — an INFO notes the
  ///    persistent-UB vs resampled-LB process mismatch at SDDP setup
  ///    when probabilities are non-uniform).
  ///
  /// REMOVED 2026-07-08: `accumulate`, `broadcast_mean` (alias
  /// `expected`), and `max` — KNOWN INVALID broadcasts onto the
  /// destination scene's own α that produced compounding LB > UB on
  /// distinct sample paths (juan/gtopt_iplp regressed at 7225× before
  /// this was diagnosed).  Their names now hard-error at ingestion.
  /// See `test/source/test_sddp_bounds_sanity.cpp` for the regression
  /// guards and `test/source/test_sddp_cut_oracle.cpp` for the
  /// extensive-form certification harness.
  CutSharingMode cut_sharing {CutSharingMode::none};

  /// Markov-chain configuration for `cut_sharing = markov` (scene →
  /// state assignment + row-stochastic transition matrix).  Ignored by
  /// every other mode.  Validated by `validate_markov_config` at SDDP
  /// setup; see `docs/formulation/sddp-markov.md`.
  MarkovChainConfig markov {};

  /// Integer-cut mode for backward-pass cells whose LP carries integer
  /// columns (integer expansion modules, unit-commitment binaries):
  ///
  ///  * `none` (default): legacy behaviour, byte-identical.  Pure-LP
  ///    cells emit the certified Theorem-O1 cut; integer-bearing cells
  ///    are UNSOUND (no reduced costs on a MIP — see
  ///    `docs/analysis/investigations/sddp/`
  ///    `sddip_integer_expansion_2026-07.md` §1).
  ///  * `strengthened`: LP-relaxation cut + one-MIP Lagrangian
  ///    intercept (Zou, Ahmed & Sun 2019).  Valid by weak Lagrangian
  ///    duality (Theorem SB1), never looser than the LP cut
  ///    (Corollary SB2); silent LP fallback on MIP failure/timeout.
  IntegerCutsMode integer_cuts {IntegerCutsMode::none};

  /// How terminal/boundary cuts are shared across scenes on the terminal α —
  /// the terminal-phase analogue of `cut_sharing` (resolved from
  /// `SddpOptions::boundary_cut_sharing_mode`, with the legacy
  /// `boundary_cuts_mode` scope as the fallback: separated→per_scene,
  /// combined→shared).  `per_scene` (default) and `multicut` are valid;
  /// `shared` is valid only when the post-horizon value is scenario-identical.
  BoundaryCutSharingMode boundary_cut_sharing {
      BoundaryCutSharingMode::per_scene};

  /// How to drain in-flight cuts after the aggregate-convergence stop
  /// signal fires in `SDDPMethod::solve_async`.  See `CutDrainMode`
  /// in `sddp_enums.hpp` for the full rationale.  Default `iteration`:
  /// symmetric across scenes, run-to-run reproducible.
  CutDrainMode cut_drain_mode {CutDrainMode::iteration};

  /// How the forward pass samples stochastic scene data:
  ///
  ///  * `persistent` (default): each scene-driver simulates its own
  ///    scenario path at every phase — historical behaviour,
  ///    byte-identical (the resampling code is never entered).
  ///  * `resampled`: per-phase-boundary probability-weighted re-draw
  ///    applied via the bound-only `update_aperture` machinery; the UB
  ///    then estimates the same `q_r = p_r` resampled process the
  ///    multicut LB certifies.  v1: one sampled path per scene-driver
  ///    per iteration, deterministic in (iteration, scene, phase).
  ///
  /// See `ForwardSamplingMode` (sddp_enums.hpp) for the full story.
  ForwardSamplingMode forward_sampling {ForwardSamplingMode::persistent};

  /// Elastic filter mode: how the FORWARD pass emits feasibility
  /// cuts when a phase LP is infeasible at the trial state.  Only
  /// the forward pass has an elastic branch; the backward pass
  /// produces optimality cuts exclusively.
  ///   `single_cut` (default): one classical PLP/Birge-Louveaux
  ///                 Benders feasibility cut from row duals of the
  ///                 state-fixing equations at the elastic clone's
  ///                 Phase-1 optimum.  Matches `plp-agrespd.f`.
  ///   `multi_cut` : `single_cut` plus one bound-constraint cut per
  ///                 activated slack variable.
  ///   `chinneck`  : IIS-filtered multi-cut — runs an extra re-fix
  ///                 pass to narrow the cut set to the irreducible
  ///                 infeasible subset of relaxed bounds.  Not
  ///                 re-validated against the row-dual fcut builder
  ///                 introduced in commit 0307c58e.
  ElasticFilterMode elastic_filter_mode {ElasticFilterMode::single_cut};

  /// Absolute tolerance for filtering tiny Benders cut coefficients.
  /// Coefficients with |value| < cut_coeff_eps are dropped from the cut.
  /// 0.0 = no filtering (default).
  double cut_coeff_eps {0.0};

  /// Forward-pass infeasibility counter threshold for automatic switching
  /// from single_cut / chinneck-fcut-only to multi_cut.  When the forward
  /// pass has encountered infeasibility at (scene, phase) this many times
  /// without recovery, the elastic branch additionally installs
  /// `build_multi_cuts` bound cuts alongside the Benders fcut.
  ///  = 0  always use multi_cut for any infeasibility (force multi_cut).
  ///  > 0  switch to multi_cut after the counter reaches this threshold.
  ///  < 0  never auto-switch (disabled; use explicit mode only).
  /// Default: 100.  Raised from 10 because `build_multi_cuts` emits
  /// `source_col {<=,>=} dep_val_phys` bound cuts from the Chinneck
  /// Phase-1 LP, whose `dep_val_phys` may exceed the source phase's
  /// physically reachable set (observed on juan/gtopt_iplp after ~3
  /// iterations: phase 0 gets `reservoir_energy_1 >= 330.33` while its
  /// physical cap is ~207.78 + small inflow).  Keeping the IIS-filtered
  /// fcut alone for longer avoids the structurally-infeasible cuts that
  /// mcut mode can install before the master has found a good trial.
  int multi_cut_threshold {100};

  /// Forward-pass backtracking cap: maximum cumulative LP solves per
  /// scene per iteration.  When an elastic branch fires at phase p,
  /// gtopt (PLP-style) installs the feasibility cut on phase p-1 and
  /// BACKTRACKS to phase p-1, re-solving it under the new cut.  If
  /// p-1 is also infeasible, an fcut is installed on p-2 and the
  /// backtrack continues until a feasible phase is reached, after
  /// which the pass resumes forward.  `forward_max_attempts` caps
  /// the total number of solves to prevent an infinite
  /// backtrack/retry loop — when exceeded, the scene is declared
  /// infeasible for this iteration and excluded from UB aggregation.
  /// Matches PLP's `FactMXC` knob in `plp-faseprim.f` (`getopts.f:257`,
  /// default **5000**).  Prior gtopt default was 100 — too tight for
  /// pathological cases whose iter-0 forward pass needs to backtrack
  /// across many phases to build the initial cut set (observed on
  /// juan/gtopt_iplp: every scene hits 100 before phase 1 master has
  /// enough cuts to be feasible).  Default now matches PLP's 5000.
  int forward_max_attempts {5000};

  /// Scene-level fail-stop forward pass (default: true).
  ///
  /// When true, an infeasible phase that produces a feasibility cut on
  /// its predecessor causes the scene's forward pass to STOP for the
  /// current iteration: the fcut is installed on phase p-1, the scene
  /// is marked failed (returned Error → caller sets scene_feasible=0),
  /// and the next iteration starts fresh from p1 with the newly
  /// accumulated cuts (preserved in the global cut store).  Other
  /// scenes continue uninterrupted.  Avoids the iter-0 cascade that
  /// walks back through many stages and produces degenerate cuts.
  ///
  /// When false, restores the legacy PLP-style backtracking forward
  /// pass: after installing the fcut on p-1, `phase_idx` is decremented
  /// and p-1 is re-solved under the new cut.  If p-1 is still
  /// infeasible, a fresh fcut is installed on p-2 and the cascade
  /// continues — bounded by `forward_max_attempts`.  Kept available
  /// for regression tests and academic fixtures that depend on the
  /// cascade dynamics.
  // Default flipped 2026-04-29 — see planning_options_lp.hpp's
  // ``default_sddp_forward_fail_stop`` for rationale.  PLP-style
  // backtracking cascade is the natural-intuition default; the
  // single-fcut-and-exit coordination strategy is opt-in.
  bool forward_fail_stop {false};

  /// Per-scene rollback on forward-pass infeasibility (default: false).
  ///
  /// When `true` and a scene S is declared infeasible at iteration k's
  /// forward pass (`scene_feasible[S] == 0`):
  ///   1. Every cut S has accumulated in `m_cut_store_.scene_cuts()[S]`
  ///      is deleted (rows removed from S's LP cells via
  ///      `LinearInterface::delete_rows` + `record_cut_deletion`,
  ///      vector cleared).  Both forward-pass fcuts and earlier
  ///      backward-pass optcuts are dropped — the bad trajectory that
  ///      produced any of them is no longer trusted.
  ///   2. The global stored-cut count is snapshot in
  ///      `m_scene_retry_state_[S].global_cuts_at_last_failure`.
  ///   3. At iteration k+1's forward dispatch, S retries iff the
  ///      global cut count grew (peers contributed cuts via cut sharing
  ///      or their own backward pass).  Otherwise S is "stalled" — and
  ///      if every previously-failed scene is stalled, the run aborts
  ///      with `Error{SolverError, "no recovery path"}` to avoid an
  ///      infinite-loop on degenerate single-scene / no-progress runs.
  ///
  /// When `false` (legacy default): cuts S installed on its own bad
  /// trajectory persist across iterations, and the run loops until
  /// `max_iterations` without rollback or stall detection.
  bool forward_infeas_rollback {false};

  /// Re-solve target phase t LP at v̂_{t-1} before extracting cut data
  /// (default: true).  When true, cuts on α_t added earlier in this
  /// backward pass (when the loop processed phase t+1) are reflected
  /// in z_t and reduced costs, producing a one-iter-tight Benders
  /// cut on α_{t-1}.  See `SDDPOptions::backward_resolve_target` for
  /// the full rationale and cost analysis.
  bool backward_resolve_target {true};

  /// Save cuts after each training iteration (default: true).
  /// When false, cuts are only saved at the end of the solve or on stop.
  bool save_per_iteration {true};

  /// Save feasibility cuts produced during the simulation pass (default:
  /// false).  When false, only training-iteration cuts are persisted,
  /// ensuring hot-start reproducibility.
  bool save_simulation_cuts {false};

  /// Skip the post-training **simulation pass** entirely (default:
  /// false → sim pass runs).  When `true`, `solve()` returns immediately
  /// after the last training iteration: no final forward pass with
  /// `crossover=true`, no per-cell `SystemLP::write_out()`, no sim-pass
  /// feasibility-cut bookkeeping.
  ///
  /// Set to `true` for **non-final** cascade levels — their sim-pass
  /// output would be overwritten by the next level's sim pass anyway,
  /// and their crossover-quality duals are not consumed by anything
  /// (state-variable targets are read from the last training forward
  /// pass via `svar.col_sol_physical()`, and stored optimality cuts
  /// come from the training backward passes).  See
  /// `CascadePlanningMethod::solve` for the orchestration.  On
  /// juan-scale this saves the ~9 s/cell × 816 cells / 16-way
  /// parallel ≈ 7 min per intermediate level.
  bool skip_simulation_pass {false};

  /// Global solve timeout in seconds (0 = no timeout).
  /// When non-zero, each forward-pass LP solve is given this time limit;
  /// if exceeded, the LP is saved to a debug file, a CRITICAL message is
  /// logged, and the scene is marked as failed.
  double solve_timeout {0.0};

  /// File path for saving cuts (empty = no save)
  std::string cuts_output_file {};
  /// File path for loading initial cuts (empty = no load / cold start)
  std::string cuts_input_file {};
  /// Hot-start mode: controls both cut loading and output file handling.
  /// - `none`:    cold start -- no cuts loaded (default)
  /// - `keep`:    load cuts; keep original output file unchanged
  /// - `append`:  load cuts; append new cuts to original file
  /// - `replace`: load cuts; replace original file with all cuts
  HotStartMode cut_recovery_mode {HotStartMode::none};

  /// Controls what is recovered from a previous SDDP run:
  /// - `none`:  no recovery (cold start)
  /// - `cuts`:  recover only Benders cuts
  /// - `full`:  recover cuts + state variable solutions (default)
  RecoveryMode recovery_mode {RecoveryMode::full};

  /// Caller-supplied lower bound for the iteration index at which this
  /// solver should start counting.  Composed with the hot-start offset
  /// via `std::max`, so hot-start cuts always win when they demand a
  /// higher offset.  Used by CascadePlanningMethod to place each level's
  /// iteration indices in a disjoint global range (avoids in-memory cut
  /// store collisions and gives log lines a globally monotonic index).
  /// Absent = solver starts at iteration 0 (or at the hot-start offset,
  /// whichever is higher).
  std::optional<IterationIndex> iteration_offset_hint {};

  /// Path to a sentinel file: if the file exists, the solver stops
  /// gracefully after the current iteration (analogous to PLP's userstop).
  /// All accumulated cuts are saved before stopping.
  std::string sentinel_file {};

  /// Directory for log and error LP files (default: "logs").
  /// Error LP files for infeasible scenes are saved here.
  std::string log_directory {"logs"};

  /// When true, save a debug LP file for every (iteration, scene, phase)
  /// in the SDDP passes selected by `lp_debug_passes`.
  /// Files land in `log_directory` and are named using
  /// `sddp_file::debug_lp_fmt` (forward) /
  /// `debug_backward_lp_fmt` (backward) /
  /// `debug_aperture_lp_fmt` (aperture).
  bool lp_debug {false};

  /// When true, write an error LP file when a (scene, phase) is infeasible
  /// (independent of `lp_debug`, which gates the per-pass debug dumps).
  bool lp_error {false};

  /// Comma-separated list of SDDP passes whose LP-debug dump is
  /// active when `lp_debug=true`.  Empty / unset selects the legacy
  /// default `"forward,aperture"`.  Tokens (case-insensitive,
  /// comma-separated): `forward`, `backward`, `aperture`, `all`.
  /// Inspected at debug-write time via
  /// `lp_debug_passes_includes(...)` from `lp_debug_passes.hpp` —
  /// kept as a string (rather than parsed enum bitmask) to mirror
  /// the sister field `lp_debug_compression`, which is also a
  /// pass-through JSON string.
  std::string lp_debug_passes {};

  /// Compression format for LP debug files ("gzip" / "uncompressed" / "").
  /// Empty or "uncompressed" means no compression; any other value uses
  /// gzip.
  std::string lp_debug_compression {};

  /// Selective LP debug filters: when set, only save LP files whose
  /// scene/phase UIDs fall within [min, max] (inclusive).
  OptInt lp_debug_scene_min {};
  OptInt lp_debug_scene_max {};
  OptInt lp_debug_phase_min {};
  OptInt lp_debug_phase_max {};

  /// When true, every LP-debug dump is gated on the SDDP method being
  /// in its simulation pass (``m_in_simulation_ == true``).  Training
  /// iterations produce no LP files.  Useful for diagnosing residual
  /// infeasibilities that surface only on the simulation pass after
  /// the training cuts have matured — without flooding the log
  /// directory with hundreds of intermediate iter-N forward LPs that
  /// the training loop already considered "transient" and recovered
  /// from.  Combine with `lp_debug_scene_min/max` and
  /// `lp_debug_phase_min/max` for a tightly scoped capture.
  bool lp_debug_simulation_only {false};

  /// Enable the monitoring API: write a JSON status file after each
  /// iteration and periodically update real-time workpool statistics.
  /// Consumers (e.g. sddp_monitor.py) can poll this file to display
  /// live charts.  Default: true.
  bool enable_api {true};

  /// Path for the JSON status file.  If empty, the solver writes to
  /// "<output_directory>/solver_status.json" (derived at solve time from
  /// the PlanningLP options).
  std::string api_status_file {};

  /// Path for the monitoring API stop-request file.  When this file
  /// exists the solver stops gracefully after the current iteration and
  /// saves cuts, exactly like the sentinel_file mechanism.  The file is
  /// written by the webservice soft-stop endpoint as part of the
  /// bidirectional monitoring API.  Use sddp_file::stop_request
  /// ("sddp_stop_request.json") as the filename in the output directory.
  /// Empty = feature disabled.
  std::string api_stop_request_file {};

  /// Interval at which the background monitoring thread refreshes
  /// real-time workpool statistics (CPU load, active workers) in the
  /// status file.
  std::chrono::milliseconds api_update_interval {500};

  /// Number of apertures (hydrological realisations) to solve in each
  /// backward-pass phase.  Each aperture clones the phase LP and updates
  /// the flow column bounds to the corresponding scenario's discharge
  /// values, then solves the clone to obtain an independent Benders cut.
  /// The final cut added to the previous phase is the
  /// probability-weighted average of all aperture cuts (expected cut).
  /// Aperture UIDs for the backward pass.
  ///
  ///  nullopt -- use per-phase `Phase::apertures` or simulation-level
  ///            `aperture_array` (default behaviour).
  ///  empty   -- no apertures; use pure Benders backward pass.
  ///  [1,2,3] -- use exactly these aperture UIDs, overriding per-phase
  ///             sets.
  ///
  /// When a non-empty list is given but no matching `Aperture` definitions
  /// exist in `simulation.aperture_array`, synthetic apertures are built
  /// from scenarios whose UIDs match.
  ///
  /// Note: apertures only update flow column bounds (affluent values).
  /// Other stochastic parameters (demand, generator profiles) are not
  /// updated.  State variable bounds remain fixed at the forward-pass
  /// trial values.
  std::optional<std::vector<Uid>> apertures {};

  /// First-N selector applied to each phase's `Phase::apertures` list.
  ///
  /// - `nullopt`  – no truncation; use the full per-phase list (default).
  /// - `0`        – no apertures (pure Benders).
  /// - `N > 0`    – take `Phase::apertures.first(N)` per phase.
  ///
  /// Pairs with the wettest-first sort applied by `plp2gtopt` to
  /// `Phase::apertures`: `num_apertures = N` picks the N wettest
  /// apertures per phase.  Each cascade level can override this value
  /// (e.g. L0 = 4, L1 = 8, L2 absent → all).
  ///
  /// `apertures` and `num_apertures` compose: truncation happens on
  /// `Phase::apertures` first, then each surviving UID is resolved
  /// against the (possibly whitelisted) aperture pool.
  std::optional<int> num_apertures {};

  /// Selection rule used by `num_apertures` to pick a subset from
  /// each phase's `Phase::apertures` list.
  ///
  /// - `head` (default): first N entries = N wettest per phase.
  /// - `stride`: N entries evenly spaced across the full list.
  ///
  /// See `ApertureSelectionMode` for details.  Ignored when
  /// `num_apertures` is `nullopt` or ≥ the per-phase list length.
  ApertureSelectionMode aperture_selection_mode {ApertureSelectionMode::head};

  /// Timeout in seconds for individual aperture LP solves in the backward
  /// pass.  When an aperture LP exceeds this time, it is treated as
  /// infeasible (skipped), a WARNING is logged, and the solver continues
  /// with the remaining apertures.  0 = no timeout (default).
  double aperture_timeout {0.0};

  /// Save LP files for infeasible apertures to log_directory (default:
  /// false).
  bool save_aperture_lp {false};

  /// Use the manual `clone_from_flat` route for aperture clones instead
  /// of the backend's native `clone()` (`CPXcloneprob`).  Manual route
  /// uses only env-local solver calls and does NOT acquire
  /// `s_global_clone_mutex`, so 80 aperture clones can be built in
  /// parallel rather than one-at-a-time under the lock.  Pre-condition:
  /// the source phase LP must hold a decompressed `FlatLinearProblem`
  /// snapshot (typical for `low_memory_mode = compress` SDDP runs).
  /// When the pre-condition is not met, the call site falls back to
  /// the native route.  Default: false (preserve legacy behaviour).
  bool aperture_use_manual_clone {false};

  /// Apertures solved serially per backward-pass task (chunked aperture
  /// pass).  Each chunk is one work-pool task that clones the phase LP
  /// once and solves its inner apertures in series with warm-start
  /// reuse on the shared clone.
  ///
  /// Sentinel values (resolved at SDDPMethod setup):
  ///
  ///   *  0 → auto.  Picked by
  ///         `compute_auto_aperture_chunk_size(A_max, S, C,
  ///         parallel_factor=2.0)`.
  ///   *  1 → legacy: one task per aperture (no chunking).
  ///   * >1 → use exactly this many apertures per task.
  ///   * -1 → cap at A_max per phase (single task per scene, fully
  ///         serial inside).
  ///
  /// Default 0 (auto).  See `sddp_options.hpp::aperture_chunk_size` for
  /// the JSON-facing field documentation.
  int aperture_chunk_size {0};

  /// Aperture solve / cut-recovery mode: `cold` (barrier + crossover,
  /// vertex reduced-cost cuts), `warm` (warm simplex off the resident
  /// chunk basis; default), `reduced_cost` (barrier without crossover,
  /// interior-point reduced-cost cuts), `dual_shared` (representative
  /// solve + Infanger–Morton dual-shared synthesis), or `screened`
  /// (`dual_shared` + exact re-solve of the top `aperture_screen_count`
  /// cuts by |correction|).  See
  /// `sddp_options.hpp::aperture_solve_mode` for the full rationale.
  ApertureSolveMode aperture_solve_mode {ApertureSolveMode::warm};

  /// Number of dual-shared aperture cuts re-solved exactly under
  /// `aperture_solve_mode = screened` (picked by largest |intercept
  /// correction|).  Ignored by every other mode.  Default 2.  See
  /// `sddp_options.hpp::aperture_screen_count`.
  int aperture_screen_count {2};

  /// Seed each iteration's first backward aperture from the previous
  /// iteration's first-aperture basis (dual warm start).  Orthogonal to
  /// `aperture_solve_mode`; only acts when the mode is `cold`/`warm`.
  /// Default false.  See `sddp_options.hpp::aperture_seed_basis`.
  bool aperture_seed_basis {false};

  /// Cross-pass simplex-basis warm-start reuse between the forward and
  /// backward passes.  Default `off`.  See `sddp_options.hpp::basis_cross_mode`
  /// and `BasisCrossMode` for the full rationale.
  BasisCrossMode basis_cross_mode {BasisCrossMode::full_cross};

  /// Enable warm-start optimizations for SDDP resolves (forward pass,
  /// backward pass, apertures, elastic filter).  When true, resolves use
  /// dual simplex + no presolve, pivoting from the saved forward-pass
  /// solution.  Especially important when the initial solve uses barrier
  /// (the default).
  bool warm_start {true};

  /// Maximum number of retained cuts per (scene, phase) LP after pruning.
  /// 0 = unlimited (default, no pruning).  When non-zero, at every
  /// cut_prune_interval iterations the solver removes inactive cuts
  /// (|dual| < prune_dual_threshold) until at most max_cuts_per_phase
  /// active cuts remain.
  int max_cuts_per_phase {0};

  /// Number of iterations between cut pruning passes.
  /// Only used when max_cuts_per_phase > 0.  Default: 10.
  int cut_prune_interval {10};

  /// Dual-value threshold for considering a cut inactive.
  /// Cuts with |dual| below this value are candidates for removal.
  /// Default: 1e-8.
  double prune_dual_threshold {1e-8};

  /// Use single cut storage: store cuts only in per-scene vectors.
  /// Combined storage for persistence is built on demand from the
  /// per-scene vectors.  Halves the memory cost of stored cuts.
  /// Default: false (backward compatible).
  bool single_cut_storage {false};

  /// Maximum total stored cuts per scene (0 = unlimited).  When
  /// non-zero, the oldest cuts beyond this limit are dropped after
  /// each iteration.  Default: 0 (no cap).
  int max_stored_cuts {0};

  /// Low memory mode: off (default), snapshot, compress, or rebuild.
  /// Trades CPU time (reconstruction + optional decompression, or full
  /// re-flatten under `rebuild`) for significant memory savings on
  /// large problems.  Under `rebuild` the initial up-front build loop
  /// is skipped and each per-(scene, phase) LP is built lazily inside
  /// the same task that solves or clones it.
  LowMemoryMode low_memory_mode {LowMemoryMode::off};

  /// In-memory compression codec for low_memory compress mode.
  /// Default: auto_select (picks best available: lz4 > snappy > zstd > gzip).
  CompressionCodec memory_codec {CompressionCodec::auto_select};

  /// CSV file with boundary (future-cost) cuts for the last phase.
  ///
  /// These cuts approximate the expected future cost beyond the planning
  /// horizon, analogous to PLP's "planos de embalse" (reservoir
  /// future-cost function).  Each cut has the form:
  ///   a >= rhs + S_i coeff_i . state_var_i
  ///
  /// The CSV header row names the state variables (reservoirs /
  /// batteries).  The solver maps these names to LP columns in the last
  /// phase and adds each cut as a lower-bound constraint on the future
  /// cost variable a.  Empty = no boundary cuts.
  std::string boundary_cuts_file {};

  /// How boundary cuts are loaded:
  /// - "noload"    -- skip loading even if a file is specified
  /// - "separated" -- assign each cut to the scene matching its `scene`
  ///                 column (scene UID); unmatched UIDs are skipped
  /// - "combined"  -- broadcast all cuts to all scenes
  /// Default: "separated".
  BoundaryCutsMode boundary_cuts_mode {BoundaryCutsMode::separated};

  /// Maximum number of SDDP iterations to load from the boundary cuts
  /// file.  Only cuts from the last N distinct iterations (by the
  /// `iteration` column / PLP IPDNumIte) are retained.  0 = load all.
  int boundary_max_iterations {0};

  /// How to handle cut rows referencing state variables not in the model.
  MissingCutVarMode missing_cut_var_mode {MissingCutVarMode::skip_coeff};

  /// Apply an α-rebase (mean-shift) to boundary cuts on load.
  ///
  /// When `true`, per scene: subtract the per-scene mean from every
  /// boundary cut's `lowb`, and carry the offset via
  /// `LinearInterface::add_obj_constant` so `get_obj_value()` stays
  /// algebraically equivalent to the unshifted formulation.
  /// Mathematically exact (α' = α − c̄); LP-side α is centred near
  /// zero so equilibration sees comparable RHS magnitudes for
  /// boundary vs. runtime cuts.  See `SddpOptions::boundary_cuts_mean_shift`
  /// (input-side option, defaulted to true via
  /// `planning_options_lp.hpp::sddp_boundary_cuts_mean_shift`) and
  /// `source/sddp_boundary_cuts.cpp` for the implementation.  Plain-bool
  /// storage default is `true` so callers that construct an
  /// `SddpOptions` directly (test fixtures, etc.) inherit the same
  /// default as the input-option path.
  bool boundary_cuts_mean_shift {true};

  // ``named_cuts_file`` was retired in 2026-05.  Internal hot-start
  // cuts now use the typed Parquet path (``cuts_input_file``); the
  // legacy CSV-with-column-per-state-variable format is no longer
  // supported.

  // ── Secondary (stationary gap) convergence ────────────────────────────

  /// Tolerance for the secondary stationary-gap convergence criterion.
  ///
  /// When the relative change in the convergence gap over the last
  /// `stationary_window` iterations falls below this value, the solver
  /// declares convergence even if the gap is above `convergence_tol`.
  /// This handles problems where the SDDP gap converges to a non-zero
  /// stationary value due to stochastic noise or problem structure
  /// (a known theoretical limitation of SDDP/Benders on certain
  /// programs).
  ///
  /// Criterion (after min_iterations and stationary_window iters done):
  ///   gap_change = |gap[i] - gap[i - window]|
  ///              / max(1e-10, gap[i - window])
  ///   gap_change < stationary_tol -> declare convergence
  ///
  /// Convergence criterion mode.  Default: gap_stationary — the
  /// primary gap test plus the stationary safety net are enough for
  /// most users.  The statistical CI test (``ConvergenceMode::statistical``)
  /// is opt-in because heterogeneous-scene σ scatter (Maule/juan) can
  /// trivially fire it at large %gap.
  ConvergenceMode convergence_mode {ConvergenceMode::gap_stationary};

  /// Tolerance for "gap stopped moving" detection (gap_change < tol).
  /// Tighter than ``convergence_tol`` so the safety net only fires when
  /// the gap has *clearly* stalled, not while it is still trending.
  /// Default: 0.005 (0.5%).  Set to 0.0 to disable.
  double stationary_tol {0.005};

  /// Number of iterations to look back when checking gap stationarity.
  /// Only used when stationary_tol > 0.0.  Default: 4 (one season).
  int stationary_window {4};

  /// Absolute gap ceiling above which the secondary tests (stationary +
  /// statistical CI) are SUPPRESSED.  Even when ``gap_change <
  /// stationary_tol`` or the CI bound passes, convergence will NOT be
  /// declared while ``gap >= stationary_gap_ceiling`` — only the primary
  /// ``convergence_tol`` test can close the run at high gaps.
  ///
  /// Defends against two known pathologies:
  ///  * Frozen-LB (LB stuck near 0, UB > 0 → gap = 1 flat) where
  ///    gap_change = 0 used to spuriously declare convergence at 100 %.
  ///  * Heterogeneous-scene σ explosion where the CI threshold z·σ
  ///    swallows the absolute gap even at 25 % relative (juan run
  ///    2026-05-02 trace_28 — see ``z_score_for_alpha`` doc).
  ///
  /// Default: 0.05 (5 %) — refuse to declare "converged via stationary
  /// or CI" until we are at least within 5 %.  Set to 1.0 to disable
  /// the ceiling (legacy behaviour).
  double stationary_gap_ceiling {0.05};

  /// Confidence level for statistical convergence criterion (0-1).
  /// When > 0 and multiple scenes exist, convergence is also checked
  /// via confidence interval: UB - LB <= z_{α/2} · σ (PLP-style).
  /// Combined with ``stationary_tol``, declares convergence when the
  /// gap stabilises above the CI threshold (non-zero-gap case).
  ///
  /// Default: 0.0 (DISABLED) — the CI test is opt-in because under
  /// heterogeneous-scene scatter (σ ≈ 50 % of mean on juan) z·σ
  /// trivially exceeds the absolute gap at 25 % relative, declaring
  /// premature convergence.  Set to 0.95 to enable the standard 95 %
  /// CI test, or 0.50 for a much tighter narrow-CI variant.
  double convergence_confidence {0.0};

  /// Number of consecutive iterations of structural infeasibility
  /// (Chinneck filter produces no useful fcut at the same scene)
  /// before the scene is marked terminal and skipped at dispatch
  /// until fresh cuts arrive globally.  Set to 0 to disable the
  /// terminal-skip mechanism entirely (legacy behaviour: every
  /// failed scene is re-dispatched every iteration).  Default: 2 —
  /// one bad iteration may be transient (state still settling), but
  /// two identical structural failures in a row indicate an
  /// LP-level issue that more iterations of the same forward-pass
  /// cannot resolve.  Observed on juan/gtopt_iplp 2026-05-02
  /// trace_29: 10 of 16 scenes wasted ~33 min before this guard
  /// existed.
  int terminal_failure_threshold {2};

  /// Optional LP solver options for the forward pass.
  /// When set, these override the global solver options for forward-pass
  /// solves.  The options are pre-merged with the global solver options
  /// at construction time (forward takes precedence).
  std::optional<SolverOptions> forward_solver_options {};

  /// Optional LP solver options for the backward pass.
  /// When set, these override the global solver options for
  /// backward-pass solves.  The options are pre-merged with the global
  /// solver options at construction time (backward takes precedence).
  std::optional<SolverOptions> backward_solver_options {};

  /// SDDP work pool CPU over-commit factor.  Multiplied by
  /// hardware_concurrency to set max pool threads.  Default 4.0 — extra
  /// threads keep CPUs busy while others block on the clone mutex.
  ///
  /// When the user did NOT set this explicitly (`pool_cpu_factor_user_set
  /// == false`), `SDDPMethod::effective_pool_cpu_factor()` caps it to 1.0
  /// for many-scene runs (see that helper for the threshold + rationale):
  /// the scene×aperture backward tasks already saturate every core, so the
  /// 4× over-commit only adds scheduler/futex contention.
  double pool_cpu_factor {4.0};

  /// True when the user explicitly set `pool_cpu_factor` (via `--cpu-factor`
  /// or `sddp_options.pool_cpu_factor` in JSON).  When true the value above
  /// is honored verbatim; when false the scene-aware auto-cap may lower it.
  bool pool_cpu_factor_user_set {false};

  /// Process memory limit in MB for the SDDP work pool.
  /// When non-zero, the pool blocks task dispatch if process RSS exceeds
  /// this value.  0 = no limit (default).
  double pool_memory_limit_mb {0.0};

  /// Maximum iteration spread between fastest and slowest scene when
  /// cut_sharing == none and multiple scenes exist.  When > 0, the
  /// solver runs scenes asynchronously: each scene progresses through
  /// its own forward/backward iteration loop, and the pool's priority
  /// queue (SDDPTaskKey) naturally gives higher priority to scenes at
  /// earlier iterations, self-regulating the spread.
  /// 0 = synchronous (current behavior, default).
  int max_async_spread {0};
};

// ─── Iteration result ───────────────────────────────────────────────────────

/// Result of a single SDDP iteration (forward + backward pass)
struct SDDPIterationResult
{
  IterationIndex iteration_index {};  ///< Iteration number (0-based)
  double lower_bound {};  ///< Lower bound (phase 0 obj including a)
  double upper_bound {};  ///< Upper bound (sum of actual phase costs)
  double gap {};  ///< Relative gap: (UB - LB) / max(1, |UB|)
  /// Relative change in **UB** vs. ``stationary_window`` iterations ago:
  ///
  ///     gap_change = |UB[i] - UB[i - window]| / max(eps, |UB[i - window]|)
  ///
  /// Under multi-cut / aperture-mode SDDP the cuts can over-tighten the
  /// master LB above the true optimum, producing a negative ``gap``
  /// (LB > UB) that oscillates while the cut family is still being
  /// refined.  The relative-Δgap stationarity test that previously
  /// drove this field was confounded by the LB drift (Δgap_relative
  /// explodes near gap = 0 because of the small denominator).  UB is
  /// the unbiased Monte-Carlo estimate of the realised policy cost —
  /// it is what we are optimising — so stationarity of **UB** is the
  /// signal that actually says "the policy has stopped moving".
  /// Populated only when ``stationary_tol > 0`` and enough iterations
  /// have elapsed; 1.0 otherwise (meaning "not yet checked / not
  /// applicable"), so the convergence test naturally skips early
  /// iters that do not yet have a usable look-back window.
  double gap_change {1.0};
  bool converged {};  ///< True if gap < convergence tolerance
  /// True when convergence was declared by the stationary-gap criterion
  /// (gap_change < stationary_tol) rather than the primary criterion.
  bool stationary_converged {};
  /// True when convergence was declared by the statistical CI criterion
  /// (|UB - LB| <= z_{a/2} * s / sqrt(N)) rather than the primary
  /// criterion.
  bool statistical_converged {};
  int cuts_added {};  ///< Number of Benders cuts added this iteration
  bool feasibility_issue {};  ///< True if elastic filter was activated

  /// Wall-clock time in seconds for the forward pass (all scenes).
  double forward_pass_s {};
  /// Wall-clock time in seconds for the backward pass (all scenes).
  double backward_pass_s {};
  /// Total wall-clock time in seconds for this iteration.
  double iteration_s {};

  /// Number of successful elastic-filter solves this iteration.
  /// Each elastic-filter solve corresponds to an LP infeasibility event;
  /// in the backward pass these become Benders feasibility cuts.
  int infeasible_cuts_added {};

  /// Per-scene upper bounds (forward-pass costs).  Size = num_scenes.
  std::vector<double> scene_upper_bounds {};
  /// Per-scene lower bounds (phase-0 objective values).  Size =
  /// num_scenes.
  std::vector<double> scene_lower_bounds {};
  /// Per-scene feasibility flag (1 = feasible, 0 = infeasible).
  /// Mirror of ``ForwardPassOutcome::scene_feasible``.  Size =
  /// num_scenes when populated, empty otherwise (e.g. async mode
  /// where scenes complete out-of-order).  Surfaces in the
  /// per-iteration headline as ``feasible=K/N`` so operators see
  /// the fraction of the original problem the bounds actually
  /// cover — UB / LB are computed *only over feasible scenes*
  /// (renormalising probability weights), so a low K/N means the
  /// gap reflects a sub-problem, not the original N-scene SDDP.
  std::vector<uint8_t> scene_feasible {};

  /// Total original probability mass of scenes that were marked
  /// infeasible this iteration.  Computed as
  /// ``Σ_{infeasible i} p_orig_i / Σ_all p_orig_j`` so the value is
  /// always in [0, 1] regardless of how the user normalised the
  /// scenario probability_factors.  Lets downstream tooling and
  /// solver-status JSON readers see "47 % of probability mass is
  /// missing from this iteration's bounds" without recomputing
  /// from scene_feasible.  Stays 0 when every scene is feasible.
  double scene_probability_lost {};

  /// Per-scene iteration at the time this result was computed.
  /// Populated only in async mode (max_async_spread > 0).  Shows the
  /// iteration each scene had completed when this aggregate convergence
  /// check was triggered.  Size = num_scenes when populated, empty
  /// otherwise.
  std::vector<int> scene_iterations {};
};

// ─── Utility free functions (independently testable) ────────────────────────

/// Compute normalised per-scene probability weights.
///
/// For each scene: weight = sum of scenario probability_factors if
/// positive, else 1.0.  Infeasible scenes (scene_feasible[si]==0) get
/// weight 0.  Weights are normalised to sum to 1 across feasible scenes.
/// Falls back to equal weights when no positive probabilities are found.
///
/// @param scenes         The scene objects from SimulationLP
/// @param scene_feasible Per-scene feasibility flag (0 = infeasible);
///                       output size equals scene_feasible.size()
/// @param rescale_mode   When `runtime`, normalize weights over feasible
///                       scenes to sum 1.0.  When `build` or `none`, use
///                       raw probability weights (no re-normalization).
/// @returns Weight vector of size scene_feasible.size()
[[nodiscard]] std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible,
    ProbabilityRescaleMode rescale_mode = ProbabilityRescaleMode::runtime);

/// Compute relative convergence gap: (UB - LB) / max(1.0, |UB|).
/// Always returns a non-negative value.
[[nodiscard]] double compute_convergence_gap(double upper_bound,
                                             double lower_bound) noexcept;

/// Look up the alpha (future-cost) state variable registered by
/// `SDDPMethod::initialize_alpha_variables` for the given (scene, phase).
/// Returns `nullptr` for any phase that has not yet been initialised.
///
/// Callers should read the alpha column index freshly via
/// `svar->col()` and the forward-pass trial value via `svar->col_sol()`,
/// which is populated by `capture_state_variable_values` after every
/// successful solve.  This replaces the former `PhaseStateInfo::alpha_col`
/// cache, which could become stale on low_memory reconstruct/clone paths.
/// @param kind  Which registry to look in — `forward` (default) for the
///              regular system's α, `aperture` for the backward-pass
///              aperture system's α (registered in parallel).
[[nodiscard]] const StateVariable* find_alpha_state_var(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index,
    SystemKind kind = SystemKind::forward) noexcept;

/// Multicut overload: look up the α column dedicated to @p source_scene
/// within the (scene_index, phase_index) cell.  Under
/// `CutSharingMode::multicut` each scene-LP carries N α columns
/// (`varphi_0..N-1`), keyed by `uid = sddp_alpha_uid + source_scene`; a
/// scenario-s backward cut is installed on `varphi_s` in EVERY destination
/// scene-LP (never the destination's own α).  For the single-α modes the
/// only registered column is `source_scene == scene_index` (uid offset 0),
/// so passing `source_scene = scene_index` reproduces the legacy lookup.
[[nodiscard]] const StateVariable* find_alpha_state_var(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index,
    SceneIndex source_scene,
    SystemKind kind = SystemKind::forward) noexcept;

/// Look up the USER-authored α (future-cost) state variable for the dynamic
/// `AmplFutureCost` recourse (piece 5 step 2c).  Unlike `find_alpha_state_var`
/// (which is keyed on the built-in `sddp_alpha_lp_class`), this resolves the
/// user's global `state`/`link` `DecisionVariable` registered under
/// `DecisionVariableLP::StateClassName` ("UserStateVar") with `key.uid ==
/// user_alpha_uid`.  Returns `nullptr` when no such state variable exists on
/// `(scene_index, phase_index, kind)`.  A separate function — NOT an overload —
/// because the built-in overloads are class-keyed; keeping the intent explicit
/// avoids ambiguity at the call sites that must dispatch on
/// `has_active_use_user_alpha`.
[[nodiscard]] const StateVariable* find_user_alpha_state_var(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index,
    Uid user_alpha_uid,
    SystemKind kind = SystemKind::forward) noexcept;

/// Enumerate every α (future-cost) column registered on
/// `(scene_index, phase_index, kind)`, in source-scene order, as
/// `(col, uid)` pairs.  Single-α modes yield exactly one entry (uid
/// `sddp_alpha_uid`, offset 0); `CutSharingMode::multicut` (or terminal
/// `BoundaryCutSharingMode::multicut`) yields N entries
/// `varphi_0..N-1` (uid = `sddp_alpha_uid + source_scene`).  α uids are
/// contiguous from `sddp_alpha_uid`, so enumeration stops at the first
/// registry gap.  Shared by `apply_alpha_floor` and `bound_alpha_for_cut`
/// so the per-`varphi_s` iteration lives in one place.
[[nodiscard]] std::vector<std::pair<ColIndex, Uid>> alpha_cols_on_cell(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index,
    SystemKind kind = SystemKind::forward);

/// THE single source of truth for the objective weight applied to every
/// α (future-cost) column on a scene-LP — shared by
/// `register_alpha_variables` (column pricing) and the forward-pass UB
/// strip so the two can never diverge.
///
/// Pricing rule (Prop. M4, `docs/formulation/sddp-cut-validity.md` §8;
/// ledger §1.2):
///
///  * single-α layouts (`n_alpha <= 1`): weight `1.0` — α carries the
///    full cost-to-go (legacy behaviour, byte-identical).
///  * multicut layouts (`n_alpha == N > 1`): EVERY `varphi_r`
///    (r = 0..N-1) in scene-s's LP is priced at `w_r = p_s` — the
///    normalized probability of the scene that OWNS the LP, uniform
///    across the N columns (NOT `p_r` per column).  With
///    `varphi_r ≈ V_r = p_r·Ṽ_r`, the scene-LP future term becomes
///    `Σ_r p_s·p_r·Ṽ_r`, i.e. after dividing by `p_s` the Bellman
///    recursion of the process resampled with measure `q_r = p_r` — a
///    certified lower bound for that process for ANY probability
///    vector.  Under uniform probabilities `p_s = 1/N` this reproduces
///    the historical `1/N` pricing exactly.
///
/// Probabilities are normalized over all scenes (`p_s =
/// scene_prob(s) / Σ_r scene_prob(r)`).  Guard: when `Σ_r prob <= 0`,
/// or the owning scene's probability is non-positive (a degenerate
/// zero-probability scene whose folded objective is 0 anyway), fall
/// back to the uniform `1/n_alpha` weight.
///
/// Per-column measures (Markov): `alpha_col_weights` below is the
/// mode-aware wrapper — consumers (column registration, UB strip)
/// call IT, and it delegates here for every non-Markov layout.
[[nodiscard]] double alpha_unit_cost(const SimulationLP& sim,
                                     SceneIndex scene_index,
                                     std::size_t n_alpha) noexcept;

/// THE single mode-aware accessor for the per-column objective weights
/// of the `n_alpha` α (future-cost) columns on a scene-LP cell —
/// shared by `register_alpha_variables` (column pricing) and the
/// forward-pass UB strip so the priced future term and the stripped
/// future term are structurally unable to diverge.
///
///  * `cut_sharing = markov` (non-empty @p markov config, NON-terminal
///    phase, `markov->num_states == n_alpha`): the MK1 per-state
///    weights `w_{s,m'} = p_s·P[m(s)][m']/pi_{m'}`
///    (`markov_alpha_weights`; `docs/formulation/sddp-markov.md` §2).
///  * every other layout — `none` (n_alpha = 1 → weight 1.0),
///    `multicut` (w_r = p_s), and ANY terminal phase (which follows
///    `boundary_cut_sharing`, never markov): the uniform M4 weight
///    `alpha_unit_cost(sim, scene_index, n_alpha)` replicated
///    `n_alpha` times.
///
/// Always returns exactly `n_alpha` entries.  @p markov may be null
/// (treated as empty — non-markov pricing).
[[nodiscard]] std::vector<double> alpha_col_weights(
    const SimulationLP& sim,
    CutSharingMode cut_sharing,
    const MarkovChainConfig* markov,
    SceneIndex scene_index,
    std::size_t n_alpha,
    bool terminal_phase);

/// Deterministic probability-weighted index draw for the forward-pass
/// resampling (`ForwardSamplingMode::resampled`).
///
/// Pure function of `(weights, iteration, scene, phase)` — a
/// splitmix64 hash chain mapped through the normalized weight CDF —
/// deliberately NOT a `<random>` engine/distribution pair, whose
/// output is implementation-defined across standard libraries: the
/// sampled path must be bit-reproducible everywhere, stable under
/// forward-pass BACKTRACKING (re-entering the same (iteration, scene,
/// phase) re-draws the SAME realization) and independent of thread
/// scheduling.
///
/// Degenerate inputs: an empty span returns 0; a non-positive total
/// weight falls back to a uniform draw over `weights.size()`.
/// Negative entries are clamped to 0.
[[nodiscard]] std::size_t sample_weighted_index(std::span<const double> weights,
                                                uint64_t iteration,
                                                uint64_t scene,
                                                uint64_t phase) noexcept;

/// SimulationLP-facing wrapper over `sample_weighted_index`: draws a
/// SCENE index with probability proportional to each scene's
/// `probability_factor` (sum of its scenarios'), keyed on
/// `(iteration_index, scene_index, phase_index)`.  Consumed by the
/// forward pass under `ForwardSamplingMode::resampled`; single-scene
/// simulations always return scene 0.
[[nodiscard]] SceneIndex sample_forward_realization(
    const SimulationLP& sim,
    IterationIndex iteration_index,
    SceneIndex scene_index,
    PhaseIndex phase_index) noexcept;

/// Release α's bootstrap pin (`lowb = uppb = 0`) at the given
/// `(scene, phase)` cell.  Sets `lowb = -DblMax`, `uppb = +DblMax`
/// on the live LP backend and mirrors the change into the
/// `m_dynamic_cols_` entry so release+reload replay preserves the
/// freed bounds.  Idempotent and safe to call on a cell whose α
/// has not been registered (no-op).  Shared by the SDDPMethod
/// backward/feasibility paths and the boundary-cut loader in
/// `source/sddp_cut_io.cpp` so every cut-install site uses the
/// same free-α semantics.
void bound_alpha(PlanningLP& planning_lp,
                 SceneIndex scene_index,
                 PhaseIndex phase_index,
                 SystemKind kind = SystemKind::forward);

/// Release α's bootstrap pin at `(scene, phase)` ONLY when @p cut
/// actually references α — i.e., α is registered on the cell AND
/// the cut's sparse coefficient map contains the α column.  No-op
/// otherwise.  Meant to be called at every optimality-cut install
/// site (backward-pass aperture expected-cut, aperture bcut fallback,
/// and cut-file loaders for CSV/JSON optimality cuts) so a cut that
/// does not constrain α — e.g. α coefficient filtered by
/// `cut_coeff_eps` at save time, or a pure state-coupling cut —
/// does not prematurely release the bootstrap pin.  Feasibility cuts
/// should never call this (they carry no lower-bound information on
/// the future-cost variable); callers must gate on cut type upstream.
void bound_alpha_for_cut(PlanningLP& planning_lp,
                         SceneIndex scene_index,
                         PhaseIndex phase_index,
                         const SparseRow& cut,
                         SystemKind kind = SystemKind::forward);

/// Release the bootstrap pin (`lowb = uppb = 0`) on the USER-authored α
/// (dynamic `AmplFutureCost`, piece 5 step 2c) at `(scene, phase)`.  Locates
/// the user α via `find_user_alpha_state_var` and frees its column to
/// `[-∞, +∞]` on the live backend, mirroring the change into the
/// compress-replay channel so a `release_backend` + `ensure_backend` cycle
/// preserves the freed bounds.  No-op when the user α is not registered on the
/// cell.  The user α is a single column (NOT N `varphi_s`), so this is the
/// `bound_alpha` analogue for the user-overridable FCF — it does NOT compute a
/// cut-derived floor (the user's own `UserConstraint` cuts already bound it).
void bound_user_alpha(PlanningLP& planning_lp,
                      SceneIndex scene_index,
                      PhaseIndex phase_index,
                      Uid user_alpha_uid,
                      SystemKind kind = SystemKind::forward);

/// Set the RAW `(lowb, uppb)` bounds on column @p col at `(scene, phase)` AND
/// record the change into the compress-replay channel, so a `release_backend` +
/// `ensure_backend` cycle (`LowMemoryMode::compress`) preserves the bounds.
/// Used by the user-α bootstrap pin / release (piece 5 step 2c) — a dedicated
/// helper rather than `bound_alpha` (which is keyed on the built-in α class and
/// computes a cut-derived floor).  `±DblMax` is accepted as the "unbounded"
/// sentinel (mapped to the solver's infinity via `normalize_bound`).  The
/// replay record is established by `LinearInterface::set_col_*_raw`, which
/// write the pending-col-bounds channel (keyed by column index) whenever the
/// interface is not mid-replay and not a throwaway clone.  No-op when @p col is
/// out of range or the cell's system is absent.
void record_col_bounds_dynamic(PlanningLP& planning_lp,
                               SceneIndex scene_index,
                               PhaseIndex phase_index,
                               ColIndex col,
                               double lowb,
                               double uppb,
                               SystemKind kind = SystemKind::forward) noexcept;

/// Install an SDDP cut (feasibility or optimality) on the LP backend
/// at `(scene, phase)`.  Single unified entry point for every cut
/// install site:
///   1. For optimality cuts that reference α, release α's bootstrap
///      pin via `bound_alpha_for_cut`.  Feasibility cuts, and
///      optimality cuts with no α term, leave the pin intact.
///   2. Add the row to the live LP backend and record it for
///      low-memory replay via `LinearInterface::add_cut_row`.
/// Callers that also persist the cut into `SDDPCutManager` should call
/// `SDDPMethod::store_cut(...)` afterwards using the returned
/// `RowIndex` — `store_cut` no longer re-records the cut for replay
/// (that now happens once inside this function).
[[nodiscard]] RowIndex add_cut_row(PlanningLP& planning_lp,
                                   SceneIndex scene_index,
                                   PhaseIndex phase_index,
                                   CutType cut_type,
                                   const SparseRow& cut,
                                   double eps = 0.0);

/// Register the α (future-cost) column on every (scene, phase)
/// cell of @p planning_lp that does not already have it.  Each α
/// is added as a `SparseCol` pinned at `lowb = uppb = 0` (bootstrap)
/// with `cost = scale_alpha`, mirrored into `m_dynamic_cols_` for
/// low-memory replay, and registered in the simulation-level
/// `StateVariable` map so cross-level / cut I/O machinery treats α
/// uniformly.  Cut-install sites (backward pass, boundary/named
/// cut loaders) subsequently call `bound_alpha(...)` to release the
/// pin.  Called once per scene during `SDDPMethod::initialize_solver`;
/// exposed here so isolated callers (tests, direct cut-loader
/// harnesses) can establish the same precondition without standing
/// up a full `SDDPMethod`.
/// @param register_as_state_variable  When `true` (default) each α column is
///        registered in the simulation `StateVariable` map (the byte-for-byte
///        legacy behaviour, drives cut routing + the forward-pass UB
///        estimator).  When `false` (a user-overridable FCF —
///        `FutureCost::use_user_alpha`), the α columns are still ADDED to each
///        LP but with `cost = 0` and are NOT registered as state variables, so
///        the built-in α is inert (pinned `lowb = uppb = 0`, never priced,
///        never floored, never cut) and the user-authored α + cuts fully
///        replace it.
/// @param markov  Markov-chain configuration, consulted only when
///        `cut_sharing == CutSharingMode::markov`: non-terminal phases
///        then carry `markov->num_states` α columns priced by
///        `markov_alpha_weights` (theorem MK1,
///        `docs/formulation/sddp-markov.md`).  `nullptr` / empty keeps
///        the mode-independent legacy layout.
void register_alpha_variables(PlanningLP& planning_lp,
                              SceneIndex scene_index,
                              double scale_alpha,
                              CutSharingMode cut_sharing = CutSharingMode::none,
                              BoundaryCutSharingMode boundary_cut_sharing =
                                  BoundaryCutSharingMode::per_scene,
                              bool register_as_state_variable = true,
                              const MarkovChainConfig* markov = nullptr);

/// Apply a derived lower-bound floor on α_T at the last phase by
/// projecting every installed boundary cut onto the worst-case
/// trial-state box.  For each cut
///
///     α + Σⱼ coefⱼ · vⱼ ≥ rhs
///
/// the universal floor over the feasible state-variable box
/// ``[vⱼ_min, vⱼ_max]`` is
///
///     floor_cut = rhs − Σⱼ max(coefⱼ · vⱼ_max, coefⱼ · vⱼ_min)
///
/// (when ``coefⱼ > 0`` the maximiser is ``vⱼ_max``; when ``coefⱼ < 0``
/// it is ``vⱼ_min``).  Taking the ``max`` of ``floor_cut`` across every
/// cut and clamping at ``0`` (cost-to-go is non-negative under
/// non-negative stage costs) gives the tightest universal lower bound
/// on α_T implied by the cuts alone, independent of which trial state
/// the master / aperture clones pick.
///
/// The motivation is the juan/gtopt_iplp_plain pathology: boundary
/// cuts cover only the trial-state regions they were generated at,
/// and aperture-perturbed trial states can fall outside that
/// polyhedral approximation, leaving α_T effectively unbounded below
/// in the aperture clone (CPLEX returns ``CPX_STAT_UNBOUNDED``,
/// logged as "infeasible (status 2)" by the aperture pass).  Pinning
/// the column at the cut-derived floor closes that hole without
/// over-tightening (boundary cuts that produce a tighter row-based
/// bound still dominate).
///
/// The floor is seeded at ``0`` before any cut is examined and only
/// ratchets UP from there, so the function is safe to call even when
/// no cuts (or no α-referencing cuts) are installed at the last phase
/// — the result in that case is the weak universal floor ``α_T ≥ 0``,
/// which always holds under non-negative stage costs and prevents
/// α_T from going unbounded below in aperture clones.  No-op only
/// when α is not registered at the last phase on @p scene_index (a
/// pre-`register_alpha_variables` call site).  The mirror
/// `update_dynamic_col_bounds` is called so the floor survives a
/// `release_backend` + `ensure_backend` cycle under
/// `LowMemoryMode::compress`.
///
/// **Unit convention.** All arithmetic in this helper runs in
/// **physical-space** units:
///   * `LinearInterface::active_cuts()` returns rows captured by
///     `record_cut_row` BEFORE `compose_physical` mutated them — so
///     ``cut.lowb`` is ``rhs_phys`` and ``cut.cmap[col]`` is
///     ``coef_phys``.
///   * State-variable column bounds are read via
///     `get_col_low()` / `get_col_upp()` (ScaledView returning
///     ``raw × col_scale``), so the multiplication
///     ``coef_phys · v_phys`` lands in the same units as
///     ``rhs_phys``.
///   * The floor is written via the **physical** setter
///     `set_col_low(α_col, floor_phys)` (which divides by α's
///     ``col_scale = scale_alpha`` internally to land raw in the
///     backend) and mirrored to `update_dynamic_col_bounds` with the
///     same physical floor (``SparseCol.lowb`` is physical — see
///     `LinearProblem::flatten`).  Both representations stay
///     coherent across `release_backend` / `ensure_backend` cycles.
void apply_terminal_alpha_floor(PlanningLP& planning_lp,
                                SceneIndex scene_index);

/// Generalised cut-derived α floor for an arbitrary phase.
///
/// Same projection logic as `apply_terminal_alpha_floor` but
/// parametrised on the phase index and on the seed (universal weak
/// floor that always holds before any cut is examined).  The terminal
/// helper is a one-line wrapper around this with
/// ``phase_index = sim.last_phase_index()`` and ``seed_phys = 0.0``.
///
/// Exposed so a follow-up can refresh the floor at every interior
/// (scene, phase) cell after a new optimality cut is installed —
/// closing the same `CPX_STAT_UNBOUNDED` window at intermediate
/// phases whose backward cuts cover only a sub-region of the
/// post-aperture trial-state polytope.  See plan §4 in the issue
/// thread for the wiring proposal (`SDDPOptions::alpha_floor_mode`).
///
/// @param planning_lp Owning planning LP carrying the per-cell
///                    LinearInterface / SystemContext.
/// @param scene_index Target scene cell.
/// @param phase_index Target phase cell.  No-op when α is not
///                    registered on (scene_index, phase_index).
/// @param seed_phys   Weak universal floor used to seed the
///                    cut-driven max.  Defaults to ``0`` (valid under
///                    non-negative stage costs).
/// @param bound_above When true, ALSO clamp α's column UPPER bound to the
///                    cut-derived ceiling
///                    ``ceil = rhs − Σⱼ min(coefⱼ·vⱼ_max, coefⱼ·vⱼ_min)``
///                    (symmetric to the floor), turning α from a one-sided
///                    / free column into a finite box ``[floor, ceil]``.
///                    Only safe where the full cut set is already installed
///                    (monolithic boundary-cut load) — NOT the SDDP
///                    per-cut path, where a later cut can legitimately push
///                    α above a ceiling derived from earlier cuts.  Both
///                    bounds are in the rebased-physical frame (``cut.lowb``
///                    already carries the mean-shift offset ``c``) and are
///                    written via ``set_col_low/upp`` which divide by the
///                    α column's ``col_scale`` (= ``scale_alpha``), so the
///                    offset and α-scale are both accounted for.  Defaults
///                    to false (SDDP behaviour unchanged: upper stays +∞).
void apply_alpha_floor(PlanningLP& planning_lp,
                       SceneIndex scene_index,
                       PhaseIndex phase_index,
                       SystemKind kind = SystemKind::forward,
                       bool bound_above = false);

// ─── Per-phase tracking ─────────────────────────────────────────────────────

/// Per-phase SDDP state: a variable, outgoing links, forward-pass cost.
///
/// Per-state-variable trial values (consumed by cut building and
/// next-phase propagation) live on `StateVariable::col_sol()`, populated
/// by `capture_state_variable_values` after every forward solve.
struct PhaseStateInfo
{
  std::vector<StateVarLink> outgoing_links {};  ///< Links TO next phase
  size_t base_nrows {0};  ///< Row count before any Benders cuts
  double forward_objective {0.0};  ///< Opex from last forward pass
  /// Full objective from last forward solve (including α), in physical
  /// ($) space — i.e. `LinearInterface::get_obj_value()`, not
  /// the scaled LP raw value.  Cached here so the backward pass can
  /// call `build_benders_cut_physical` without re-querying the
  /// original LP.
  double forward_full_obj_physical {0.0};
  /// Cross-iteration warm-start seed: the simplex basis captured from this
  /// cell's FIRST backward aperture last iteration.  Empty until first
  /// captured; only populated/consumed when `aperture_seed_basis` is on.
  /// One per `(scene, phase)` cell — `O(cells)` memory, not per-aperture.
  Basis aperture_warm_basis {};
  /// Cross-pass warm-start seed: the simplex basis captured from this cell's
  /// FORWARD-pass solve last iteration, reused to warm-start the backward
  /// dual/aperture solve (`BasisCrossMode::forward_to_backward`/`full_cross`).
  /// Empty until first captured; only populated/consumed when
  /// `basis_cross_mode` requests forward→backward reuse.  `O(cells)` memory.
  Basis forward_basis {};
  /// Cross-pass warm-start seed: the simplex basis captured from this cell's
  /// BACKWARD-pass solve last iteration, reused to warm-start the next
  /// forward solve (`BasisCrossMode::backward_to_forward`/`full_cross`).
  Basis backward_basis {};
  /// Forward-sampling realization cache (`ForwardSamplingMode::resampled`):
  /// the scene whose scenario data is currently pinned onto this cell's
  /// forward-LP stochastic bounds — drawn by `sample_forward_realization`
  /// in the forward pass and re-applied by the backward pass's target
  /// re-solve so the cut is provably built from the SAME realization the
  /// forward pass simulated.  `nullopt` under `persistent` (never
  /// written — byte-identical default path) and after the simulation
  /// pass restores the scene's own data.
  std::optional<SceneIndex> sampled_scene {};
};

// ─── Callback / observer API ────────────────────────────────────────────────

/// Callback invoked after each SDDP iteration.
/// If the callback returns `true`, the solver stops after this iteration.
using SDDPIterationCallback =
    std::function<bool(const SDDPIterationResult& result)>;

/// Outcome of running the forward pass across all scenes
struct ForwardPassOutcome
{
  std::vector<double> scene_upper_bounds {};
  std::vector<uint8_t> scene_feasible {};
  int scenes_solved {0};
  bool has_feasibility_issue {false};
  /// Total feasibility cuts (including multi-cut bound rows) installed
  /// across all scenes in this pass.  When zero and
  /// has_feasibility_issue is true, retrying will produce the same
  /// result — the caller's retry loop should break.
  std::size_t n_fcuts_installed {0};
  double elapsed_s {0.0};
};

/// Outcome of running the backward pass across all scenes
struct BackwardPassOutcome
{
  int total_cuts {0};
  bool has_feasibility_issue {false};
  double elapsed_s {0.0};
};

}  // namespace gtopt
