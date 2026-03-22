/**
 * @file      reservoir_efficiency_lp.hpp
 * @brief     LP representation of reservoir-dependent turbine efficiency
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the `ReservoirEfficiencyLP` class which manages the LP
 * coefficient update for turbines whose conversion rate varies with
 * the hydraulic head (reservoir volume).
 *
 * During the initial LP build (`add_to_lp`), this class locates the
 * turbine's conversion-rate constraint row and the waterway flow column,
 * stores their indices, and computes the initial conversion rate from
 * the reservoir's initial volume (`eini`).
 *
 * During SDDP forward-pass iterations, `update_conversion_coeff()`
 * recomputes the conversion rate from the current reservoir volume and
 * calls `LinearInterface::set_coeff()` to update the LP matrix in-place.
 */

#pragma once

#include <gtopt/options_lp.hpp>
#include <gtopt/reservoir_efficiency.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine_lp.hpp>

namespace gtopt
{

class LinearInterface;

/**
 * @brief LP representation of a ReservoirEfficiency element
 *
 * Stores per-(scenario,stage,block) row/column indices for the turbine
 * conversion-rate constraint so that the coefficient can be updated
 * when the reservoir volume changes during SDDP iterations.
 */
class ReservoirEfficiencyLP : public ObjectLP<ReservoirEfficiency>
{
public:
  static constexpr LPClassName ClassName {"ReservoirEfficiency", "ref"};

  explicit ReservoirEfficiencyLP(const ReservoirEfficiency& pre,
                                 [[maybe_unused]] InputContext& ic) noexcept
      : ObjectLP<ReservoirEfficiency>(pre)
  {
  }

  [[nodiscard]] constexpr auto&& efficiency(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto turbine_sid() const noexcept
  {
    return TurbineLPSId {efficiency().turbine};
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {efficiency().reservoir};
  }

  /// Evaluate the piecewise-linear efficiency at the given volume
  [[nodiscard]] auto compute_efficiency(Real volume) const noexcept -> Real
  {
    return evaluate_efficiency(efficiency().segments, volume);
  }

  /// Return the mean (fallback) efficiency value
  [[nodiscard]] constexpr auto mean_efficiency() const noexcept -> Real
  {
    return efficiency().mean_efficiency;
  }

  /// Get the effective efficiency update skip count for this element.
  /// Per-element value takes priority; falls back to global SDDP option.
  [[nodiscard]] auto effective_update_skip(const OptionsLP& options) const
      -> Int
  {
    return efficiency().sddp_efficiency_update_skip.value_or(
        options.sddp_efficiency_update_skip());
  }

  /**
   * @brief Register this efficiency element in the LP
   *
   * Locates the turbine's conversion rows and the waterway's flow columns,
   * stores their indices for later coefficient updates.  The initial
   * conversion-rate coefficient is set during TurbineLP::add_to_lp().
   */
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] static bool add_to_output(OutputContext& out);

  /**
   * @brief Update the conversion-rate LP coefficient for a given volume
   *
   * Evaluates the piecewise-linear efficiency at @p volume and sets the
   * coefficient to `-efficiency` for every (row, col) pair stored for the
   * given (scenario, stage).  The negative sign matches the turbine
   * conversion-row convention: `generation − conversion_rate × flow = 0`.
   *
   * @param li   The linear interface to modify
   * @param suid Scenario UID
   * @param tuid Stage UID
   * @param volume Current reservoir volume [dam³]
   * @return Number of coefficients updated
   */
  auto update_conversion_coeff(LinearInterface& li,
                               ScenarioUid suid,
                               StageUid tuid,
                               Real volume) const -> int;

  /// Per-block conversion row and flow column indices for coefficient updates
  struct CoeffIndex
  {
    RowIndex row;  ///< Conversion-rate constraint row
    ColIndex col;  ///< Waterway flow column
  };

  using BCoeffMap = flat_map<BlockUid, CoeffIndex>;

  /// Access stored coefficient indices for a given (scenario, stage)
  [[nodiscard]] auto coeff_indices_at(ScenarioUid suid, StageUid tuid) const
      -> const BCoeffMap&
  {
    return m_coeff_indices_.at({suid, tuid});
  }

  /// Check if coefficient indices are available for a given (scenario, stage)
  [[nodiscard]] auto has_coeff_indices(ScenarioUid suid, StageUid tuid) const
      -> bool
  {
    return m_coeff_indices_.contains({suid, tuid});
  }

private:
  /// Stored row/column indices indexed by (scenario, stage) → block
  IndexHolder2<ScenarioUid, StageUid, BCoeffMap> m_coeff_indices_;
};

// ─── HasUpdateLP concept ─────────────────────────────────────────────────────

/**
 * @brief Concept satisfied by LP element types that implement `update_lp()`.
 *
 * Used by `update_lp_coefficients()` to iterate over the LP element collection
 * with `visit_elements()` and dispatch `update_lp()` only to types that
 * implement it (currently `TurbineLP` and `FiltrationLP`).
 */
template<typename T>
concept HasUpdateLP = requires(T& obj,
                               SystemLP& system_lp,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               PhaseIndex phase,
                               int iteration) {
  {
    obj.update_lp(system_lp, scenario, stage, phase, iteration)
  } -> std::same_as<int>;
};

// ─── Generalized LP coefficient update ──────────────────────────────────────

/**
 * @brief Update all volume-dependent LP coefficients for a (scene, phase)
 *
 * This is the **generalized coefficient update hook** called by the SDDP
 * solver before each phase solve.  It iterates over ALL LP element types in
 * the collection via `visit_elements` and, for each type that satisfies the
 * `HasUpdateLP` concept, calls `element.update_lp()`.  Currently this
 * dispatches to:
 *
 * 1. **TurbineLP::update_lp()** — recomputes the turbine conversion rate from
 *    the current reservoir volume and updates the LP constraint coefficient.
 *
 * 2. **FiltrationLP::update_lp()** — selects the active piecewise-linear
 *    segment based on the current reservoir volume and updates the seepage
 *    constraint slope and RHS.
 *
 * Future extensions simply require implementing `update_lp()` on the new LP
 * element type; no changes to this function are necessary.
 *
 * @param system_lp  The SystemLP for this (scene, phase)
 * @param options    Global LP options (provides default skip count)
 * @param iteration  Current SDDP iteration (0-based)
 * @param phase      Current phase index (PhaseIndex{0} = first phase)
 * @return Total number of LP coefficients modified
 */
class SystemLP;  // forward

[[nodiscard]] int update_lp_coefficients(SystemLP& system_lp,
                                         const OptionsLP& options,
                                         int iteration,
                                         PhaseIndex phase);

}  // namespace gtopt
