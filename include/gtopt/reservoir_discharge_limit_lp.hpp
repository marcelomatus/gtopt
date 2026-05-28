/**
 * @file      reservoir_discharge_limit_lp.hpp
 * @brief     LP representation of a ReservoirDischargeLimit constraint
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The ReservoirDischargeLimitLP class provides the LP formulation for
 * volume-dependent peak-discharge limits.  For each (scenario, stage, block)
 * triple it creates one inequality row of the form
 *
 *    @code{.text}
 *    flow_b - slope * efin  <=  intercept
 *    @endcode
 *
 * This enforces `flow_b <= intercept + slope * efin` block-by-block, i.e.
 * `max_b(flow_b) <= DCMax(V)` — the physically correct peak-flow capacity.
 * The previous stage-average formulation
 * (`qeh = Σ_b (dur_b/dur_stage) · flow_b`, `qeh <= DCMax`) allowed peaky
 * patterns to satisfy the cap while violating the penstock at the block
 * level; replacing the single averaged row with per-block rows fixes that
 * at the cost of N rows per stage (same nonzero pattern, well-conditioned).
 *
 * When the ReservoirDischargeLimit has multiple piecewise segments,
 * `update_lp()` selects the active segment based on the reservoir volume from
 * the previous solve and updates the LP constraint coefficients/RHS on every
 * block-level row of that stage.
 */

#pragma once

#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/update_context.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

class SystemLP;
class SimulationLP;

/**
 * @brief LP wrapper for ReservoirDischargeLimit constraints
 */
class ReservoirDischargeLimitLP : public ObjectLP<ReservoirDischargeLimit>
{
public:
  static constexpr std::string_view DvolName {"dvol"};

  explicit ReservoirDischargeLimitLP(const ReservoirDischargeLimit& ddl,
                                     InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir_discharge_limit(
      this auto&& self) noexcept
  {
    return self.object();
  }

  /// @return Whether the discharge limit references a built-in waterway
  /// turbine's flow column (true) instead of a classic Waterway (false).
  [[nodiscard]] constexpr bool uses_turbine() const noexcept
  {
    return reservoir_discharge_limit().turbine.has_value();
  }

  [[nodiscard]] auto waterway_sid() const -> WaterwayLPSId
  {
    return WaterwayLPSId {require_sid(reservoir_discharge_limit().waterway,
                                      "ReservoirDischargeLimitLP::waterway_sid",
                                      "waterway")};
  }
  [[nodiscard]] auto turbine_sid() const -> TurbineLPSId
  {
    return TurbineLPSId {require_sid(reservoir_discharge_limit().turbine,
                                     "ReservoirDischargeLimitLP::turbine_sid",
                                     "turbine")};
  }
  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {reservoir_discharge_limit().reservoir};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Update volume-dependent LP coefficients
   *
   * Selects the active piecewise segment based on the reservoir's current
   * volume and updates the slope coefficient on eini/efin columns and the
   * RHS intercept in the volume constraint row.
   *
   * @return Number of LP coefficients/bounds modified (0 if unchanged)
   */
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  /// Tracks current LP state for coefficient updates
  struct RDLState
  {
    ColIndex eini_col {};
    ColIndex efin_col {};
    Real current_slope {0.0};
    Real current_rhs {0.0};
    /// Cached reservoir refs so `update_lp` doesn't need
    /// `sys.element<ReservoirLP>(reservoir_sid())` on the current sys.
    ReservoirRefCache reservoir_cache {};
  };

private:
  /// Per-block peak-flow rows: flow_b - slope·efin ≤ intercept
  STBIndexHolder<RowIndex> vol_rows;

  /// Per-(scenario, stage) state for coefficient tracking
  IndexHolder2<ScenarioUid, StageUid, RDLState> m_states_;
};

// Pin the data-struct constant value so an accidental rename of the
// `ReservoirDischargeLimit::class_name` literal fails the build (LP row labels
// and CSV outputs depend on the exact string `"ReservoirDischargeLimit"`).
static_assert(ReservoirDischargeLimitLP::Element::class_name
                  == LPClassName {"ReservoirDischargeLimit"},
              "ReservoirDischargeLimit::class_name must remain "
              "\"ReservoirDischargeLimit\"");

}  // namespace gtopt
