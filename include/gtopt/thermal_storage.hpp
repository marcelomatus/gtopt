/**
 * @file      thermal_storage.hpp
 * @brief     Thermal Energy Storage (TES) — CSP / district-heat storage
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ``ThermalStorage`` element: a peer of ``Battery`` and
 * ``Reservoir`` that sits directly on the ``StorageLP<>`` framework.
 * Used to model two-tank molten-salt TES in Concentrated Solar Power
 * plants and large-scale thermal storage in district-heat / industrial
 * heat applications.
 *
 * Energy is in **MWh_th** (thermal megawatt-hours) — distinct from
 * the MWh_e on Battery.  The carrier-side balance node is a
 * ``ThermalNode``, not a ``Bus``: the two are separate C++ types,
 * so the validator cannot accidentally resolve ``thermal_node`` to
 * an electric bus.
 *
 * ### Topology (CSP example)
 * ```
 *  ThermalNode (solar field side)
 *      │ MWh_th
 *      ▼
 *  ThermalStorage
 *      │ MWh_th
 *      ▼
 *  Converter ─► PowerBlock Generator (electric Bus, MW_e)
 * ```
 *
 * ### Differences vs Battery
 *   * No ``source_generator`` (handled externally by the power block).
 *   * No internal ``commitment`` (the power block carries it).
 *   * ``emin`` represents the molten-salt freezing heel.
 *   * ``annual_loss`` is TES self-discharge from insulation losses
 *     (typical 5%/year ≈ 1 °C/day on a well-insulated two-tank
 *     molten-salt system).
 *   * Default unit is MWh_th, not MWh_e.
 *
 * @see thermal_node.hpp    carrier-tagged balance node
 * @see battery.hpp         peer electric-carrier storage
 * @see reservoir.hpp       peer water-carrier storage
 * @see storage.hpp         generic Storage base
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct ThermalStorage
 * @brief Thermal Energy Storage (sensible / latent / molten-salt).
 *
 * Data-only definition.  The LP formulation is provided by
 * ``ThermalStorageLP`` which inherits directly from ``StorageLP<>``.
 *
 * @see ThermalStorageLP
 */
struct ThermalStorage
{
  /// Canonical class-name constant used in LP row labels and PAMPL
  /// element resolution.  Matches ``thermal_storage_array`` in JSON.
  static constexpr LPClassName class_name {"ThermalStorage"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional tag (e.g. "molten_salt", "concrete")

  /// Reference to a ``ThermalNode`` (NOT a ``Bus``).  The validator
  /// resolves this against ``system.thermal_node_array``; mistakenly
  /// pointing at an electric bus is rejected at planning time.
  OptSingleId thermal_node {};

  // ── Efficiencies ────────────────────────────────────────────────
  OptTBRealFieldSched
      input_efficiency {};  ///< Charging efficiency [p.u.]  per-(stage, block).
                            ///< Captures HTF→salt heat-exchanger losses.
  OptTBRealFieldSched
      output_efficiency {};  ///< Discharging efficiency [p.u.] per-(stage,
                             ///< block). Captures salt→HTF losses.
  OptTRealFieldSched annual_loss {};  ///< TES self-discharge [p.u./year].
                                      ///< Typical 0.05 (≈ 5%/year ≈ 1 °C/day on
                                      ///< a two-tank molten salt system).

  // ── Energy bounds (MWh_th) ──────────────────────────────────────
  OptTBRealFieldSched
      emin {};  ///< Minimum thermal SoC [MWh_th].  The **molten-salt
                ///< freezing heel** — TES cannot fall below the energy
                ///< corresponding to the cold-tank floor temperature
                ///< (≈230 °C for solar-salt 60/40 NaNO₃/KNO₃).
  OptTBRealFieldSched
      emax {};  ///< Maximum thermal SoC [MWh_th] — useful TES capacity
                ///< (hot-tank − cold-tank Δ in stored-energy terms).
                ///< 100 MW_e plant × 6 h × 0.4 Rankine η = 1500 MWh_th.
  OptTBRealFieldSched ecost {};  ///< Storage usage cost [$/MWh_th]; optional
                                 ///< holding penalty, usually 0.

  OptReal eini {};  ///< Initial thermal SoC [MWh_th].
  OptReal efin {};  ///< Minimum terminal SoC [MWh_th].
  OptReal efin_cost {};  ///< Penalty per unit of efin shortfall [$/MWh_th].

  OptTBRealFieldSched soft_emin {};  ///< Soft minimum SoC [MWh_th].
  OptTBRealFieldSched
      soft_emin_cost {};  ///< Penalty on soft-emin shortfall [$/MWh_th].

  // ── Capacity expansion ──────────────────────────────────────────
  OptTRealFieldSched capacity {};  ///< Installed thermal capacity [MWh_th].
  OptTRealFieldSched expcap {};  ///< Capacity per expansion module [MWh_th].
  OptTRealFieldSched expmod {};  ///< Maximum number of expansion modules.
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity [MWh_th].
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MWh_th-year].
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year].
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable.

  // ── State-variable / cycle policy ───────────────────────────────
  OptBool use_state_variable {};  ///< SDDP-style cross-phase coupling.  Default
                                  ///< true for CSP (multi-day inertia common).
  OptBool daily_cycle {};  ///< PLP-style daily-cycle scaling.  Default false
                           ///< for CSP (TES typically spans more than 24 h).
};

}  // namespace gtopt
