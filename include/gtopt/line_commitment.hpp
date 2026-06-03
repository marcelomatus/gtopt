/**
 * @file      line_commitment.hpp
 * @brief     Per-line Optimal Transmission Switching (OTS) parameters
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the LineCommitment structure which marks a transmission
 * Line as a switching candidate (Fisher-O'Neill-Ferris 2008 [1] OTS
 * decision variable: per-line ``u_l ∈ {0, 1}`` opens or closes the
 * circuit during the optimization).  Each LineCommitment entry links
 * to exactly one Line via a foreign key.
 *
 * OTS is strictly opt-in: the presence of a LineCommitment row makes
 * the referenced Line a switching candidate, absence keeps it pure
 * LP.  This sparsity discipline is critical — Fisher 2008 shows that
 * most savings come from a handful of lines [1], and even single-
 * stage 118-bus all-line OTS exceeds Gurobi's 1 h limit on ~half the
 * test instances at 0.01 % gap [2].
 *
 * LineCommitment constraints are only enforced on stages marked as
 * chronological.  On non-chronological stages (e.g. duration-weighted
 * representative blocks) the commitment is silently skipped and the
 * line behaves as if no LineCommitment row existed — matching the
 * convention of the existing ``Commitment`` element for generators
 * (see commitment.hpp:14).
 *
 * Method gate: OTS is rejected on the SDDP and cascade methods.  The
 * mathematical reason is in issue #509 §"Why monolithic only, not
 * SDDP" — Benders cuts on a mixed-integer subproblem are unsound
 * (Zou-Ahmed-Sun 2019).  The validation step in
 * ``validate_planning.cpp`` raises a structured error if any active
 * LineCommitment row coexists with ``method = sddp`` or ``cascade``.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 42,
 *   "name": "L_18_19_ots",
 *   "line": "L_18_19",
 *   "initial_status": 1,
 *   "kvl_big_m": 18.5
 * }
 * ```
 *
 * @see LineCommitmentLP for the LP formulation (capacity gating +
 *      KVL big-M disjunction)
 * @see Line for the linked transmission line
 *
 * ### References
 *
 * [1] Fisher, O'Neill, Ferris (2008).  *Optimal Transmission
 *     Switching*.  IEEE T-PWRS 23(3):1346-1355.
 * [2] Aguilar-Moreno, Pineda, Morales (2025).  *On the computational
 *     complexity of OTS*.  arXiv:2502.10333.
 */

#pragma once

#include <gtopt/commitment.hpp>  // for StartsScope / OptStartsScope
#include <gtopt/enum_option.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct LineCommitment
 * @brief Marks a Line as an OTS switching candidate (issue #509)
 *
 * Links to a Line and exposes the binary status variable
 * ``u_l ∈ {0, 1}`` that the LP can flip to open or close the circuit.
 * The LP rows emitted by ``LineCommitmentLP`` (see ``line_commitment_lp.hpp``)
 * are:
 *
 *   1. Capacity gating (always emitted, both Kirchhoff and transport):
 *
 *        −F^max_ba · u_l ≤ f_l ≤ F^max_ab · u_l
 *
 *      so ``u_l = 0`` ⇒ ``f_l = 0`` (open circuit, no flow).
 *
 *   2. KVL big-M disjunction (Kirchhoff mode only):
 *
 *        f_l ≥ b_eff · Δθ_l − M_l · (1 − u_l)
 *        f_l ≤ b_eff · Δθ_l + M_l · (1 − u_l)
 *
 *      where ``M_l = |b_eff| · (θ_max − θ_min)`` (Fisher 2008
 *      baseline; iterative tightening per Pineda 2024 [3] is a v1
 *      follow-up that writes back to ``kvl_big_m``).
 *
 * Roadmap status (see issue #509):
 *   * v0   — capacity gating.
 *   * v0.5 — node_angle KVL big-M disjunction.
 *   * v1   — cycle_basis KVL big-M disjunction (both Kirchhoff modes
 *            now supported); per-line ``kvl_big_m`` override.
 *   * v1.1 — u/v/w three-binary decomposition with
 *            ``startup_cost`` / ``shutdown_cost``.
 *   * v1.2 — ``min_up_time`` / ``min_down_time`` anti-flicker rows,
 *            ``max_starts`` / ``min_starts`` / ``starts_scope``
 *            rolling-window cap (mirroring ``Commitment``).
 *
 * Deferred for v1.3+: PowerModels.jl IND+ indicator-constraint
 * alternative, iterative big-M tightening (Pineda 2024 [3] pre-solve
 * that writes back to ``kvl_big_m``), startup tiers.
 *
 * ### References
 *
 * [3] Pineda, Morales, Porras, Domínguez (2024).  *Iterative tightening
 *     of big-M for OTS*.  arXiv:2306.02784.
 */
struct LineCommitment
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields.  Single source of truth — ``LineCommitmentLP`` exposes
  /// no separate ``ClassName`` member; callers reach the constant via
  /// ``LineCommitment::class_name`` directly.
  static constexpr LPClassName class_name {"LineCommitment"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional element type/category tag
  OptName description {};  ///< Optional free-text description

  SingleId line {unknown_uid};  ///< FK to the Line being switched

  /// Initial closed/open state at ``t = 0``.  ``1.0`` = closed
  /// (in-service), ``0.0`` = open.  When unset, the LP leaves
  /// ``u_l`` free at ``t = 0``.  Used by the v0 LP build as a one-
  /// time bound on the first-block ``u_l``; the v1 u/v/w transition
  /// logic will additionally constrain ``v_l[0] − w_l[0] = u_l[0] −
  /// initial_status``.
  OptReal initial_status {};

  /// Force the binary status to ``u_l = 1`` for every block
  /// (commitment-equivalent "must run" — line pinned closed).  When
  /// ``true`` the LP folds the bound ``u_l = 1`` into the binary
  /// column directly so the MIP solver has no branching choice on
  /// this line; the LP build degenerates to the pre-OTS pure-LP
  /// path (capacity rows reduce to ``±F^max ≥ ±f_l``, KVL big-M
  /// rows collapse to the exact equality).  Default ``false``.
  OptBool must_run {};

  /// Per-(stage, block) forced commitment status — pins ``u_l`` to a
  /// specific value at every (stage, block) where it is set.  ``1.0``
  /// ⇒ pinned closed, ``0.0`` ⇒ pinned open; blocks where the
  /// schedule has no entry leave ``u_l`` free.  Mirrors
  /// ``Commitment.fixed_status`` semantics for line topology
  /// scheduling.  Overrides ``must_run`` per block when both are
  /// set.  When the underlying ``Line.in_service`` schedule resolves
  /// to ``0`` for a block (exogenous outage), the LP pins
  /// ``u_l[t,b] = 0`` REGARDLESS of ``fixed_status`` — the
  /// physical outage trumps the operator pin.
  OptTBRealFieldSched fixed_status {};

  /// LP-relax the binary ``u_l`` to ``[0, 1]`` continuous.
  /// Diagnostic switch: turns the OTS MIP into a pure LP whose
  /// optimal objective is a LOWER bound on the MIP optimum.  Useful
  /// to scope the integrality gap before committing to a full MIP
  /// solve.  Default ``false``.
  OptBool relax {};

  /// Per-line override of the KVL disjunction big-M parameter.
  /// When unset, ``LineCommitmentLP`` defaults to
  /// ``M_l = |b_eff| · (θ_max − θ_min)`` (Fisher 2008 baseline,
  /// known loose).  Set explicitly when an iterative-tightening
  /// pre-solve (Pineda 2024 [3]) has computed a tighter bound for
  /// this line — providing the write-back target from day 1 means
  /// the v1 tightening pass doesn't require a schema migration.
  ///
  /// REQUIRED to be positive when set.  A non-positive ``kvl_big_m``
  /// makes the KVL disjunction unbounded and the LP-relaxation
  /// trivially feasible (every flow value allowed regardless of
  /// θ-difference); ``validate_planning`` rejects such inputs.
  ///
  /// Inert in transport mode (``use_kirchhoff = false``) — the LP
  /// build emits no KVL row in that mode and the big-M is never
  /// consumed.
  OptReal kvl_big_m {};

  /// Per-event line-CLOSING (startup) cost ($).  When set together
  /// with ``shutdown_cost`` (or alone), ``LineCommitmentLP`` switches
  /// from the simple ``u_l ∈ {0,1}`` form to the Knueven–Ostrowski–
  /// Watson 2020 / Morales-España et al. 2013 three-binary
  /// decomposition: ``u_l`` (status) is the only declared integer,
  /// ``v_l ∈ [0,1]`` (closing event) and ``w_l ∈ [0,1]`` (opening
  /// event) are continuous-but-implied-binary auxiliaries, joined by
  ///   C1:  u_l[t] − u_l[t−1] − v_l[t] + w_l[t] = 0
  ///   C3:  v_l[t] + w_l[t] ≤ 1
  /// The objective gains ``startup_cost · Σ v_l`` and
  /// ``shutdown_cost · Σ w_l``.  Inert on non-chronological stages
  /// (the v/w decomposition is meaningless without block ordering;
  /// mirrors ``CommitmentLP``).
  OptReal startup_cost {};

  /// Per-event line-OPENING (shutdown) cost ($).  Symmetric to
  /// ``startup_cost``; setting either field activates the u/v/w
  /// decomposition.  Both default to zero (no event cost ⇒ the
  /// simple ``u_l``-only form remains in use).
  OptReal shutdown_cost {};

  /// Minimum up time [hours].  Mirrors ``Commitment::min_up_time``.
  /// Once the breaker closes (``v_l[t] = 1``), the LP must keep
  /// ``u_l = 1`` for at least ``min_up_time`` hours starting at
  /// block ``t``.  Emitted as the row family
  ///   Σ_{q=t}^{t + UT_blocks − 1} u_l[q] ≥ UT_blocks · v_l[t]
  /// where ``UT_blocks`` is the smallest number of forward blocks
  /// whose cumulative duration covers ``min_up_time``.  Trivially
  /// satisfied (and skipped) when the window collapses to 1 block.
  /// Requires u/v/w (``startup_cost`` or ``shutdown_cost`` set);
  /// silently inert when u/v/w is off.
  OptReal min_up_time {};

  /// Minimum down time [hours].  Mirrors ``Commitment::min_down_time``.
  /// Symmetric to ``min_up_time``: once the breaker opens
  /// (``w_l[t] = 1``), the LP must keep ``u_l = 0`` for at least
  /// ``min_down_time`` hours.  Emitted as
  ///   Σ_{q=t}^{t + DT_blocks − 1} u_l[q] + DT_blocks · w_l[t]
  ///       ≤ DT_blocks
  /// where ``DT_blocks`` covers ``min_down_time`` hours starting at
  /// block ``t``.  Same activation gate as ``min_up_time``.
  OptReal min_down_time {};

  /// Upper bound on the number of CLOSE events (``v_l = 1``) over a
  /// rolling time window.  Mirrors ``Commitment::max_starts`` (PLEXOS
  /// "Max Starts {Hour|Day|Week|Month|Year}").  Emitted as one row
  /// per window:
  ///   Σ_{t ∈ window} v_l[t] ≤ max_starts
  /// Window length is resolved by the SHARED ``starts_scope`` —
  /// see ``LineCommitment::starts_window_hours``.
  OptInt max_starts {};

  /// Lower bound on the number of CLOSE events (``v_l = 1``) over a
  /// rolling time window.  Mirrors ``Commitment::min_starts``.  Both
  /// bounds share the same window LHS:
  ///   min_starts ≤ Σ_{t ∈ window} v_l[t] ≤ max_starts
  /// Unset ⇒ no lower row emitted (effectively 0).
  OptInt min_starts {};

  /// Window-scope for ``max_starts`` / ``min_starts``.  Mirrors
  /// ``Commitment::starts_scope`` — accepts either the named enum
  /// (``"hour" | "day" | "week" | "horizon"``; aliases ``"month"``
  /// and ``"year"`` collapse to horizon) or an integer hour count
  /// (e.g. ``48`` for 2 days).  Resolved at LP-build time by
  /// ``starts_window_hours()``.  Unset / unknown ⇒ horizon (one row
  /// per stage).
  OptStartsScope starts_scope {};

  /// Resolve ``starts_scope`` to a window length in hours.  Mirrors
  /// ``Commitment::starts_window_hours()``: integer values pass
  /// through verbatim; named values map ``hour → 1``, ``day → 24``,
  /// ``week → 168``, ``horizon → 0`` (sentinel "never flush until
  /// stage end").  Unrecognised names / unset both return ``0``.
  [[nodiscard]] constexpr double starts_window_hours() const noexcept
  {
    if (!starts_scope.has_value()) {
      return 0.0;
    }
    if (std::holds_alternative<Int>(*starts_scope)) {
      const auto h = std::get<Int>(*starts_scope);
      return h > 0 ? static_cast<double>(h) : 0.0;
    }
    const auto& name = std::get<Name>(*starts_scope);
    const auto resolved = enum_from_name<StartsScope>(name);
    if (!resolved.has_value()) {
      return 0.0;
    }
    switch (*resolved) {
      case StartsScope::Hour:
        return 1.0;
      case StartsScope::Day:
        return 24.0;
      case StartsScope::Week:
        return 7.0 * 24.0;
      case StartsScope::Horizon:
        return 0.0;
    }
    return 0.0;
  }

  /// @name Startup-cost tiers (hot/warm/cold) — v1.3.
  /// Mirrors ``Commitment``'s hot/warm/cold tier scheme.  Activates
  /// when ALL FIVE fields are set and u/v/w is on (startup_cost or
  /// shutdown_cost present).  When active, the flat ``startup_cost``
  /// is REPLACED by a tier-dependent cost based on how long the
  /// breaker was open before the close event:
  ///
  ///   offline_hours ∈ [0, hot_start_time]            → hot_start_cost
  ///   offline_hours ∈ (hot_start_time, cold_start_time] → warm_start_cost
  ///   offline_hours >  cold_start_time               → cold_start_cost
  ///
  /// LP form (mirrors ``CommitmentLP`` C8/C9/C10): one tier indicator
  /// per block ``y^hot_l[t], y^warm_l[t], y^cold_l[t] ∈ [0, 1]`` plus
  ///   C8:   v_l[t] = y^hot_l[t] + y^warm_l[t] + y^cold_l[t]
  ///   C9:   y^hot_l[t]  ≤ Σ_{q ∈ hot window}  w_l[q]
  ///   C10:  y^warm_l[t] ≤ Σ_{q ∈ warm window} w_l[q]
  /// (cold is the residual via C8 — no explicit row needed).
  /// Pre-event window membership uses block durations: a block ``q``
  /// is in ``t``'s hot window if Σ duration[q+1..t-1] ≤ hot_start_time.
  ///
  /// Requires ``cold_start_time >= hot_start_time``; ``validate_planning``
  /// rejects otherwise (and ``LineCommitmentLP`` logs + skips tiers as
  /// a defensive fallback).
  /// @{
  OptReal hot_start_cost {};  ///< $/start when offline ≤ hot_start_time
  OptReal warm_start_cost {};  ///< $/start when offline in (hot, cold]
  OptReal cold_start_cost {};  ///< $/start when offline > cold_start_time
  OptReal hot_start_time {};  ///< Hot/warm boundary [hours]
  OptReal cold_start_time {};  ///< Warm/cold boundary [hours]
  /// @}

  /// Hours offline before the stage starts (pre-stage state).
  /// Used to size the C9/C10 hot/warm windows that look BACKWARDS
  /// from block ``t = 0``: an empty stage prefix means there's no
  /// past ``w`` data to draw from, so ``initial_hours`` substitutes
  /// for the pre-stage shutdown duration.  Defaults to a large
  /// sentinel (``1e6``) so an unset value means "the breaker has been
  /// open forever" (⇒ always cold start at ``t = 0``).
  OptReal initial_hours {};

  /// True iff all five tier fields are set (LP build then activates
  /// the tier rows and zeroes the flat startup_cost contribution).
  [[nodiscard]] constexpr bool has_startup_tiers() const noexcept
  {
    return hot_start_cost.has_value() && warm_start_cost.has_value()
        && cold_start_cost.has_value() && hot_start_time.has_value()
        && cold_start_time.has_value();
  }
};

}  // namespace gtopt
