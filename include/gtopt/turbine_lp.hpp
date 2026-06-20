/**
 * @file      turbine_lp.hpp
 * @brief     Defines the TurbineLP class for linear programming representation
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the TurbineLP class which provides the linear programming
 * representation of a hydroelectric turbine, including its constraints and
 * relationships with waterways and generators.
 */
#pragma once

#include <gtopt/flow_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// turbine_lp.hpp).
class SystemLP;

/// Single-ID alias for referencing a TurbineLP in other LP elements
using TurbineLPSId = ObjectSingleId<class TurbineLP>;

/**
 * @brief Linear programming representation of a hydroelectric turbine
 *
 * This class extends ObjectLP to provide LP-specific functionality for
 * turbines, including:
 * - Conversion rate constraints between water flow and power generation
 * - Relationships with connected waterways and generators
 * - Output of dual variables for sensitivity analysis
 */
class TurbineLP : public ObjectLP<Turbine>
{
public:
  static constexpr std::string_view ConversionName {"conversion"};
  static constexpr std::string_view CapacityName {"capacity"};
  /// Name of the turbine-owned flow column emitted in built-in waterway mode.
  static constexpr std::string_view FlowName {"flow"};

  /**
   * @brief Construct a TurbineLP from a Turbine and input context
   * @param pturbine The turbine to represent
   * @param ic Input context containing system configuration
   */
  explicit TurbineLP(const Turbine& pturbine, InputContext& ic);

  [[nodiscard]] constexpr auto&& turbine(this auto&& self) noexcept
  {
    return self.object();
  }

  /// @return Whether this turbine uses a flow reference (not a waterway)
  [[nodiscard]] constexpr bool uses_flow() const noexcept
  {
    return turbine().flow.has_value();
  }

  /// @return Whether this turbine carries its own flow arc between junctions
  /// (built-in waterway mode), enabled when `junction_a` is set.  In this mode
  /// the turbine owns a flow column that debits `junction_a`, optionally
  /// credits `junction_b` (when set), and converts the flow to power — no
  /// separate Waterway element is needed.
  [[nodiscard]] constexpr bool uses_junctions() const noexcept
  {
    return turbine().junction_a.has_value();
  }

  /// @return Whether the built-in waterway mode credits a downstream junction.
  /// When false (junction_b unset), the turbined flow drains out of the system
  /// — used for terminal (run-to-sea) plants.
  [[nodiscard]] constexpr bool has_junction_b() const noexcept
  {
    return turbine().junction_b.has_value();
  }

  [[nodiscard]] auto junction_a_sid() const -> JunctionLPSId
  {
    return JunctionLPSId {require_sid(
        turbine().junction_a, "TurbineLP::junction_a_sid", "junction_a")};
  }

  [[nodiscard]] auto junction_b_sid() const -> JunctionLPSId
  {
    return JunctionLPSId {require_sid(
        turbine().junction_b, "TurbineLP::junction_b_sid", "junction_b")};
  }

  [[nodiscard]] auto waterway_sid() const -> WaterwayLPSId
  {
    return WaterwayLPSId {
        require_sid(turbine().waterway, "TurbineLP::waterway_sid", "waterway")};
  }

  [[nodiscard]] auto flow_sid() const -> FlowLPSId
  {
    return FlowLPSId {
        require_sid(turbine().flow, "TurbineLP::flow_sid", "flow")};
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {turbine().generator};
  }

  /// @return Whether this turbine has drainage enabled
  [[nodiscard]] constexpr auto drain() const noexcept
  {
    return turbine().drain.value_or(false);
  }

  /// @return The turbine efficiency [p.u.] for the given stage (default 1.0)
  [[nodiscard]] auto stage_efficiency(StageUid tuid) const -> Real
  {
    return efficiency.at(tuid).value_or(1.0);
  }

  /// @name Parameter accessors for user constraint resolution.
  /// `production_factor` / `capacity` are per-(stage, block) schedules
  /// (scalar / per-stage forms broadcast to every block); `efficiency`
  /// stays per-stage.  Returned by the ``ampl_dispatch_registry`` shims
  /// so PAMPL constraints can reference ``turbine('X').<field>``.
  /// @{
  [[nodiscard]] auto param_production_factor(StageUid s, BlockUid b) const
  {
    return production_factor.optval(s, b);
  }
  [[nodiscard]] auto param_efficiency(StageUid s) const
  {
    return efficiency.at(s);
  }
  [[nodiscard]] auto param_capacity(StageUid s, BlockUid b) const
  {
    return capacity.optval(s, b);
  }
  /// @}

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Access conversion-rate constraint row indices for a (scenario, stage)
  [[nodiscard]] auto conversion_rows_at(const ScenarioLP& scenario,
                                        const StageLP& stage) const
      -> const BIndexHolder<RowIndex>&
  {
    return conversion_rows.at({scenario.uid(), stage.uid()});
  }

  /// Access the turbine-owned flow columns (built-in waterway mode) for a
  /// (scenario, stage).  Returns an empty inner map when the turbine does not
  /// use junctions or every block was elided by the zero-bound skip.
  [[nodiscard]] const auto& flow_cols_at(const ScenarioLP& scenario,
                                         const StageLP& stage) const
  {
    return find_or_empty_inner(flow_cols, scenario, stage);
  }

private:
  OptTBRealSched production_factor;
  OptTRealSched efficiency;
  OptTBRealSched capacity;

  STBIndexHolder<RowIndex> conversion_rows;
  STBIndexHolder<RowIndex> capacity_rows;
  STBIndexHolder<ColIndex> flow_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `Turbine::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Turbine"`).
static_assert(TurbineLP::Element::class_name == LPClassName {"Turbine"},
              "Turbine::class_name must remain \"Turbine\"");

}  // namespace gtopt
