/**
 * @file      flow.hpp
 * @brief     Exogenous water flow (inflow/outflow) at a hydraulic junction
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Flow structure representing an exogenous water inflow (natural
 * river runoff) or outflow (minimum environmental discharge) at a junction.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "inflow_j1",
 *   "junction": "j1",
 *   "direction": 1,
 *   "discharge": "inflow"
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 3-D inline array indexed by `[scenario][stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Flow/`
 */

#pragma once

#include <optional>

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Opt-in autoregressive inflow model attached to a Flow element
 *
 * When present, the SDDP machinery treats the flow's discharge as an
 * AR(1) process
 *   `q_t = mu_t + phi·(q_{t-1} − mu_{t-1}) + eps_t`
 * where `mu_t` is the existing `discharge` schedule (the v1 forward
 * realization IS the schedule, `eps = 0`), and the lagged inflow is
 * registered as a cross-phase state variable so Benders cuts gain
 * `∂V/∂inflow` coefficients automatically.  Absent (`std::nullopt`),
 * behaviour is byte-identical to the historical fixed-schedule flow.
 * See `docs/formulation/sddp-ar-inflows.md`.
 */
struct InflowModel
{
  OptName type {};  ///< Model type; only "ar1" is supported (default)
  OptReal phi {};  ///< AR(1) lag-1 coefficient (default 0.0)
  OptReal sigma {};  ///< Residual std-dev [m³/s] — tooling metadata only;
                     ///< the LP never reads it (future sampled-eps modes
                     ///< will)
};

using OptInflowModel = std::optional<InflowModel>;

/**
 * @brief Exogenous water flow at a hydraulic junction
 *
 * Positive direction (direction = +1) models natural river inflow; negative
 * direction (direction = -1) models mandatory minimum discharge releases or
 * evaporation losses.
 *
 * @see Junction for the connected node
 * @see FlowLP for the LP water-balance contribution
 */
struct Flow
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `FlowLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Flow::class_name` directly (or
  /// `FlowLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Flow"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional element type/category tag
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  OptInt direction {
      1};  ///< Flow direction: +1 = inflow, −1 = outflow [dimensionless]

  OptSingleId junction {};  ///< ID of the connected junction (optional for
                            ///< flow-turbine mode)
  /// Water discharge schedule [m³/s].  Optional: when unset AND
  /// ``fcost`` is set, the LP column upper bound defaults to
  /// ``+inf`` (``DblMax``) — useful for non-physical inflow slacks
  /// that don't need an explicit cap.  When unset AND ``fcost`` is
  /// also unset, the Flow contributes no column for that block (no
  /// bound information available).
  OptSTBRealFieldSched discharge {};

  /// Optional per-(stage, block) flow cost [$/(m³/s)/h].  When set,
  /// the LP-side ``Flow`` column relaxes from the default hard
  /// equality ``lowb = uppb = discharge`` to a soft band
  /// ``lowb = 0, uppb = discharge`` (or ``+inf`` if ``discharge`` is
  /// unset) and the column receives a positive objective coefficient
  /// ``fcost × cost_factor(scenario, stage, block)`` so the LP pays
  /// ``fcost · flow · duration`` per block.  Models PLEXOS-style
  /// "Non-physical Inflow Penalty" — a costed slack column that the
  /// solver only activates when the junction balance cannot otherwise
  /// close.  When unset the legacy hard-forced semantics are
  /// preserved (no cost, ``flow = discharge`` exactly).
  OptTBRealFieldSched fcost {};

  /// Opt-in AR(1) inflow model (see InflowModel).  Absent = the
  /// historical fixed-schedule behaviour, byte-identical LP.
  OptInflowModel inflow_model {};

  /// @return true if flow is directed into the junction (inflow)
  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return direction.value_or(1) >= 0;
  }
};

}  // namespace gtopt
