#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""PCP UC full-audit builder.

Compares the user-constraints emitted by plexos2gtopt against the
solved PLEXOS Constraint rows in a RES*.accdb solution database.
Surfaces real divergences (RHS scale mismatches, missing UCs that
PLEXOS solves hard, soft/hard classification drift) and silences
structural noise (UCs realised as native gtopt primitives,
constraints PLEXOS itself never binds).

Inputs
------
* ``--plexos-cache DIR``: cached PLEXOS sol tables produced by
  :mod:`cen2gtopt.pcp_solution.cache_plexos_tables` (the bundle's
  ``accdb_cache_dir`` populated by the converter when
  ``--plexos-solution-accdb`` is honoured).  Reads ``t_object.csv``,
  ``t_membership.csv``, ``t_key.csv`` and ``t_data_0.csv``.
* ``--gtopt-dir DIR``: a converter output directory containing the
  ``uc_*.pampl`` PAMPL files and the converted planning JSON.
* ``--hard-list PATH`` (optional): the PLEXOS-HARD audit list, defaults
  to :file:`data/cen_pcp_hard_ucs.txt`.
* ``--output PATH`` (optional): write the audit JSON to this path
  (default: stdout summary only).

Output
------
A summary table on stdout, plus an optional JSON dump containing:

* identity counts (intersection / missing / synthetic-in-gtopt)
* per-row diff for the intersection
* named mismatch buckets (B2 RHS, B3 missing UC, B5/B6 soft/hard, etc.)
* the duplicate-name index

Exit code
---------
* ``0``: no significant bugs detected (B2/B5 empty, no B17 HIGH flag)
* ``1``: significant bugs detected (RHS scale mismatch, hard-list drift, or a
  B17 hard-in-gtopt-but-soft-in-PLEXOS reserve mismatch)
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from gtopt_shared.pampl_ident import pampl_ident as _pampl_ident

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# PLEXOS sol .accdb property ids (constraint solution columns)
# ---------------------------------------------------------------------------
PROP_ACTIVITY = 3069
PROP_SLACK = 3070
PROP_HRSBIND = 3072
PROP_RHS = 3073
PROP_PRICE = 3074
# INPUT penalty properties (modelled soft data), distinct from the solution
# columns above.  PLEXOS ``Penalty Quantity`` / ``Penalty Price`` on a
# Constraint mark it as genuinely soft.  These are NOT present in the
# constraint-SOLUTION cache (only solution pids 3069-3074 are), so their
# absence is the signal that the B6 "soft in PLEXOS" check has no modelled
# evidence to fire on (see the B6 refinement in :func:`run_audit`).
PROP_PENALTY_QUANTITY = 4392
PROP_PENALTY_PRICE = 4393
_INPUT_PENALTY_PIDS = frozenset({PROP_PENALTY_QUANTITY, PROP_PENALTY_PRICE})
_WANTED_PIDS = frozenset(
    {
        PROP_ACTIVITY,
        PROP_SLACK,
        PROP_HRSBIND,
        PROP_RHS,
        PROP_PRICE,
        PROP_PENALTY_QUANTITY,
        PROP_PENALTY_PRICE,
    }
)
CONSTRAINT_CLASS_ID = 70
SYS_CONSTRAINT_COLLECTION_ID = 700


# ---------------------------------------------------------------------------
# Name sanitization lives in :mod:`gtopt_shared` (imported above) so
# ``gtopt_writer`` and this audit share byte-exact behaviour.  The
# private ``_pampl_ident`` alias keeps the call sites untouched.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# UC family / hydro classifiers
# ---------------------------------------------------------------------------
UC_FAMILY_PATTERNS = (
    (re.compile(r"^BatMaxCycDay_"), "battery_cycle"),
    (re.compile(r"^CPF_BESS_"), "battery_cycle"),
    (re.compile(r"^Gas_MaxOpDay"), "gas_maxopday"),
    (re.compile(r"^FueMaxOff"), "fuel_offtake_week"),
    (re.compile(r"^GenMaxStarts(Week|Day|Month)?_"), "gen_max_starts"),
    (re.compile(r"_starting$"), "commit_startup"),
    (re.compile(r"^MutuallyExclusive_", re.IGNORECASE), "mutually_exclusive"),
    (re.compile(r"^Inertia_"), "inertia_calc"),
    (re.compile(r"_PMax$"), "pmax_cap"),
    (re.compile(r"_PMin$"), "pmin_floor"),
    (re.compile(r"^discharge_"), "discharge_min"),
    (re.compile(r"reserve$"), "reservoir_energy"),
    (re.compile(r"eco$"), "reservoir_economy"),
    (re.compile(r"max$"), "hydro_max"),
    (re.compile(r"min$"), "hydro_min"),
    (
        re.compile(
            r"(maxramp|rampdown|rampup|ramp|lagrampup|lagrampdown|rampdownact)$"
        ),
        "hydro_ramp",
    ),
    (re.compile(r"^limited_generation"), "comparison"),
    (re.compile(r"_Uniq$"), "config_uniq"),
    (re.compile(r"^FCF"), "fcf"),
    (re.compile(r"^Reserv(e|a)"), "reserve_reg"),
    (re.compile(r"AGC", re.IGNORECASE), "agc_reserve"),
    (re.compile(r"^[0-9]+_"), "transmission_security"),
    (re.compile(r"^SD_"), "transmission_security"),
    (re.compile(r"^CTF"), "config_transfer"),
    (re.compile(r"^CSF_|^CPF_|^CRC"), "reserve_provision"),
)


def categorise(name: str) -> str:
    """Map a UC name to its family label (one of UC_FAMILY_PATTERNS)."""
    for r, label in UC_FAMILY_PATTERNS:
        if r.search(name):
            return label
    return "other"


# Families that PLEXOS expresses as Constraint objects but gtopt promotes to
# a NATIVE LP primitive on an existing entity (no UserConstraint emitted).
# These are intentional architectural choices, not data loss — the
# constraint is enforced by entity LP code, not via the UC mechanism.
#
# Mapping:
#   battery_cycle      → Battery.max_cycles_day (BatteryLP::add_to_lp emits
#                        a per-(battery, day) `cycle_limit` row)
#   gas_maxopday       → Fuel.max_offtake schedule (FuelLP enforces via
#                        a per-(fuel, day) `max_offtake` row)
#   fuel_offtake_week  → Fuel.max_offtake schedule (weekly variant)
#   gen_max_starts     → Commitment.max_starts + starts_scope (CommitmentLP
#                        emits the Σ startup ≤ max_starts row natively)
NATIVE_PRIMITIVE_FAMILIES: dict[str, str] = {
    "battery_cycle": "Battery.max_cycles_day (BatteryLP cycle_limit row)",
    "gas_maxopday": "Fuel.max_offtake schedule (per-day)",
    "fuel_offtake_week": "Fuel.max_offtake schedule (per-week)",
    "gen_max_starts": "Commitment.max_starts + starts_scope",
    # ``reserve_provision`` family (CSF_/CPF_/CRC_): the PLEXOS Constraint
    # objects have RHS = 0 because PLEXOS encodes the reserve requirement
    # via its native ``Reserve.Min Provision`` property (not on the
    # Constraint).  gtopt promotes the requirement to a UC RHS (via the
    # ``reserve_prov_sum`` directive), so the same-name comparison shows
    # a B2 RHS mismatch (gtopt = schedule, PLEXOS = 0).  This is an
    # encoding difference, not a data bug — the LP-side primitive
    # (ReserveZone / ReserveProvision) carries the requirement.
    "reserve_provision": "ReserveZone.requirement + ReserveProvision sum",
    # Hydro per-plant min / max / ramp UCs (ANTUCOmin/max, ANGOSTURAeco,
    # PANGUEramp, ...): gtopt deliberately keeps these in the SOFT tier
    # ($10/MWh slack) because PLEXOS gates them internally on commit
    # status (the unit's row is auto-relaxed when commit=0).  gtopt has
    # no commit-gated UC primitive yet, so hardening them would cause
    # primal infeasibility whenever PLEXOS would have OFF'd the unit.
    # The RHS mismatch in B2 (gtopt pmax=137 vs PLEXOS-sol max=85.3 etc.)
    # is the natural consequence of this soft-tier choice — NOT a parser
    # extraction bug.  See ``parsers.py`` lines 5500-5560 for the full
    # design comment ("hydro per-plant min/max/ramp — special handling").
    "hydro_min": "soft tier ($10/MWh slack); commit-gating handled by Commitment",
    "hydro_max": "soft tier ($10/MWh slack); commit-gating handled by Commitment",
    "hydro_ramp": "soft tier ($10/MWh slack); commit-gating handled by Commitment",
    "reservoir_economy": "soft tier ($10/MWh slack); hydro economy UC",
    # ``inertia_calc`` family (Inertia_Calculation_e1, _e2, ...): gtopt
    # encodes the system inertia balance as a UC with a synthetic
    # ``decision_variable("Inertia_SEN")`` term:
    #   −1000·Inertia_SEN + Σ (inertia_const·commit.status) ≥ rhs_raw
    # The raw RHS from PLEXOS XML (here −622.54) reads correctly; PLEXOS
    # at solve time reports ``rhs_max = 0`` because its internal
    # encoding cancels the Inertia_SEN contribution at the binding point.
    # gtopt's raw-DB value is the right input — the discrepancy is the
    # different LP formulation, not a parser bug.
    "inertia_calc": "decision_variable Inertia_SEN + Σ inertia·commit.status",
}


def is_hydro_minmax(name: str) -> bool:
    """True when ``name`` is a per-plant hydro min/max/ramp UC.

    These are excluded from the B5 hard-soft mismatch bucket by design
    — gtopt keeps them soft because it lacks a commit-status primitive
    to gate the floor on unit-online, the PLEXOS semantics.

    Delegates to :func:`plexos2gtopt.parsers._is_hydro_min_max_uc` so the
    audit's filter stays in lock-step with the converter's actual
    hard-list exclusion logic (matches the same regex + plant stems,
    including case-sensitive suffixes like ``_PMax`` and
    ``caudal_min_diario`` that a naive case-folding check misses).
    """
    from .parsers import _is_hydro_min_max_uc

    return _is_hydro_min_max_uc(name)


# ---------------------------------------------------------------------------
# PAMPL parsing
# ---------------------------------------------------------------------------
_UC_DEF_RE = re.compile(
    r"(?P<flag>(?:^|\n)\s*(?:inactive\s+)?)constraint\s+"
    r"(?P<name>\w+)"
    r"(?:\s+penalty\s+(?P<penalty>\w+))?"
    r"(?:\s+rhs\s+\[(?P<rhs>[^\]]*)\])?"
    r"\s*:\s*"
    r"(?P<body>[^;]*);"
)
_OP_RE = re.compile(r"(<=|>=|=)\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$")
_PARAM_RE = re.compile(r"\bparam\s+(\w+)\s*=\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*;")
_TERM_RE = re.compile(
    r"(?P<sign>[+\-]?)\s*(?P<coeff>\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)"
    r"\s*\*\s*"
    r"(?P<func>\w+)\(\"(?P<elem>[^\"]+)\"\)"
    r"(?:\.(?P<attr>\w+))?"
)


def _parse_lhs_terms(lhs_str: str) -> list[dict]:
    terms: list[dict] = []
    last_end = 0
    for tm in _TERM_RE.finditer(lhs_str):
        sign_token = tm.group("sign") or "+"
        between = lhs_str[last_end : tm.start()]
        extra_sign = 1
        for ch in between:
            if ch == "-":
                extra_sign *= -1
        coeff = float(tm.group("coeff"))
        sign = 1 if sign_token != "-" else -1
        terms.append(
            {
                "coeff": sign * extra_sign * coeff,
                "func": tm.group("func"),
                "elem": tm.group("elem"),
                "attr": tm.group("attr"),
            }
        )
        last_end = tm.end()
    return terms


def parse_pampl_file(path: Path) -> list[dict]:
    """Parse one ``uc_*.pampl`` file → list of UC dicts.

    Each dict carries ``{name, file, active, op, rhs_scalar, rhs_profile,
    penalty_ident, penalty_value, n_terms, terms, lhs_raw}``.
    """
    text = path.read_text()
    # Strip per-line comments
    text_clean = "".join(
        ln for ln in text.splitlines(keepends=True) if not ln.lstrip().startswith("#")
    )
    pen_params = {m.group(1): float(m.group(2)) for m in _PARAM_RE.finditer(text_clean)}
    results: list[dict] = []
    for m in _UC_DEF_RE.finditer(text_clean):
        flag = (m.group("flag") or "").strip()
        is_inactive = "inactive" in flag
        name = m.group("name")
        penalty_ident = m.group("penalty")
        rhs_vec_raw = m.group("rhs")
        body = m.group("body").strip()
        m_op = _OP_RE.search(body)
        if not m_op:
            op = "?"
            rhs_scalar = None
            lhs_str = body
        else:
            op = m_op.group(1)
            rhs_scalar = float(m_op.group(2))
            lhs_str = body[: m_op.start()].strip()
        terms = _parse_lhs_terms(lhs_str)
        penalty_val = pen_params.get(penalty_ident, None) if penalty_ident else 0.0
        rhs_profile = None
        if rhs_vec_raw is not None:
            rhs_profile = [
                float(x.strip()) for x in rhs_vec_raw.split(",") if x.strip()
            ]
        results.append(
            {
                "name": name,
                "file": path.name,
                "active": not is_inactive,
                "op": op,
                "rhs_scalar": rhs_scalar,
                "rhs_profile": rhs_profile,
                "penalty_ident": penalty_ident,
                "penalty_value": penalty_val,
                "n_terms": len(terms),
                "terms": terms,
                "lhs_raw": lhs_str,
            }
        )
    return results


def parse_pampl_dir(pampl_dir: Path) -> list[dict]:
    """All ``uc_*.pampl`` files in ``pampl_dir``, concatenated."""
    rows: list[dict] = []
    for p in sorted(pampl_dir.glob("uc_*.pampl")):
        rows.extend(parse_pampl_file(p))
    return rows


def parse_json_ucs(planning_json: Path) -> list[dict]:
    """Extract inline UCs from the planning JSON ``user_constraint_array``."""
    data = json.loads(planning_json.read_text())
    rows: list[dict] = []
    uc_arr = data.get("system", {}).get("user_constraint_array", [])
    for uc in uc_arr:
        expr = uc.get("expression", "") or ""
        m_op = _OP_RE.search(expr)
        if not m_op:
            op = "?"
            rhs_scalar = None
            lhs_str = expr
        else:
            op = m_op.group(1)
            rhs_scalar = float(m_op.group(2))
            lhs_str = expr[: m_op.start()].strip()
        terms = _parse_lhs_terms(lhs_str)
        rows.append(
            {
                "name": uc.get("name"),
                "file": f"{planning_json.name}:user_constraint_array",
                "active": uc.get("active", True),
                "op": op,
                "rhs_scalar": rhs_scalar,
                "rhs_profile": uc.get("rhs_profile") or uc.get("rhs"),
                "penalty_ident": None,
                "penalty_value": uc.get("penalty"),
                "n_terms": len(terms),
                "terms": terms,
                "lhs_raw": lhs_str,
                "daily_sum": uc.get("daily_sum", False),
                "constraint_type": uc.get("constraint_type"),
            }
        )
    return rows


# ---------------------------------------------------------------------------
# Native-constraint LP row parsing (B11 source)
# ---------------------------------------------------------------------------
# B2 (the UC-name RHS comparison) is BLIND to anything gtopt encodes as a
# NATIVE LP primitive rather than a UserConstraint: the
# ``NATIVE_PRIMITIVE_FAMILIES`` are explicitly skipped, so a reserve_zone fold
# or a Commitment per-day-floor flatten is never RHS-compared to PLEXOS.  The
# handle is that gtopt emits one NAMED LP row per (native element, scenario,
# stage, block).  Scanning those rows recovers the RHS gtopt actually enforces.
#
# Row-name grammar (standalone / CPLEX LP writer):
#   reservezone_urequirement_<uid>_<sc>_<st>_<blk>:  Σ provision  >= <rhs>
#   reservezone_drequirement_<uid>_<sc>_<st>_<blk>:  Σ provision  >= <rhs>
#   <genname>_pmax_constraint_<id>_<sc>_<st>_<blk>:  Σ generation <= <rhs>
#   <genname>_pmin_constraint_<id>_<sc>_<st>_<blk>:  Σ generation >= <rhs>
# Element identity is recovered from the JSON for reserve zones
# (``uid`` → ``name`` via ``reserve_zone_array``); for commitments the
# lower-cased ``<genname>`` prefix IS the identity (the trailing ``<id>`` is a
# per-row counter, NOT a stable uid, so it is discarded).
#
# REFINEMENT (vs reading the enforced ``.lp`` requirement row): the
# reserve-zone requirement VALUES are read from the JSON ``reserve_zone_array``
# (``urreq`` / ``drreq``), NOT from the enforced LP row.  The enforced LP row
# folds in the ``ReserveZone.urmin`` / ``drmin`` Min-Provision FLOOR, so gtopt
# correctly enforces ``max(requirement, MinProvision)`` while PLEXOS REPORTS
# the bare requirement and enforces the floor separately.  Comparing the
# enforced row false-positives (e.g. CTF_LW gtopt 183 floor vs PLEXOS 94
# requirement).  The ``.lp`` is still used to ENUMERATE which reserve-zone
# elements exist; the COMPARED values come from the JSON requirement.
_LP_RESERVEZONE_RE = re.compile(
    r"^\s*reservezone_(?P<dir>u|d)requirement_(?P<uid>\d+)_\d+_\d+_\d+\s*:"
)
_LP_COMMIT_RE = re.compile(
    r"^\s*(?P<gen>[A-Za-z0-9_]+?)_(?P<bound>pmin|pmax)_constraint_\d+_\d+_\d+_\d+\s*:"
)
# Operator + numeric RHS terminating an LP row.  CPLEX wraps long rows across
# many continuation lines; the operator sits on the row's FINAL line.
_LP_OP_RE = re.compile(r"(<=|>=|=)\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$")

# Keys are (element_kind, identity, direction):
#   element_kind: "reserve_zone" | "commitment"
#   identity:     reserve-zone name (from JSON uid) | commitment genname prefix
#   direction:    "up"/"down" (reserve_zone) | "pmin"/"pmax" (commitment)
NativeKey = tuple[str, str, str]


_BESS_RESERVE_ZONE_RE = re.compile(r"^(CPF|CSF|CTF)_(LW|RS)_BESS$")


def is_provision_only_bess_reserve_zone(name: str) -> bool:
    """True when ``name`` is a PLEXOS ``*_BESS`` reserve sub-tracker.

    These zones (``CTF_LW_BESS``, ``CSF_RS_BESS``, …) are PROVISION-ONLY
    sub-trackers, NOT independent reserve requirements: PLEXOS ships them with
    ``Min Provision = 0``, ``Min Requirement = 0`` and no ``Res_Requirement.csv``
    row, so ``extract_reserves`` faithfully emits an empty requirement
    (``drreq=[0.0]``).  The requirement is carried ONCE on the non-BESS twin
    (e.g. ``CTF_LW``), and the BESS term contributes 0 to the shared
    ``*MinProvision`` aggregation.  B11 must DEFER these (their zero requirement
    is correct) rather than flag a "fold" against the full constraint RHS.

    PLEXOS-doc basis (Energy Exemplar ``Reserve.MinProvision``): Min Provision is
    the minimum reserve SUPPLIED by designated generators/purchasers — a
    supply-side floor, distinct from the Risk/requirement.  A zone with
    provision-eligible batteries but zero Min Provision and zero requirement is a
    provision-only aggregator feeding the shared parent requirement.
    """
    return _BESS_RESERVE_ZONE_RE.match(name) is not None


def primary_reserve_zone_for_bess(name: str) -> str | None:
    """Return the non-BESS twin for a ``*_BESS`` sub-tracker (``CTF_LW_BESS`` →
    ``CTF_LW``); ``None`` when ``name`` is not a ``*_BESS`` reserve zone."""
    m = _BESS_RESERVE_ZONE_RE.match(name)
    if m is None:
        return None
    return f"{m.group(1)}_{m.group(2)}"


def _reserve_zone_uid_to_name(planning_json: Path) -> dict[int, str]:
    """Map ``reserve_zone_array`` uid → name from the planning JSON."""
    if not planning_json.is_file():
        return {}
    data = json.loads(planning_json.read_text())
    out: dict[int, str] = {}
    for z in data.get("system", {}).get("reserve_zone_array", []):
        try:
            out[int(z["uid"])] = str(z["name"])
        except (KeyError, ValueError, TypeError):
            continue
    return out


def _flatten_block_schedule(raw: Any) -> list[float]:
    """Flatten a (possibly nested) per-block schedule into a flat float list.

    The converter emits requirement schedules as ``urreq``/``drreq`` shaped
    either as a flat ``[v, v, ...]`` list or a nested ``[[...], [...]]``
    (per-stage rows of per-block values).  Recover a single flat per-block
    series, dropping non-numeric entries.
    """
    out: list[float] = []

    def _walk(node: Any) -> None:
        if isinstance(node, (list, tuple)):
            for sub in node:
                _walk(sub)
            return
        try:
            out.append(float(node))
        except (TypeError, ValueError):
            return

    _walk(raw)
    return out


def reserve_zone_requirement_series(
    planning_json: Path,
) -> dict[tuple[str, str], list[float]]:
    """Per-block reserve-zone REQUIREMENT series from the planning JSON.

    Returns ``{(zone_name, direction): [rhs, rhs, ...]}`` where ``direction``
    is ``"up"`` (from ``urreq``) or ``"down"`` (from ``drreq``).  This is the
    bare requirement the converter emits — the value PLEXOS REPORTS — NOT the
    enforced ``max(requirement, MinProvision)`` floor on the LP row.  Used by
    the B11 reserve-zone comparison so the Min-Provision floor never
    false-positives a fold.
    """
    if not planning_json.is_file():
        return {}
    data = json.loads(planning_json.read_text())
    out: dict[tuple[str, str], list[float]] = {}
    for z in data.get("system", {}).get("reserve_zone_array", []):
        name = z.get("name")
        if name is None:
            continue
        for direction, key in (("up", "urreq"), ("down", "drreq")):
            if key in z and z[key] is not None:
                series = _flatten_block_schedule(z[key])
                if series:
                    out[(str(name), direction)] = series
    return out


def parse_lp_native_constraints(
    lp_path: Path, planning_json: Path
) -> dict[NativeKey, list[float]]:
    """Scan a gtopt ``.lp`` → per-block RHS series for native constraints.

    Returns ``{(element_kind, identity, direction): [rhs, rhs, ...]}`` — the
    per-(sc, st, blk) RHS gtopt enforces for every reserve-zone requirement
    row and every commitment pmin/pmax bound row.  Collapsing into distinct
    levels is left to the B11 comparator so it reuses :func:`_distinct_values`.
    """
    zone_names = _reserve_zone_uid_to_name(planning_json)
    series: dict[NativeKey, list[float]] = defaultdict(list)
    cur: NativeKey | None = None  # row being accumulated across continuations
    with lp_path.open() as f:
        for line in f:
            mz = _LP_RESERVEZONE_RE.match(line)
            if mz:
                uid = int(mz.group("uid"))
                direction = "up" if mz.group("dir") == "u" else "down"
                ident = zone_names.get(uid, f"uid{uid}")
                cur = ("reserve_zone", ident, direction)
            else:
                mc = _LP_COMMIT_RE.match(line)
                if mc:
                    cur = ("commitment", mc.group("gen"), mc.group("bound"))
            if cur is not None:
                mo = _LP_OP_RE.search(line)
                if mo:
                    series[cur].append(float(mo.group(2)))
                    cur = None
    return dict(series)


def discover_gtopt_lp(gtopt_dir: Path) -> Path | None:
    """Return the first ``*.lp`` in ``gtopt_dir`` (None when none exists)."""
    for p in sorted(gtopt_dir.glob("*.lp")):
        return p
    return None


# ---------------------------------------------------------------------------
# B12: parameter-bounds consistency (gtopt JSON vs PLEXOS input CSV)
# ---------------------------------------------------------------------------
# This bucket is INDEPENDENT of the UC-name audit above: it compares the
# converted gtopt element bounds against the RAW PLEXOS input time-series
# (NOT the sol cache).  It catches converter extraction bugs where gtopt's
# base rating disagrees with the per-element CSV.
#
# NOTE on EL=0 soft-cap lines: the converter INTENTIONALLY inflates the cap
# on ``Enforce Limits = 0`` lines (free flow up to 3× the rating, penalised
# up to a 6× hard cap) to reproduce the over-rating PLEXOS itself runs on
# such lines (it never enforces their limit).  That inflation is by design,
# so this raw bounds diff will report it as a large delta — it is NOT a
# converter bug.  The cable case (an EL=0 line whose PLEXOS solution never
# exceeds the rating, so the free band is unwarranted) is classified
# separately downstream; this bucket only proves the BASE rating was read.
#
# Comparison rules (DIRECTIONAL — PLEXOS Max Rating is the forward/export
# limit, Min Rating the reverse/import limit stored negative):
#   * generators: gtopt ``pmax`` vs max(Gen_Rating profile);
#                 gtopt ``pmin`` vs max(Gen_MinStableLevel profile).
#   * lines fwd:  gtopt ``tmax_ab`` / ``tmax_normal_ab`` vs
#                 max(Lin_MaxRating profile).
#   * lines rev:  gtopt ``tmax_ba`` / ``tmax_normal_ba`` vs
#                 |min(Lin_MinRating profile)| (falls back to the forward
#                 rating when no Min Rating ships = symmetric line).
# A mismatch fires when |gtopt - plexos| exceeds BOTH a relative (1%) AND an
# absolute (0.5 MW) tolerance — float / rounding noise is tolerated.

# B12 bounds tolerances: a value is "the same" only when it is within BOTH
# the relative AND the absolute band (so a tiny 0.5 MW line is not flagged on
# 1% rounding, and a large line is not flagged on a 0.4 MW float wobble).
_BOUNDS_REL_TOL = 0.01
_BOUNDS_ABS_TOL = 0.5

_BOUNDS_INPUT_FILES = {
    "gen_rating": "Gen_Rating.csv",
    "gen_min_stable": "Gen_MinStableLevel.csv",
    # Dispatch-state CSVs the converter layers on top of Gen_Rating to
    # derive the EFFECTIVE pmin/pmax (see parsers.py ~1755-1790 and the
    # gtopt_writer build_generator_array rule).  B12 must mirror these or
    # it false-flags every units-out / fixed-load generator.  (Gen_Commit
    # is intentionally NOT loaded: its codes never zero pmax — see
    # _expected_gen_bounds.)
    "gen_fixed_load": "Gen_FixedLoad.csv",
    "gen_units_out": "Gen_UnitsOut.csv",
    "lin_max_rating": "Lin_MaxRating.csv",
    "lin_min_rating": "Lin_MinRating.csv",
}


def _bounds_differ(gtopt: float, plexos: float) -> bool:
    """True when ``gtopt`` and ``plexos`` differ beyond the bounds tolerance."""
    d = abs(gtopt - plexos)
    return d > _BOUNDS_ABS_TOL and d > _BOUNDS_REL_TOL * max(
        abs(gtopt), abs(plexos), 1.0
    )


def _load_plexos_input_series(
    input_dir: Path,
) -> dict[str, dict[str, list[float]]]:
    """Load the PLEXOS input bound CSVs from ``input_dir``.

    Returns ``{key: {name: [v_per_period, ...]}}`` for each of the four
    bound files (``gen_rating``, ``gen_min_stable``, ``lin_max_rating``,
    ``lin_min_rating``).  A missing file maps to an empty dict so the
    comparator degrades gracefully (that side is simply not compared).
    """
    from .plexos_csv import read_long

    out: dict[str, dict[str, list[float]]] = {}
    for key, fname in _BOUNDS_INPUT_FILES.items():
        path = input_dir / fname
        if path.is_file():
            out[key] = read_long(path, n_days=1, fill_forward=True)
        else:
            logger.warning("B12: PLEXOS input file not found: %s", path)
            out[key] = {}
    return out


def _parse_gtopt_bounds(planning_json: Path) -> tuple[list[dict], list[dict]]:
    """Extract ``(generators, lines)`` bound records from the planning JSON.

    Each generator record is ``{name, pmin, pmax}`` (``None`` when unset);
    each line record is ``{name, tmax_ab, tmax_ba, tmax_normal_ab,
    tmax_normal_ba, tmin_ab, tmin_ba}``.
    """
    if not planning_json.is_file():
        return [], []
    data = json.loads(planning_json.read_text())
    system = data.get("system", {})
    gens: list[dict] = []
    for g in system.get("generator_array", []):
        name = g.get("name")
        if name is None:
            continue
        gens.append({"name": str(name), "pmin": g.get("pmin"), "pmax": g.get("pmax")})
    lines: list[dict] = []
    for ln in system.get("line_array", []):
        name = ln.get("name")
        if name is None:
            continue
        lines.append(
            {
                "name": str(name),
                "tmax_ab": ln.get("tmax_ab"),
                "tmax_ba": ln.get("tmax_ba"),
                "tmax_normal_ab": ln.get("tmax_normal_ab"),
                "tmax_normal_ba": ln.get("tmax_normal_ba"),
                "tmin_ab": ln.get("tmin_ab"),
                "tmin_ba": ln.get("tmin_ba"),
            }
        )
    return gens, lines


def _field_peak(val: Any) -> float | None:
    """Peak numeric value of a (possibly nested ``[[...]]``) JSON field.

    Reserve-provision ``urmax``/``drmax`` are emitted as per-block matrices when
    a CFdata profile drives them; the peak recovers the max reserve capability
    to compare against the PLEXOS CFdata cap.  Scalars pass through; ``None`` /
    non-numeric give ``None``.
    """
    if val is None:
        return None
    if isinstance(val, (int, float)):
        return float(val)
    flat: list[float] = []

    def _walk(node: Any) -> None:
        if isinstance(node, list):
            for e in node:
                _walk(e)
        elif isinstance(node, (int, float)):
            flat.append(float(node))

    _walk(val)
    return max(flat) if flat else None


def _parse_gtopt_provisions(planning_json: Path) -> list[dict]:
    """Extract reserve-provision cap records from the planning JSON.

    Each record is ``{name, generator, urmax, drmax}`` where ``urmax``/``drmax``
    are the emitted (scalar or per-block) reserve caps.  Empty when the JSON has
    no ``reserve_provision_array``.
    """
    if not planning_json.is_file():
        return []
    data = json.loads(planning_json.read_text())
    provs: list[dict] = []
    for p in data.get("system", {}).get("reserve_provision_array", []):
        gen = p.get("generator")
        if gen is None:
            continue
        provs.append(
            {
                "name": str(p.get("name", gen)),
                "generator": str(gen),
                "urmax": p.get("urmax"),
                "drmax": p.get("drmax"),
            }
        )
    return provs


def _bounds_item(name: str, field_name: str, gval: Any, pval: float) -> dict | None:
    """Emit a B12 mismatch dict when ``gval`` (gtopt) differs from ``pval``.

    ``gval`` may be ``None`` (field unset in the JSON) — in that case there
    is nothing to compare and ``None`` is returned.
    """
    if gval is None:
        return None
    try:
        g = float(gval)
    except (TypeError, ValueError):
        return None
    if not _bounds_differ(g, pval):
        return None
    return {
        "name": name,
        "field": field_name,
        "gtopt": round(g, 4),
        "plexos": round(pval, 4),
        "delta": round(g - pval, 4),
    }


def _expected_gen_bounds(
    name: str,
    series: dict[str, dict[str, list[float]]],
) -> tuple[float | None, float | None, str | None]:
    """Compute the converter's EXPECTED ``(pmin, pmax)`` for a generator.

    Mirrors the converter's effective-bound rule (parsers.py ~1755-1790,
    gtopt_writer.build_generator_array) so B12 compares gtopt against what
    the converter is SUPPOSED to emit, not the raw nameplate:

      * Fully OUT  → expected ``pmin = pmax = 0``.  A unit is "out" when
        ``max(Gen_UnitsOut) >= 1`` — for the single-unit CEN daily case
        (almost all gens) 1 unit out means the converter zeroes pmax for
        the whole horizon.  NOTE: ``Gen_Commit`` is deliberately NOT a
        forced-off signal: per PLEXOS semantics (parsers.py ~1783) the
        Commit code ``-1`` means "Endogenous / no commitment" (55% of CEN
        gens — the LP dispatches them freely), NOT forced-off; ``0`` =
        "don't commit" leaves pmax untouched; ``+1`` = must-run.  None of
        them zero pmax, so Commit is irrelevant to the effective bound.
      * Fixed Load set → expected ``pmin = pmax = max(|Gen_FixedLoad|)``.
        PLEXOS Fixed Load is a hard required-generation equality that
        OVERRIDES MinStableLevel for non-renewable units.
      * Otherwise   → expected ``pmax = max(Gen_Rating)``,
        ``pmin = max(Gen_MinStableLevel)`` (the legacy comparison).

    Returns ``(expected_pmin, expected_pmax, explanation)`` where each
    bound is ``None`` when the driving CSV is missing (so the caller skips
    that field rather than comparing against a phantom value) and
    ``explanation`` is a short tag (``"units_out"`` / ``"fixed_load"`` /
    ``None``) used only for the explained-diff summary log.
    """
    rating = series.get("gen_rating", {}).get(name)
    msl = series.get("gen_min_stable", {}).get(name)
    fixed = series.get("gen_fixed_load", {}).get(name)
    units_out = series.get("gen_units_out", {}).get(name)

    # --- Fully out: all units out for the horizon ⇒ pmax derated to 0. ---
    if units_out is not None and max(units_out) >= 1.0:
        return 0.0, 0.0, "units_out"

    # --- Fixed Load overrides MinStableLevel (hard pmin=pmax equality). ---
    if fixed is not None and max(abs(v) for v in fixed) > 0.0:
        fl = max(abs(v) for v in fixed)
        return fl, fl, "fixed_load"

    # --- Default: nameplate rating / min stable level. ---
    exp_pmax = max(rating) if rating else None
    exp_pmin = max(msl) if msl else None
    return exp_pmin, exp_pmax, None


def build_b12_bounds(planning_json: Path, input_dir: Path) -> list[dict]:
    """Compare gtopt element bounds vs the PLEXOS input CSVs (B12 bucket).

    Returns the list of mismatch items, one per (element, field) that
    diverges beyond tolerance.  Generators compare ``pmin``/``pmax``
    against the converter's EFFECTIVE expected bounds (see
    :func:`_expected_gen_bounds`) — accounting for forced-off
    (Gen_UnitsOut / Gen_Commit) and Fixed Load overrides — NOT the raw
    nameplate, so a correctly-zeroed forced-off unit or a fixed-load unit
    is not false-flagged.  When the rule-driving CSVs are absent the
    expected bound falls back to the legacy ``max(Gen_Rating)`` /
    ``max(Gen_MinStableLevel)`` comparison.  Lines compare every
    ``tmax_*`` field against the max of ``Lin_MaxRating`` and the reverse
    legs against ``|min(Lin_MinRating)|``.  An element with no matching
    CSV row is skipped (nothing to compare against).
    """
    series = _load_plexos_input_series(input_dir)
    lin_max = series.get("lin_max_rating", {})
    lin_min = series.get("lin_min_rating", {})

    raw_rating = series.get("gen_rating", {})
    gens, lines = _parse_gtopt_bounds(planning_json)
    items: list[dict] = []
    explained = 0  # gen diffs ABSORBED by units-out / fixed-load rules.

    for g in gens:
        name = g["name"]
        exp_pmin, exp_pmax, why = _expected_gen_bounds(name, series)
        if exp_pmax is not None:
            it = _bounds_item(name, "pmax", g["pmax"], exp_pmax)
            if it is not None:
                items.append(it)
        if exp_pmin is not None:
            it = _bounds_item(name, "pmin", g["pmin"], exp_pmin)
            if it is not None:
                items.append(it)
        # Count an "explained" diff only when the effective rule fired AND
        # gtopt's pmax would otherwise have been flagged against the raw
        # nameplate (max(Gen_Rating)) — i.e. the rule genuinely absorbed a
        # would-be false positive.
        if why is not None:
            rating = raw_rating.get(name)
            try:
                gpmax = float(g["pmax"])
            except (TypeError, ValueError):
                gpmax = None
            if rating and gpmax is not None and _bounds_differ(gpmax, max(rating)):
                explained += 1

    if explained:
        logger.info(
            "B12: %d generator(s) explained by the converter's units-out / "
            "fixed-load effective-bound rule (would false-flag vs raw "
            "nameplate; not counted as mismatches)",
            explained,
        )

    for ln in lines:
        name = ln["name"]
        maxr = lin_max.get(name)
        minr = lin_min.get(name)
        # PLEXOS ships DIRECTIONAL ratings: ``Lin_MaxRating`` is the forward
        # (export) limit, ``Lin_MinRating`` the reverse (import) limit, stored
        # as a negative number.  The converter models these as two caps —
        # ``tmax_ab = max(Lin_MaxRating)`` and ``tmax_ba = |min(Lin_MinRating)|``
        # — so the reverse leg MUST be compared against the Min Rating
        # magnitude.  Comparing ``tmax_ba`` against ``Lin_MaxRating`` would
        # false-flag every line with an asymmetric rating (e.g.
        # Crucero220->Laberinto220: MaxRating 282 / MinRating -276 → tmax_ba
        # 276 is correct, not a 6 MW "mismatch").  When no Min Rating ships,
        # PLEXOS treats the line as symmetric and the reverse leg mirrors the
        # forward rating.
        fwd = max(maxr) if maxr else None
        rev = abs(min(minr)) if minr else fwd
        if fwd is not None:
            for fld in ("tmax_ab", "tmax_normal_ab"):
                it = _bounds_item(name, fld, ln[fld], fwd)
                if it is not None:
                    items.append(it)
        if rev is not None:
            for fld in ("tmax_ba", "tmax_normal_ba"):
                it = _bounds_item(name, fld, ln[fld], rev)
                if it is not None:
                    items.append(it)

    # --- Reserve provision caps (gtopt urmax/drmax vs PLEXOS CFdata MRU/MRD) ---
    # The converter sets ``reserve_provision.urmax``/``drmax`` to the per-(gen,
    # hour) MAX RESERVE CAPABILITY aggregated from CFdata/{CPF,CSF,CTF} MRU/MRD
    # files (the AUTHORITATIVE PLEXOS cap — sol max == CFdata cap exactly).  B12
    # never covered these (only Gen/Lin bounds), so a wrong provision cap was
    # invisible to the LP-level audit.  Compare the emitted peak against the
    # CFdata peak; skip provisions with no CFdata data (their urmax falls back
    # to the pmax nameplate, which is not a CFdata-driven value to audit).
    if (input_dir / "CFdata").is_dir():
        from .parsers import _cf_maxresp_aggregate  # noqa: PLC0415
        from .plexos_loader import PlexosBundle  # noqa: PLC0415

        bundle = PlexosBundle(root=input_dir, source=input_dir)
        for prov in _parse_gtopt_provisions(planning_json):
            gen = prov["generator"]
            for fld, direction in (("urmax", "MRU"), ("drmax", "MRD")):
                cf = _cf_maxresp_aggregate(bundle, gen, direction)
                if not cf:
                    continue
                exp = max(cf)
                it = _bounds_item(
                    f"{prov['name']} ({gen})", fld, _field_peak(prov[fld]), exp
                )
                if it is not None:
                    items.append(it)

    return items


def build_b13_profile_collapse(planning_json: Path, input_dir: Path) -> list[dict]:
    """B13: per-block profile-collapse detector (gtopt JSON vs PLEXOS input).

    B12 compares only the PEAK of a rating against ``max(CSV)``, so a converter
    that collapses a time-varying input profile to its peak scalar matches B12
    exactly and slips through — the loss is in the de-rated blocks B12 never
    inspects (e.g. battery BAT_TOCOPILLA 72→110 MW emitted as a flat 110).
    B13 closes that blind spot: when a per-period ``Gen_Rating`` series VARIES
    across the horizon, the emitted gtopt cap MUST be a per-block profile
    (a JSON list), not a scalar.

    Scope: ``generator.pmax`` and ``battery.pmax_discharge`` — the fields whose
    rating maps DIRECTLY to ``Gen_Rating`` with no lift / fixed-load / units-out
    override that could legitimately produce a scalar from a varying input
    (lines carry the --lift-line-caps inflation, commitments the MSL clamp, so
    they are intentionally excluded to avoid false positives; the parse-time
    ``_warn_if_series_varies`` guard and B12 cover those).  An absent or
    zero-capacity field (val <= 0) is N/A.
    """
    items: list[dict] = []
    if not planning_json.is_file():
        return items
    rating_path = input_dir / "Gen_Rating.csv"
    if not rating_path.is_file():
        return items
    from .plexos_csv import read_long  # noqa: PLC0415

    # Read the FULL horizon, not just day 1 — battery DLR ratings vary
    # across DAYS (e.g. BAT_ALFALFAL_VR2), so an n_days=1 read would miss
    # cross-day variation.  Count the distinct (Y,M,D) days in the CSV.
    _days: set[tuple[str | None, str | None, str | None]] = set()
    with rating_path.open(encoding="utf-8-sig", newline="") as _fh:
        for _row in csv.DictReader(_fh):
            _days.add((_row.get("YEAR"), _row.get("MONTH"), _row.get("DAY")))
    gen_rating = read_long(rating_path, n_days=max(1, len(_days)))
    data = json.loads(planning_json.read_text())
    system = data.get("system", {})

    def _varies(series: list[float]) -> bool:
        # Compare only DEFINED (non-zero) periods — read_long zero-pads sparse
        # CSVs, and 0 means "no row", not a real rating.
        nz = [v for v in series if v != 0.0]
        return bool(nz) and (max(nz) - min(nz)) > 1.0e-9

    def _check(name: str | None, val: Any, label: str) -> None:
        if name is None:
            return
        series = gen_rating.get(name)
        if not series or not _varies(series):
            return
        if isinstance(val, list):
            return  # emitted as a per-block profile — correct
        if not isinstance(val, (int, float)) or val <= 0.0:
            return  # absent / zero-capacity — not applicable
        nz = [v for v in series if v != 0.0]
        items.append(
            {
                "name": name,
                "field": label,
                "input": "Gen_Rating",
                "input_range": [round(min(nz), 4), round(max(nz), 4)],
                "gtopt_scalar": round(float(val), 4),
            }
        )

    for g in system.get("generator_array", []):
        cap = g.get("capacity")
        _check(
            g.get("name"), cap if cap is not None else g.get("pmax"), "generator.pmax"
        )
    for b in system.get("battery_array", []):
        _check(b.get("name"), b.get("pmax_discharge"), "battery.pmax_discharge")

    return items


def _parse_gtopt_reservoir_bounds(planning_json: Path) -> list[dict]:
    """Extract reservoir ``emin``/``emax`` records from the planning JSON.

    Each record is ``{name, emin, emax}`` where the bound is the RAW JSON
    value — either a scalar or a per-block profile (a nested ``[[...]]``
    list).  Empty when the JSON ships no ``reservoir_array``.
    """
    if not planning_json.is_file():
        return []
    data = json.loads(planning_json.read_text())
    out: list[dict] = []
    for r in data.get("system", {}).get("reservoir_array", []):
        name = r.get("name")
        if name is None:
            continue
        out.append({"name": str(name), "emin": r.get("emin"), "emax": r.get("emax")})
    return out


def build_b14_reservoir_bounds(planning_json: Path, input_dir: Path) -> list[dict]:
    """B14: reservoir storage-bound consistency (gtopt JSON vs PLEXOS input).

    Reservoir ``emin``/``emax`` are NATIVE element bounds on
    ``reservoir_array`` — they are outside the Generator/Line scope of B12
    and the UC-name audit, so a collapsed or wrong reservoir bound was
    previously INVISIBLE to the audit (the exact silent-collapse class this
    bucket guards: ``Hydro_MaxVolume`` 12,330 → 10,570 emitted as a flat
    10,570 scalar, so gtopt can never hold above the day-1 cap PLEXOS
    honours).

    Two checks against ``Hydro_MinVolume.csv`` / ``Hydro_MaxVolume.csv``
    (WIDE per-period series, keyed by reservoir name):

      * ``emax_peak_below_input`` — the PLEXOS Max Volume series peaks
        above the gtopt ``emax`` peak (gtopt caps the reservoir tighter
        than PLEXOS ever did — CANUTILLAR loses its 12,331 intra-day cap).
      * ``profile_collapsed`` — the PLEXOS series VARIES across the horizon
        but gtopt emitted a bare scalar (mirrors B13 for reservoir bounds).

    Emin is the PLEXOS *operational floor* and is intentionally NOT flagged
    when gtopt's scalar floor sits at-or-above the PLEXOS peak floor (the
    soft ``efin`` slack carries the operational end-of-day target); only a
    genuine scalar collapse of a varying floor is reported.
    """
    items: list[dict] = []
    if not planning_json.is_file():
        return items
    from .plexos_csv import read_wide  # noqa: PLC0415

    emax_path = input_dir / "Hydro_MaxVolume.csv"
    emin_path = input_dir / "Hydro_MinVolume.csv"

    def _read_days(path: Path) -> dict[str, list[float]]:
        if not path.is_file():
            return {}
        days: set[tuple[str | None, str | None, str | None]] = set()
        with path.open(encoding="utf-8-sig", newline="") as fh:
            for row in csv.DictReader(fh):
                days.add((row.get("YEAR"), row.get("MONTH"), row.get("DAY")))
        return read_wide(path, n_days=max(1, len(days)))

    emax_series = _read_days(emax_path)
    emin_series = _read_days(emin_path)
    if not emax_series and not emin_series:
        return items  # no reservoir input to audit against

    def _nonzero(series: list[float]) -> list[float]:
        return [v for v in series if v != 0.0]

    def _varies(series: list[float]) -> bool:
        nz = _nonzero(series)
        return bool(nz) and (max(nz) - min(nz)) > 1.0e-9

    for r in _parse_gtopt_reservoir_bounds(planning_json):
        name = r["name"]
        # ── emax: must not cap below the PLEXOS Max Volume peak ──────────
        emax_in = emax_series.get(name)
        if emax_in:
            nz = _nonzero(emax_in)
            in_peak = max(nz) if nz else None
            g_peak = _field_peak(r["emax"])
            if (
                in_peak is not None
                and g_peak is not None
                and _bounds_differ(g_peak, in_peak)
            ):
                if g_peak < in_peak:  # gtopt caps tighter than PLEXOS ever was
                    items.append(
                        {
                            "name": name,
                            "field": "reservoir.emax",
                            "input": "Hydro_MaxVolume",
                            "kind": "emax_peak_below_input",
                            "input_peak": round(in_peak, 4),
                            "gtopt_peak": round(g_peak, 4),
                        }
                    )
            # Varying input but scalar gtopt value ⇒ collapsed profile.
            if _varies(emax_in) and not isinstance(r["emax"], list):
                items.append(
                    {
                        "name": name,
                        "field": "reservoir.emax",
                        "input": "Hydro_MaxVolume",
                        "kind": "profile_collapsed",
                        "input_range": [round(min(nz), 4), round(max(nz), 4)],
                        "gtopt_scalar": r["emax"],
                    }
                )
        # ── emin: only flag a genuine scalar collapse of a varying floor ─
        emin_in = emin_series.get(name)
        if emin_in and _varies(emin_in) and not isinstance(r["emin"], list):
            nz = _nonzero(emin_in)
            items.append(
                {
                    "name": name,
                    "field": "reservoir.emin",
                    "input": "Hydro_MinVolume",
                    "kind": "profile_collapsed",
                    "input_range": [round(min(nz), 4), round(max(nz), 4)],
                    "gtopt_scalar": r["emin"],
                }
            )

    return items


# ---------------------------------------------------------------------------
# PLEXOS sol .accdb cache loaders
# ---------------------------------------------------------------------------
def _read_csv(path: Path) -> list[dict]:
    with path.open() as f:
        return list(csv.DictReader(f))


def build_plexos_solution(cache_dir: Path) -> dict[str, dict]:
    """Aggregate per-constraint solution metrics from the PLEXOS cache."""
    objects = {}
    for r in _read_csv(cache_dir / "t_object.csv"):
        try:
            objects[int(r["object_id"])] = {
                "name": r["name"],
                "class_id": int(r["class_id"]),
            }
        except (ValueError, KeyError):
            continue
    constraint_oids = {
        oid: rec["name"]
        for oid, rec in objects.items()
        if rec["class_id"] == CONSTRAINT_CLASS_ID
    }
    sys_mem_by_constraint: dict[int, int] = {}
    for m in _read_csv(cache_dir / "t_membership.csv"):
        try:
            if (
                int(m["collection_id"]) == SYS_CONSTRAINT_COLLECTION_ID
                and int(m["child_class_id"]) == CONSTRAINT_CLASS_ID
            ):
                sys_mem_by_constraint[int(m["child_object_id"])] = int(
                    m["membership_id"]
                )
        except (ValueError, KeyError):
            continue
    mid_to_constraint_oid = {mid: cid for cid, mid in sys_mem_by_constraint.items()}
    keys_by_constraint: dict[int, dict[int, list[int]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for k in _read_csv(cache_dir / "t_key.csv"):
        try:
            cid = mid_to_constraint_oid.get(int(k["membership_id"]))
            pid = int(k["property_id"])
        except (ValueError, KeyError):
            continue
        if cid is None or pid not in _WANTED_PIDS:
            continue
        keys_by_constraint[cid][pid].append(int(k["key_id"]))

    by_key: dict[int, list[float]] = defaultdict(list)
    for r in _read_csv(cache_dir / "t_data_0.csv"):
        try:
            kid = int(r["key_id"])
            v = float(r["value"])
        except (ValueError, KeyError):
            continue
        by_key[kid].append(v)

    result: dict[str, dict] = {}
    for oid, name in constraint_oids.items():
        prop_keys = keys_by_constraint.get(oid, {})
        rec: dict[str, Any] = {"name": name, "object_id": oid}
        for label, pid in (
            ("activity", PROP_ACTIVITY),
            ("slack", PROP_SLACK),
            ("hours_binding", PROP_HRSBIND),
            ("rhs", PROP_RHS),
            ("price", PROP_PRICE),
            # MODELLED input penalty price (pid 4393) — distinct from the
            # solution-side ``price`` (shadow price) above.  Absent from the
            # constraint-solution cache; its presence (non-zero) is the only
            # reliable "genuinely soft in PLEXOS" signal the B6 check trusts.
            ("input_penalty", PROP_PENALTY_PRICE),
        ):
            values: list[float] = []
            for kid in prop_keys.get(pid, ()):
                values.extend(by_key.get(kid, ()))
            if values:
                rec[f"{label}_n"] = len(values)
                rec[f"{label}_sum"] = sum(values)
                rec[f"{label}_max"] = max(values)
                rec[f"{label}_min"] = min(values)
                rec[f"{label}_sum_abs"] = sum(abs(v) for v in values)
                if label == "rhs":
                    # Keep the full per-interval series so the audit can do a
                    # DIRECT distinct-value comparison vs gtopt (catching a
                    # profile flattened to a scalar) rather than a lenient
                    # range-overlap test.
                    rec["rhs_values"] = list(values)
            else:
                rec[f"{label}_n"] = 0
                rec[f"{label}_sum"] = 0.0
                rec[f"{label}_max"] = 0.0
                rec[f"{label}_min"] = 0.0
                rec[f"{label}_sum_abs"] = 0.0
        # Modelled input penalty price (max over intervals); 0.0 when the
        # cache carries no penalty data (the usual case).
        rec["input_penalty_price"] = rec.get("input_penalty_max", 0.0)
        rec["input_penalty_present"] = bool(rec.get("input_penalty_n", 0) > 0)
        rec["plexos_hard_solved"] = bool(
            rec["price_sum_abs"] > 0.0 and rec["slack_sum_abs"] == 0.0
        )
        rec["plexos_binding"] = bool(rec["hours_binding_sum"] > 0)
        rec["plexos_active"] = bool(
            rec["activity_n"] > 0 or rec["rhs_n"] > 0 or rec["hours_binding_n"] > 0
        )
        result[name] = rec
    return result


# ---------------------------------------------------------------------------
# PLEXOS sol Storage / per-period property loader (B14/B15 sol path)
# ---------------------------------------------------------------------------
# Storages are ``collection_id`` 93, child ``class_id`` 8.  The per-period
# solution columns (verified against ``t_property`` in the 02-15 sol cache):
#   643 Max Volume   644 Min Volume   645 Initial Volume   646 End Volume
#   680 Shadow Price
# Each (object, property) maps to ONE ``key_id`` in ``t_key.csv``; the
# per-block series lives in ``t_data_0.csv`` ordered by ``period_id``.
STORAGE_COLLECTION_ID = 93
STORAGE_PROP_MAX_VOLUME = 643
STORAGE_PROP_MIN_VOLUME = 644
STORAGE_PROP_END_VOLUME = 646
STORAGE_PROP_SHADOW_PRICE = 680
# Fuel objects: ``System → Fuels`` collection_id 40.  The CEN sol does NOT
# report a per-period Fuel "Price" — only Offtake (376) and Shadow Price
# (394) — so the B15 sol path is opportunistic (it fires only when a Fuel
# Price property is present), with the converter-parsed input CSV as the
# robust fallback (clearly marked "input-vs-emitted").
FUEL_COLLECTION_ID = 40
# Candidate per-period Fuel Price property names (resolved from t_property
# when a name map is supplied — the numeric id is schema-dependent).
_FUEL_PRICE_PROP_NAMES = ("Price",)

# ---------------------------------------------------------------------------
# PLEXOS sol Reserve property loader (B17 soft-slack coverage)
# ---------------------------------------------------------------------------
# Reserve objects are ``class_id`` 14, exposed through the
# ``System → Reserves`` collection_id 156.  The properties that prove the
# reserve requirement is modelled SOFT in PLEXOS (validated against the CEN
# PCP 2026-02-15 sol cache):
#   851 Provision   854 Shortage   857 (Risk/CleanProvision)   860 Price
#   862 (Marginal/ShadowPrice)
# The PRESENCE of the ``Shortage`` property (854) on the reserve membership is
# the signal that PLEXOS treats the requirement as soft — when it can't meet
# the reserve it under-provides at a penalty (Σ provision + shortage ≥ req)
# rather than failing.  The clearing ``Price`` (860) max is the PLEXOS reserve
# marginal value — a sound lower bound for gtopt's ``reserve_shortage_cost``
# (it must exceed the typical reserve marginal yet stay below ``demand_fail_cost``
# VoLL so a reserve shortfall is always cheaper than shedding load).
RESERVE_CLASS_ID = 14
SYS_RESERVE_COLLECTION_ID = 156
RESERVE_PROP_SHORTAGE = 854
RESERVE_PROP_PRICE = 860


def load_plexos_reserve_softness(cache_dir: Path) -> dict[str, Any]:
    """Detect PLEXOS reserve softness from the sol cache.

    Reads the Reserve membership (collection 156, child class 14) and reports
    whether the ``Shortage`` property (854) is present — the marker that PLEXOS
    models the reserve requirement SOFT — plus the clearing ``Price`` (860)
    maximum across all reserve objects (the suggested ``reserve_shortage_cost``
    lower bound).  Returns a dict::

        {
          "shortage_present": bool,   # 854 present on any reserve membership
          "price_max": float | None,  # max clearing price (860), None if absent
          "shortage_max": float | None,  # max reported shortage (854)
          "n_reserves": int,          # reserve objects in the cache
          "reserve_names": list[str],
        }

    A missing cache / table degrades gracefully to ``shortage_present=False``,
    ``price_max=None`` so the B17 cross-check simply does not fire.
    """
    out: dict[str, Any] = {
        "shortage_present": False,
        "price_max": None,
        "shortage_max": None,
        "n_reserves": 0,
        "reserve_names": [],
    }
    try:
        objects: dict[int, str] = {}
        names: list[str] = []
        for r in _read_csv(cache_dir / "t_object.csv"):
            try:
                oid = int(r["object_id"])
                cid = int(r["class_id"])
            except (ValueError, KeyError):
                continue
            objects[oid] = r["name"]
            if cid == RESERVE_CLASS_ID:
                names.append(r["name"])
        # membership_id -> reserve object_id (collection 156, child class 14)
        mem_to_oid: dict[int, int] = {}
        for m in _read_csv(cache_dir / "t_membership.csv"):
            try:
                if (
                    int(m["collection_id"]) == SYS_RESERVE_COLLECTION_ID
                    and int(m["child_class_id"]) == RESERVE_CLASS_ID
                ):
                    mem_to_oid[int(m["membership_id"])] = int(m["child_object_id"])
            except (ValueError, KeyError):
                continue
        # key_id -> property_id for reserve memberships
        shortage_keys: set[int] = set()
        price_keys: set[int] = set()
        shortage_present = False
        for k in _read_csv(cache_dir / "t_key.csv"):
            try:
                mid = int(k["membership_id"])
                pid = int(k["property_id"])
            except (ValueError, KeyError):
                continue
            if mid not in mem_to_oid:
                continue
            if pid == RESERVE_PROP_SHORTAGE:
                shortage_present = True
                shortage_keys.add(int(k["key_id"]))
            elif pid == RESERVE_PROP_PRICE:
                price_keys.add(int(k["key_id"]))
        price_max: float | None = None
        shortage_max: float | None = None
        if shortage_keys or price_keys:
            for r in _read_csv(cache_dir / "t_data_0.csv"):
                try:
                    kid = int(r["key_id"])
                    v = float(r["value"])
                except (ValueError, KeyError):
                    continue
                if kid in price_keys:
                    price_max = v if price_max is None else max(price_max, v)
                elif kid in shortage_keys:
                    shortage_max = v if shortage_max is None else max(shortage_max, v)
    except FileNotFoundError:
        return out
    out["shortage_present"] = shortage_present
    out["price_max"] = price_max
    out["shortage_max"] = shortage_max
    out["n_reserves"] = len(names)
    out["reserve_names"] = sorted(names)
    return out


def load_plexos_reserve_vors(
    xml_path: Path | None,
) -> dict[str, dict[str, Any]]:
    """Per-reserve raw VoRS (shortage-penalty) INPUT from the PLEXOS model XML.

    The solution cache exposes only reserve OUTPUTS (Provision / Shortage /
    Price); the shortage PENALTY is an INPUT (``VoRS`` family) carried on the
    Reserve objects in the model XML.  Reads it via the same probe the converter
    uses (:func:`plexos2gtopt.parsers.probe_reserve_violation_cost`) so B17 can
    QUOTE the actual penalty input instead of only inferring softness from the
    ``Shortage`` output.

    Returns ``{reserve_name: {"vors": float, "source_prop": str | None,
    "interpreted": "default_sentinel" | "explicit_cost" | "no_penalty"}}``:

    * ``default_sentinel`` — ``VoRS = -1`` (PLEXOS uses its default penalty;
      the reserve stays SOFT with a ``Shortage`` variable).
    * ``explicit_cost`` — a strictly positive penalty was set.
    * ``no_penalty`` — no VoRS-family property defined (gtopt treats it hard).

    Returns ``{}`` when the XML is absent / unreadable (the B17 cross-check then
    falls back to the ``Shortage``-output inference).
    """
    if xml_path is None or not Path(xml_path).is_file():
        return {}
    try:
        from .parsers import probe_reserve_violation_cost
        from .plexos_xml import load_xml
    except ImportError as exc:  # pragma: no cover - defensive
        logger.warning("reserve VoRS read unavailable: %s", exc)
        return {}
    try:
        db = load_xml(xml_path)
    except (OSError, ValueError) as exc:  # pragma: no cover - defensive
        logger.warning("reserve VoRS read failed (%s): %s", xml_path, exc)
        return {}
    out: dict[str, dict[str, Any]] = {}
    for r in db.objects_of_class("Reserve"):
        raw, src = probe_reserve_violation_cost(db, r.object_id)
        if raw == -1.0:
            interp = "default_sentinel"
        elif raw > 0.0:
            interp = "explicit_cost"
        else:
            interp = "no_penalty"
        out[r.name] = {"vors": raw, "source_prop": src, "interpreted": interp}
    return out


def _storage_series_from_cache(
    cache_dir: Path,
    prop_ids: tuple[int, ...],
) -> dict[str, dict[int, list[float]]]:
    """Per-period Storage solution series from the PLEXOS sol cache.

    Returns ``{storage_name: {property_id: [v_period1, v_period2, ...]}}`` for
    the requested ``prop_ids``.  The series is ordered by ``period_id`` (the
    LP block index), so it aligns 1:1 with gtopt's block layout for a
    single-stage daily case.  Missing tables / properties degrade to an empty
    dict so the caller skips the sol path gracefully.
    """
    needed = set(prop_ids)
    objects: dict[int, str] = {}
    try:
        for r in _read_csv(cache_dir / "t_object.csv"):
            try:
                objects[int(r["object_id"])] = r["name"]
            except (ValueError, KeyError):
                continue
        # membership_id -> storage object_id (collection 93)
        mem_to_oid: dict[int, int] = {}
        for m in _read_csv(cache_dir / "t_membership.csv"):
            try:
                if int(m["collection_id"]) == STORAGE_COLLECTION_ID:
                    mem_to_oid[int(m["membership_id"])] = int(m["child_object_id"])
            except (ValueError, KeyError):
                continue
        # key_id -> (storage_name, property_id)
        key_to_tag: dict[int, tuple[str, int]] = {}
        for k in _read_csv(cache_dir / "t_key.csv"):
            try:
                mid = int(k["membership_id"])
                pid = int(k["property_id"])
            except (ValueError, KeyError):
                continue
            oid = mem_to_oid.get(mid)
            if oid is None or pid not in needed:
                continue
            name = objects.get(oid)
            if name is not None:
                key_to_tag[int(k["key_id"])] = (name, pid)
        # ordered per-period values
        ordered: dict[int, list[tuple[int, float]]] = defaultdict(list)
        for r in _read_csv(cache_dir / "t_data_0.csv"):
            try:
                kid = int(r["key_id"])
            except (ValueError, KeyError):
                continue
            if kid not in key_to_tag:
                continue
            try:
                period = int(r.get("period_id", 0))
            except (ValueError, TypeError):
                period = len(ordered[kid])
            try:
                ordered[kid].append((period, float(r["value"])))
            except (ValueError, KeyError, TypeError):
                continue
    except FileNotFoundError:
        return {}
    out: dict[str, dict[int, list[float]]] = defaultdict(dict)
    for kid, pairs in ordered.items():
        name, pid = key_to_tag[kid]
        pairs.sort(key=lambda t: t[0])
        out[name][pid] = [v for _, v in pairs]
    return dict(out)


# ---------------------------------------------------------------------------
# gtopt emitted-profile expansion (scalar | [[stage][block]] matrix)
# ---------------------------------------------------------------------------
def _emitted_block_series(val: Any, n_blocks: int) -> list[float] | None:
    """Expand a gtopt JSON field into a flat per-block series of length ``n_blocks``.

    Handles BOTH forms the writer emits:
      * a scalar (``int`` / ``float``)            → broadcast to every block,
      * a ``[[stage][block]]`` matrix (nested list) → flattened in stage order.
    A flat ``[block]`` list is also accepted.  Returns ``None`` when the value
    is absent / non-numeric, or an empty list when there is nothing to expand.
    """
    if val is None:
        return None
    if isinstance(val, (int, float)):
        return [float(val)] * max(0, n_blocks)
    if isinstance(val, list):
        flat: list[float] = []

        def _walk(node: Any) -> None:
            if isinstance(node, list):
                for e in node:
                    _walk(e)
            else:
                try:
                    flat.append(float(node))
                except (TypeError, ValueError):
                    pass

        _walk(val)
        return flat or None
    return None


def _series_varies(series: list[float], *, drop_zero: bool = False) -> bool:
    """True when ``series`` carries more than one distinct value (tolerance 1e-6).

    When ``drop_zero`` is set, zero entries are ignored first (PLEXOS / read_long
    zero-pad sparse periods; 0 means "no row", not a real value).
    """
    vals = [v for v in series if v != 0.0] if drop_zero else list(series)
    return len(vals) > 1 and (max(vals) - min(vals)) > 1.0e-6


def _trim_trailing_zeros(series: list[float]) -> list[float]:
    """Drop trailing zero entries (``read_long`` zero-pads undefined periods).

    The CEN long-format readers return a fixed ``periods × n_days`` length and
    zero-fill periods with no CSV row.  Those trailing zeros are "no data", not
    a real value, so they must be stripped before a per-block comparison —
    otherwise a short (e.g. monthly-constant) input series resampled against a
    24-period-padded vector picks up phantom zeros.  At least one element is
    always kept.
    """
    end = len(series)
    while end > 1 and series[end - 1] == 0.0:
        end -= 1
    return series[:end]


def _resample_to(series: list[float], n: int) -> list[float]:
    """Resample ``series`` to length ``n`` by nearest-index sampling.

    PLEXOS per-period series and gtopt's emitted block series usually share the
    same block count, but a benign length mismatch (e.g. a stage padded with a
    trailing block) is bridged by proportional index mapping rather than
    rejected — the per-block comparison is robust to a one-off off-by-one.
    """
    if not series or n <= 0:
        return []
    if len(series) == n:
        return list(series)
    out: list[float] = []
    for i in range(n):
        j = min(len(series) - 1, (i * len(series)) // n)
        out.append(series[j])
    return out


# B14/B15/B16 per-block comparison tolerance.  A value is "the same" only when
# within BOTH a relative and an absolute band (reuses the spirit of B12).
_PROFILE_REL_TOL = 0.01
_PROFILE_ABS_TOL = 1.0e-3


def _distinct_set_mismatch(gtopt: list[float], plexos: list[float]) -> dict | None:
    """Compare two series by their DISTINCT VALUE SETS (position-independent).

    Used by the INPUT-vs-EMITTED checks (B15 fuel price, B16 turbine PF) where
    the gtopt value is already aggregated to the block layout but the PLEXOS
    input is per-(hour) — so a positional per-block compare is unsound (we lack
    the writer's period→block reducer).  Comparing the distinct levels each side
    carries still catches a unit / magnitude / sign error and a coarser-than-
    input quantisation, without false-flagging a benign re-bucketing.  Returns a
    diff dict when the distinct sets differ beyond tolerance, else ``None``.
    """
    g_dist = _distinct_values(list(gtopt))
    p_dist = _distinct_values(list(plexos))
    if not g_dist or not p_dist:
        return None
    same_count = len(g_dist) == len(p_dist)
    aligned = same_count and not any(
        _value_differs(a, b) for a, b in zip(g_dist, p_dist)
    )
    if aligned:
        return None
    return {
        "gtopt_distinct": [round(v, 6) for v in g_dist],
        "plexos_distinct": [round(v, 6) for v in p_dist],
    }


def _profile_block_diff(
    gtopt: list[float], plexos: list[float]
) -> tuple[float, float, int] | None:
    """Worst per-block absolute / relative diff between two equal-block series.

    Returns ``(max_abs, max_rel, worst_block)`` when the two series differ
    beyond tolerance in at least one block, else ``None``.  ``plexos`` is
    resampled to ``len(gtopt)`` first.
    """
    if not gtopt or not plexos:
        return None
    p = _resample_to(plexos, len(gtopt))
    worst_abs = 0.0
    worst_rel = 0.0
    worst_blk = -1
    flagged = False
    for i, (g, q) in enumerate(zip(gtopt, p)):
        d = abs(g - q)
        rel = d / max(abs(g), abs(q), 1.0)
        if d > _PROFILE_ABS_TOL and rel > _PROFILE_REL_TOL:
            flagged = True
            if d > worst_abs:
                worst_abs, worst_rel, worst_blk = d, rel, i
    if not flagged:
        return None
    return worst_abs, worst_rel, worst_blk


def build_b14_reservoir_bounds_sol(
    planning_json: Path,
    storages: dict[str, dict[int, list[float]]],
) -> list[dict]:
    """B14 (sol-based): reservoir emin/emax vs the PLEXOS SOLUTION Storage series.

    Unlike the input-CSV variant (:func:`build_b14_reservoir_bounds`, which
    needs ``Hydro_Min/MaxVolume.csv`` — usually absent inside CEN
    ``DATOS*.zip.xz`` bundles), this reads the per-period Storage
    Min Volume (644) / Max Volume (643) / End Volume (646) that the PLEXOS sol
    ALWAYS carries, so the check actually runs on a CEN bundle.

    For every reservoir present in BOTH the JSON and the sol it emits, with a
    clear ``kind`` bucket tag:

      * ``profile_collapsed`` — PLEXOS Min/Max Volume VARIES across the
        horizon but gtopt emitted a bare scalar (the silent-collapse
        regression this whole effort targets).
      * ``profile_spurious`` — gtopt emitted a varying profile but the PLEXOS
        series is constant.
      * ``value_mismatch`` — the per-block emax values differ beyond tolerance
        (reports gtopt vs PLEXOS, max abs/rel diff, worst block).
      * ``emin_over_constrained`` (HIGH) — gtopt's emitted ``emin`` floor lies
        ABOVE PLEXOS's per-period Min Volume at one or more blocks: gtopt's
        hard floor forbids reservoir levels PLEXOS itself uses, so gtopt could
        never reproduce the PLEXOS trajectory.  Always a real converter bug
        (the POLCURA 2026-02-15 case: gtopt 3.998 vs PLEXOS 3.093, 15 blocks).
      * ``emin_value_mismatch`` (INFO) — gtopt's ``emin`` differs from PLEXOS's
        Min Volume but sits BELOW it: gtopt may intentionally relax to a
        physical floor + soft ``efin``, so this is informational.
      * ``bound_violated_in_sol`` — PLEXOS End Volume ever lies OUTSIDE gtopt's
        ``[emin, emax]`` band (gtopt's bound is inconsistent with PLEXOS's own
        trajectory — gtopt could never reproduce the PLEXOS solution).

    Every profile check asserts gtopt's emitted matrix value EQUALS PLEXOS's
    matrix value per block — the emax via :func:`_profile_block_diff` and the
    emin via the per-block over/under comparison above.  The emin direction is
    split because a too-HIGH floor is a hard bug while a too-LOW floor is the
    documented soft-efin design.
    """
    items: list[dict] = []
    if not planning_json.is_file() or not storages:
        return items

    for r in _parse_gtopt_reservoir_bounds(planning_json):
        name = r["name"]
        sol = storages.get(name)
        if not sol:
            continue
        end_vol = sol.get(STORAGE_PROP_END_VOLUME, [])
        n_blocks = len(
            sol.get(STORAGE_PROP_MAX_VOLUME) or sol.get(STORAGE_PROP_MIN_VOLUME) or []
        ) or len(end_vol)
        if n_blocks == 0:
            continue

        for field_name, gval, pid, is_floor in (
            ("reservoir.emax", r["emax"], STORAGE_PROP_MAX_VOLUME, False),
            ("reservoir.emin", r["emin"], STORAGE_PROP_MIN_VOLUME, True),
        ):
            p_series = sol.get(pid)
            if not p_series:
                continue
            p_varies = _series_varies(p_series, drop_zero=True)
            g_is_list = isinstance(gval, list)
            g_series = _emitted_block_series(gval, n_blocks)
            base = {"name": name, "field": field_name, "source": "plexos_sol"}
            # profile_collapsed: PLEXOS Min/Max Volume VARIES but gtopt emitted a
            # bare scalar.  For the FLOOR (emin) only flag a GENUINE collapse —
            # i.e. gtopt's scalar sits BELOW the PLEXOS floor peak (it could not
            # reproduce the high intra-day floor), not the documented efin design
            # where gtopt's physical floor legitimately sits below the PLEXOS
            # operational Min (efin carries that on the soft slack).
            if p_varies and not g_is_list:
                nz = [v for v in p_series if v != 0.0]
                g_scalar = float(gval) if isinstance(gval, (int, float)) else None
                genuine = True
                if is_floor and g_scalar is not None and nz:
                    # efin-design floor: gtopt scalar at-or-below PLEXOS floor is
                    # expected, not a collapse.
                    genuine = g_scalar > max(nz) + _PROFILE_ABS_TOL
                if genuine:
                    items.append(
                        {
                            **base,
                            "kind": "profile_collapsed",
                            "plexos_range": [round(min(nz), 4), round(max(nz), 4)],
                            "gtopt_scalar": gval,
                        }
                    )
            # The CAP (emax) gets the full profile_spurious / value_mismatch
            # checks: gtopt's emitted matrix value MUST equal PLEXOS's Max
            # Volume value per block.
            elif not is_floor:
                # profile_spurious: gtopt varying, PLEXOS constant.
                if g_is_list and _series_varies(g_series or []) and not p_varies:
                    items.append(
                        {
                            **base,
                            "kind": "profile_spurious",
                            "plexos_constant": round(p_series[0], 4),
                            "gtopt_range": [
                                round(min(g_series or [0.0]), 4),
                                round(max(g_series or [0.0]), 4),
                            ],
                        }
                    )
                elif g_series is not None:
                    diff = _profile_block_diff(g_series, p_series)
                    if diff is not None:
                        worst_abs, worst_rel, worst_blk = diff
                        items.append(
                            {
                                **base,
                                "kind": "value_mismatch",
                                "max_abs_diff": round(worst_abs, 4),
                                "max_rel_diff": round(worst_rel, 6),
                                "worst_block": worst_blk,
                                "gtopt_at_worst": round(g_series[worst_blk], 4),
                                "plexos_at_worst": round(
                                    _resample_to(p_series, len(g_series))[worst_blk], 4
                                ),
                            }
                        )

            # ── emin VALUE comparison (gtopt floor vs PLEXOS per-period Min) ──
            # gtopt's emitted Reservoir.emin is a HARD LP floor (vol >= emin).
            # It MUST NOT exceed PLEXOS's actual per-period Min Volume, or gtopt
            # forbids reservoir levels PLEXOS itself uses (over-constraint).
            # The converter sometimes takes emin from PLEXOS's *static* Min
            # Volume property while PLEXOS's matrix carries a lower per-period
            # Min — the POLCURA 2026-02-15 case (gtopt 3.998 vs PLEXOS 3.093).
            #   * emin_over_constrained (HIGH): gtopt_emin > PLEXOS_min beyond
            #     tol at any block — always a real bug (gtopt could never
            #     reproduce the PLEXOS trajectory).
            #   * emin_value_mismatch (informational): gtopt_emin < PLEXOS_min
            #     (or otherwise differs) — gtopt may intentionally relax to a
            #     physical floor + soft efin, so lower severity.
            if is_floor and g_series is not None:
                p_blk = _resample_to(p_series, len(g_series))
                worst_over = 0.0
                blk_over = -1
                worst_under = 0.0
                blk_under = -1
                n_over = 0
                for i, (g, q) in enumerate(zip(g_series, p_blk)):
                    # PLEXOS zero-pads periods that carry no Min Volume row
                    # (read_long / sol export); a 0.0 means "no floor", not a
                    # real floor of 0 — skip it so gtopt's physical floor above
                    # a zero-pad period is never mis-flagged (ELTORO 130/132).
                    if q == 0.0:
                        continue
                    over = g - q  # gtopt floor ABOVE PLEXOS min → over-constraint
                    if over > _PROFILE_ABS_TOL:
                        n_over += 1
                        if over > worst_over:
                            worst_over, blk_over = over, i
                    under = q - g  # gtopt floor BELOW PLEXOS min → relaxed
                    if under > _PROFILE_ABS_TOL and under > worst_under:
                        worst_under, blk_under = under, i
                if blk_over >= 0:
                    items.append(
                        {
                            **base,
                            "kind": "emin_over_constrained",
                            "priority": "HIGH",
                            "direction": "gtopt_emin_above_plexos_min",
                            "max_over_by": round(worst_over, 4),
                            "worst_block": blk_over,
                            "n_blocks_over": n_over,
                            "gtopt_at_worst": round(g_series[blk_over], 4),
                            "plexos_at_worst": round(p_blk[blk_over], 4),
                        }
                    )
                elif blk_under >= 0:
                    items.append(
                        {
                            **base,
                            "kind": "emin_value_mismatch",
                            "priority": "INFO",
                            "direction": "gtopt_emin_below_plexos_min",
                            "max_under_by": round(worst_under, 4),
                            "worst_block": blk_under,
                            "gtopt_at_worst": round(g_series[blk_under], 4),
                            "plexos_at_worst": round(p_blk[blk_under], 4),
                        }
                    )

        # bound_violated_in_sol: End Volume outside gtopt's [emin, emax].  Uses
        # the gtopt-emitted bound series (scalar broadcast or matrix) per block.
        if end_vol:
            emin_s = _emitted_block_series(r["emin"], len(end_vol))
            emax_s = _emitted_block_series(r["emax"], len(end_vol))
            worst_below = 0.0
            worst_above = 0.0
            blk_below = blk_above = -1
            for i, ev in enumerate(end_vol):
                if emin_s is not None:
                    lo = emin_s[min(i, len(emin_s) - 1)]
                    if lo - ev > _PROFILE_ABS_TOL and (lo - ev) > worst_below:
                        worst_below, blk_below = lo - ev, i
                if emax_s is not None:
                    hi = emax_s[min(i, len(emax_s) - 1)]
                    if ev - hi > _PROFILE_ABS_TOL and (ev - hi) > worst_above:
                        worst_above, blk_above = ev - hi, i
            if blk_below >= 0 or blk_above >= 0:
                items.append(
                    {
                        "name": name,
                        "field": "reservoir.end_volume",
                        "source": "plexos_sol",
                        "kind": "bound_violated_in_sol",
                        "below_emin_by": round(worst_below, 4),
                        "below_block": blk_below,
                        "above_emax_by": round(worst_above, 4),
                        "above_block": blk_above,
                    }
                )

    return items


def build_b15_fuel_price(
    planning_json: Path,
    input_dir: Path | None,
    plexos_fuel_price: dict[str, list[float]] | None = None,
) -> list[dict]:
    """B15: Fuel.price per-block profile vs PLEXOS price (sol-first, input fallback).

    gtopt emits ``Fuel.price`` as a scalar OR a ``[[stage][block]]`` matrix when
    the input ``Fuel_Price.csv`` series varies.  This check compares the emitted
    profile against the PLEXOS price series, PREFERRING the sol when the sol
    reports a per-period Fuel Price (``plexos_fuel_price``, keyed by fuel name)
    and falling back to the converter input ``Fuel_Price.csv`` otherwise.  The
    ``source`` field on each item records which side was used
    (``"plexos_sol"`` or ``"input_csv"``); the latter is an INPUT-vs-EMITTED
    check, not a vs-sol check.

    Buckets (``kind``): ``profile_collapsed`` (PLEXOS varies, gtopt scalar),
    ``profile_spurious`` (gtopt varies, PLEXOS constant), ``value_mismatch``
    (per-block values differ beyond tolerance).
    """
    items: list[dict] = []
    if not planning_json.is_file():
        return items

    source = "plexos_sol"
    price_series: dict[str, list[float]] = plexos_fuel_price or {}
    if not price_series:
        # Input-CSV fallback (clearly marked input-vs-emitted).
        if input_dir is None:
            return items
        fp = input_dir / "Fuel_Price.csv"
        if not fp.is_file():
            return items
        from .plexos_csv import read_long  # noqa: PLC0415

        # Read the full horizon so cross-day price changes are not missed.
        days: set[tuple[str | None, str | None, str | None]] = set()
        with fp.open(encoding="utf-8-sig", newline="") as fh:
            for row in csv.DictReader(fh):
                days.add((row.get("YEAR"), row.get("MONTH"), row.get("DAY")))
        price_series = read_long(fp, n_days=max(1, len(days)), fill_forward=True)
        source = "input_csv"
    if not price_series:
        return items

    data = json.loads(planning_json.read_text())
    for f in data.get("system", {}).get("fuel_array", []):
        name = f.get("name")
        if name is None:
            continue
        p_series = price_series.get(str(name))
        if not p_series:
            continue
        if source == "input_csv":
            p_series = _trim_trailing_zeros(p_series)
        p_varies = _series_varies(p_series, drop_zero=True)
        gval = f.get("price")
        g_is_list = isinstance(gval, list)
        base = {"name": str(name), "field": "fuel.price", "source": source}
        if p_varies and not g_is_list:
            nz = [v for v in p_series if v != 0.0]
            items.append(
                {
                    **base,
                    "kind": "profile_collapsed",
                    "plexos_range": [round(min(nz), 4), round(max(nz), 4)],
                    "gtopt_scalar": gval,
                }
            )
            continue
        g_series = _emitted_block_series(gval, len(p_series))
        if g_is_list and _series_varies(g_series or []) and not p_varies:
            items.append(
                {
                    **base,
                    "kind": "profile_spurious",
                    "plexos_constant": round(p_series[0], 4),
                    "gtopt_range": [
                        round(min(g_series or [0.0]), 4),
                        round(max(g_series or [0.0]), 4),
                    ],
                }
            )
            continue
        if g_series is not None:
            diff = _distinct_set_mismatch(g_series, p_series)
            if diff is not None:
                items.append({**base, "kind": "value_mismatch", **diff})
    return items


def build_b16_turbine_pf(planning_json: Path, input_dir: Path | None) -> list[dict]:
    """B16: Turbine production_factor / capacity vs the bundle input series.

    ``Turbine.production_factor`` (MW per m³/s) is NOT a native PLEXOS solution
    property, so this is always an INPUT-vs-EMITTED check (every item carries
    ``source = "input_csv"``).  The per-period engineering series is
    ``Hydro_EfficiencyIncr.csv`` (keyed by the turbine's GENERATOR name); the
    turbine ``capacity`` (when emitted) is compared against ``Gen_Rating.csv``.

    Buckets (``kind``): ``profile_collapsed`` (input varies, gtopt scalar),
    ``profile_spurious`` (gtopt varies, input constant), ``value_mismatch``
    (per-block values differ beyond tolerance).  Skips gracefully when the
    input dir / CSV is absent (the usual CEN-bundle case).
    """
    items: list[dict] = []
    if not planning_json.is_file() or input_dir is None:
        return items
    pf_path = input_dir / "Hydro_EfficiencyIncr.csv"
    rating_path = input_dir / "Gen_Rating.csv"
    if not pf_path.is_file() and not rating_path.is_file():
        return items
    from .plexos_csv import read_long  # noqa: PLC0415

    def _read_full(path: Path) -> dict[str, list[float]]:
        if not path.is_file():
            return {}
        days: set[tuple[str | None, str | None, str | None]] = set()
        with path.open(encoding="utf-8-sig", newline="") as fh:
            for row in csv.DictReader(fh):
                days.add((row.get("YEAR"), row.get("MONTH"), row.get("DAY")))
        return read_long(path, n_days=max(1, len(days)), fill_forward=True)

    pf_series = _read_full(pf_path)
    rating_series = _read_full(rating_path)
    if not pf_series and not rating_series:
        return items

    data = json.loads(planning_json.read_text())
    for t in data.get("system", {}).get("turbine_array", []):
        name = t.get("name")
        gen = t.get("generator")
        if name is None:
            continue
        for field_name, gval, series_by_gen in (
            ("turbine.production_factor", t.get("production_factor"), pf_series),
            ("turbine.capacity", t.get("capacity"), rating_series),
        ):
            if gval is None:
                continue
            key = str(gen) if gen is not None else None
            p_series = series_by_gen.get(key) if key is not None else None
            if not p_series:
                continue
            p_series = _trim_trailing_zeros(p_series)
            p_varies = _series_varies(p_series, drop_zero=True)
            g_is_list = isinstance(gval, list)
            base = {
                "name": str(name),
                "generator": key,
                "field": field_name,
                "source": "input_csv",
            }
            if p_varies and not g_is_list:
                nz = [v for v in p_series if v != 0.0]
                items.append(
                    {
                        **base,
                        "kind": "profile_collapsed",
                        "input_range": [round(min(nz), 6), round(max(nz), 6)],
                        "gtopt_scalar": gval,
                    }
                )
                continue
            g_series = _emitted_block_series(gval, len(p_series))
            if g_is_list and _series_varies(g_series or []) and not p_varies:
                items.append(
                    {
                        **base,
                        "kind": "profile_spurious",
                        "input_constant": round(p_series[0], 6),
                        "gtopt_range": [
                            round(min(g_series or [0.0]), 6),
                            round(max(g_series or [0.0]), 6),
                        ],
                    }
                )
                continue
            if g_series is not None:
                diff = _distinct_set_mismatch(g_series, p_series)
                if diff is not None:
                    items.append({**base, "kind": "value_mismatch", **diff})
    return items


# ---------------------------------------------------------------------------
# B17: soft-slack coverage for the integer-coupled constraint families
# ---------------------------------------------------------------------------
# A whole CLASS the UC-name / bounds audits MISS: constraints that COUPLE to
# the integer unit-commitment and are enforced HARD in gtopt while PLEXOS
# treats them SOFT (priced slack).  The bug this guards: gtopt's reserve
# requirement was HARD (``reserve_zone_lp`` enforces ``Σ provision ≥ req`` with
# no shortage slack), so a water-short hydro unit was forced ON only to satisfy
# the floor → primal infeasibility; PLEXOS's reserve has a ``Shortage``
# property and simply under-provides at a penalty.  B17 reports, per
# integer-coupled family, whether gtopt uses a soft-slack COST or a hard floor,
# the slack-cost VALUES, and FLAGS hard-in-gtopt-but-soft-in-PLEXOS mismatches.
#
# Families (all couple to the binary commit status u):
#   1. reserve requirement   → model_options.reserve_shortage_cost (soft slack)
#   2. commitment min-stable  → Generator.pmin_fcost (soft pmin slack)
#   3. waterway forced flow   → Waterway.fmin_fcost   (soft fmin slack)
# Context (reported, not flagged): model_options.hydro_spill_cost,
# strict_storage_emin.
#
# The KEY cross-check is reserve: when the PLEXOS sol exposes the reserve
# ``Shortage`` property (soft in PLEXOS) AND gtopt's reserve_shortage_cost is
# unset/0 (hard in gtopt) → HIGH ``reserve_requirement_hard_but_plexos_soft``.


def _model_options(planning_json: Path) -> dict[str, Any]:
    """Return the ``options.model_options`` dict from the planning JSON.

    Empty dict when the file / block is absent so the caller degrades cleanly.
    """
    if not planning_json.is_file():
        return {}
    data = json.loads(planning_json.read_text())
    opts = data.get("options", {}) or {}
    mo = opts.get("model_options", {}) or {}
    return mo if isinstance(mo, dict) else {}


def _commitment_pmin_generators(planning_json: Path) -> set[str]:
    """Generators carrying a non-trivial COMMITMENT min-stable level (pmin>0).

    These are the units whose ``gen ≥ pmin·u`` floor couples to the binary
    commit status — the family whose soft slack is ``Generator.pmin_fcost``.
    A commitment with ``pmin`` unset / 0 imposes no floor, so it is excluded.
    """
    if not planning_json.is_file():
        return set()
    data = json.loads(planning_json.read_text())
    system = data.get("system", {})
    out: set[str] = set()
    for c in system.get("commitment_array", []):
        gen = c.get("generator")
        if gen is None:
            continue
        peak = _field_peak(c.get("pmin"))
        if peak is not None and peak > 0.0:
            out.add(str(gen))
    return out


def build_b17_soft_slack(
    planning_json: Path,
    reserve_softness: dict[str, Any] | None,
    reserve_vors: dict[str, dict[str, Any]] | None = None,
) -> list[dict]:
    """B17: soft-slack coverage for the integer-coupled constraint families.

    Reports, per family, whether gtopt uses a soft-slack cost or a hard floor
    and the cost VALUES, and emits the HIGH cross-check flag
    ``reserve_requirement_hard_but_plexos_soft`` when PLEXOS models the reserve
    soft but gtopt's ``reserve_shortage_cost`` is unset/0.

    ``reserve_softness`` is the dict from :func:`load_plexos_reserve_softness`
    (``None`` ⇒ the reserve cross-check is skipped, only the gtopt-side report
    is emitted).  ``reserve_vors`` is the dict from
    :func:`load_plexos_reserve_vors` (``None``/empty ⇒ PLEXOS softness is
    inferred from the ``Shortage`` OUTPUT alone).  When present it lets the
    reserve items QUOTE the actual penalty INPUT (``plexos_vors_*``) and treats
    PLEXOS as soft when the VoRS is the ``-1`` default sentinel or a positive
    cost — even if the ``Shortage`` output is absent from the cache.  Each item
    carries a ``family`` tag and either an ``info`` / ``report`` payload or a
    ``priority`` (``HIGH``) flag with the suggested remediation.
    """
    items: list[dict] = []
    if not planning_json.is_file():
        return items
    data = json.loads(planning_json.read_text())
    system = data.get("system", {})
    mo = _model_options(planning_json)
    rs = reserve_softness or {}
    rv = reserve_vors or {}

    # ── 1. Reserve requirement (reserve_zone) ───────────────────────────────
    rsc_raw = mo.get("reserve_shortage_cost")
    try:
        rsc = float(rsc_raw) if rsc_raw is not None else None
    except (TypeError, ValueError):
        rsc = None
    gtopt_reserve_soft = rsc is not None and rsc > 0.0
    n_reserve_zones = len(system.get("reserve_zone_array", []))
    demand_fail = mo.get("demand_fail_cost")
    price_max = rs.get("price_max")
    # PLEXOS softness: the ``Shortage`` OUTPUT being present, OR — when the input
    # VoRS is available — the VoRS being the ``-1`` default sentinel or a
    # positive explicit cost (both keep a soft ``Shortage`` variable).
    plexos_soft_by_vors = any(
        d.get("interpreted") in ("default_sentinel", "explicit_cost")
        for d in rv.values()
    )
    plexos_reserve_soft = bool(rs.get("shortage_present", False)) or plexos_soft_by_vors
    # Quote the actual penalty INPUT so B17 reports VoRS, not only the inferred
    # Shortage output.  Empty when no XML was supplied → items omit these keys.
    vors_payload: dict[str, Any] = {}
    if rv:
        vors_payload = {
            "plexos_vors_distinct": sorted(
                {round(float(d.get("vors", 0.0)), 4) for d in rv.values()}
            ),
            "plexos_vors_source": sorted(
                {d["source_prop"] for d in rv.values() if d.get("source_prop")}
            ),
            "plexos_vors_interpreted": sorted(
                {str(d.get("interpreted")) for d in rv.values()}
            ),
        }

    if gtopt_reserve_soft:
        # Soft in gtopt — matches the soft-PLEXOS treatment, no flag.
        items.append(
            {
                "family": "reserve_requirement",
                "kind": "soft_slack_set",
                "field": "model_options.reserve_shortage_cost",
                "value": round(rsc, 4) if rsc is not None else None,
                "n_reserve_zones": n_reserve_zones,
                "plexos_reserve_soft": plexos_reserve_soft,
                "plexos_reserve_price_max": (
                    round(float(price_max), 4) if price_max is not None else None
                ),
                **vors_payload,
                "note": "soft reserve slack set — matches PLEXOS soft Shortage "
                "treatment",
            }
        )
    elif n_reserve_zones > 0 and plexos_reserve_soft:
        # HARD in gtopt (unset/0) but SOFT in PLEXOS (Shortage present) — the
        # exact class that was missed (reserve floor → forced-ON infeasibility).
        suggested = round(float(price_max), 4) if price_max is not None else None
        items.append(
            {
                "family": "reserve_requirement",
                "kind": "reserve_requirement_hard_but_plexos_soft",
                "priority": "HIGH",
                "field": "model_options.reserve_shortage_cost",
                "gtopt_value": rsc,  # None / 0 — the hard floor
                "n_reserve_zones": n_reserve_zones,
                "plexos_shortage_present": bool(rs.get("shortage_present", False)),
                "plexos_reserve_price_max": suggested,
                "suggested_reserve_shortage_cost": suggested,
                "demand_fail_cost": demand_fail,
                **vors_payload,
                "note": "gtopt enforces the reserve requirement HARD (no "
                "shortage slack) while PLEXOS models it SOFT (Shortage "
                "property present); set model_options.reserve_shortage_cost "
                ">= the PLEXOS reserve clearing-price max and below "
                "demand_fail_cost (VoLL)",
            }
        )
    elif n_reserve_zones > 0:
        # Hard in gtopt, and no PLEXOS evidence of softness — informational.
        items.append(
            {
                "family": "reserve_requirement",
                "kind": "hard_floor_no_plexos_evidence",
                "field": "model_options.reserve_shortage_cost",
                "gtopt_value": rsc,
                "n_reserve_zones": n_reserve_zones,
                "plexos_reserve_soft": plexos_reserve_soft,
                **vors_payload,
                "note": "reserve requirement is HARD in gtopt; no PLEXOS "
                "Shortage evidence in the cache to flag against",
            }
        )

    # ── 2. Commitment min-stable level (gen ≥ pmin·u) ───────────────────────
    # Soft slack = Generator.pmin_fcost.  Keeping the COMMITMENT pmin HARD is
    # intentional, so this is INFORMATIONAL (never a HIGH flag): we report the
    # count of committed gens with vs without pmin_fcost and the value(s).
    committed_gens = _commitment_pmin_generators(planning_json)
    if committed_gens:
        gen_by_name = {
            str(g.get("name")): g
            for g in system.get("generator_array", [])
            if g.get("name") is not None
        }
        with_fcost: list[float] = []
        n_with = 0
        n_without = 0
        for gen in committed_gens:
            g = gen_by_name.get(gen)
            if g is None:
                continue
            peak = _field_peak(g.get("pmin_fcost"))
            if peak is not None and peak > 0.0:
                n_with += 1
                with_fcost.append(peak)
            else:
                n_without += 1
        items.append(
            {
                "family": "commitment_min_stable",
                "kind": "pmin_fcost_report",
                "field": "Generator.pmin_fcost",
                "n_committed_gens": len(committed_gens),
                "n_with_pmin_fcost": n_with,
                "n_without_pmin_fcost": n_without,
                "pmin_fcost_values": sorted({round(v, 4) for v in with_fcost}),
                "note": "commitment min-stable pmin is kept HARD by design "
                "(commit-gated); pmin_fcost softens it where set — "
                "informational, not a mismatch",
            }
        )

    # ── 3. Waterway forced flow (fmin > 0) ──────────────────────────────────
    # Soft slack = Waterway.fmin_fcost.  Report which forced flows are soft vs
    # hard and the cost value (Caudal_Eco_*, B_*, Filt_*).
    forced: list[dict] = []
    for w in system.get("waterway_array", []):
        name = w.get("name")
        if name is None:
            continue
        fmin_peak = _field_peak(w.get("fmin"))
        if fmin_peak is None or fmin_peak <= 0.0:
            continue
        fcost_peak = _field_peak(w.get("fmin_fcost"))
        is_soft = fcost_peak is not None and fcost_peak > 0.0
        forced.append(
            {
                "name": str(name),
                "fmin_peak": round(fmin_peak, 4),
                "soft": is_soft,
                "fmin_fcost": (
                    round(fcost_peak, 4) if fcost_peak is not None and is_soft else None
                ),
            }
        )
    if forced:
        n_soft = sum(1 for f in forced if f["soft"])
        items.append(
            {
                "family": "waterway_forced_flow",
                "kind": "fmin_fcost_report",
                "field": "Waterway.fmin_fcost",
                "n_forced_flows": len(forced),
                "n_soft": n_soft,
                "n_hard": len(forced) - n_soft,
                "forced_flows": sorted(forced, key=lambda f: f["name"]),
                "note": "forced river flows softened via fmin_fcost where set; "
                "hard fmin can force water release that conflicts with a "
                "water-short trajectory",
            }
        )

    # ── 4. Context options (reported, never flagged) ────────────────────────
    context: dict[str, Any] = {}
    for key in ("hydro_spill_cost", "strict_storage_emin"):
        if key in mo and mo[key] is not None:
            context[key] = mo[key]
    if context:
        items.append(
            {
                "family": "context",
                "kind": "context_options",
                **context,
                "note": "context options reported for soft/hard interpretation",
            }
        )

    return items


# ---------------------------------------------------------------------------
# Hard-list loader
# ---------------------------------------------------------------------------
def load_hard_list(path: Path) -> set[str]:
    """Read ``cen_pcp_hard_ucs.txt`` → set of constraint names."""
    if not path.is_file():
        return set()
    names: set[str] = set()
    for ln in path.read_text().splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        name = ln.split("#", 1)[0].strip()
        if name:
            names.add(name)
    return names


# ---------------------------------------------------------------------------
# Audit runner
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class AuditInputs:
    """Paths consumed by :func:`run_audit`."""

    plexos_cache_dir: Path
    gtopt_pampl_dir: Path
    gtopt_json: Path
    hard_list: Path | None = None
    # Optional gtopt ``.lp`` enabling the B11 native-constraint RHS check.
    # When None the native check is skipped gracefully (B11 stays empty).
    gtopt_lp: Path | None = None
    # Optional RAW PLEXOS input dir (the one containing ``Lin_MaxRating.csv``,
    # ``Gen_Rating.csv``, ...) enabling the B12 parameter-bounds check.  When
    # None the bounds check is skipped gracefully (B12 stays empty).
    plexos_input_dir: Path | None = None
    # Optional PLEXOS model XML (``DBSEN_PRGDIARIO.xml``) enabling the B17
    # reserve-VoRS read — lets B17 QUOTE the actual shortage-penalty INPUT
    # instead of only inferring softness from the ``Shortage`` output.  When
    # None the VoRS read is skipped (B17 falls back to the output inference).
    plexos_xml: Path | None = None


@dataclass
class AuditResult:
    """In-memory audit output (also serialisable via :func:`to_dict`)."""

    plexos_solution: dict[str, dict]
    gtopt_ucs: list[dict]
    duplicates: dict[str, list[str]]
    intersection: list[str]
    missing_from_gtopt: list[str]
    synthetic_in_gtopt: list[str]
    buckets: dict[str, list[dict]]
    per_row_diff: list[dict]
    summary: dict
    hard_list: set[str] = field(default_factory=set)
    # B11 native-RHS subsets whose PLEXOS source isn't in the constraint cache
    # (e.g. reserve requirements carried on a Reserve object, not a Constraint).
    native_deferred: list[dict] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "summary": self.summary,
            "duplicates": self.duplicates,
            "intersection_count": len(self.intersection),
            "missing_from_gtopt": self.missing_from_gtopt,
            "synthetic_in_gtopt": self.synthetic_in_gtopt,
            "buckets": {
                k: {"count": len(v), "items": v} for k, v in self.buckets.items()
            },
            "per_row_diff": self.per_row_diff,
            "native_deferred": self.native_deferred,
            "hard_list_total": len(self.hard_list),
        }


# Numerical-error tolerance for the DIRECT RHS value comparison (B2).  Two
# values are "the same" only when they differ by < ABS (absolute) AND < REL
# (relative) — i.e. we accept float / 6-significant-figure rounding noise but
# flag any genuine difference (a flattened profile collapsed to one value, or a
# unit / sign / magnitude error).  No ranges, no overlap slack.
_RHS_REL_TOL = 1e-4
_RHS_ABS_TOL = 1e-4


def _value_differs(a: float, b: float) -> bool:
    """True when ``a`` and ``b`` differ beyond numerical-error tolerance."""
    d = abs(a - b)
    return d > _RHS_ABS_TOL and d > _RHS_REL_TOL * max(abs(a), abs(b), 1.0)


def _distinct_values(values: list[float]) -> list[float]:
    """Sorted distinct values, merging only entries within numerical tolerance.

    The length of the result is the number of genuinely-distinct RHS levels —
    1 for a flattened/constant RHS, K for a K-level per-day/per-block profile.
    """
    out: list[float] = []
    for v in sorted(values):
        if not out or _value_differs(v, out[-1]):
            out.append(v)
    return out


def _is_nolimit_sentinel(v: float) -> bool:
    """PLEXOS contingency-off "no-limit" placeholder (±10000 / ±100000 MW).

    These are not real RHS levels — they mark a row as unconstrained on an
    inactive contingency — so they must be excluded from the RHS value set.
    """
    a = abs(v)
    return abs(a - 10000.0) < 1.0 or abs(a - 100000.0) < 1.0


# ---------------------------------------------------------------------------
# B11: native-encoded constraint RHS comparison
# ---------------------------------------------------------------------------
# Map a gtopt native LP element → its PLEXOS source Constraint name (the
# Constraint that gtopt DROPPED or PROMOTED, which therefore shows up only in
# the cache, not as a gtopt UC).
#
#   commitment  (<gen>_pmin/pmax_constraint): the source is the original
#     per-plant hydro ``<NAME>min`` / ``<NAME>max`` PLEXOS Constraint, present
#     in the cache but dropped from gtopt by the auto-promotion.  Matched by
#     the genname prefix (case-insensitive).  CLEAN case — the LP pmin/pmax
#     row carries no separate floor, so the enforced LP value IS the bound.
#
#   reserve_zone (reservezone_(u|d)requirement): the requirement is carried on
#     PLEXOS's native ``Reserve.Min Provision`` property (a Reserve object,
#     NOT a Constraint — so NOT in the constraint-only cache).  The redundant
#     ``<prefix>_<Up|Down>MinProvision`` Constraint IS in the cache and, when
#     it carries a non-trivial RHS series, is the best available proxy for the
#     enforced requirement.  Zone-name suffix governs direction:
#       ``*_RS`` → up (raise/spinning) → ``<prefix>_UpMinProvision``
#       ``*_LW`` → down (lower)        → ``<prefix>_DownMinProvision``
#     If no usable redundant constraint exists, the subset is REPORTED as
#     "source not in solution cache" rather than guessed.  IMPORTANT: the
#     gtopt-side values compared are the JSON ``urreq``/``drreq`` REQUIREMENT
#     schedule (what PLEXOS reports), NOT the enforced LP row (which folds in
#     the ``urmin``/``drmin`` Min-Provision FLOOR and would false-positive).


def _commitment_plexos_source(genname: str, plexos: dict[str, dict]) -> str | None:
    """PLEXOS source Constraint name for a commitment genname (or None).

    The LP genname is the lower-cased, sanitised plant stem.  Match it
    case-insensitively against ``<NAME>max`` / ``<NAME>min`` / ``<NAME>_PMax``
    / ``<NAME>_PMin`` style PLEXOS Constraint names in the cache.
    """
    stem = genname.lower()
    suffixes = ("max", "min", "_pmax", "_pmin")
    for cname in plexos:
        cl = cname.lower()
        for suf in suffixes:
            if cl == stem + suf:
                return cname
    return None


def _reserve_zone_plexos_source(
    zone_name: str, direction: str, plexos: dict[str, dict]
) -> str | None:
    """PLEXOS redundant ``*MinProvision`` Constraint for a reserve zone.

    ``direction`` is ``"up"`` or ``"down"``.  Returns the cache constraint
    name when a usable redundant ``*MinProvision`` exists for this zone +
    direction, else None.

    Only the zone whose SUFFIX matches the direction carries the real
    requirement (``_LW`` → lower/down, ``_RS`` → raise/up).  The opposite-
    direction row on a zone is a structural fold (all-zero) and must NOT be
    compared against the source — otherwise every zone double-counts its
    sibling's source and spuriously flags a 0-vs-real "flatten".
    """
    m = re.search(r"_(RS|LW)(_BESS)?$", zone_name)
    if m is None:
        return None
    suffix = m.group(1)
    suffix_dir = "down" if suffix == "LW" else "up"
    if suffix_dir != direction:
        return None
    prefix = zone_name[: m.start()]
    tag = "Up" if direction == "up" else "Down"
    cand = f"{prefix}_{tag}MinProvision"
    if cand not in plexos:
        return None
    return cand


def _compare_native_element(
    kind: str,
    ident: str,
    direction: str,
    g_series: list[float],
    plexos: dict[str, dict],
) -> dict | None:
    """Compare one native element's gtopt RHS vs its PLEXOS source.

    Returns a B11 item dict on a material mismatch, ``{"deferred": ...}`` when
    the source is not in the cache, or ``None`` when matched / immaterial.

    For ``reserve_zone`` ``g_series`` is the JSON ``urreq``/``drreq``
    REQUIREMENT schedule (NOT the enforced LP row), so the ``urmin``/``drmin``
    Min-Provision floor gtopt enforces never false-positives a fold.  For
    ``commitment`` ``g_series`` is the enforced LP pmin/pmax row (correct —
    no separate floor there).
    """
    if kind == "commitment":
        src = _commitment_plexos_source(ident, plexos)
    elif is_provision_only_bess_reserve_zone(ident):
        # *_BESS zones are PLEXOS provision-only sub-trackers (Min Provision = 0,
        # no requirement of their own — it is enforced once on the non-BESS twin
        # that shares the *MinProvision source).  Defer rather than flag the
        # legitimate zero requirement as a fold.
        return {
            "element_kind": kind,
            "name": ident,
            "direction": direction,
            "deferred": "provision-only BESS sub-tracker; requirement on "
            + (primary_reserve_zone_for_bess(ident) or "primary zone"),
        }
    else:
        src = _reserve_zone_plexos_source(ident, direction, plexos)
    if src is None:
        return {
            "element_kind": kind,
            "name": ident,
            "direction": direction,
            "deferred": "source not in solution cache",
        }
    p = plexos[src]
    if p["rhs_n"] == 0:
        return {
            "element_kind": kind,
            "name": ident,
            "direction": direction,
            "plexos_source": src,
            "deferred": "source has no RHS series in cache",
        }
    # Gate like B2: only compare when PLEXOS actually BOUND the source row
    # (non-zero shadow price AND binding hours) — an unbound source RHS can't
    # change the dispatch, so a divergence there is immaterial.
    if not (p["price_sum_abs"] > 0.0 and p["hours_binding_sum"] > 0):
        return None
    g_vals = [v for v in g_series if not _is_nolimit_sentinel(v)]
    p_vals = [v for v in (p.get("rhs_values") or []) if not _is_nolimit_sentinel(v)]
    if not g_vals or not p_vals:
        return None
    g_dist = _distinct_values(g_vals)
    p_dist = _distinct_values(p_vals)
    flattened = len(g_dist) == 1 and len(p_dist) > 1
    fixed_vs_variable = len(g_dist) < len(p_dist)
    mismatch = len(g_dist) != len(p_dist) or any(
        _value_differs(a, b) for a, b in zip(g_dist, p_dist)
    )
    if not mismatch:
        return None
    return {
        "element_kind": kind,
        "name": ident,
        "direction": direction,
        "plexos_source": src,
        "flattened": flattened,
        "fixed_vs_variable": fixed_vs_variable,
        "gtopt_rhs_distinct": [round(v, 4) for v in g_dist],
        "plexos_rhs_distinct": [round(v, 4) for v in p_dist],
    }


def build_b11_native_rhs(
    native: dict[NativeKey, list[float]],
    plexos: dict[str, dict],
    reserve_zone_requirement: dict[tuple[str, str], list[float]] | None = None,
) -> tuple[list[dict], list[dict]]:
    """Build the B11 bucket + the deferred-source list from native LP rows.

    Returns ``(b11_items, deferred_items)``.  Commitment rows compare the
    enforced LP pmin/pmax series.  Reserve-zone rows compare the JSON
    ``urreq``/``drreq`` REQUIREMENT series (passed in
    ``reserve_zone_requirement``, keyed ``(zone_name, direction)``) — NOT the
    enforced LP requirement row, which folds in the ``urmin``/``drmin``
    Min-Provision floor and would false-positive (gtopt enforces
    ``max(requirement, MinProvision)``; PLEXOS reports the bare requirement).
    The ``.lp`` is still used to ENUMERATE which reserve-zone elements exist.
    A reserve-zone subset with no JSON requirement series is skipped.
    """
    reqs = reserve_zone_requirement or {}
    b11: list[dict] = []
    deferred: list[dict] = []
    for (kind, ident, direction), g_series in sorted(native.items()):
        if kind == "reserve_zone":
            req = reqs.get((ident, direction))
            if req is None:
                # No requirement schedule emitted for this zone/direction —
                # nothing to compare (the LP row may be a pure structural fold).
                continue
            compare_series = req
        else:
            compare_series = g_series
        item = _compare_native_element(kind, ident, direction, compare_series, plexos)
        if item is None:
            continue
        if "deferred" in item:
            deferred.append(item)
        else:
            b11.append(item)
    return b11, deferred


def run_audit(inputs: AuditInputs) -> AuditResult:
    """Run the full audit and return :class:`AuditResult`."""
    plexos = build_plexos_solution(inputs.plexos_cache_dir)
    # Whether ANY constraint carries modelled input penalty data (pid 4393).
    # Drives the refined B6 gate: when no modelled penalty is present (the
    # normal case for a constraint-SOLUTION cache), the only "soft" signal
    # available is a solution-side relaxation shadow price, which is NOT
    # evidence of a modelled soft constraint — so B6 stays empty.
    input_penalty_present = any(
        rec.get("input_penalty_present", False) for rec in plexos.values()
    )
    gtopt_pampl = parse_pampl_dir(inputs.gtopt_pampl_dir)
    gtopt_json = (
        parse_json_ucs(inputs.gtopt_json) if inputs.gtopt_json.is_file() else []
    )
    gtopt_all = gtopt_pampl + gtopt_json
    gtopt_by_name = {row["name"]: row for row in gtopt_all}
    name_counts: dict[str, list[str]] = defaultdict(list)
    for row in gtopt_all:
        name_counts[row["name"]].append(row["file"])
    duplicates = {n: lst for n, lst in name_counts.items() if len(lst) > 1}

    hard_list = (
        load_hard_list(inputs.hard_list) if inputs.hard_list is not None else set()
    )
    # Apply the same identifier sanitization the converter uses
    # (``gtopt_writer._pampl_ident``) to PLEXOS names before set-diffing.
    # Without this, PLEXOS names containing ``-``, ``.``, ``(``, ``)``,
    # spaces, or leading digits show up as "missing from gtopt" and their
    # sanitised gtopt counterparts as "synthetic", inflating B3 / B7 / B8
    # with naming-only noise (~95 false-positive entries on v0407).
    plexos_to_sanitised: dict[str, str] = {n: _pampl_ident(n) for n in plexos}
    # Rekey the PLEXOS dict by sanitised identifier (only for diff matching;
    # the original raw name is still available via ``plexos_to_sanitised``).
    plexos_keyed = {plexos_to_sanitised[n]: r for n, r in plexos.items()}

    plexos_names = set(plexos_keyed.keys())
    gtopt_names = set(gtopt_by_name.keys())
    intersection = sorted(plexos_names & gtopt_names)
    missing_from_gtopt = sorted(plexos_names - gtopt_names)
    synthetic_in_gtopt = sorted(gtopt_names - plexos_names)
    # Replace ``plexos`` indexing key with the sanitised key so downstream
    # lookups (in the per-row diff, B2 RHS comparison, etc.) hit the same
    # entries the diff just computed.
    plexos = plexos_keyed

    buckets: dict[str, list[dict]] = defaultdict(list)
    per_row: list[dict] = []
    for name in intersection:
        p = plexos[name]
        g = gtopt_by_name[name]
        pv = g["penalty_value"] or 0.0
        if pv == 0.0:
            pen_class = "hard"
        elif pv >= 1000.0:
            pen_class = "soft_resv"
        elif pv > 0.0:
            pen_class = "soft_op"
        else:
            pen_class = "unknown"
        row = {
            "name": name,
            "family": categorise(name),
            "gtopt_op": g["op"],
            "gtopt_penalty": pv,
            "gtopt_penalty_class": pen_class,
            "gtopt_active": g["active"],
            "gtopt_n_terms": g["n_terms"],
            "plexos_rhs_max": p["rhs_max"] if p["rhs_n"] > 0 else None,
            "plexos_rhs_min": p["rhs_min"] if p["rhs_n"] > 0 else None,
            "plexos_hours_binding": p["hours_binding_sum"],
            "plexos_slack_sum_abs": p["slack_sum_abs"],
            "plexos_price_sum_abs": p["price_sum_abs"],
            "plexos_active_in_sol": p["plexos_active"],
            "plexos_hard_solved": p["plexos_hard_solved"],
            "in_hard_list": name in hard_list,
        }

        # B2: RHS SCALE mismatch — a gross unit / sign / magnitude error
        # (e.g. ANTUCO 137 MW vs 83.3 m³/s), NOT a per-block profile
        # wiggle.  Both gtopt and PLEXOS carry block-varying RHS, so the
        # only sound scalar test is whether the two value RANGES OVERLAP.
        # Comparing a single aggregate (max-vs-max OR min-vs-min) is unsafe
        # because the extrema occur in different blocks: the old max-vs-max
        # silently matched ``Reg_SouthZone`` (both peak at 320) while
        # min-vs-min would falsely flag its 187.42 floor against PLEXOS's
        # 207 floor — two non-aligned blocks.  Range overlap also dissolves
        # PLEXOS's "contingency-off" no-limit sentinels (10000 / 100000 MW):
        # gtopt's real 400 MW cap sits inside PLEXOS's [400, 10000] band, so
        # ``SD_*_Guacolda_Maitencillo`` no longer false-positives.
        #
        # We also gate on PLEXOS having ACTUALLY bound the row (non-zero
        # shadow price AND binding hours): when PLEXOS never pays for the
        # constraint its RHS value cannot change the solution, so a
        # mismatch is immaterial — the same guard the B6 bucket uses below
        # and the audit's stated goal of silencing never-binding noise.
        # This drops the date-windowed ``PANGUEpriority`` (gtopt picks a
        # relaxed ``>= -10000`` floor row on the Oct horizon while PLEXOS
        # holds it at 20, but never binds it: price = 0, hours_binding = 0).
        # B2: RHS mismatch — DIRECT distinct-value comparison (no ranges).
        # PLEXOS and gtopt must carry the SAME set of distinct RHS values up to
        # numerical error.  We cluster each side's values (merging only entries
        # within numerical tolerance) and compare the sorted distinct lists:
        #   * a different COUNT catches a per-day/per-block profile FLATTENED to
        #     a scalar (gtopt 1 distinct value vs PLEXOS K) — the profile→scalar
        #     bug class (Hydro_AntucoBounds day-to-day bounds, FlowRight
        #     targets, fuel-price/battery series read with ``[0]`` / ``max()``).
        #     The old range-OVERLAP test silently passed these: a flattened
        #     scalar sitting inside PLEXOS's [min,max] band showed zero gap.
        #   * a value DIFFERENCE catches a unit / sign / magnitude error.
        # Gated on PLEXOS actually binding the row (price & hours > 0) — an
        # unbound constraint's RHS can't change the dispatch.
        if (  # pylint: disable=too-many-boolean-expressions
            p["rhs_n"] > 0
            and g["rhs_scalar"] is not None
            and not g.get("daily_sum", False)
            and p["price_sum_abs"] > 0.0
            and p["hours_binding_sum"] > 0
            and categorise(name) not in NATIVE_PRIMITIVE_FAMILIES
            # GEN_BAT_*/LOAD_BAT_* are battery charge/discharge shut-off rows
            # gtopt correctly emits as ``rhs [0,0,…]``; PLEXOS echoes the
            # battery's moving ACTIVITY back as the row "RHS" (solution pid
            # 3073 ≡ activity pid 3069), so a direct compare spuriously reads
            # gtopt-fixed-0 vs PLEXOS-varying.  Skip them here exactly as B9
            # already does (see project_bat_cf_comp_activity_flow).
            and not name.startswith(("GEN_BAT_", "LOAD_BAT_"))
        ):
            g_raw = list(g["rhs_profile"]) if g["rhs_profile"] else [g["rhs_scalar"]]
            p_raw = p.get("rhs_values") or [p["rhs_min"], p["rhs_max"]]
            # Drop the contingency-off no-limit sentinels (±10000 / ±100000)
            # from BOTH sides: they are "unconstrained" placeholders, not real
            # RHS levels.  gtopt's ``rhs_date_overlay`` emits the real limit on
            # active blocks and a sentinel on inactive ones, while PLEXOS only
            # reports the active-block value — so filtering only PLEXOS (the
            # old behaviour) left gtopt's sentinel as a spurious extra distinct
            # level (the SDCF_Rx*/SD_*/IL_*_Capacity false positives).
            g_vals = [v for v in g_raw if not _is_nolimit_sentinel(v)]
            p_vals = [v for v in p_raw if not _is_nolimit_sentinel(v)]
            if p_vals and g_vals:
                g_dist = _distinct_values(g_vals)
                p_dist = _distinct_values(p_vals)
                flattened = len(g_dist) == 1 and len(p_dist) > 1
                # gtopt carries FEWER distinct RHS levels than PLEXOS — it
                # collapsed a VARIABLE per-block requirement into a FIXED (or
                # coarser) RHS.  Covers both a full flatten (1 vs K, e.g. a
                # scalar where PLEXOS varies) and a partial fix (a constant
                # floor band where PLEXOS varies, e.g. CTF_DownMinProvision
                # 7→12 before the Min-Provision split).  This is the
                # "fixed-instead-of-variable" signal.
                fixed_vs_variable = len(g_dist) < len(p_dist)
                mismatch = len(g_dist) != len(p_dist) or any(
                    _value_differs(a, b) for a, b in zip(g_dist, p_dist)
                )
                if mismatch:
                    buckets["B2_rhs_mismatch"].append(
                        {
                            "name": name,
                            "gtopt_op": g["op"],
                            "flattened": flattened,
                            "fixed_vs_variable": fixed_vs_variable,
                            "plexos_rhs_distinct": [round(v, 4) for v in p_dist],
                            "gtopt_rhs_distinct": [round(v, 4) for v in g_dist],
                        }
                    )

        # B5: hard in PLEXOS audit list, soft in gtopt (skip hydro-by-design)
        if name in hard_list and pen_class != "hard" and not is_hydro_minmax(name):
            buckets["B5_hard_in_plexos_soft_in_gtopt"].append(
                {"name": name, "gtopt_penalty": pv}
            )

        # B6: PLEXOS models the constraint as SOFT, gtopt enforces it HARD.
        #
        # REFINED (false-positive fix).  The previous gate fired whenever
        # PLEXOS reported a non-zero shadow price + slack on a row gtopt keeps
        # hard.  That signal is UNRELIABLE: a sibling investigation proved
        # PLEXOS ships ZERO modelled ``Penalty Price`` (pid 4393) / ``Penalty
        # Quantity`` (pid 4392) on ALL constraints — the price the gate reads
        # is PLEXOS's GLOBAL infeasibility-relaxation shadow price (a
        # solution-side artefact), NOT modelled soft data.  So the old gate
        # flagged rows gtopt CORRECTLY leaves hard.
        #
        # The constraint-solution cache carries only solution columns (pids
        # 3069-3074); the input penalty pids (4392/4393) are NOT present, so a
        # solution-only shadow price can never distinguish a genuinely-soft
        # constraint from a relaxation artefact.  B6 therefore only fires when
        # there is EXPLICIT evidence of a modelled soft constraint
        # (``input_penalty_present``).  When that evidence is absent — the
        # normal case for the constraint-solution cache — B6 stays empty by
        # design rather than emitting a false alarm.
        gtopt_keeps_hard = pen_class == "hard" and name not in hard_list
        plexos_modelled_soft = (
            input_penalty_present and p.get("input_penalty_price", 0.0) > 0.0
        )
        if (
            gtopt_keeps_hard
            and plexos_modelled_soft
            and not p["plexos_hard_solved"]
            and p["hours_binding_sum"] > 0
        ):
            buckets["B6_soft_in_plexos_hard_in_gtopt"].append(
                {
                    "name": name,
                    "plexos_slack": p["slack_sum_abs"],
                    "plexos_price": p["price_sum_abs"],
                    "input_penalty_price": p.get("input_penalty_price", 0.0),
                    "plexos_hours_binding": p["hours_binding_sum"],
                }
            )

        # B9: inactive in gtopt but PLEXOS reports binding activity.
        # Suppress for ``GEN_BAT_*`` / ``LOAD_BAT_*`` tautological
        # non-negativity rows: PLEXOS records a "price" because its
        # reduced-cost reporting includes the dual on the natural
        # ``battery.charge >= 0`` / ``battery.discharge >= 0`` bound,
        # but the row carries no real constraint — gtopt correctly
        # marks it inactive (commit ``bfa2f817e``, see memory
        # ``project_bat_cf_comp_activity_flow``).
        if name.startswith(("GEN_BAT_", "LOAD_BAT_")):
            per_row.append(row)
            continue
        if (
            not g["active"]
            and p["plexos_active"]
            and p["activity_sum_abs"] > 0.0
            and p["price_sum_abs"] > 0.0  # economically active, not just LHS-flow
        ):
            buckets["B9_inactive_gtopt_active_plexos"].append(
                {
                    "name": name,
                    "plexos_activity": p["activity_sum_abs"],
                    "plexos_price": p["price_sum_abs"],
                }
            )
        per_row.append(row)

    missing_by_family: dict[str, list[str]] = defaultdict(list)
    for name in missing_from_gtopt:
        missing_by_family[categorise(name)].append(name)
    for fam, names in missing_by_family.items():
        # B10: native-primitive promotion — gtopt enforces the constraint
        # via an entity LP primitive (Battery / Fuel / Commitment) instead
        # of as a UserConstraint.  NOT data loss; the LP row exists, it
        # just doesn't carry a UC name.  Splitting these into their own
        # bucket keeps B3 / B7 focused on REAL missing data.
        if fam in NATIVE_PRIMITIVE_FAMILIES:
            buckets["B10_native_primitive"].append(
                {
                    "family": fam,
                    "primitive": NATIVE_PRIMITIVE_FAMILIES[fam],
                    "count": len(names),
                    "sample": names[:5],
                }
            )
            continue
        if len(names) >= 50:
            buckets["B7_missing_uc_family"].append(
                {"family": fam, "count": len(names), "sample": names[:5]}
            )
        else:
            for n in names:
                buckets["B3_missing_uc"].append(
                    {
                        "name": n,
                        "family": fam,
                        "plexos_hours_binding": plexos[n]["hours_binding_sum"],
                        "plexos_price_max": plexos[n]["price_max"],
                        "plexos_active_in_sol": plexos[n]["plexos_active"],
                    }
                )

    for name in synthetic_in_gtopt:
        buckets["B8_synthetic_in_gtopt"].append(
            {
                "name": name,
                "family": categorise(name),
                "gtopt_penalty": gtopt_by_name[name]["penalty_value"],
                "gtopt_active": gtopt_by_name[name]["active"],
            }
        )

    # The B10 bucket promotes PLEXOS Constraints that gtopt encodes via
    # entity LP primitives (``Battery.max_cycles_day``, ``Fuel.max_offtake``,
    # ``Commitment.max_starts_week``) instead of UserConstraints — these
    # ARE enforced in the LP, the UC name just doesn't appear.  Subtract
    # their count from the "real missing" tally so the summary doesn't
    # double-count them as data loss.
    n_b10_native = sum(
        int(item.get("count", 0)) for item in buckets["B10_native_primitive"]
    )

    # B11: native-encoded constraint RHS comparison.  The UC-name comparison
    # (B2) is blind to anything gtopt encodes as a native LP primitive
    # (reserve_zone requirement, Commitment pmin/pmax) — those families are in
    # NATIVE_PRIMITIVE_FAMILIES and skipped.  When a gtopt ``.lp`` is supplied
    # we ENUMERATE the native rows it carries and compare the RHS gtopt
    # actually enforces to the PLEXOS source Constraint the converter dropped /
    # promoted (``<plant>min``/``max`` for commitments, ``*MinProvision`` for
    # reserve zones).  Reserve-zone VALUES come from the JSON ``urreq``/
    # ``drreq`` requirement (not the enforced LP row, which folds in the
    # Min-Provision floor).  ``plexos`` is rekeyed to sanitised identifiers
    # above, but the native source names (ANTUCO_PMax, CTF_DownMinProvision,
    # ...) are already valid identifiers, so the sanitised dict resolves 1:1.
    native_deferred: list[dict] = []
    if inputs.gtopt_lp is not None and inputs.gtopt_lp.is_file():
        native = parse_lp_native_constraints(inputs.gtopt_lp, inputs.gtopt_json)
        rz_req = reserve_zone_requirement_series(inputs.gtopt_json)
        b11_items, native_deferred = build_b11_native_rhs(native, plexos, rz_req)
        for item in b11_items:
            buckets["B11_native_rhs_mismatch"].append(item)

    # B12: parameter-bounds consistency (gtopt JSON vs PLEXOS input CSV).
    # Independent of the UC audit: compares converted generator pmin/pmax and
    # line tmax/tmin against the raw PLEXOS input time-series, catching
    # converter extraction bugs (e.g. the Chacao cable over-rated 2x because
    # the converter ignored Lin_MaxRating.csv = 90 and used the XML fallback).
    if inputs.plexos_input_dir is not None and inputs.plexos_input_dir.is_dir():
        for item in build_b12_bounds(inputs.gtopt_json, inputs.plexos_input_dir):
            buckets["B12_bounds_mismatch"].append(item)
        # B13: per-block profile-collapse (a varying Gen_Rating must not be
        # emitted as a scalar cap — the blind spot B12's peak-vs-peak misses).
        for item in build_b13_profile_collapse(
            inputs.gtopt_json, inputs.plexos_input_dir
        ):
            buckets["B13_profile_collapse"].append(item)
        # Legacy input-CSV variant of B14 (only fires when Hydro_Min/MaxVolume
        # CSVs ship in the input dir — usually absent on CEN DATOS*.zip.xz
        # bundles, where the sol-based B14 below is the real check).
        for item in build_b14_reservoir_bounds(
            inputs.gtopt_json, inputs.plexos_input_dir
        ):
            buckets["B14_reservoir_bounds_mismatch"].append(item)

    # B14 (sol-based): reservoir emin/emax vs the PLEXOS SOLUTION Storage
    # Min/Max/End Volume series — ALWAYS available from the constraint cache
    # (unlike the input CSVs).  Detects the silent scalar-collapse of a
    # time-varying PLEXOS bound and any End-Volume that lies outside gtopt's
    # [emin, emax] band.  B15/B16 audit the recently-fixed Fuel.price and
    # Turbine.production_factor profiles.
    storages = _storage_series_from_cache(
        inputs.plexos_cache_dir,
        (
            STORAGE_PROP_MAX_VOLUME,
            STORAGE_PROP_MIN_VOLUME,
            STORAGE_PROP_END_VOLUME,
        ),
    )
    for item in build_b14_reservoir_bounds_sol(inputs.gtopt_json, storages):
        buckets["B14_reservoir_bounds_mismatch"].append(item)

    # B15: Fuel.price per-block profile.  The CEN sol does not report a
    # per-period Fuel Price, so this falls back to the input Fuel_Price.csv
    # (input-vs-emitted, marked per item) when no sol price is supplied.
    for item in build_b15_fuel_price(inputs.gtopt_json, inputs.plexos_input_dir):
        buckets["B15_fuel_price_mismatch"].append(item)

    # B16: Turbine.production_factor / capacity (always input-vs-emitted —
    # not a native sol property).  Skips gracefully when the bundle CSVs are
    # absent.
    for item in build_b16_turbine_pf(inputs.gtopt_json, inputs.plexos_input_dir):
        buckets["B16_turbine_pf_mismatch"].append(item)

    # B17: soft-slack coverage for the integer-coupled constraint families
    # (reserve requirement / commitment min-stable / waterway forced flow).
    # Reports whether gtopt uses a soft-slack cost or a hard floor and the
    # cost VALUES, and emits the HIGH cross-check
    # ``reserve_requirement_hard_but_plexos_soft`` when PLEXOS exposes the
    # reserve ``Shortage`` property (soft) but gtopt's reserve_shortage_cost is
    # unset/0 (hard).  The reserve softness is read from the same PLEXOS sol
    # cache the rest of the audit uses.
    reserve_softness = load_plexos_reserve_softness(inputs.plexos_cache_dir)
    reserve_vors = load_plexos_reserve_vors(inputs.plexos_xml)
    for item in build_b17_soft_slack(inputs.gtopt_json, reserve_softness, reserve_vors):
        buckets["B17_soft_slack_coverage"].append(item)

    summary = {
        "n_plexos": len(plexos_names),
        "n_gtopt_pampl": len(gtopt_pampl),
        "n_gtopt_json": len(gtopt_json),
        "n_gtopt_total": len(gtopt_all),
        "n_intersection": len(intersection),
        "n_missing_from_gtopt": len(missing_from_gtopt),
        "n_missing_real": len(missing_from_gtopt) - n_b10_native,
        "n_missing_native_primitive": n_b10_native,
        "n_synthetic_in_gtopt": len(synthetic_in_gtopt),
        "n_duplicates_in_gtopt": len(duplicates),
        "n_native_deferred": len(native_deferred),
        "input_penalty_present": input_penalty_present,
        "bucket_counts": {k: len(v) for k, v in buckets.items()},
        "hard_list_total": len(hard_list),
        # Count of B17 HIGH-priority soft/hard mismatches (the missed class:
        # hard-in-gtopt-but-soft-in-PLEXOS).  Drives --strict alongside B2/B5.
        "n_b17_high": sum(
            1
            for it in buckets.get("B17_soft_slack_coverage", ())
            if it.get("priority") == "HIGH"
        ),
    }
    return AuditResult(
        plexos_solution=plexos,
        gtopt_ucs=gtopt_all,
        duplicates=duplicates,
        intersection=intersection,
        missing_from_gtopt=missing_from_gtopt,
        synthetic_in_gtopt=synthetic_in_gtopt,
        buckets=dict(buckets),
        per_row_diff=per_row,
        summary=summary,
        hard_list=hard_list,
        native_deferred=native_deferred,
    )


def _print_summary(result: AuditResult) -> None:
    s = result.summary
    print(f"PLEXOS constraints in sol: {s['n_plexos']}")
    print(
        f"gtopt UCs:                 {s['n_gtopt_total']} "
        f"({s['n_gtopt_pampl']} PAMPL + {s['n_gtopt_json']} JSON)"
    )
    print(f"intersection (compared):   {s['n_intersection']}")
    n_b10 = s.get("n_missing_native_primitive", 0)
    n_real = s.get("n_missing_real", s["n_missing_from_gtopt"])
    if n_b10:
        print(
            f"missing from gtopt:        {s['n_missing_from_gtopt']} "
            f"({n_real} REAL + {n_b10} encoded as native primitives, see B10)"
        )
    else:
        print(f"missing from gtopt:        {s['n_missing_from_gtopt']}")
    print(f"synthetic in gtopt:        {s['n_synthetic_in_gtopt']}")
    print(f"duplicate names in gtopt:  {s['n_duplicates_in_gtopt']}")
    n_def = s.get("n_native_deferred", 0)
    if n_def:
        print(
            f"native RHS source deferred:{n_def} "
            "(PLEXOS source not in constraint cache)"
        )
    if s["hard_list_total"]:
        print(f"hard-list size:            {s['hard_list_total']}")
    print("buckets:")
    for k, n in sorted(s["bucket_counts"].items(), key=lambda kv: -kv[1]):
        print(f"  {k:40s} {n:>6d}")
    # Surface the B17 HIGH soft/hard mismatches (the missed class) prominently.
    for it in result.buckets.get("B17_soft_slack_coverage", ()):
        if it.get("priority") == "HIGH":
            print(
                f"  [HIGH] {it.get('kind')} ({it.get('family')}): "
                f"{it.get('field')} unset/0 vs PLEXOS soft; "
                f"suggest reserve_shortage_cost >= "
                f"{it.get('suggested_reserve_shortage_cost')}"
            )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="plexos2gtopt.uc_audit",
        description="Audit gtopt user-constraints against the PLEXOS sol .accdb",
    )
    parser.add_argument(
        "--plexos-cache",
        type=Path,
        required=True,
        help="cached PLEXOS sol tables (t_object.csv, t_membership.csv, "
        "t_key.csv, t_data_0.csv) — usually the bundle's accdb_cache_dir",
    )
    parser.add_argument(
        "--gtopt-dir",
        type=Path,
        required=True,
        help="converter output dir containing uc_*.pampl + planning JSON",
    )
    parser.add_argument(
        "--gtopt-json",
        type=Path,
        default=None,
        help="explicit path to the planning JSON (defaults to the first *.json "
        "in --gtopt-dir that is NOT a *.provenance.json)",
    )
    parser.add_argument(
        "--gtopt-lp",
        type=Path,
        default=None,
        help="explicit path to the gtopt .lp enabling the B11 native-RHS check "
        "(defaults to the first *.lp in --gtopt-dir; the check is skipped "
        "gracefully when no .lp is available)",
    )
    parser.add_argument(
        "--plexos-input-dir",
        type=Path,
        default=None,
        help="raw PLEXOS input dir (containing Lin_MaxRating.csv, Gen_Rating.csv, "
        "...) enabling the B12 parameter-bounds check; skipped when omitted",
    )
    parser.add_argument(
        "--plexos-xml",
        type=Path,
        default=None,
        help="PLEXOS model XML (DBSEN_PRGDIARIO.xml) enabling the B17 reserve-VoRS "
        "read — B17 then quotes the actual shortage-penalty INPUT instead of "
        "only inferring softness from the Shortage output; skipped when omitted",
    )
    parser.add_argument(
        "--hard-list",
        type=Path,
        default=Path(__file__).parent / "data" / "cen_pcp_hard_ucs.txt",
        help="PLEXOS-HARD audit list (default: bundled cen_pcp_hard_ucs.txt)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write the full audit JSON to this path (omit for stdout summary only)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero if B2 (RHS), B5 (hard-soft), or a B17 HIGH "
        "(hard-in-gtopt-but-soft-in-PLEXOS reserve) flag is present",
    )
    return parser


def _resolve_gtopt_json(args: argparse.Namespace) -> Path:
    if args.gtopt_json is not None:
        return args.gtopt_json
    for p in sorted(args.gtopt_dir.glob("*.json")):
        if not p.name.endswith(".provenance.json"):
            return p
    raise SystemExit(
        f"no planning JSON found in {args.gtopt_dir} (pass --gtopt-json explicitly)"
    )


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    args = make_parser().parse_args(argv)
    if not args.plexos_cache.is_dir():
        raise SystemExit(f"--plexos-cache not a directory: {args.plexos_cache}")
    if not args.gtopt_dir.is_dir():
        raise SystemExit(f"--gtopt-dir not a directory: {args.gtopt_dir}")
    gtopt_json = _resolve_gtopt_json(args)
    gtopt_lp = args.gtopt_lp
    if gtopt_lp is None:
        gtopt_lp = discover_gtopt_lp(args.gtopt_dir)
    if gtopt_lp is not None and not gtopt_lp.is_file():
        logger.warning("--gtopt-lp not found (%s); skipping B11 native check", gtopt_lp)
        gtopt_lp = None
    plexos_input_dir = args.plexos_input_dir
    if plexos_input_dir is not None and not plexos_input_dir.is_dir():
        logger.warning(
            "--plexos-input-dir not a directory (%s); skipping B12 bounds check",
            plexos_input_dir,
        )
        plexos_input_dir = None
    plexos_xml = args.plexos_xml
    if plexos_xml is not None and not plexos_xml.is_file():
        logger.warning(
            "--plexos-xml not a file (%s); skipping B17 reserve-VoRS read",
            plexos_xml,
        )
        plexos_xml = None
    inputs = AuditInputs(
        plexos_cache_dir=args.plexos_cache,
        gtopt_pampl_dir=args.gtopt_dir,
        gtopt_json=gtopt_json,
        hard_list=args.hard_list if args.hard_list.is_file() else None,
        gtopt_lp=gtopt_lp,
        plexos_input_dir=plexos_input_dir,
        plexos_xml=plexos_xml,
    )
    result = run_audit(inputs)
    _print_summary(result)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result.to_dict(), indent=1, default=float))
        print(f"\nwrote audit JSON: {args.output}")
    if args.strict:
        n_b2 = len(result.buckets.get("B2_rhs_mismatch", ()))
        n_b5 = len(result.buckets.get("B5_hard_in_plexos_soft_in_gtopt", ()))
        n_b17 = result.summary.get("n_b17_high", 0)
        if n_b2 or n_b5 or n_b17:
            print(
                f"\n[strict] B2={n_b2} B5={n_b5} B17_high={n_b17} "
                "— significant divergence detected",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
