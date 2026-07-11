/**
 * @file      benders_cut.hpp
 * @brief     Modular Benders cut construction and handling
 * @date      2026-03-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module extracts the cut-creation logic from the SDDP solver into
 * standalone, testable functions and a class-based interface.
 *
 * ## Free functions
 *
 * - `build_benders_cut()`        – standard optimality cut from reduced costs
 * - `relax_fixed_state_variable()` – elastic-filter column relaxation
 * - `build_feasibility_cut()`    – clone → relax → solve → extract cut
 * - `build_multi_cuts()`         – per-slack bound-constraint cuts
 * - `weighted_average_benders_cut()` – probability-weighted average
 *
 * ## BendersCut class
 *
 * `BendersCut` wraps the free functions as member functions and adds:
 * - An optional `AdaptiveWorkPool` for LP solve/resolve operations.
 *   When a pool is provided, the elastic-filter LP solve is submitted to
 *   the pool; otherwise it is performed synchronously.
 * - An infeasible-cut counter: every successful elastic-filter solve
 *   (i.e. every LP infeasibility event handled by the filter) is counted.
 *   The counter can be queried for monitoring-API integration.
 *
 * The SDDP solver (`sddp_method.hpp`) re-exports the free-function symbols
 * so that existing code that includes `sddp_method.hpp` continues to compile
 * without changes.
 */

#pragma once

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

// Forward declaration to avoid including the heavy work_pool.hpp header.
// Consumers that need the full AdaptiveWorkPool definition (e.g. to call
// make_solver_work_pool) should include <gtopt/work_pool.hpp> directly.
namespace gtopt
{
class AdaptiveWorkPool;
}

namespace gtopt
{

// ─── State variable linkage ─────────────────────────────────────────────────

/// Raw-bound gap below which a dependent state column counts as PINNED
/// (`lowb ≈ uppb`, i.e. fixed to a forward trial value).  THE single
/// pin-detection epsilon shared by `relax_fixed_state_variable`, the
/// elastic-filter pre-pass (`compute_relaxation_specs`), and the
/// strengthened-cut box relaxation (`build_strengthened_benders_cut`)
/// — the strengthened-cut validity argument (theorem SB1's box domain)
/// requires all three sites to agree on what "pinned" means, so keep
/// them on this one constant.
inline constexpr double kStatePinDetectEps = 1e-10;

/**
 * @brief Describes one state-variable linkage between consecutive phases
 *
 * A state variable has a *source* column in phase t (e.g. efin, capainst)
 * and a *dependent* column in phase t+1 (e.g. eini, capainst_ini).  The
 * source column's physical bounds are captured at initialisation time so
 * the elastic filter can relax to the correct domain.
 */
struct StateVarLink
{
  ColIndex source_col {};  ///< Source column in source phase's LP
  ColIndex dependent_col {};  ///< Dependent column in target phase's LP
  PhaseIndex source_phase_index {};  ///< Phase where the source column lives
  PhaseIndex target_phase_index {};  ///< Phase where the dependent column lives
  double trial_value {0.0};  ///< Trial value from the last forward pass
  double source_low {0.0};  ///< Physical lower bound of source column
  double source_upp {0.0};  ///< Physical upper bound of source column
  /// Variable scale factor: physical = LP × var_scale.
  /// Used by the elastic filter to scale the penalty per LP unit.
  double var_scale {1.0};
  /// Per-variable state cost for elastic penalty [$/physical_unit / scale_obj].
  /// When > 0, overrides the global penalty for this link.
  /// Pre-divided by scale_objective during link construction for consistency
  /// with the global penalty (which is also pre-divided by scale_objective).
  double scost {0.0};
  /// Back-pointer to the StateVariable this link represents.
  ///
  /// The StateVariable lives in `SimulationLP::m_global_variable_map_`
  /// (a `flat_map` populated once at LP build time, never reshuffled),
  /// so its address is stable for the lifetime of the solver run — the
  /// same lifetime that already couples source and dependent LP columns.
  ///
  /// Used by the SDDP solve passes to read/write runtime values
  /// (col_sol, reduced_cost) directly on the state variable, avoiding
  /// full per-LP `std::vector<double>` snapshots.  Nullable for test
  /// fixtures that exercise cut builders in isolation.
  const StateVariable* state_var {nullptr};
  /// Identity of the state-variable element, for diagnostic logs.
  /// Captured once at link collection time from the simulation registry
  /// Key.  The string_views reference the same stable storage as the
  /// map keys, so they outlive the link for the whole solver lifetime.
  /// LP class identity stored by value.  Mirrors
  /// `StateVariable::Key::class_name` — copied from the source Key
  /// at link-collection time.  Zero-runtime access to both
  /// PascalCase `full_name()` and snake_case `short_name()`.
  LPClassName class_name {};
  std::string_view col_name {};  ///< e.g. "efin", "sini"
  Uid uid {unknown_uid};  ///< Element UID
  /// Scenario / stage identity of the producing state variable.  The same
  /// element uid + col_name is registered once per (scenario, stage) within
  /// a scene, so these are needed to map a link to its counterpart in the
  /// aperture-system state-variable registry (aperture-system cut path).
  ScenarioUid scenario_uid {unknown_uid_of<Scenario>()};
  StageUid stage_uid {unknown_uid_of<Stage>()};
  /// Human-readable element name (e.g. "LMAULE", "RALCO").  Resolved
  /// from `SimulationLP::m_ampl_element_names_` at link-collection time
  /// via a reverse scan of the (class_name, name) → uid map.  Empty
  /// when the element was never registered in the AMPL name map — in
  /// that case diagnostic logs fall back to the numeric uid.
  std::string_view name {};
};

// ─── Cut-emission box-edge bookkeeping ──────────────────────────────────────

/// Per-cut tally of how many contributing links land in each decile band of
/// the source-column box ``[source_low, source_upp]``.
///
/// Shared diagnostic structure used by both
/// ``build_feasibility_cut_physical`` (single-cut path) and
/// ``build_multi_cuts`` (per-link multi-cut path).  The same band-classifier
/// drift between the two emitters used to silently produce subtly different
/// degeneracy thresholds; centralising it here keeps the threshold tunable
/// in exactly one place.
///
/// **Decile bands** (10 % of ``box_width = source_upp − source_low``):
///   - ``upper`` ← clamped value within the top 10 % of the box
///     (``≥ source_upp − 0.10 × box_width``).
///   - ``lower`` ← clamped value within the bottom 10 % of the box.
///   - ``inside`` ← everything else (the middle 80 %).
///
/// The "all-upper" pattern (``all_upper_degenerate()`` returns true) is the
/// signature of a degenerate "every state floored at emax" cut: the master
/// cannot satisfy a floor at every reservoir's emax simultaneously in one
/// phase, so installing such a cut guarantees cascade-infeasibility on the
/// next forward attempt.  Cut emitters use this signal to drop the cut
/// rather than install a doomed retry chain.
///
/// Exposed (rather than kept anonymous) so that the band-classification
/// invariant is unit-testable in isolation, without spinning up a full
/// elastic clone.
struct BoxEdgeStats
{
  int upper {};
  int lower {};
  int inside {};
  std::string upper_names;  ///< Comma-joined element names for WARN message

  /// Total active links contributing to a cut (= ``upper + lower + inside``).
  [[nodiscard]] constexpr int total() const noexcept
  {
    return upper + lower + inside;
  }

  /// True when EVERY active link clamps at the upper edge.  The threshold
  /// of ``≥ 2 active`` keeps the warning quiet for trivial single-link
  /// cuts on a small reservoir while still flaring on the multi-reservoir
  /// degenerate cascade observed on juan/gtopt_iplp.
  [[nodiscard]] constexpr bool all_upper_degenerate() const noexcept
  {
    return total() >= 2 && upper == total();
  }

  /// Tally one link's clamp into the appropriate decile bucket.
  /// ``clamped`` is the cut's implied source value after the per-link
  /// ``std::clamp(dep_clone_phys, source_low, source_upp)``; the helper
  /// compares it to the box's top / bottom 10 % bands.  Names land in
  /// ``upper_names`` only when the link has a non-empty AMPL name —
  /// otherwise the diagnostic falls back to the numeric uid in the WARN
  /// message that consumes ``upper_names``.
  void tally(double clamped, const StateVarLink& link);
};

// ─── Elastic relaxation result ──────────────────────────────────────────────

/// Result of relaxing one state-variable column via the elastic filter.
/// Contains the relaxation status and the indices of the penalised slack
/// columns added to the LP.
struct RelaxedVarInfo
{
  bool relaxed {false};  ///< True if the column was relaxed (was fixed)
  /// Overshoot slack col (sup > 0 → solution < trial)
  ColIndex sup_col {unknown_index};
  /// Undershoot slack col (sdn > 0 → solution > trial)
  ColIndex sdn_col {unknown_index};
  /// Row index of the elastic fixing equation
  /// `dep_col + sup_col − sdn_col = trial_value` added to the clone
  /// by `relax_fixed_state_variable`.  The dual of this row at the
  /// clone's Phase-1 optimum is the Farkas ray component that becomes
  /// the feasibility-cut coefficient on `source_col` — matches
  /// `plp-agrespd.f::AgrElastici` which reads `lp->getRowPrice()[row]`.
  RowIndex fixing_row {unknown_index};

  /// Implicit bool conversion: true iff the column was relaxed.
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  operator bool() const noexcept { return relaxed; }
};

// ─── Optimality cut ─────────────────────────────────────────────────────────

/// Propagate trial values: fix dependent columns to source-column solution
/// via column bounds (lo == hi).  This is the standard SDDP bound-fixing
/// formulation: the reduced cost of the fixed column at optimum is the
/// shadow price of the implicit bound, which becomes the cut coefficient.
void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept;

/// Propagate trial values using the source StateVariable's cached
/// col_sol() instead of a full-LP solution span.  This is the only
/// overload used by production SDDP code — the older span-based overload
/// above is kept for tests that drive propagation directly.
void propagate_trial_values(std::span<StateVarLink> links,
                            LinearInterface& target_li) noexcept;

/// Physical-space Benders optimality cut builder.
///
///   α_phys ≥ z_t_phys + Σ_i rc_phys_i · (x_{t-1,i}_phys − v̂_i_phys)
///
/// Intended for use with `LinearInterface::add_equilibrated_row`, which
/// folds `col_scales` and the per-row equilibration internally.  All
/// inputs are in physical space:
///
/// @param alpha_col              α column index in the source phase's LP.
/// @param links                  State-variable linkage descriptors (same
///                               struct the LP-space overload consumes).
///                               `link.dependent_col` selects the target
///                               entry in @p reduced_costs_physical;
///                               `link.source_col` selects the matching
///                               entry in @p trial_values_physical.
/// @param reduced_costs_physical Physical reduced costs of the target
///                               LP's dependent columns — read from
///                               `target_li.get_col_cost()` (which
///                               applies `LP × scale_objective /
///                               col_scale`).
/// @param trial_values_physical  Physical trial values for each link's
///                               source column — read from
///                               `source_li.get_col_sol()[link.source_col]`
///                               (= `LP × col_scale_source`).  Indexed
///                               in parallel with `links`, so entry `i`
///                               corresponds to `links[i]`.
/// @param objective_value_physical Target subproblem optimum in $,
///                               i.e. `target_li.get_obj_value()`.
/// @param cut_coeff_eps          Drop state-var coefficients below this
///                               absolute threshold (default 0 =
///                               keep all).
///
/// Returns a SparseRow with:
///   * row[alpha_col] = 1.0
///   * row[source_col] = -rc_phys
///   * row.lowb = obj_phys − Σ rc_phys · v̂_phys
///   * row.uppb = DblMax, row.scale = 1.0
///
/// The caller is expected to pass this row to
/// `LinearInterface::add_equilibrated_row`, which applies the LP's
/// column scales and per-row row-max equilibration to produce the
/// final LP-space row.
[[nodiscard]] auto build_benders_cut_physical(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    std::span<const double> reduced_costs_physical,
    std::span<const double> trial_values_physical,
    double objective_value_physical,
    double cut_coeff_eps = 0.0) -> SparseRow;

/// Physical-space Benders cut builder that reads reduced cost and trial
/// value from each link's back-pointer state variable instead of taking
/// flat spans.  The SDDP backward pass uses this overload: the forward
/// pass already mirrors the target LP's `col_sol` / `reduced_cost` onto
/// every `StateVariable` via `capture_state_variable_values`, so the
/// builder can read the live values directly without re-taking the full
/// per-LP snapshots.
///
///   rc_phys  = state_var->reduced_cost_physical(scale_objective)
///   v̂_phys  = state_var->col_sol_physical()
///   lowb    = objective_value_physical − Σ rc_phys · v̂_phys
///   row[src_col] = −rc_phys,  row[alpha_col] = 1.0
///
/// The returned row carries no `already_lp_space` flag: `add_row` on an
/// equilibrated LP will fold `col_scales` and run per-row row-max
/// equilibration, so the resulting row is well-conditioned without any
/// post-hoc `rescale_benders_cut` pass.
///
/// @param alpha_col               α column in the source phase's LP.
/// @param links                   State-variable linkage descriptors.
///                                Links with a null `state_var` contribute
///                                zero (rc_phys = v̂_phys = 0), matching
///                                the test-fixture convention.
/// @param objective_value_physical Target subproblem optimum in $,
///                                i.e. `target_li.get_obj_value()`.
/// @param scale_objective         Global objective scale (required so
///                                `reduced_cost_physical()` can descale
///                                the LP reduced cost to $/phys_unit).
/// @param cut_coeff_eps           Drop state-var coefficients below this
///                                absolute threshold (default 0 = keep all).
[[nodiscard]] auto build_benders_cut_physical(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    double objective_value_physical,
    double scale_objective,
    double cut_coeff_eps = 0.0) -> SparseRow;

/// Physical-space Benders cut builder that reads reduced cost from an
/// arbitrary `LinearInterface` (typically an elastic clone) and trial
/// value from each link's back-pointer `StateVariable`.  Intended for
/// cut paths whose rc comes from a local LP solve rather than the base
/// forward pass — the forward-pass feasibility cut and the aperture
/// per-aperture cut both fit this shape.
///
///   rc_phys  = rc_source.get_col_cost()[link.dependent_col]
///              (`ScaledView`: `LP × scale_objective / col_scale`)
///   v̂_phys  = link.state_var->col_sol_physical()
///
/// `scale_objective` is not a parameter: it is embedded in the
/// `get_col_cost()` view.  The clone must have been solved (rc and col
/// solution available) before this call.
[[nodiscard]] auto build_benders_cut_physical(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    const LinearInterface& rc_source,
    double objective_value_physical,
    double cut_coeff_eps = 0.0) -> SparseRow;

// ─── Strengthened Benders cut (integer_cuts_mode = strengthened) ────────────

/// Result of `build_strengthened_benders_cut`.
struct StrengthenedCutResult
{
  /// The emitted cut `α + Σ(−λ_i)·x_i ≥ rhs` with
  /// `rhs = max(lp_rhs, mip_rhs)` — the LP-relaxation cut, tightened
  /// by the Lagrangian intercept when the MIP solve succeeded.
  SparseRow row {};
  /// LP-relaxation intercept `b_LP = z*_LP − ⟨λ, v̂⟩` (the certified
  /// Theorem-O1 baseline).
  double lp_rhs {};
  /// Lagrangian intercept `m* = L(λ) − ⟨λ, v̂⟩` from the single MIP
  /// solve.  Equals `lp_rhs` when the MIP was skipped / failed.
  double mip_rhs {};
  /// True when the MIP intercept was applied (`mip_rhs > lp_rhs`).
  bool tightened {false};
};

/// Strengthened Benders optimality cut for an integer-bearing target
/// subproblem (Zou, Ahmed & Sun 2019 §5; Theorem SB1 / Corollary SB2 in
/// `docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md`).
///
/// On a CLONE of @p target_li (never mutated; state pins `dep = v̂`
/// must be in place, i.e. post-`propagate_trial_values`):
///
///  1. relax integrality, solve the **LP relaxation** at v̂ →
///     `z*_LP` and pinned reduced costs λ (the sound LP-relaxation
///     duals — a MIP re-solve has none);
///  2. build the Theorem-O1 cut `α + Σ(−λ_i)x_i ≥ z*_LP − ⟨λ, v̂⟩`
///     (`cut_coeff_eps` filter as usual; dropped links get λ_i = 0);
///  3. restore integrality, relax every pinned dependent column to its
///     physical source box `[source_low, source_upp]`, charge `−λ_i`
///     on each kept dependent column's objective coefficient, and
///     solve ONE MIP.  Its physical optimum is directly the
///     strengthened intercept `m* = L(λ) − ⟨λ, v̂⟩`;
///  4. emit `rhs = max(b_LP, m*)` — valid for every state in the box
///     by weak Lagrangian duality, and never looser than the LP cut.
///
/// The strengthening MIP runs under @p mip_opts — callers should pin a
/// tight `mip_gap` (the incumbent value over-tightens by up to
/// gap × |m*|; see the investigation doc §6.3) and a `time_limit`.
///
/// @return nullopt when the LP-relaxation solve fails (caller falls
///         back to its legacy cut path).  Otherwise the cut, with
///         `tightened == false` when the MIP solve failed / timed out
///         (the row is then the plain LP-relaxation cut — still valid).
[[nodiscard]] auto build_strengthened_benders_cut(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    const LinearInterface& target_li,
    double cut_coeff_eps,
    const SolverOptions& lp_opts,
    const SolverOptions& mip_opts) -> std::optional<StrengthenedCutResult>;

/// Physical-space *feasibility* cut builder — PLP / classical-Benders
/// convention (no α term).  Produces a pure state-coupling constraint
///   Σ πᵢ · sᵢ ≥ Σ πᵢ · v̂ᵢ + Σ slack_iᵢ
/// where πᵢ is the dual of the fixing equation
/// `dep_col + sup − sdn = v̂` at the elastic clone's Phase-1 optimum,
/// and `slack_i = x[sup_i] - x[sdn_i]` is the net slack activation
/// (the amount the trial had to move to become feasible).  Matches
/// `plp-agrespd.f::AgrElastici` line-for-line (`lp->getRowPrice()[row]`
/// for the coefficient, `(b[row] + dx) * ray[i]` for the RHS term).
///
/// Why no α: a feasibility cut certifies "this trial point leads to
/// downstream infeasibility" — it carries no lower-bound information
/// on the expected future cost.  Mixing α into the cut would let the
/// master drive α negative to "satisfy" the cut trivially (the
/// LB-frozen-at-0 pathology).
///
/// @param links                   State-variable linkage descriptors.
///                                Each link contributes its
///                                `source_col` coefficient derived
///                                from its paired `link_info.fixing_row`.
/// @param link_infos              RelaxedVarInfo per link (same order
///                                and size as @p links).  Provides
///                                `fixing_row` for the row-dual read
///                                and `sup_col`/`sdn_col` for the
///                                slack-activation term in the RHS.
/// @param rc_source               Solved elastic clone — must have
///                                `crossover = true` so `get_row_price()`
///                                returns a valid vertex dual.
/// @param cut_coeff_eps           Drop state-var coefficients below
///                                this absolute threshold (default 0).
[[nodiscard]] auto build_feasibility_cut_physical(
    std::span<const StateVarLink> links,
    std::span<const RelaxedVarInfo> link_infos,
    LinearInterface& rc_source,
    double cut_coeff_eps,
    int niter) -> SparseRow;

// ─── Elastic filter ─────────────────────────────────────────────────────────

/// Slack-pricing policy for the elastic Phase-1 clone.
///
/// Selects how `elastic_filter_solve` prices the per-link slack pair
/// (`sup` pulls the state down, `sdn` lifts it up) on the clone's
/// zeroed objective.  Default-constructed instances reproduce the
/// legacy behaviour byte-for-byte, so every existing call site
/// compiles and behaves unchanged.
struct ElasticCostPolicy
{
  enum class Model : uint8_t
  {
    /// Legacy gtopt pricing: both slack directions cost
    /// `penalty × dep_scale_phys` (see `compute_relaxation_specs`).
    penalty_scaled = 0,
    /// PLP `AgrElastici` pricing (plp-agrespd.f:496-815 +
    /// osicallsc.cpp:611-768): unit costs with a reduced-cost tilt
    /// toward the cheapest reservoir —
    ///   sdn (raises the state; PLP "sp"): `1 + max(tilt, 0)`
    ///   sup (lowers the state; PLP "sn"): `1 − min(tilt, 0)`
    /// with `tilt = rc_tilt_factor × rc_prev_phys` and
    /// `rc_prev_phys = StateVariable::source_reduced_cost() ×
    /// scale_objective / var_scale` — the source column's reduced
    /// cost in the PREVIOUS phase's last solved basis, lifted to the
    /// LP-folded "physical" convention (cost_factor stays folded,
    /// same as every other rc consumer).  Ignores the `penalty`
    /// argument, `dep_scale_phys`, and the phase discount — PLP
    /// prices slacks flat at ~1.  Links with a null `state_var`
    /// (test fixtures) get tilt = 0, i.e. cost exactly 1.0 — PLP's
    /// value when rc = 0.
    plp_unit_rc_tilt = 1,
    /// Flat unit costs, NO rc tilt: both slack directions cost
    /// exactly 1.0 — the Füllner–Rebennack §17.2 Phase-1 objective
    /// `eᵀy⁺ + eᵀy⁻` (`elastic_filter_mode = farkas_recursive`).
    /// Ignores `penalty`, `dep_scale_phys`, and the phase discount.
    unit = 2,
  };
  Model model {Model::penalty_scaled};
  /// PLP's 0.01 multiplier on the previous-basis reduced cost.
  double rc_tilt_factor {0.01};
  /// Global objective scale used to lift the stored raw rc to the
  /// physical convention (`rc_phys = rc_raw × scale_objective /
  /// var_scale`).  Callers pass `li.scale_objective()`.
  double scale_objective {1.0};
};

/// Relax a single fixed state-variable column to its physical source bounds,
/// adding penalised slack variables.
/// Returns a RelaxedVarInfo with the relaxation status and slack column
/// indices.  Converts to bool (true iff relaxed) for backward compatibility.
[[nodiscard]] RelaxedVarInfo relax_fixed_state_variable(
    LinearInterface& li,
    const StateVarLink& link,
    PhaseIndex phase_index,
    double penalty);

/// One elasticized feasibility-cut row in the clone
/// (`elastic_filter_mode = farkas_recursive` only): the installed cut
/// row `a_rᵀx ≥ b_r` received a slack `z_r ≥ 0` with raw coefficient
/// +1 (`a_rᵀx + z_r ≥ b_r`) and unit cost — the Füllner–Rebennack
/// §17.2 `+ I z` term that keeps the Phase-1 clone solvable when a
/// previously-installed cut is itself unsatisfiable at the trial.
struct FcutSlackInfo
{
  RowIndex row {unknown_index};  ///< Cut row index (clone == source LP)
  ColIndex z_col {unknown_index};  ///< The added z_r slack column
};

/// Result of the elastic-filter clone–solve step.
/// Contains the solved LP clone and per-link slack column information.
struct ElasticSolveResult
{
  LinearInterface clone;  ///< Elastic clone (solved when `solved == true`)
  /// One RelaxedVarInfo per outgoing link (same order as @p links)
  std::vector<RelaxedVarInfo> link_infos {};
  /// One entry per elasticized feasibility-cut row (empty unless the
  /// caller passed `elastic_fcut_rows` — farkas_recursive mode).
  std::vector<FcutSlackInfo> fcut_infos {};
  /// True when the clone reached an optimal solution and its duals /
  /// primal values are usable for cut construction.  False when the
  /// clone itself came back non-optimal (i.e. state-variable
  /// relaxation alone cannot restore feasibility — infeasibility is
  /// rooted in rows the elastic filter cannot relax).  When false,
  /// callers should treat the clone as diagnostic-only (write it to
  /// disk for post-mortem) and NOT read duals from it.
  bool solved {true};
};

/// Clone the LP, apply elastic relaxation on fixed state-variable columns,
/// and solve the clone.  The original LP is never modified.
///
/// α is intentionally outside the state-variable pool this filter
/// operates on: callers (via `collect_state_variable_links`) already
/// exclude α from @p links, so α never receives slack variables and
/// its bounds are left untouched in the clone.  The Chinneck Phase-1
/// objective zeroing applies to every column including α, but with α
/// kept at whatever bounds the source LP has (pinned at 0 by
/// bootstrap or freed by a prior optimality cut), the clone's solve
/// measures the true feasibility gap on the forward state variables
/// without α absorbing it.
///
/// @param li                The LP to clone (not modified)
/// @param links             Outgoing state-variable links from the previous
/// phase.  Must not contain the α column — `collect_state_variable_links`
/// skips α by class name.
/// @param penalty           Elastic penalty coefficient for slack variables
/// @param opts              Solver options for the clone solve
/// @param cost_policy       Slack pricing model (default: legacy
///                          `penalty × dep_scale_phys`; the
///                          state_repair mode passes
///                          `plp_unit_rc_tilt`, farkas_recursive
///                          passes `unit`).
/// @param elastic_fcut_rows Feasibility-cut rows already installed in
///                          @p li to relax with a `+z_r` slack in the
///                          clone (`a_rᵀx + z_r ≥ b_r`, unit cost —
///                          the §17.2 `+ I z` term).  Default empty:
///                          no cut row is touched (legacy behaviour,
///                          byte-identical for every existing mode).
///                          The added slacks are reported in
///                          `ElasticSolveResult::fcut_infos`.
/// @return Solved elastic clone and per-link slack info, or nullopt if
///         no columns were fixed or the clone solve failed.
[[nodiscard]] auto elastic_filter_solve(
    const LinearInterface& li,
    std::span<const StateVarLink> links,
    double penalty,
    const SolverOptions& opts,
    const ElasticCostPolicy& cost_policy = {},
    std::span<const RowIndex> elastic_fcut_rows = {})
    -> std::optional<ElasticSolveResult>;

/// Chinneck-style elastic IIS filter.
///
/// Single-pass deletion-filter approximation of Chinneck's elastic algorithm
/// (2008, *Feasibility and Infeasibility in Optimization*, §3.5).
///
/// Procedure:
/// 1. Clone the LP and relax every fixed state-variable bound with two
///    penalised slack variables (same as `elastic_filter_solve()`).
/// 2. Solve the relaxed clone with @p penalty.  If it is itself infeasible
///    or some other solver error occurs, return nullopt.
/// 3. Inspect the slack values:
///      - links whose `sup` and `sdn` are both ≤ @p slack_tol are NOT
///        contributing to the infeasibility ("non-essential")
///      - links with at least one slack > @p slack_tol form the IIS
///        candidate set
/// 4. Re-fix the non-essential links to their original `trial_value`
///    (drop their slack columns out of consideration by zeroing the
///    upper bound on `sup`/`sdn`) and re-solve.  If the re-solve stays
///    feasible at the same penalty cost, the IIS is confirmed.  If it
///    becomes infeasible (one of the supposedly non-essential links was
///    actually essential due to penalty competition), undo the re-fix
///    and fall back to the conservative full-elastic IIS.
///
/// The returned `ElasticSolveResult` carries `link_infos` whose `sup_col`
/// / `sdn_col` are reset to `unknown_index` for non-essential links, so
/// downstream `build_multi_cuts()` skips them and emits cuts only on the
/// IIS subset.
///
/// @param li        The LP to clone (not modified)
/// @param links     Outgoing state-variable links from the previous phase
/// @param penalty   Elastic penalty coefficient (smaller than for
///                  `elastic_filter_solve` is fine — IIS uses *which*
///                  slack is non-zero, not by how much it dominates cost)
/// @param opts      Solver options
/// @param slack_tol Tolerance for considering a slack "active" (default 1e-6)
/// @return Solved IIS-filtered clone and link-infos, or nullopt on failure.
[[nodiscard]] auto chinneck_filter_solve(const LinearInterface& li,
                                         std::span<const StateVarLink> links,
                                         double penalty,
                                         const SolverOptions& opts,
                                         double slack_tol = 1e-6)
    -> std::optional<ElasticSolveResult>;

/// @brief Result structure for feasibility cut building
struct FeasibilityCutResult
{
  SparseRow cut {};  ///< The feasibility Benders cut
  ElasticSolveResult elastic;  ///< Clone + slack info (for multi-cut)
};

/// Build a Benders feasibility cut from a solved elastic clone.
///
/// This wraps the common pattern: clone → relax → solve → extract cut.
/// The function calls `elastic_filter_solve()` internally.
///
/// @param li          The LP to clone (not modified)
/// @param alpha_col   α column in the source phase's LP
/// @param links       Outgoing state-variable links from the source phase
/// @param penalty     Elastic penalty coefficient
/// @param opts        Solver options
/// @return A feasibility cut (SparseRow) and the ElasticSolveResult,
///         or nullopt if the elastic solve fails.
[[nodiscard]] auto build_feasibility_cut(const LinearInterface& li,
                                         ColIndex alpha_col,
                                         std::span<const StateVarLink> links,
                                         double penalty,
                                         const SolverOptions& opts,
                                         double scale_alpha = 1.0,
                                         double scale_objective = 1.0)
    -> std::optional<FeasibilityCutResult>;

/// Build per-slack bound-constraint cuts from a solved elastic clone.
///
/// For each outgoing link whose slack was activated (non-zero) in the
/// elastic-clone solution, this function generates one feasibility cut
/// on the source column:
///
///   π · source_col ≥ π · clamp(v̂ + dx · scale, source_low, source_upp)
///
///   π   = −row_dual[fixing_row]   (signed sensitivity dual at the
///                                   elastic clone's optimum)
///   dx  = sdn_val − sup_val       (net slack activation on the
///                                   fixing row ``dep + sup − sdn = v̂``)
///
/// Per-link drops (mirroring PLP `osicallsc.cpp::osi_lp_get_feasible_cut`):
///   1. ``|π| < cut_coeff_eps``                       — drop tiny duals.
///   2. ``|π · dx| < 1e-9 × (|π · v̂| + 1e-8)``        — drop sub-tolerance
///      slack activations (the ``dx``-relative low-activation filter).
///
/// Whole-family drop (gtopt-specific guard, no PLP equivalent):
///   - When EVERY surviving per-link cut clamps at ``source_upp``
///     (top 10 % of ``[source_low, source_upp]``), the family is a
///     "every state floored at emax" degenerate cut.  Master retries
///     installing such a family always cascade-fail (the master cannot
///     reach emax on every reservoir simultaneously in one phase).
///     The function CLEARS its return vector in that case so the
///     caller sees "no cut produced" and declares infeasibility
///     directly, skipping the wasted retry chain (juan/gtopt_iplp:
///     28 backtracks → 0).  A WARN is emitted naming the reservoirs.
///
/// @param elastic        Solved elastic clone + per-link slack info
///                       (non-const because reading dual prices may
///                       trigger ``ensure_duals()`` on the backend).
///                       Per-direction slack costs
///                       (``slack_cost_sup``/``slack_cost_sdn``)
///                       are read directly from the clone's slack
///                       column costs via ``get_col_cost_raw()``,
///                       which transparently picks up the asymmetric
///                       bias applied at relaxation time and any
///                       future per-link variation.  No external
///                       penalty parameter needs to be plumbed
///                       through — the elastic clone is the single
///                       source of truth for slack pricing.
/// @param links          Outgoing state-variable links (same order
///                       as ``elastic.link_infos``).
/// @param context        LP context for the generated cut rows.
/// @param cut_coeff_eps  Threshold for the ``|π| < ε`` drop above
///                       (default 1e-8 in production, override for
///                       tests).
/// @param niter          Iteration counter; controls the outward
///                       FactEPS perturbation (``1e-10 × niter``).
/// @return Vector of feasibility cuts (one per surviving link), OR
///         an empty vector when the whole-family degeneracy guard
///         fires.  Caller (``sddp_forward_pass.cpp``) treats empty
///         as "no feasibility cut produced" and flags the scene
///         infeasible.
[[nodiscard]] auto build_multi_cuts(ElasticSolveResult& elastic,
                                    std::span<const StateVarLink> links,
                                    const LpContext& context,
                                    double cut_coeff_eps,
                                    int niter) -> std::vector<SparseRow>;

// ─── PLP-exact feasibility cuts (`elastic_filter_mode = state_repair`) ──────

/// Return status of `build_plp_feasibility_cuts` — mirrors PLP's
/// `AgrElastici` outcome codes.
enum class PlpCutStatus : uint8_t
{
  /// ≥ 1 single-variable rows emitted (PLP: cut installed on t−1,
  /// caller backtracks and re-solves).
  cuts_added = 0,
  /// The elastic clone solved to optimality but EVERY ray/dx was
  /// zeroed by the FactEPS tolerances — the elastic repaired
  /// feasibility without moving any state (PLP `IStat = −1`
  /// "holguras").  PLP tolerates this in the dual (backward) phase
  /// only; the forward pass declares the scene infeasible.
  holguras = 1,
  /// The elastic clone itself was not proven optimal — the
  /// infeasibility is rooted in rows the elastic filter cannot relax.
  /// No cut exists; the caller declares infeasibility.
  fail = 2,
};

/// Result bundle of `build_plp_feasibility_cuts`.
struct PlpFeasibilityCuts
{
  PlpCutStatus status {PlpCutStatus::fail};
  /// One single-variable row per emitted link:
  /// `ray_i · x_src_i ≥ rhsi_i + fact_eps·|rhsi_i|` (physical space,
  /// ready for `LinearInterface::add_rows` on the previous phase LP).
  std::vector<SparseRow> cuts {};
};

/// PLP-exact per-link feasibility-cut builder — reproduces
/// `plp-agrespd.f::AgrElastici` + `osicallsc.cpp::
/// osi_lp_get_feasible_cut` under the default `FOneFeasRay = FALSE`
/// (the only branch PLP production runs exercise).
///
/// From the solved elastic Phase-1 clone (unit slack costs — pair with
/// `ElasticCostPolicy::Model::plp_unit_rc_tilt`), per link i with
/// `eps = fact_eps + 2⁻³⁶`:
///
///   ray_i  = −dual(fixing_row_i);            |ray_i| < eps  → 0
///   dx_i   = sdn_i − sup_i  (LP-raw; = PLP's sp − sn);
///            |dx_i| < fact_eps·(|trial_i| + 1e-8) → dx_i = 0 ∧ ray_i = 0
///   nx_i   = v̂_phys_i + dx_i·dep_scale_i    (the minimally repaired
///            initial state — the `dep_clone_phys` convention)
///   rhsi_i = ray_i · nx_i
///
/// Emission: `deps = 1e-3·fact_eps·|Σ rhsi| + 2⁻³⁶`; every link with
/// `|ray_i| > deps` yields ONE single-variable row
/// `ray_i · x_i ≥ rhsi_i + fact_eps·|rhsi_i|`.
///
/// Deliberately ABSENT (PLP guarantees bound-consistency BY
/// CONSTRUCTION — the slack caps equal the previous phase's state
/// box, so `nx_i ∈ [source_low, source_upp]` always):
///   * no clamping of the RHS to the state box,
///   * no top-10 % saturation drop,
///   * no all-upper degenerate-family drop,
///   * no fallback aggregated cut.
///
/// @param elastic   Solved elastic clone + per-link slack info
///                  (non-const: reading duals may trigger
///                  `ensure_duals()`).  `elastic.solved == false`
///                  short-circuits to `PlpCutStatus::fail`.
/// @param links     Outgoing state-variable links (same order as
///                  `elastic.link_infos`).
/// @param context   LP context stamped on each emitted row; when it
///                  carries an `IterationContext` the 4th slot is
///                  made per-sibling-unique (same scheme as
///                  `build_multi_cuts`) so the StoredCut `CutKey`
///                  dedup keeps every sibling.
/// @param fact_eps  PLP FactEPS (`SDDPOptions::fact_eps`, default 1e-8).
[[nodiscard]] auto build_plp_feasibility_cuts(
    ElasticSolveResult& elastic,
    std::span<const StateVarLink> links,
    const LpContext& context,
    double fact_eps) -> PlpFeasibilityCuts;

// ─── Recursive feasibility cut (`elastic_filter_mode = farkas_recursive`) ───

/// Füllner–Rebennack recursive feasibility cut (SIAM Review 2023,
/// §17.2–17.3) — ONE aggregated cut per infeasibility event, built
/// from ORDINARY optimal duals of the elastic Phase-1 clone.  Never
/// touches a solver Farkas/ray API (CPXdualfarkas, GRB FarkasDual, …)
/// so the construction is identical across backends
/// (solver-independence directive, 2026-07-11).
///
/// The clone must come from `elastic_filter_solve` with
/// `ElasticCostPolicy::Model::unit` (the review's `eᵀy⁺ + eᵀy⁻`
/// objective) and with every INSTALLED feasibility-cut row passed as
/// `elastic_fcut_rows` (the §17.2 `+ I z` term: `a_rᵀx + z_r ≥ b_r`,
/// z_r ≥ 0, cost 1) — that is what makes the clone ALWAYS solvable
/// when a previously-installed cut is itself the unsatisfiable row.
///
/// **Mapping to the review's eq. (17.3)** `(σᵀT)x ≥ σᵀh_t + ωᵀα^f`:
///   * `h_t` ↔ the trial RHS of the state-fixing rows
///     `dep_i + sup_i − sdn_i = trial_i` (the only x-dependent RHS
///     entries in gtopt's bound-pin formulation; σ_i = −dual_i is the
///     `σᵀT` coefficient on the source column, same sign convention
///     as `build_feasibility_cut_physical`),
///   * `α^f_r` ↔ `b_r`, the installed cut row's lower bound, with
///     dual ω_r = dual(fcut_row_r) ≥ 0 at the min-slack optimum.
///
/// The intercept `σᵀh_t + ωᵀα^f` (plus gtopt's extra column-box dual
/// folds, absent from the review's y ≥ 0 canonical form) equals — by
/// STRONG DUALITY — the clone optimum V(v̂) rebased at the trial:
///
///     Σ_i σ_i·x_i  ≥  Σ_i σ_i·v̂_i + V(v̂),
///
/// which is what this builder emits (with the `fact_eps` relative
/// outward margin `rhs += fact_eps·|rhs|`, like state_repair).  The
/// downstream cut intercepts ω_r·b_r are FOLDED through V — the
/// poisoned rows' residual violation Σ ω_r·z*_r is exactly the
/// z-part of the unit-cost objective.  Note this deliberately does
/// NOT add ω_r·b_r on top of the σ·nx form: with unit costs
/// `Σ σ_i·nx_i = σᵀv̂ + V_slack`, so the literal sum would
/// double-count by `Σ ω_r·(a_rᵀx*) ≥ 0` and cut off feasible master
/// states.  Validity: V is convex in the fixing-row RHS and any
/// optimal dual is a subgradient — `V(x) ≥ V(v̂) + σ̃ᵀ(x − v̂)` with
/// `σ̃ = −σ`, and feasibility requires `V(x) ≤ 0` — so the cut is
/// valid even at degenerate vertices.
///
/// σ zero-guard: `|σ| < fact_eps + 2⁻³⁶ → 0`; non-finite duals are
/// zeroed with a WARN.  Coefficients are emitted per-physical-unit
/// (`σ_view / col_scale(dep)`), exact under ruiz/var_scale scaling.
///
/// Status semantics mirror `build_plp_feasibility_cuts`:
///   * `cuts_added` — one aggregated row in `cuts`;
///   * `holguras`   — clone optimal but every σ zeroed (no state
///     sensitivity; no valid state cut exists);
///   * `fail`       — clone not proven optimal (infeasibility rooted
///     in rows the filter cannot relax).
///
/// @param elastic   Solved elastic clone + slack info (non-const:
///                  reading duals may trigger `ensure_duals()`).
/// @param links     Outgoing state-variable links (same order as
///                  `elastic.link_infos`).
/// @param context   LP context stamped on the emitted row.
/// @param fact_eps  Zero-guard + outward-margin tolerance
///                  (`SDDPOptions::fact_eps`, default 1e-8).
[[nodiscard]] auto build_farkas_recursive_cut(
    ElasticSolveResult& elastic,
    std::span<const StateVarLink> links,
    const LpContext& context,
    double fact_eps) -> PlpFeasibilityCuts;

// ─── Cut averaging ──────────────────────────────────────────────────────────
//
// `average_benders_cut` / `accumulate_benders_cuts` were removed
// 2026-07-08 with the invalid `broadcast_mean` / `accumulate` sharing
// modes — a plain SUM of valid cuts asserts K·F(x) and is never a valid
// underestimator for K > 1 (`docs/formulation/sddp-cut-validity.md` §5).

/// Compute a probability-weighted average cut from a collection of cuts.
///
/// Each cut is weighted by the corresponding element in @p weights.
/// The weights are normalised internally so they need not sum to 1.
/// If all weights are zero the function returns an empty SparseRow.
///
/// @param cuts    Collection of Benders optimality cuts (SparseRow)
/// @param weights Per-cut probability weights (must be same size as cuts)
[[nodiscard]] auto weighted_average_benders_cut(
    const std::vector<SparseRow>& cuts, const std::vector<double>& weights)
    -> SparseRow;

// ─── BendersCut class ────────────────────────────────────────────────────────

/**
 * @class BendersCut
 * @brief Class-based interface for Benders cut construction with work-pool
 *        support and infeasibility monitoring.
 *
 * Wraps the free functions declared above as member functions and adds:
 * - An optional `AdaptiveWorkPool` used for LP solve/resolve operations.
 *   When a pool is provided, the elastic-filter clone LP solve is submitted
 *   to the pool (rather than run synchronously on the calling thread).
 *   This allows the solver's work pool to track and schedule elastic-filter
 *   solves alongside other LP subproblems.
 * - An infeasible-cut counter: every successful elastic-filter solve
 *   (i.e. every LP infeasibility event handled by the filter) increments
 *   the counter.  The counter can be reset per-iteration and queried for
 *   monitoring-API integration (e.g. logged to the SDDP JSON status file).
 *
 * ## Usage
 *
 * ```cpp
 * // In SDDPMethod::solve():
 * auto pool = make_solver_work_pool();
 * m_benders_cut_.set_pool(pool.get());
 *
 * // In elastic_solve():
 * auto result = m_benders_cut_.elastic_filter_solve(li, links, penalty, opts);
 *
 * // After each iteration:
 * ir.infeasible_cuts_added = m_benders_cut_.infeasible_cut_count();
 * m_benders_cut_.reset_infeasible_cut_count();
 * ```
 */
class BendersCut
{
public:
  /// Construct with an optional work pool for LP solve/resolve operations.
  /// If @p pool is nullptr, LP solves are performed synchronously on the
  /// calling thread (same behaviour as the standalone free functions).
  explicit BendersCut(AdaptiveWorkPool* pool = nullptr) noexcept
      : m_pool_(pool)
  {
  }

  BendersCut(const BendersCut&) = delete;
  BendersCut& operator=(const BendersCut&) = delete;
  BendersCut(BendersCut&&) = delete;
  BendersCut& operator=(BendersCut&&) = delete;
  ~BendersCut() = default;

  /// Update the work pool used for LP solves.
  /// Must be called from a single thread (e.g. before starting the parallel
  /// solve loop).  Not safe to call concurrently with elastic_filter_solve().
  void set_pool(AdaptiveWorkPool* pool) noexcept { m_pool_ = pool; }

  /// Access the current work pool (may be nullptr).
  [[nodiscard]] AdaptiveWorkPool* pool() const noexcept { return m_pool_; }

  /// Number of successful elastic-filter solves since construction (or last
  /// reset).  Each such solve corresponds to an LP infeasibility event; in
  /// the backward pass these become feasibility cuts.
  [[nodiscard]] int infeasible_cut_count() const noexcept
  {
    return m_infeasible_cut_count_.load(std::memory_order_relaxed);
  }

  /// Reset the infeasible-cut counter (typically called at the start of each
  /// SDDP iteration to obtain per-iteration counts).
  void reset_infeasible_cut_count() noexcept
  {
    m_infeasible_cut_count_.store(0, std::memory_order_relaxed);
  }

  /// Clone @p li, apply elastic relaxation on fixed state-variable columns,
  /// and solve the clone.  When a work pool is set, the LP solve is submitted
  /// to the pool (allowing the pool's scheduling and monitoring to observe it);
  /// otherwise the solve is performed synchronously.
  ///
  /// Increments the infeasible-cut counter on each successful solve.
  ///
  /// @param cost_policy Slack pricing model — see the free function.
  /// @param elastic_fcut_rows Installed feasibility-cut rows to relax
  ///        with a `+z_r` slack in the clone — see the free function.
  /// @return Solved elastic clone and per-link slack info, or nullopt if no
  ///         columns were fixed or the clone solve failed.
  [[nodiscard]] auto elastic_filter_solve(
      const LinearInterface& li,
      std::span<const StateVarLink> links,
      double penalty,
      const SolverOptions& opts,
      const ElasticCostPolicy& cost_policy = {},
      std::span<const RowIndex> elastic_fcut_rows = {})
      -> std::optional<ElasticSolveResult>;

  /// Build a Benders feasibility cut using this object's elastic_filter_solve.
  /// Equivalent to the free function `build_feasibility_cut()` but uses the
  /// work pool (if set) for the internal LP solve.
  ///
  /// @return A feasibility cut and the ElasticSolveResult, or nullopt if the
  ///         elastic solve fails.
  [[nodiscard]] auto build_feasibility_cut(const LinearInterface& li,
                                           ColIndex alpha_col,
                                           std::span<const StateVarLink> links,
                                           double penalty,
                                           const SolverOptions& opts,
                                           double scale_alpha = 1.0,
                                           double scale_objective = 1.0)
      -> std::optional<FeasibilityCutResult>;

private:
  AdaptiveWorkPool* m_pool_ {nullptr};
  std::atomic<int> m_infeasible_cut_count_ {0};
};

}  // namespace gtopt
