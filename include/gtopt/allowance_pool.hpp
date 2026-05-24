/**
 * @file      allowance_pool.hpp
 * @brief     CO₂ / pollutant Cap-and-Trade allowance pool
 * @date      Sat May 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ``AllowancePool`` element: peer of ``Battery`` /
 * ``ThermalStorage`` / ``HydrogenStorage`` / ``AmmoniaStorage`` on
 * the ``StorageLP<>`` framework, retargeted to tradable emission
 * allowances (tCO₂ equivalent).
 *
 * Mirrors the EU ETS / California Cap-and-Trade / RGGI mechanism:
 *
 *   * **Free allocation** (``delivery``) — allowances granted to
 *     the pool each stage at no cost (the grandfathering /
 *     benchmarking entitlement).
 *   * **Emissions consumption** — total CO₂ produced by the
 *     EmissionZones coupled to this pool is debited each stage
 *     (LP-side: ``EmissionZoneLP::production_cols`` feeds the
 *     pool's outflow row; see Phase 3 of the CO₂ work).
 *   * **Banking** — unused allowances carry forward to the next
 *     stage (``Storage`` state variable).  ``emin`` defaults to
 *     zero (no borrowing); set ``emin < 0`` to allow borrowing
 *     against future allocations.
 *   * **Auction** (Phase 4) — additional allowances bought at
 *     ``auction_price`` ($/tCO₂), capped by ``auction_cap``.
 *
 * Energy unit is **tCO₂** (or whichever pollutant the pool's
 * ``emission`` reference points at — the unit follows the upstream
 * ``Emission`` element's basis).
 *
 * @see emission_zone.hpp        per-region production + cap row
 * @see emission_source.hpp      per-generator emission rate
 * @see storage.hpp              generic Storage base
 * @see battery.hpp              electric-carrier storage peer
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct AllowancePool
 * @brief Tradable emission-allowance pool (cap-and-trade).
 *
 * Data-only definition.  The LP formulation is provided by
 * ``AllowancePoolLP`` which inherits directly from ``StorageLP<>``.
 *
 * @see AllowancePoolLP
 */
struct AllowancePool
{
  /// Canonical class-name constant used in LP row labels and PAMPL
  /// element resolution.  Matches ``allowance_pool_array`` in JSON.
  static constexpr LPClassName class_name {"AllowancePool"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name (e.g. "EU_ETS", "CA_CapTrade")
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional regulatory regime tag (e.g.
                    ///< "eu_ets", "california_capntrade", "rggi",
                    ///< "voluntary_offset")

  /// Reference to the pollutant ``Emission`` this pool covers
  /// (typically "co2" — but any Emission element works:
  /// ``"so2"``, ``"nox"``, ``"ch4"``, etc.).  The validator
  /// resolves against ``system.emission_array``; the pool's
  /// outflow ties into the EmissionZones that target the same
  /// pollutant (see ``AllowancePoolLP``).
  OptSingleId emission {};

  /// Annualised allowance-decay rate `[p.u./year]`, stage-
  /// schedulable.  Typically **0** for allowance pools (CO₂
  /// permits don't physically decay), but some regulatory
  /// schemes apply a Linear Reduction Factor (LRF) or scheduled
  /// retirement of bank balances — e.g. the EU ETS Market
  /// Stability Reserve removes ~12 %/yr of any bank above its
  /// upper threshold.  Default unset = no decay.
  OptTRealFieldSched annual_loss {};

  // ── Allowance bounds (tCO₂) ─────────────────────────────────────
  OptTBRealFieldSched emin {};  ///< Minimum banked allowances [tCO₂].  Defaults
                                ///< to 0 (no borrowing).  Set ``emin < 0`` to
                                ///< allow borrowing against future allocations
                                ///< (some ETS schemes permit limited borrowing
                                ///< within a compliance period).
  OptTBRealFieldSched emax {};  ///< Maximum banked allowances [tCO₂].  Defaults
                                ///< to +∞ (unlimited banking).  Set a finite
                                ///< value when the regulator caps long-term
                                ///< hoarding (e.g. some Phase IV ETS rules).
  OptTBRealFieldSched ecost {};  ///< Holding cost [$/tCO₂] (optional).
                                 ///< Typically 0 — allowances don't
                                 ///< decay materially.  Set positive
                                 ///< for behavioural decay or to
                                 ///< incentivise emission reductions
                                 ///< over banking.

  OptReal eini {};  ///< Initial banked allowances at simulation start
                    ///< [tCO₂].
  OptReal efin {};  ///< Minimum terminal banked allowances [tCO₂].
                    ///< Compliance with end-of-period banking
                    ///< requirements.
  OptReal efin_cost {};  ///< Penalty per unit of efin shortfall
                         ///< [$/tCO₂].  Soft-cap behaviour mirrors
                         ///< ``Reservoir.efin_cost``.

  OptTBRealFieldSched soft_emin {};  ///< Soft minimum banked level [tCO₂].
  OptTBRealFieldSched
      soft_emin_cost {};  ///< Penalty on soft-emin shortfall [$/tCO₂].

  // ── Free allocation (grandfathering / benchmarking) ─────────────
  OptTRealFieldSched delivery {};  ///< Free allowance allocation per
                                   ///< stage [tCO₂/stage].  Mirrors EU
                                   ///< ETS / CA C&T grandfathered
                                   ///< allowance bookkeeping.  In a
                                   ///< pure auction market (no free
                                   ///< allocation), leave unset.

  // ── Auction / market purchases (Phase 4 — wired later) ──────────
  OptTBRealFieldSched
      auction_price {};  ///< Market price for buying additional
                         ///< allowances [$/tCO₂].  When set, gtopt
                         ///< adds a per-block ``auction`` column to
                         ///< the pool's outflow, priced at this
                         ///< per-tonne cost.  Models the secondary
                         ///< market / regulator auction.
  OptTBRealFieldSched
      auction_cap {};  ///< Maximum allowances purchasable per
                       ///< (stage, block) [tCO₂].  Defaults to +∞;
                       ///< set when an auction has a fixed lot size
                       ///< or the secondary market is illiquid.

  // ── Capacity expansion (rare — allowance pools usually don't expand) ──
  OptTRealFieldSched capacity {};  ///< Installed pool size [tCO₂].
  OptTRealFieldSched expcap {};  ///< Per-expansion-module capacity.
  OptTRealFieldSched expmod {};  ///< Maximum expansion modules.
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity.
  OptTRealFieldSched annual_capcost {};  ///< Annualised investment cost.
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor.
  OptBool integer_expmod {};  ///< Integer-constrain expansion modules.

  // ── State-variable / cycle policy ───────────────────────────────
  OptBool use_state_variable {};  ///< SDDP-style cross-phase coupling.
                                  ///< Default true — multi-year banking
                                  ///< is the canonical use case.
  OptBool daily_cycle {};  ///< PLP-style daily-cycle scaling.  Default
                           ///< false — allowance pools span months /
                           ///< years, not days.
};

}  // namespace gtopt
