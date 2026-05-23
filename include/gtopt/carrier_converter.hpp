/**
 * @file      carrier_converter.hpp
 * @brief     Multi-carrier energy converter (electrolyser, fuel cell, HB, …)
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * ``CarrierConverter`` bridges any pair of carrier-typed balance nodes.
 * One input unit at the ``from`` node produces ``efficiency`` units at
 * the ``to`` node — same shape as a one-stage chemical / thermal /
 * electrochemical converter:
 *
 *   * ``from = Electric, to = Hydrogen``  → **electrolyser** (η ≈ 0.7)
 *   * ``from = Hydrogen, to = Electric``  → **fuel cell** (η ≈ 0.5)
 *   * ``from = Hydrogen, to = Ammonia``   → **Haber-Bosch** (η ≈ 0.7)
 *   * ``from = Ammonia,  to = Hydrogen``  → **NH₃ cracker** (η ≈ 0.7)
 *   * ``from = Electric, to = Thermal``   → **e-boiler / e-heater**
 *   * ``from = Thermal,  to = Electric``  → **CSP power block** (η ≈ 0.4)
 *
 * Composing two converters with the appropriate efficiencies gives the
 * full P2H2 + H2P round trip (~35%) and the P2NH3 + NH3-to-P chain
 * (~25–30%) cited in the literature (Acar & Dincer 2018; NREL/TP-
 * 5400-89569; MacFarlane et al., Joule 2020).
 *
 * The carrier of each side is encoded explicitly via the ``Carrier``
 * enum (no magic strings).  The validator resolves ``from_node`` /
 * ``to_node`` against the matching ``XxxNode`` array; mistakenly
 * pointing an electrolyser's output at a thermal node fails at
 * planning time.
 *
 * @see carrier.hpp           Carrier enum
 * @see thermal_node.hpp      typed balance nodes
 * @see hydrogen_node.hpp
 * @see ammonia_node.hpp
 * @see converter.hpp         legacy battery-coupling Converter (different)
 */

#pragma once

#include <gtopt/carrier.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct CarrierConverter
 * @brief One-stage flow converter between two carrier-typed nodes.
 *
 * The LP creates one ``input`` column per (scenario, stage, block)
 * with operating cost ``ocost``, upper bound ``capacity`` (per-block
 * MW).  The from-node balance row gets coefficient ``−1`` × input
 * (withdraws); the to-node balance row gets coefficient
 * ``+efficiency`` × input (injects).
 */
struct CarrierConverter
{
  /// Canonical class-name used in LP row labels.
  static constexpr LPClassName class_name {"CarrierConverter"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional tag ("electrolyser", "fuel_cell",
                    ///< "haber_bosch", "nh3_cracker", "power_block", …)

  /// Source carrier.  REQUIRED — the validator uses this to resolve
  /// ``from_node`` against the matching ``Xxx`` array (Bus,
  /// ThermalNode, HydrogenNode, AmmoniaNode).
  Carrier from_carrier {Carrier::Electric};

  /// Destination carrier.  REQUIRED — same role as ``from_carrier``.
  Carrier to_carrier {Carrier::Electric};

  /// Reference to the source node.  Resolved against the
  /// carrier-matching node array at planning time.
  OptSingleId from_node {};

  /// Reference to the destination node.  Same rules as ``from_node``.
  OptSingleId to_node {};

  /// Conversion efficiency (output units per input unit).  Default 1.0.
  /// Per-(stage, block) so part-load efficiency curves can be modelled
  /// by passing a TBRealFieldSched.
  OptTBRealFieldSched efficiency {};

  /// Operating cost per unit of input flow.  Default 0.0.  Captures
  /// O&M, water cost (electrolyser), nitrogen cost (Haber-Bosch),
  /// catalyst replacement amortised per MWh, etc.
  OptTBRealFieldSched ocost {};

  // ── Capacity expansion ──────────────────────────────────────────
  OptTRealFieldSched capacity {};  ///< Installed input capacity
                                   ///< (per-block, MW on whichever side
                                   ///< ``from_carrier`` measures it).
  OptTRealFieldSched expcap {};  ///< Capacity per expansion module.
  OptTRealFieldSched expmod {};  ///< Maximum number of expansion modules.
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity.
  OptTRealFieldSched annual_capcost {};  ///< Annualised investment cost.
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor.
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable.
};

}  // namespace gtopt
