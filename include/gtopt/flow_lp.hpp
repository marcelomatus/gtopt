/**
 * @file      flow_lp.hpp
 * @brief     Linear programming representation of network flows
 * @date      Wed Jul 30 15:54:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the FlowLP class which provides methods to:
 * - Represent flows in linear programming problems
 * - Manage flow variables and constraints
 * - Interface with junctions and other network components
 */

#pragma once

#include <functional>
#include <optional>

#include <gtopt/basic_types.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class ApertureDataCache;  // forward declaration

/// Single-ID alias for referencing a FlowLP in other LP elements
using FlowLPSId = ObjectSingleId<class FlowLP>;

class FlowLP : public ObjectLP<Flow>
{
public:
  static constexpr std::string_view FlowName {"flow"};
  /// StateVariable column name for the outgoing (lagged) inflow state
  /// registered on the last block's flow column when
  /// ``Flow.inflow_model`` is set.  Consumed by the next phase's
  /// dependent ``inflow_lag`` column via `PendingStateLink` — the
  /// exact `efin → eini` machinery.  Static storage: referenced by
  /// `StateVariable::Key::col_name` (a string_view) for the solver
  /// lifetime.
  static constexpr std::string_view InflowName {"inflow"};
  /// Dependent lag column name (cross-phase incoming inflow state).
  static constexpr std::string_view InflowLagName {"inflow_lag"};
  /// AR(1) recursion row name:  ``q_b − phi·lag = mu_b − phi·mu_ref``.
  static constexpr std::string_view ArBalanceName {"ar1"};

  explicit FlowLP(const Flow& pflow, const InputContext& ic);

  [[nodiscard]] constexpr auto&& flow(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool has_junction() const noexcept
  {
    return flow().junction.has_value();
  }

  [[nodiscard]] auto junction_sid() const
  {
    return JunctionLPSId {
        require_sid(flow().junction, "FlowLP::junction_sid", "junction")};
  }

  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return flow().is_input();
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& flow_cols_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

  /// Tolerant lookup over `flow_cols` — see `lookup_inner`
  /// (`index_holder.hpp`).  Consumers (turbine_lp, etc.) use this
  /// to skip blocks where the producer elided the column, instead
  /// of throwing `flat_map::at`.
  [[nodiscard]] std::optional<ColIndex> lookup_flow_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    return lookup_inner(flow_cols, scenario, stage, buid);
  }

  /**
   * @brief Return the discharge value for a given scenario/stage/block.
   * @param scenario_uid Scenario UID to look up
   * @param stage_uid Stage UID to look up
   * @param block_uid Block UID to look up
   * @return Optional discharge value if present
   */
  [[nodiscard]] std::optional<double> aperture_value(ScenarioUid scenario_uid,
                                                     StageUid stage_uid,
                                                     BlockUid block_uid) const
  {
    return discharge.at(scenario_uid, stage_uid, block_uid);
  }

  /**
   * @brief Update flow column bounds in a cloned LP for an aperture scenario.
   *
   * During the SDDP backward pass with apertures, the flow columns that were
   * originally fixed to the base scenario's discharge values are updated to
   * reflect the aperture scenario's discharge values.  The value_fn callable
   * provides the new discharge value for each (stage, block) pair.
   *
   * @param li            The cloned LinearInterface to modify (in-place).
   * @param base_scenario The scenario used when the LP was originally built.
   * @param value_fn      (StageUid, BlockUid) -> optional<double> provider.
   * @param stage         The stage for which to update the bounds.
   * @return true on success.
   */
  [[nodiscard]] bool update_aperture(
      LinearInterface& li,
      const ScenarioLP& base_scenario,
      const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
      const StageLP& stage) const;

  /// True when this flow carries AR(1) recursion rows for the given
  /// (scenario, stage) — i.e. `update_aperture` rewrites the AR-row
  /// RHS (a ROW write) instead of pinning column bounds.  Consumed by
  /// the dual-shared aperture row-touch gate
  /// (`solve_apertures_for_phase`): Lemma AP2's shared-intercept
  /// arithmetic prices column-bound deltas only, so an AR-mode flow
  /// must disable cut synthesis for the chunk
  /// (docs/formulation/sddp-cut-validity.md §6, "Implementation
  /// guards").
  [[nodiscard]] bool has_ar_rows(ScenarioUid scenario_uid,
                                 StageUid stage_uid) const noexcept
  {
    return ar_info.contains(std::tuple {scenario_uid, stage_uid});
  }

private:
  /// Per-(scenario, stage) AR(1) bookkeeping, populated only when
  /// ``Flow.inflow_model`` is set AND the stage has a usable lag (a
  /// previous stage with a schedule value).  Consumed only by FlowLP's
  /// own members (`update_aperture` rewrites the AR row RHS instead of
  /// the flow column bounds — the columns are free under the AR model;
  /// `has_ar_rows` exposes the presence test), hence private.
  struct ArStageInfo
  {
    BIndexHolder<RowIndex> rows;  ///< Per-block AR recursion rows
    ColIndex lag_col {unknown_index};  ///< Lag column referenced by the rows
    double phi {0.0};  ///< AR(1) coefficient
    double mu_prev_ref {0.0};  ///< Schedule value at the lag reference
    StageUid prev_stage_uid {};  ///< Lag reference stage (for apertures)
    BlockUid prev_ref_block_uid {};  ///< Lag reference block (for apertures)
    /// True when the lag is the cross-phase dependent ``inflow_lag``
    /// column (pinned by trial propagation); false when it is the
    /// previous stage's in-LP flow column (same-phase chaining).
    bool cross_phase {false};
  };

  /// Resolved per-(scene, stage, block) discharge schedule.  Now
  /// optional — when ``Flow.discharge`` is unset and ``fcost`` is
  /// set, the LP column upper bound defaults to ``DblMax``
  /// (unbounded above) so non-physical inflow slacks need only
  /// provide the penalty cost, not a cap.
  OptSTBRealSched discharge;
  /// Resolved per-(stage, block) flow penalty schedule.  Empty when
  /// ``Flow.fcost`` is unset — the LP then keeps the legacy hard
  /// ``lowb = uppb = discharge`` semantics.  When populated, the LP
  /// column relaxes to ``lowb = 0, uppb = discharge`` and the
  /// per-block cost ``fcost · cost_factor`` is applied so the LP
  /// pays for the slack only when the junction balance demands it.
  OptTBRealSched fcost;
  STBIndexHolder<ColIndex> flow_cols;
  /// AR(1) inflow-model state (empty unless ``Flow.inflow_model``).
  STIndexHolder<ArStageInfo> ar_info;
};

// Pin the data-struct constant value so an accidental rename of the
// `Flow::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Flow"`).
static_assert(FlowLP::Element::class_name == LPClassName {"Flow"},
              "Flow::class_name must remain \"Flow\"");

}  // namespace gtopt
