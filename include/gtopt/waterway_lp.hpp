/**
 * @file      waterway_lp.hpp
 * @brief     LP formulation for waterways between junctions
 * @date      Wed Jul 30 11:48:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the WaterwayLP class, which builds LP variables and
 * constraints for water flow between hydro junctions.
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/waterway.hpp>

namespace gtopt
{

class WaterwayLP : public ObjectLP<Waterway>
{
public:
  static constexpr std::string_view FlowName {"flow"};
  /// soft-`fmin` slack column ("unserved" forced flow) + its row, used when
  /// `fmin_fcost` is set (mirrors GeneratorLP's UnservedName/PminSoftName).
  static constexpr std::string_view FminUnservedName {"fmin_unserved"};
  static constexpr std::string_view FminSoftName {"fmin_soft"};

  explicit WaterwayLP(const Waterway& pwaterway, const InputContext& ic);

  [[nodiscard]] constexpr auto&& waterway(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto junction_a_sid() const noexcept
  {
    return JunctionLPSId {waterway().junction_a};
  }

  /// @return Whether the waterway credits a downstream junction.  False
  /// means outflow mode: the carried flow drains out of the system at
  /// ``junction_a``, no synthetic ocean / sink junction needed.
  [[nodiscard]] constexpr bool has_junction_b() const noexcept
  {
    return waterway().junction_b.has_value();
  }

  [[nodiscard]] auto junction_b_sid() const -> JunctionLPSId
  {
    return JunctionLPSId {require_sid(
        waterway().junction_b, "WaterwayLP::junction_b_sid", "junction_b")};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] const auto& flow_cols_at(const ScenarioLP& scenario,
                                         const StageLP& stage) const
  {
    // Conditional-insert in `add_to_lp` (only when fcols is non-
    // empty + waterway+both junctions active) leaves the outer key
    // absent for inactive waterways.  Callers (turbine_lp, pump_lp,
    // reservoir_seepage_lp, reservoir_discharge_limit_lp,
    // reservoir_production_factor_lp) need a graceful empty fallback
    // rather than a throw — pre-fix the throw triggered a NULL-deref
    // during cascade level transitions.
    return find_or_empty_inner(flow_cols, scenario, stage);
  }

  /// Tolerant lookup over `flow_cols` — see `lookup_inner`
  /// (`index_holder.hpp`).  Consumers (turbine_lp, pump_lp, etc.)
  /// use this to skip the matching constraint row when the P1
  /// zero-bound skip (waterway_lp.cpp:72) elided the flow column,
  /// instead of throwing `flat_map::at`.
  [[nodiscard]] std::optional<ColIndex> lookup_flow_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    return lookup_inner(flow_cols, scenario, stage, buid);
  }

  /// @name Parameter accessors for user constraint resolution.
  /// Per-(stage, block) and per-stage Waterway schedules.  Returned by
  /// the ``ampl_dispatch_registry`` shims so PAMPL constraints can
  /// reference ``waterway('X').<field>`` as a constant scalar.
  /// @{
  [[nodiscard]] auto param_fmin(StageUid s, BlockUid b) const
  {
    return fmin.at(s, b);
  }
  [[nodiscard]] auto param_fmax(StageUid s, BlockUid b) const
  {
    return fmax.at(s, b);
  }
  [[nodiscard]] auto param_capacity(StageUid s) const { return capacity.at(s); }
  [[nodiscard]] auto param_lossfactor(StageUid s) const
  {
    return lossfactor.at(s);
  }
  [[nodiscard]] auto param_fcost(StageUid s) const { return fcost.at(s); }
  /// @}

private:
  OptTBRealSched fmin;
  OptTBRealSched fmin_fcost;  // soft-`fmin` penalty ($/(m³/s)/h); empty ⇒ hard
  OptTBRealSched fmax;
  OptTRealSched capacity;
  OptTRealSched lossfactor;
  OptTRealSched fcost;

  STBIndexHolder<ColIndex> flow_cols;
};

using WaterwayLPId = ObjectId<class WaterwayLP>;
using WaterwayLPSId = ObjectSingleId<class WaterwayLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Waterway::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Waterway"`).
static_assert(WaterwayLP::Element::class_name == LPClassName {"Waterway"},
              "Waterway::class_name must remain \"Waterway\"");

}  // namespace gtopt
