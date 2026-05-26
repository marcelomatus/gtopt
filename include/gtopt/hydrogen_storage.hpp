/**
 * @file      hydrogen_storage.hpp
 * @brief     Hydrogen storage (steel cylinders, salt caverns, LH₂)
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ``HydrogenStorage`` element: peer of ``Battery``,
 * ``Reservoir`` and ``ThermalStorage`` on the ``StorageLP<>`` framework.
 *
 * Common storage modalities:
 *   * High-pressure gaseous storage (350 / 700 bar steel cylinders)
 *   * Underground salt caverns (Δp ≈ 60–180 bar, > 100 GWh per cavern,
 *     ≪ 1% / year self-discharge)
 *   * Liquid hydrogen (LH₂, -253 °C, ~0.5–1% / day boil-off)
 *   * Liquid organic hydrogen carriers (LOHC), metal hydrides
 *
 * Energy unit is **MWh_LHV** so the balance row at the ``HydrogenNode``
 * sums consistently with electrolyser / fuel-cell flows expressed in
 * the same unit.  Mass conversion: 1 kg-H₂ ≈ 33.3 kWh_LHV.
 *
 * @see hydrogen_node.hpp       carrier-tagged balance node
 * @see battery.hpp             peer electric-carrier storage
 * @see thermal_storage.hpp     peer thermal-carrier storage
 * @see ammonia_storage.hpp     long-term H₂ carrier (NH₃)
 * @see storage.hpp             generic Storage base
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct HydrogenStorage
 * @brief Hydrogen energy storage (gaseous / liquid / cavern).
 *
 * Data-only definition.  The LP formulation is provided by
 * ``HydrogenStorageLP`` which inherits directly from ``StorageLP<>``.
 *
 * @see HydrogenStorageLP
 */
struct HydrogenStorage
{
  /// Canonical class-name constant used in LP row labels and PAMPL
  /// element resolution.  Matches ``hydrogen_storage_array`` in JSON.
  static constexpr LPClassName class_name {"HydrogenStorage"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional tag ("salt_cavern", "high_pressure",
                    ///< "lh2", "lohc", "metal_hydride", …)

  /// Reference to a ``HydrogenNode`` (NOT a ``Bus`` and NOT an
  /// ``AmmoniaNode``).  The validator resolves this against
  /// ``system.hydrogen_node_array``.
  OptSingleId hydrogen_node {};

  // ── Efficiencies ────────────────────────────────────────────────
  OptTBRealFieldSched input_efficiency {};  ///< Compression / liquefaction
                                            ///< efficiency [p.u.].  Salt
                                            ///< cavern ≈ 0.95; LH₂ ≈ 0.7.
  OptTBRealFieldSched output_efficiency {};  ///< Withdrawal efficiency [p.u.]
                                             ///< (pressure-reduction /
                                             ///< re-gasification losses).
  OptTRealFieldSched annual_loss {};  ///< Self-discharge [p.u./year].
                                      ///< Salt cavern: < 0.01.  LH₂: 0.5%
                                      ///< per day → 0.18 per year.

  // ── Energy bounds (MWh_LHV) ─────────────────────────────────────
  OptTBRealFieldSched emin {};  ///< Minimum stored energy [MWh_LHV].
                                ///< Salt caverns require a "cushion gas"
                                ///< inventory below which the cavern
                                ///< collapses → maps to a hard ``emin``.
  OptTBRealFieldSched emax {};  ///< Maximum stored energy [MWh_LHV].
  OptTBRealFieldSched ecost {};  ///< Holding cost [$/MWh_LHV] (optional).

  OptReal eini {};  ///< Initial stored energy [MWh_LHV].
  OptReal efin {};  ///< Minimum terminal stored energy [MWh_LHV].
  OptReal efin_cost {};  ///< Penalty per unit of efin shortfall [$/MWh_LHV].

  OptTBRealFieldSched soft_emin {};  ///< Soft minimum SoC [MWh_LHV].
  OptTBRealFieldSched soft_emin_cost {};  ///< Penalty on soft-emin shortfall.

  // ── Capacity expansion ──────────────────────────────────────────
  OptTRealFieldSched capacity {};  ///< Installed storage size [MWh_LHV].
  OptTRealFieldSched expcap {};  ///< Capacity per expansion module.
  OptTRealFieldSched expmod {};  ///< Maximum number of expansion modules.
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity.
  OptTRealFieldSched annual_capcost {};  ///< Annualised investment cost
                                         ///< [$/MWh_LHV-year].
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor.
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable.

  // ── State-variable / cycle policy ───────────────────────────────
  OptBool use_state_variable {};  ///< SDDP-style cross-phase coupling.
  OptBool daily_cycle {};  ///< PLP-style daily-cycle scaling.  Default
                           ///< false for H₂: hydrogen storage is the
                           ///< canonical seasonal / multi-week store.
};

}  // namespace gtopt
