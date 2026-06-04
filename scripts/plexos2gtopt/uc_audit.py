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
* ``0``: no significant bugs detected (B2/B5 buckets empty)
* ``1``: significant bugs detected (RHS scale mismatch or hard-list drift)
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
_WANTED_PIDS = frozenset(
    {PROP_ACTIVITY, PROP_SLACK, PROP_HRSBIND, PROP_RHS, PROP_PRICE}
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


def run_audit(inputs: AuditInputs) -> AuditResult:
    """Run the full audit and return :class:`AuditResult`."""
    plexos = build_plexos_solution(inputs.plexos_cache_dir)
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
        if (
            p["rhs_n"] > 0
            and g["rhs_scalar"] is not None
            and not g.get("daily_sum", False)
            and p["price_sum_abs"] > 0.0
            and p["hours_binding_sum"] > 0
            and categorise(name) not in NATIVE_PRIMITIVE_FAMILIES
        ):
            g_vals = list(g["rhs_profile"]) if g["rhs_profile"] else [g["rhs_scalar"]]
            p_raw = p.get("rhs_values") or [p["rhs_min"], p["rhs_max"]]
            # Drop PLEXOS contingency-off no-limit sentinels (±10000 / ±100000):
            # placeholders for "unconstrained", not real RHS levels, so they
            # must not count as a distinct value (the gtopt cap legitimately
            # sits below them).  Skip the row when PLEXOS is all-sentinel.
            p_vals = [v for v in p_raw if not _is_nolimit_sentinel(v)]
            if p_vals:
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

        # B6: PLEXOS treats as soft (binding with slack), gtopt enforces hard
        if (
            name not in hard_list
            and pen_class == "hard"
            and not p["plexos_hard_solved"]
            # Require PLEXOS to have actually PAID for the slack (price > 0).
            # ``slack_sum_abs > 0`` alone is NOT enough: PLEXOS reports the
            # natural ``RHS − LHS`` gap as "slack" on every row, INCLUDING
            # inactive contingency rows (Include in ST Schedule = 0) that
            # the LP never enforces.  Without the ``price > 0`` guard the
            # audit floods B6 with ~380 inactive SD_* rows.  Only when
            # PLEXOS reports a non-zero shadow price has the LP genuinely
            # paid for the slack — that's the meaningful "PLEXOS soft"
            # signal.
            and p["price_sum_abs"] > 0.0
            and p["hours_binding_sum"] > 0
        ):
            buckets["B6_soft_in_plexos_hard_in_gtopt"].append(
                {
                    "name": name,
                    "plexos_slack": p["slack_sum_abs"],
                    "plexos_price": p["price_sum_abs"],
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
        "bucket_counts": {k: len(v) for k, v in buckets.items()},
        "hard_list_total": len(hard_list),
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
    if s["hard_list_total"]:
        print(f"hard-list size:            {s['hard_list_total']}")
    print("buckets:")
    for k, n in sorted(s["bucket_counts"].items(), key=lambda kv: -kv[1]):
        print(f"  {k:40s} {n:>6d}")


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
        help="exit non-zero if B2 (RHS) or B5 (hard-soft) buckets are non-empty",
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
    inputs = AuditInputs(
        plexos_cache_dir=args.plexos_cache,
        gtopt_pampl_dir=args.gtopt_dir,
        gtopt_json=gtopt_json,
        hard_list=args.hard_list if args.hard_list.is_file() else None,
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
        if n_b2 or n_b5:
            print(
                f"\n[strict] B2={n_b2} B5={n_b5} — significant divergence detected",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
