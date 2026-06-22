/**
 * @file      decision_variable_lp.hpp
 * @brief     LP representation of a free continuous Decision Variable
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One LP column per (scenario, stage, block) with optional bounds and
 * an optional per-block cost coefficient.  Registered with the AMPL
 * resolver as ``decision_variable("X").value`` so UserConstraint
 * expressions can reference it directly.
 *
 * Mirrors the lightweight pattern of :class:`FlowLP` but without any
 * junction coupling — the variable is purely a free knob that other
 * constraints constrain.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/decision_variable.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/object_lp.hpp>

namespace gtopt
{

using DecisionVariableLPSId = ObjectSingleId<class DecisionVariableLP>;

class DecisionVariableLP : public ObjectLP<DecisionVariable>
{
public:
  /// AMPL accessor name: ``decision_variable("X").value``.
  static constexpr std::string_view ValueName {"value"};

  explicit DecisionVariableLP(const DecisionVariable& pdv,
                              const InputContext& /*ic*/)
      : ObjectLP<DecisionVariable>(pdv)
  {
  }

  [[nodiscard]] constexpr auto&& decision_variable(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& value_cols_at(const ScenarioLP& scenario,
                                     const StageLP& stage) const
  {
    return value_cols.at({scenario.uid(), stage.uid()});
  }

  /// Read-only access to the full per-(scenario, stage) per-block column map
  /// populated by `add_to_lp`.  Additive accessor used by `UserModelLP` to
  /// re-emit a bundled variable's `:sol` / `:cost` under the model's own
  /// `output/UserModel/<tag>/...` namespace instead of `DecisionVariable/`.
  [[nodiscard]] constexpr const STBIndexHolder<ColIndex>& value_cols_holder()
      const noexcept
  {
    return value_cols;
  }

private:
  /// Per-(scenario, stage) per-block column index map; populated by
  /// :meth:`add_to_lp` and read by :meth:`add_to_output` to emit the
  /// ``DecisionVariable/value`` solution columns.
  STBIndexHolder<ColIndex> value_cols;
};

}  // namespace gtopt
