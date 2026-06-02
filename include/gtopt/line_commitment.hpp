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
 * v0 (this commit) ships ONLY the capacity gating + KVL big-M
 * disjunction.  The u/v/w 3-bin transition logic, min-up / min-down
 * times, max-starts windows, and the indicator-constraint alternative
 * (PowerModels.jl-style IND+) land in follow-up commits — see issue
 * #509 §"Implementation roadmap".
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
};

}  // namespace gtopt
