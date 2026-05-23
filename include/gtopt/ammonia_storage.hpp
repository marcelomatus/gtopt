/**
 * @file      ammonia_storage.hpp
 * @brief     Ammonia (NH₃) storage — long-term carrier of H₂
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ``AmmoniaStorage`` element: peer of ``HydrogenStorage``
 * on the ``StorageLP<>`` framework.  Ammonia is the dominant carrier
 * proposal for long-duration / inter-continental hydrogen storage:
 *
 *   * **Liquefaction at -33 °C @ 1 atm** — vastly cheaper than the
 *     LH₂ liquefaction chain (-253 °C, ~30% energy penalty).
 *   * **Volumetric energy density ≈ 11.5 MJ/L** at 1 atm liquid —
 *     ≈ 60% higher than LH₂.
 *   * **Existing infrastructure** — port terminals, pipelines and
 *     180+ million t/year merchant market (today, almost all fossil-
 *     derived).
 *   * **Round-trip penalty** — Haber-Bosch (η ≈ 0.5) + NH₃ cracker
 *     (η ≈ 0.5) → ≈ 25–30% H₂→H₂ round trip in the literature.
 *
 * Energy is bookkept in MWh_LHV so the balance row at the
 * ``AmmoniaNode`` sums consistently with the converter flows out of
 * Haber-Bosch and into the NH₃ cracker / combustor.  Mass
 * conversion: 1 kg-NH₃ ≈ 5.17 kWh_LHV.
 *
 * Self-discharge is dominated by **boil-off** (refrigerated tank
 * losses, typically 0.04–0.1% / day → ≈ 1.5–3.5% / year), an order of
 * magnitude lower than LH₂ boil-off.
 *
 * Sources:
 *   * Acar & Dincer (2018) — *Review of NH₃ for energy storage*.
 *   * NREL/TP-5400-89569 (2023) — *Off-Grid Green Ammonia Plant TEA*.
 *   * MacFarlane et al. (2020) — *A roadmap to the ammonia economy*,
 *     Joule 4(6), DOI:10.1016/j.joule.2020.04.004.
 *
 * @see ammonia_node.hpp        carrier-tagged balance node
 * @see hydrogen_storage.hpp    upstream H₂ storage
 * @see storage.hpp             generic Storage base
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct AmmoniaStorage
 * @brief Ammonia (NH₃) energy storage — long-term H₂ carrier.
 *
 * Data-only definition.  The LP formulation is provided by
 * ``AmmoniaStorageLP`` which inherits directly from ``StorageLP<>``.
 *
 * @see AmmoniaStorageLP
 */
struct AmmoniaStorage
{
  /// Canonical class-name constant used in LP row labels and PAMPL
  /// element resolution.  Matches ``ammonia_storage_array`` in JSON.
  static constexpr LPClassName class_name {"AmmoniaStorage"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional tag ("refrigerated", "pressurised",
                    ///< "underground", …)

  /// Reference to an ``AmmoniaNode`` (NOT a ``HydrogenNode`` and NOT
  /// a ``Bus``).  Resolved against ``system.ammonia_node_array``.
  OptSingleId ammonia_node {};

  // ── Efficiencies ────────────────────────────────────────────────
  /// Charging (synthesis-side) round-trip share captured downstream of
  /// Haber-Bosch.  Default 1.0 — Haber-Bosch efficiency is modelled on
  /// the ``Converter`` that feeds this storage, not here.
  OptTBRealFieldSched input_efficiency {};
  /// Withdrawal (cracker / combustor) round-trip share captured
  /// downstream of the converter.  Default 1.0.
  OptTBRealFieldSched output_efficiency {};
  /// Self-discharge from boil-off [p.u./year].  Refrigerated NH₃ tank
  /// ≈ 0.015–0.035 (0.04–0.1% per day).  Substantially smaller than
  /// LH₂ boil-off (~0.18/year).
  OptTRealFieldSched annual_loss {};

  // ── Energy bounds (MWh_LHV) ─────────────────────────────────────
  OptTBRealFieldSched emin {};  ///< Minimum stored energy [MWh_LHV].
  OptTBRealFieldSched emax {};  ///< Maximum stored energy [MWh_LHV].
  OptTBRealFieldSched ecost {};  ///< Holding cost [$/MWh_LHV] (optional).

  OptReal eini {};  ///< Initial stored energy [MWh_LHV].
  OptReal efin {};  ///< Minimum terminal stored energy [MWh_LHV].
  OptReal efin_cost {};  ///< Penalty per unit of efin shortfall [$/MWh_LHV].

  OptTBRealFieldSched soft_emin {};  ///< Soft minimum SoC [MWh_LHV].
  OptTBRealFieldSched soft_emin_cost {};  ///< Penalty on soft-emin shortfall.

  // ── Capacity expansion ──────────────────────────────────────────
  OptTRealFieldSched capacity {};  ///< Installed storage size [MWh_LHV].
                                   ///< Typical merchant terminal:
                                   ///< 30–60 kt → 155–310 GWh_LHV.
  OptTRealFieldSched expcap {};  ///< Capacity per expansion module.
  OptTRealFieldSched expmod {};  ///< Maximum number of expansion modules.
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity.
  OptTRealFieldSched
      annual_capcost {};  ///< Annualised investment cost [$/MWh_LHV-year].
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor.
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable.

  // ── State-variable / cycle policy ───────────────────────────────
  OptBool use_state_variable {};  ///< SDDP-style cross-phase coupling.
                                  ///< Default true — NH₃ is the
                                  ///< canonical seasonal carrier.
  OptBool daily_cycle {};  ///< PLP-style daily-cycle scaling.  Default
                           ///< false — NH₃ tanks span months, not days.
};

}  // namespace gtopt
