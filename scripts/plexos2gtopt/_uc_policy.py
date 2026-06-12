"""User-constraint penalty + directive policy — single source of truth.

The legacy classification path in :func:`plexos2gtopt.parsers.extract_user_constraints`
mixed name-regex tests with hard-coded ``_HYDRO_UC_SOFT_PENALTY`` /
``_RESERVE_PROVISION_SUM_PENALTY`` scalars to decide whether a PLEXOS
``Constraint`` should ship HARD (``penalty = 0``), at the operational soft
tier ($10 / unit), or at the high reserve-aggregation tier ($1000 / unit).
The C++ side meanwhile gained ``ConstraintDirective``
(``include/gtopt/constraint_directive.hpp``) — a typed payload that names
the family + carries an optional ``penalty`` override.  ``UserConstraintLP``
prefers the directive's penalty over the scalar at LP-build time
(``source/user_constraint_lp.cpp:892-900``), so the directive is the
authoritative wire form.

This module is the **Python-side** equivalent: every PLEXOS
``Constraint`` flows through :func:`classify` which returns the
``(emitted_penalty, directive)`` pair the parser stamps onto the
emitted ``UserConstraintSpec``.  Centralising the policy here means:

* the penalty values live in ONE place (no shadow ``param soft_floor_penalty = 10``
  in the ``.pampl`` files drifting from the parser's ``_HYDRO_UC_SOFT_PENALTY``),
* every regex test has a single home — the family table below,
* the JSON-side audit reads each constraint's ``directive.kind`` and can
  attribute soft slack costs to a named family without re-running the
  classifier.

Migration status (2026-05-30): Step 4b of the AMPL/PAMPL modernization
plan.  ``Gas_MaxOpDay`` (``daily_budget``) and the
``RegRange`` / ``ReserveProvSum`` branches already emit directives; this
module promotes the remaining regex sites (``_def_equation`` / hard
override / ``MAXCSF`` placeholder / hydro-floor catch-all) to the same
typed path.
"""

from __future__ import annotations

from dataclasses import dataclass

from .entities import ConstraintDirective


# ── Penalty tiers — the single source of truth ──────────────────────────────
# Any change to these values must be made HERE; the per-tier name used in
# the ``.pampl`` files (``soft_floor_penalty`` …) is derived from
# ``_TIER_NAMES`` so the param declaration in each ``uc_<family>.pampl`` and
# the directive's ``penalty`` field stay in lockstep.
#
# Two named tiers cover every regex branch in the legacy classifier:
#
#   * ``HYDRO_SOFT`` (10 $/MWh) — hydro operational floors / caps that
#     PLEXOS gates on commitment internally but we emit as raw rows; soft
#     enough that the LP can violate by a small amount when the commitment
#     decision conflicts with the floor.  Also the catch-all for any UC
#     without an explicit penalty whose LHS doesn't reference commitment
#     binaries or pure line flow.  Matches the historical
#     ``discharge_ANTUCOmin`` precedent.
#
#   * ``RESERVE_PROV_SUM`` (1000 $/MWh) — high-soft tier for reserve
#     aggregators (CSF / CPF / CTF ``*MinProvision`` / ``*Calculation``)
#     and PLEXOS ``RegRange`` rows, both of which PLEXOS solves hard but
#     where data inconsistencies surface in plain LP-relax.  At $1000 the
#     LP almost never elects to violate (vs $5-50 marginal dispatch cost),
#     while staying feasible when block-level data inconsistencies surface
#     — exactly the regime that broke the hard-equality version.
HYDRO_SOFT: float = 10.0
RESERVE_PROV_SUM: float = 1000.0

#: Per-tier PAMPL ``param`` name.  Consumed by
#: :func:`gtopt_writer._penalty_param_name`; any other distinct PLEXOS
#: penalty gets a value-derived name (``penalty_467_19`` …).
TIER_NAMES: dict[float, str] = {HYDRO_SOFT: "soft_floor_penalty"}


# ── Classification outcomes ────────────────────────────────────────────────
@dataclass(frozen=True)
class _Outcome:
    """Output of :func:`classify` — what the parser stamps onto the UC."""

    #: Per-unit slack cost ($/unit-of-violation).  ``0.0`` ⇒ HARD.
    penalty: float
    #: Typed family directive (None when the legacy / catch-all path
    #: fires; the C++ side stays at ``std::nullopt``).
    directive: ConstraintDirective | None


# ── LHS-shape predicates (single source of truth) ──────────────────────────
def _is_pure_line_flow(expression: str) -> bool:
    """LHS references only ``line(...).flow`` terms."""
    return (
        "line(" in expression
        and "generator(" not in expression
        and "commitment(" not in expression
        and "battery(" not in expression
        and "reserve_provision(" not in expression
        and "decision_variable(" not in expression
    )


def _references_commitment(expression: str) -> bool:
    """LHS references at least one ``commitment(...)`` term."""
    return "commitment(" in expression


def _is_pure_commitment(expression: str) -> bool:
    """LHS references ONLY ``commitment(...)`` terms.

    PLEXOS pure-commitment constraints (``Campiche_starting``,
    ``NVentanas_starting``, ``*_Comparison``, …) are operational
    on/off / startup / status pins — almost always binding in PLEXOS
    with zero slack.  Mixed-atom constraints (commitment + line.flow
    or commitment + reserve_provision) can be soft (PLEXOS pays
    slack — verified 2026-06-03 on jan18 audit for SD_*_Tocopilla,
    PandeAzucar_Polpaico_CC, NRenca_SSCC_FA, CSF_LW_MIN_BAT_DON_*).
    """
    return (
        "commitment(" in expression
        and "line(" not in expression
        and "generator(" not in expression
        and "battery(" not in expression
        and "reserve_provision(" not in expression
        and "decision_variable(" not in expression
    )


def _is_reserve_provision_sum(constraint_name: str, expression: str) -> bool:
    """LHS is a reserve-provision aggregation (CSF/CPF/CTF *MinProvision* /
    *Calculation*).

    Two-pattern match:

    (a) **Pure reserve aggregation** — ≥3 ``reserve_provision(...)`` refs
        with no commitment / generator / line / battery terms.  Optional
        ``decision_variable(...)`` requirement-scaling terms are tolerated
        (CPF5mDown_Requirement / Generation_SEN attached to *MinProvision
        rows).

    (b) **Reserve *Calculation* definitional equations** — pure
        ``decision_variable(...)`` rows whose name ends in ``Calculation`` or
        contains ``Calculation_``; PLEXOS solves these hard.
    """
    has_reserve_provision_sum = (
        expression.count("reserve_provision(") >= 3
        and "generator(" not in expression
        and "commitment(" not in expression
        and "battery(" not in expression
        and "line(" not in expression
    )
    is_reserve_calculation = (
        constraint_name.endswith("Calculation") or "Calculation_" in constraint_name
    ) and (
        "decision_variable(" in expression
        and "generator(" not in expression
        and "commitment(" not in expression
        and "battery(" not in expression
        and "line(" not in expression
        and "reserve_provision(" not in expression
    )
    return has_reserve_provision_sum or is_reserve_calculation


def _is_regrange(constraint_name: str, expression: str) -> bool:
    """PLEXOS regulation-range UC (``*_RegRange_e[12]``).

    The mixed-atom shape — commitment(.) + (generator(.) or
    reserve_provision(.)) — distinguishes RegRange from pure reserve
    sums (which have no commitment / generator refs).
    """
    return "_RegRange_" in constraint_name and (
        "commitment(" in expression
        and ("generator(" in expression or "reserve_provision(" in expression)
    )


def _is_def_equation(constraint_name: str, expression: str, op: str) -> bool:
    """PLEXOS ``_Def`` rows — definitional EQUALITY equations.

    Form: ``-1 * decision_variable("X_Requirement") + Σ provision = 0``.
    PLEXOS solves these at zero slack (DV absorbs the constraint
    perfectly); when emitted soft, the LP pins the DV at its lower bound
    and pays slack on every constraint that references X_Requirement.
    """
    return (
        constraint_name.endswith("_Def")
        and op == "="
        and "decision_variable(" in expression
        and "generator(" not in expression
        and "commitment(" not in expression
        and "battery(" not in expression
        and "line(" not in expression
    )


def _is_bat_complementarity(constraint_name: str) -> bool:
    """Battery-mode complementarity (BAT_*_CF_GEN_COMP / BAT_*_CF_LOAD_COMP).

    PLEXOS audit shows 99-111/168 h binding with shadow $3,993-$5,388 —
    softening lets the LP cheat at the catch-all tier while PLEXOS pays
    the true ~$5,000/MWh marginal cost.
    """
    return constraint_name.endswith("_CF_GEN_COMP") or constraint_name.endswith(
        "_CF_LOAD_COMP"
    )


def _is_maxcsf_placeholder(constraint_name: str) -> bool:
    """``MAXCSF_*`` — N-1 secondary-frequency-control reserve sharing.

    PLEXOS audit: slack 43,000+ MWh with shadow $0 across the horizon
    (treated as free slack — N-1 placeholders that never bind in
    practice).  Demoted to the operational tier so the LP can slack
    them freely instead of accumulating phantom $48K/plant cost at the
    high-soft tier.
    """
    return constraint_name.startswith("MAXCSF")


def _is_fuel_offtake_or_limited_generation(constraint_name: str) -> bool:
    """``Gas_MaxOpDay*`` / ``limited_generation_calculation`` name override.

    RES20260422.accdb reports zero slack on these (binding 111/111,
    price $78-83/h), so PLEXOS solves them HARD — but promoting to
    ``penalty=0`` produces a CPLEX presolve infeasibility (implied
    bounds make the row infeasible on the v3 LP-relax, 2026-05-29).
    Compromise: high-soft tier so the LP almost never elects to violate
    yet the row can absorb the rare per-block contradiction.
    """
    return (
        constraint_name.startswith("Gas_MaxOpDay")
        or constraint_name == "limited_generation_calculation"
    )


# ── The single entry point ─────────────────────────────────────────────────
def classify(
    *,
    constraint_name: str,
    expression: str,
    op: str,
    plexos_penalty: float,
    is_inactive: bool,
    hard_set: frozenset[str],
) -> _Outcome:
    """Map a PLEXOS Constraint onto ``(emitted_penalty, directive)``.

    Encodes the entire regex+penalty ladder previously inlined in
    :func:`parsers.extract_user_constraints`.  The classification is
    **purely a function of constraint name + LHS shape + op + PLEXOS
    metadata** — no I/O, no env-vars — so it can be re-run from a unit
    test on any single Constraint without re-extracting the whole bundle.

    :param constraint_name: The PLEXOS Constraint object name (BEFORE
        PAMPL-ident sanitisation).
    :param expression: The emitted AMPL expression string.
    :param op: Comparison operator (``"<="`` / ``">="`` / ``"="``).
    :param plexos_penalty: PLEXOS-supplied ``Penalty Price`` (≥0).
        Already coerced to ``0.0`` when missing.
    :param is_inactive: ``True`` when the constraint is marked
        ``active = False`` upstream (contingency / inactive row).
    :param hard_set: Names PLEXOS audit confirmed as zero-slack-with-
        binding-shadow ⇒ port HARD.  Loaded once per extraction via
        :func:`parsers._load_plexos_hard_uc_list`.

    :return: ``_Outcome`` carrying the per-unit penalty + the optional
        typed directive.

    Routing rules (first-match-wins):

    1. **Inactive** → keep PLEXOS-supplied penalty, no directive.
    2. **Active + plexos_penalty > 0** → keep PLEXOS-supplied penalty,
       no directive (PLEXOS already chose a soft tier).
    3. **Gas_MaxOpDay* / limited_generation_calculation** override (not
       in hard_set) → high-soft tier, ``reserve_prov_sum`` directive.
    4. **plexos_penalty == 0** dispatch by LHS shape + name:
       a. ``_Def`` definitional equation → HARD, no directive.
       b. Battery complementarity / PLEXOS-hard-singleton → HARD, no
          directive (singleton: matched against ``hard_set``).
       c. ``MAXCSF_*`` placeholder → operational soft tier, no
          directive.
       d. Reserve aggregation / RegRange → high-soft tier with
          ``regrange`` or ``reserve_prov_sum`` directive (the typed
          family discriminator + override penalty).
       e. Pure line flow OR references commitment → HARD (PLEXOS keeps
          these binding in its solution).
       f. Catch-all (hydro floor / generator cap / etc.) →
          operational soft tier, no directive.
    """
    # 1+2 — inactive or PLEXOS-supplied non-zero penalty stays as-is.
    if is_inactive or plexos_penalty > 0.0:
        return _Outcome(penalty=plexos_penalty, directive=None)

    # 3 — name-based fuel-cap / limited-generation override.  Applied
    # BEFORE the LHS-shape ladder so a HARD PLEXOS audit entry (the
    # ``hard_set`` membership) takes precedence.
    if (
        _is_fuel_offtake_or_limited_generation(constraint_name)
        and constraint_name not in hard_set
    ):
        return _Outcome(
            penalty=RESERVE_PROV_SUM,
            directive=ConstraintDirective(
                kind="reserve_prov_sum", penalty=RESERVE_PROV_SUM
            ),
        )

    # 4a/b — HARD branches.
    if _is_def_equation(constraint_name, expression, op):
        return _Outcome(penalty=0.0, directive=None)
    if _is_bat_complementarity(constraint_name) or constraint_name in hard_set:
        return _Outcome(penalty=0.0, directive=None)

    # 4c — MAXCSF placeholder → operational soft.
    if _is_maxcsf_placeholder(constraint_name):
        return _Outcome(penalty=HYDRO_SOFT, directive=None)

    # 4d — reserve aggregation / RegRange → high-soft + typed directive.
    is_reserve_sum = _is_reserve_provision_sum(constraint_name, expression)
    is_regrange = _is_regrange(constraint_name, expression)
    if is_reserve_sum or is_regrange:
        kind = "regrange" if is_regrange else "reserve_prov_sum"
        return _Outcome(
            penalty=RESERVE_PROV_SUM,
            directive=ConstraintDirective(kind=kind, penalty=RESERVE_PROV_SUM),
        )

    # 4e — pure-shape HARD branches.  PLEXOS keeps these binding with
    # zero slack (verified on CEN PCP audits):
    #   * pure-line-flow contingency / shadow-line rows;
    #   * pure-commitment startup / status / *_Comparison pins
    #     (``Campiche_starting``, ``NVentanas_starting``, …).
    # MIXED-atom constraints (commitment + continuous variables) are
    # NOT uniformly hard — the jan18 audit found 4 commitment-mixed
    # rows that PLEXOS solves soft (SD_*_Tocopilla = gen+commit,
    # PandeAzucar_Polpaico_CC = line+commit, NRenca_SSCC_FA / CSF_LW_*
    # = reserve_provision+commit).  Audit-confirmed hard mixed rows
    # stay HARD via 4b's ``hard_set`` lookup; the rest fall through to
    # 4f's operational soft tier.
    if _is_pure_line_flow(expression) or _is_pure_commitment(expression):
        return _Outcome(penalty=0.0, directive=None)

    # 4f — catch-all hydro / generator floor / mixed-atom rows without
    # an audit-confirmed HARD listing.
    return _Outcome(penalty=HYDRO_SOFT, directive=None)


__all__ = [
    "HYDRO_SOFT",
    "RESERVE_PROV_SUM",
    "TIER_NAMES",
    "classify",
]
