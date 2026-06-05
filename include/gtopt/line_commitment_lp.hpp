/**
 * @file      line_commitment_lp.hpp
 * @brief     LP formulation for Optimal Transmission Switching (issue #509)
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines ``LineCommitmentLP`` — the LP-build companion to the
 * ``LineCommitment`` JSON element.  For every block of every active
 * (scenario, stage) the class:
 *
 *   1. Creates a binary ``u_l ∈ {0, 1}`` status column (relaxable to
 *      [0, 1] via ``LineCommitment.relax``).
 *   2. Stamps capacity gating rows so ``u_l = 0`` forces ``f_l = 0``:
 *
 *        + direction (A → B): f - F^max_ab · u_l ≤ 0
 *        − direction (B → A): F^max_ba · u_l + f ≥ 0
 *
 *      (rows scale to the per-block ``tmax_ab`` / ``tmax_ba`` schedule;
 *      transparent to whether the underlying flow column is signed —
 *      ``tangent_signed_flow`` — or split into ``fp`` / ``fn``).
 *
 * **v1 scope** (issue #509 §"Implementation roadmap"):
 *   - **Capacity gating** — always emitted (transport + Kirchhoff).
 *   - **node_angle KVL big-M disjunction** — emitted by this class:
 *     the per-line KVL equality row stamped by ``LineLP`` is
 *     rewritten in place as an upper-side ``≤`` inequality and a new
 *     lower-side ``≥`` row is added, so ``u_l = 0`` decouples
 *     ``θ_a`` from ``θ_b`` exactly like an opened breaker.
 *   - **cycle_basis KVL big-M disjunction** — emitted by
 *     ``kirchhoff::cycle_basis::add_kvl_rows`` (system-level
 *     assembler).  Each cycle row containing a switchable line is
 *     replaced by two inequalities with per-cycle big-M
 *     ``M_C = 2·θ_max · |C| · row_scale + Σ |φ_e| · row_scale``.
 *
 * **v1.2 scope additions** (mirroring ``CommitmentLP``):
 *   - **min_up_time** [hours] — anti-flicker: Σ_{q ∈ window} u[q]
 *     ≥ UT · v[t], where UT is the smallest block-window covering
 *     ``min_up_time`` hours.  Trivially satisfied (skipped) when
 *     UT ≤ 1.  Requires u/v/w (silently inert otherwise).
 *   - **min_down_time** [hours] — symmetric down-time guard.
 *   - **max_starts** / **min_starts** — two-sided rolling cap on
 *     Σ_{t ∈ window} v[t].  Window length resolved from
 *     ``starts_scope`` via ``starts_window_hours()``.
 *
 * **v1.3 scope additions** (also mirroring ``CommitmentLP``):
 *   - **Startup tiers** (hot/warm/cold) — when all five tier fields
 *     are set together with u/v/w, the flat ``startup_cost`` on
 *     ``v[t]`` is zeroed and three tier indicators ``y^hot``,
 *     ``y^warm``, ``y^cold`` ∈ [0, 1] are emitted per block, joined by
 *       C8:   v[t] = y^hot[t] + y^warm[t] + y^cold[t]
 *       C9:   y^hot[t]  ≤ Σ_{q ∈ hot window}  w[q]
 *       C10:  y^warm[t] ≤ Σ_{q ∈ warm window} w[q]
 *     where the windows look BACKWARDS by cumulative block duration:
 *     ``hot`` ⇒ sum ≤ ``hot_start_time``; ``warm`` ⇒ sum ∈
 *     (``hot_start_time``, ``cold_start_time``].  Cold is residual.
 *     Pre-stage offline-time is supplied via ``initial_hours``.
 *
 * **Mode gates** (enforced by ``validate_planning``):
 *   - ``method ∈ {sddp, cascade}`` is rejected — Benders cuts on a
 *     mixed-integer subproblem are unsound (Zou-Ahmed-Sun 2019).
 *   - Both Kirchhoff modes (``node_angle``, ``cycle_basis``) and
 *     transport mode (``use_kirchhoff = false``) are now supported.
 */

#pragma once

#include <gtopt/line_commitment.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class LineCommitmentLP : public ObjectLP<LineCommitment>
{
public:
  static constexpr std::string_view StatusName {"status"};
  static constexpr std::string_view CapacityPName {"capacity_p"};
  static constexpr std::string_view CapacityNName {"capacity_n"};
  /// Lower-side KVL big-M row label (Kirchhoff node_angle mode only).
  /// The upper-side ``≤`` half is the same row label the existing
  /// LineLP KVL emission uses (``LineLP::ThetaName``) since we
  /// MUTATE that row in place to become the upper half.
  static constexpr std::string_view KvlMinusName {"kvl_minus"};
  /// Knueven–Ostrowski–Watson 2020 three-binary decomposition labels.
  /// Active only when ``LineCommitment.startup_cost`` or
  /// ``shutdown_cost`` is set (see header docstring §"u/v/w (v1.1)").
  static constexpr std::string_view StartupName {"startup"};
  static constexpr std::string_view ShutdownName {"shutdown"};
  /// C1 logic transition row: ``u[t] − u[t−1] − v[t] + w[t] = 0``.
  static constexpr std::string_view LogicName {"logic"};
  /// C3 exclusion row: ``v[t] + w[t] ≤ 1`` (mutual exclusion of the
  /// startup and shutdown indicators).
  static constexpr std::string_view ExclusionName {"exclusion"};
  /// v1.2 time-based constraint labels (mirror ``CommitmentLP``).
  static constexpr std::string_view MinUpTimeName {"min_up_time"};
  static constexpr std::string_view MinDownTimeName {"min_down_time"};
  static constexpr std::string_view MaxStartsName {"max_starts"};
  /// Lower-side of the max_starts pair (``min_starts ≤ Σ v``).  Uses
  /// a distinct label so the row metadata dedup in
  /// ``LinearProblem::add_row`` doesn't collide with the upper-side
  /// row when both bounds are set on the same window.  ``CommitmentLP``
  /// has the same latent bug but no current test exercises the
  /// upper+lower combination at LP-build time.
  static constexpr std::string_view MinStartsName {"min_starts"};
  /// v1.3 startup-tier labels (mirror ``CommitmentLP``).  Active only
  /// when all five tier fields are set on the schema.
  static constexpr std::string_view HotStartName {"hot_start"};
  static constexpr std::string_view WarmStartName {"warm_start"};
  static constexpr std::string_view ColdStartName {"cold_start"};
  /// C8 selection row: ``v[t] = y_hot[t] + y_warm[t] + y_cold[t]``.
  static constexpr std::string_view TierSelectName {"tier_select"};
  /// C9 hot-window row: ``y_hot[t] ≤ Σ_{q ∈ hot window} w[q]``.
  static constexpr std::string_view HotWindowName {"hot_window"};
  /// C10 warm-window row: ``y_warm[t] ≤ Σ_{q ∈ warm window} w[q]``.
  static constexpr std::string_view WarmWindowName {"warm_window"};

  using Base = ObjectLP<LineCommitment>;

  explicit LineCommitmentLP(const LineCommitment& lc, const InputContext& ic);

  [[nodiscard]] constexpr auto&& line_commitment(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto line_sid() const noexcept
  {
    return LineLPSId {line_commitment().line};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Look up the status column for (scenario, stage, block).
  [[nodiscard]] std::optional<ColIndex> lookup_status_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    return lookup_inner(status_cols_, scenario, stage, buid);
  }

private:
  ElementIndex<LineLP> line_index_;
  OptTBRealSched fixed_status_;

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<RowIndex> capacity_p_rows_;
  STBIndexHolder<RowIndex> capacity_n_rows_;
  /// u/v/w decomposition (v1.1): only populated when
  /// ``startup_cost`` or ``shutdown_cost`` is set on the schema.
  STBIndexHolder<ColIndex> startup_cols_;
  STBIndexHolder<ColIndex> shutdown_cols_;
  STBIndexHolder<RowIndex> logic_rows_;
  STBIndexHolder<RowIndex> exclusion_rows_;
  /// v1.2 time-based row holders.  Only populated when the
  /// corresponding schema field is set AND u/v/w is active.
  STBIndexHolder<RowIndex> min_up_time_rows_;
  STBIndexHolder<RowIndex> min_down_time_rows_;
  STBIndexHolder<RowIndex> max_starts_rows_;
  /// v1.3 startup tier holders.  Populated only when all five tier
  /// fields are set (``has_startup_tiers()``).
  STBIndexHolder<ColIndex> hot_start_cols_;
  STBIndexHolder<ColIndex> warm_start_cols_;
  STBIndexHolder<ColIndex> cold_start_cols_;
  STBIndexHolder<RowIndex> tier_select_rows_;
  STBIndexHolder<RowIndex> hot_window_rows_;
  STBIndexHolder<RowIndex> warm_window_rows_;
};

// Pin the data-struct constant value so an accidental rename of the
// `LineCommitment::class_name` literal fails the build.
static_assert(LineCommitmentLP::Element::class_name
                  == LPClassName {"LineCommitment"},
              "LineCommitment::class_name must remain \"LineCommitment\"");

}  // namespace gtopt
