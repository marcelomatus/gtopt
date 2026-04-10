/**
 * @file      flow_right_lp.hpp
 * @brief     LP representation of flow-based water rights
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the FlowRightLP class which provides methods to:
 * - Represent flow-based water rights in LP problems
 * - Create flow and deficit variables per block
 * - Penalize unmet demands in the objective
 *
 * The flow right is NOT part of the hydrological topology.
 * It creates its own variables and constraints for rights accounting
 * without modifying junction balance rows.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/scenario.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes
class SystemLP;

using FlowRightLPSId = ObjectSingleId<class FlowRightLP>;

class FlowRightLP : public ObjectLP<FlowRight>
{
public:
  static constexpr LPClassName ClassName {"FlowRight"};
  static constexpr std::string_view FlowName {"flow"};
  static constexpr std::string_view FailName {"fail"};
  static constexpr std::string_view QehName {"qeh"};

  explicit FlowRightLP(const FlowRight& pflow, const InputContext& ic);

  [[nodiscard]] constexpr auto&& flow_right(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Update volume-dependent column bounds when bound_rule is set.
  /// Reads the referenced reservoir's current volume, evaluates the
  /// piecewise-linear bound function, and calls set_col_upp on flow
  /// columns when the computed bound differs from the cached value.
  /// @return Number of LP column bounds modified (0 if unchanged)
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  [[nodiscard]] auto&& flow_cols_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] auto&& fail_cols_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return fail_cols.at({scenario.uid(), stage.uid()});
  }

  /// Return the stage-average hourly flow column for (scenario, stage).
  /// Only present when `use_average = true`.
  /// @throws std::out_of_range if not available.
  [[nodiscard]] ColIndex qeh_col_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return qeh_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_fmax(StageUid s, BlockUid b) const
  {
    return fmax_sched.at(s, b);
  }
  [[nodiscard]] auto param_discharge(ScenarioUid sc,
                                     StageUid s,
                                     BlockUid b) const
  {
    return discharge.at(sc, s, b);
  }
  [[nodiscard]] auto param_fail_cost(StageUid s, BlockUid b) const
  {
    return fail_cost_sched.at(s, b);
  }
  [[nodiscard]] auto param_use_value(StageUid s, BlockUid b) const
  {
    return use_value_sched.at(s, b);
  }
  /// @}

private:
  STBRealSched discharge;
  int direction {-1};

  /// Resolved fail_cost schedule for per-block penalty cost.
  OptTBRealSched fail_cost_sched;

  /// Resolved use_value schedule for per-block flow value/benefit.
  OptTBRealSched use_value_sched;

  /// Resolved fmax schedule for variable-mode bounds.
  /// When fmax > 0 and discharge == 0, the flow column is variable [0, fmax].
  OptTBRealSched fmax_sched;

  STBIndexHolder<ColIndex> flow_cols;
  STBIndexHolder<ColIndex> fail_cols;

  /// Stage-average hourly flow columns (one per scenario x stage).
  /// Only populated when use_average = true.
  STIndexHolder<ColIndex> qeh_cols;
  /// Averaging constraint rows (one per scenario x stage).
  STIndexHolder<RowIndex> avg_rows;

  /// Cached bound rule evaluation per (scenario, stage).
  /// Only populated when flow_right().bound_rule is set.
  struct BoundState
  {
    Real current_bound {0.0};
  };
  IndexHolder2<ScenarioUid, StageUid, BoundState> m_bound_states_;
};

}  // namespace gtopt
