#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build a COMPLETE, COMMITMENT-FEASIBLE MIP warm-start seed from a solved
reduced case.

gtopt writes ``Commitment/status_sol.parquet`` SPARSELY — only the cells
whose commitment is ON (``value == 1``) appear.  A seed that lists just
those rows leaves every OFF (generator, block) unspecified, so CPLEX must
guess them; the resulting partial MIP-start is frequently rejected.

This builder enumerates the FULL cross product ``committable-generators ×
blocks`` and fills absent cells with ``u = 0``, producing the dense
``generator_uid, block_uid, u`` seed the full-MIP ``mip_start`` expects.

It then REPAIRS the u-patterns so the seed satisfies every commitment-only
constraint family that a strict backend feasibility check (CPLEX
``solve_fixed``) enforces without slack.  Thresholding an LP relaxation at
0.5 routinely breaks these (verified on CEN 2026-04-12: the exact minimal
conflict was ``LAUTARO_1_Order`` — ``p_BL2 <= p_BL1`` with the seed at
``u_BL1=0, u_BL2=1`` — plus min-up/down windows on LAUTARO / PEHUENCHE_U2 /
RALCO_U1):

  1. ``fixed_status`` pins (per-block profile; ``-1`` = free sentinel) and
     ``must_run`` — hard, always win.
  2. Initial-condition windows: a unit ON with ``ini_hours_up`` remaining
     min-up stays ON; a unit OFF inside its min-down window stays OFF.
  3. Interior min-up/min-down run lengths: short OFF gaps are filled ON,
     short ON runs are extended forward.
  4. PLEXOS *order* pairs from ``uc_*.pampl`` (two-term dispatch rows
     ``-1*A.generation + 1*B.generation <= 0`` with follower ``pmin > 0``):
     ``u_B = 1`` requires ``u_A = 1`` — the leader is promoted ON (or the
     follower demoted when the leader is pinned OFF).

The repair is monotone toward MORE commitment, which is dispatch-safe under
``--allow-oversupply`` (excess pmin energy is discarded as free disposal).
Rules iterate to a fixpoint; residual violations (never seen so far) are
reported, not silently shipped.

Seeds key on GENERATOR identity (network reduction never renames
generators), so the reduced and full models line up by
``(generator_uid, block_uid)``.

Usage:
    python -m gtopt_warmstart.build_full_seed REDUCED_OUT_DIR MIP_JSON OUT.csv
    (installed: ``gtopt_build_seed``; ``--no-repair`` for A/B experiments)
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import re
from pathlib import Path

import pyarrow.dataset as pads

_log = logging.getLogger(__name__)


def _generator_uid_resolver(system: dict):
    """Return ``resolve(ref) -> uid | None`` handling BOTH reference styles a
    ``commitment_array.generator`` field can use: integer uid or name."""
    gname = {int(g["uid"]): g.get("name") for g in system["generator_array"]}
    name2uid = {v: k for k, v in gname.items() if v is not None}

    def resolve(ref) -> int | None:
        if isinstance(ref, int) and not isinstance(ref, bool):
            return ref if ref in gname else None
        if isinstance(ref, str):
            return name2uid.get(ref)
        return None

    return resolve


def _commitment_to_generator(system: dict) -> dict[int, int]:
    """Map commitment uid → generator uid via ``commitment_array.generator``.

    The seed keys on GENERATOR identity because network reduction never
    renames generators — the reduced and full models agree on it.
    """
    resolve = _generator_uid_resolver(system)
    out: dict[int, int] = {}
    for c in system.get("commitment_array", []):
        guid = resolve(c.get("generator"))
        if guid is not None:
            out[int(c["uid"])] = guid
    return out


# PAMPL prints mid-expression signs DETACHED (`... - 1 * commitment(...)`),
# so the sign is a separate capture group — a `-?` glued to the digits reads
# `- 1` as `+1` and silently flips the constraint sense.
_ORDER_TERM_RE = re.compile(r'([+-]?)\s*(1)\s*\*\s*generator\("([^"]+)"\)\.generation')
# Anchored at line start so `inactive constraint X:` declarations (PLEXOS
# active=False rows — NOT emitted into the LP by gtopt) are skipped.
_CONSTRAINT_RE = re.compile(r"^constraint\s+(\S+):\s*\n(.+?);", re.S | re.M)


def _load_system(mip_json: Path) -> dict:
    with mip_json.open() as fh:
        return json.load(fh)["system"]


def _scalar(v, agg=max) -> float:
    """Coerce a scalar-or-per-block-list field to one float (``agg`` of the
    blocks for lists; the repair rules only need 'is it ever > 0')."""
    if v is None:
        return 0.0
    if isinstance(v, list):
        flat = v[0] if v and isinstance(v[0], list) else v
        vals = [float(x) for x in flat if isinstance(x, (int, float))]
        return agg(vals) if vals else 0.0
    return float(v)


def _profile(v) -> list | None:
    """Unwrap a per-block profile field ([[...]] or [...]) to a flat list."""
    if isinstance(v, list):
        return v[0] if v and isinstance(v[0], list) else v
    return None


def _commitment_meta(system: dict) -> dict[int, dict]:
    """Per generator-uid commitment metadata used by the repair rules."""
    resolve = _generator_uid_resolver(system)
    gen_pmax = {g["uid"]: g.get("pmax") for g in system.get("generator_array", [])}
    meta: dict[int, dict] = {}
    for c in system.get("commitment_array", []):
        guid = resolve(c.get("generator"))
        if guid is None:
            continue
        fs = c.get("fixed_status")
        if isinstance(fs, list) and fs and isinstance(fs[0], list):
            fs = fs[0]  # [[...]] wide-row nesting
        pmin = _scalar(c.get("pmin"), agg=max)
        # Data-derived forced-OFF blocks: the generator's pmax profile
        # (maintenance windows) below the commitment pmin makes u=1
        # dispatch-infeasible (p >= pmin > pmax >= p).  Verified on CEN
        # 04-12: SAN_ANDRES carries 21 such seed cells.  Treated as a pin
        # so the min-up/down fill rules never re-commit those blocks.
        forced_off = None
        pmax_prof = _profile(gen_pmax.get(int(guid)))
        if pmax_prof is not None and pmin > 0.0:
            forced_off = [(p is not None and p < pmin - 1e-9) for p in pmax_prof]
        meta[int(guid)] = {
            "name": c.get("name", ""),
            "min_up": int(_scalar(c.get("min_up_time"))),
            "min_down": int(_scalar(c.get("min_down_time"))),
            "ini_status": c.get("initial_status"),
            "ini_up": _scalar(c.get("ini_hours_up")),
            "ini_down": _scalar(c.get("ini_hours_down")),
            "must_run": bool(_scalar(c.get("must_run"))),
            "pins": fs if isinstance(fs, list) else None,
            "pmin": pmin,
            "forced_off": forced_off,
        }
    return meta


def _order_pairs(mip_json: Path, system: dict, meta: dict[int, dict]) -> list:
    """(leader_uid, follower_uid) pairs with follower pmin > 0.

    Parsed from every ``uc_*.pampl`` next to the case JSON: two-term
    dispatch rows ``-1*A.generation + 1*B.generation <= 0`` mean
    ``p_B <= p_A``; with ``pmin_B > 0`` this forces ``u_B <= u_A``.
    """
    name2uid = {g["name"]: g["uid"] for g in system.get("generator_array", [])}
    pairs = []
    for pampl in sorted(mip_json.parent.glob("uc_*.pampl")):
        text = pampl.read_text()
        for _, expr in _CONSTRAINT_RE.findall(text):
            rhs = re.search(r"(<=|>=|=)\s*(-?[\d.]+)\s*$", expr.strip())
            if not rhs or rhs.group(1) != "<=" or float(rhs.group(2)) != 0.0:
                continue
            terms = _ORDER_TERM_RE.findall(expr)
            # Exactly the two dispatch terms and nothing else on the row.
            if len(terms) != 2 or expr.count("*") != 2:
                continue
            coefs = {name: (-1 if sign == "-" else 1) for sign, _, name in terms}
            neg = [n for n, c in coefs.items() if c == -1]
            pos = [n for n, c in coefs.items() if c == 1]
            if len(neg) != 1 or len(pos) != 1:
                continue
            lead, follow = name2uid.get(neg[0]), name2uid.get(pos[0])
            if lead is None or follow is None:
                continue
            if meta.get(int(follow), {}).get("pmin", 0.0) > 0.0:
                pairs.append((int(lead), int(follow)))
    return pairs


# Sign captured separately: PAMPL prints `... - 1 * commitment(...)` with
# the minus detached from the digits (see _ORDER_TERM_RE note).
_STATUS_TERM_RE = re.compile(
    r'([+-]?)\s*([\d.]+(?:[eE][+-]?\d+)?)\s*\*\s*commitment\("([^"]+)"\)\.status'
)


def _status_rows(mip_json: Path, system: dict) -> list:
    """Linear u-only rows from every ``uc_*.pampl``: (name, terms, op, rhs)
    with ``terms = [(coef, generator_uid), ...]``.

    Only rows whose EVERY product term is a ``commitment("...").status`` are
    kept — mixed rows (dispatch, flows, slacks) are dispatch-repairable by
    the solver and not the seed's job.  These pure-status rows are exactly
    the ones a strict ``solve_fixed`` check cannot relax (verified: both
    CEN 04-12 rejection IISes were pure-status rows — ``LAUTARO_1_Order``
    via pmin coupling and ``SANISIDRO_2_ConfCC_GNL``).
    """
    resolve = _generator_uid_resolver(system)
    c2guid = {
        c["name"]: resolve(c.get("generator"))
        for c in system.get("commitment_array", [])
    }
    rows = []
    for pampl in sorted(mip_json.parent.glob("uc_*.pampl")):
        for cname, expr in _CONSTRAINT_RE.findall(pampl.read_text()):
            rhs_m = re.search(r"(<=|>=|=)\s*(-?[\d.]+)\s*;?\s*$", expr.strip())
            if not rhs_m:
                continue
            sterms = _STATUS_TERM_RE.findall(expr)
            if not sterms or len(sterms) != expr.count("*"):
                continue  # not a pure-status row
            terms = []
            ok = True
            for sign, coef, ucname in sterms:
                guid = c2guid.get(ucname)
                if guid is None:
                    ok = False
                    break
                val = float(coef) * (-1.0 if sign == "-" else 1.0)
                terms.append((val, int(guid)))
            if ok:
                rows.append((cname, terms, rhs_m.group(1), float(rhs_m.group(2))))
            else:
                _log.warning("dropped status row %s (unmapped unit)", cname)
    return rows


def _fuel_offtakes(system: dict) -> list:
    """Per capped fuel: (fuel_name, weekly_cap, [(generator_uid, burn/h at
    pmin), ...]).  With u fixed, every ON hour of a unit forces at least
    ``pmin × heat_rate`` fuel offtake; if the committed total exceeds the
    HARD ``Fuel.max_offtake`` cap no dispatch can satisfy the row and a
    strict backend check rejects the whole start (found on CEN 04-12: the
    transport seed forced Gas_EnelMejillones_E 5% over its weekly cap)."""
    caps = {
        f["name"]: _scalar(f.get("max_offtake"))
        for f in system.get("fuel_array", [])
        if _scalar(f.get("max_offtake")) > 0.0
    }
    if not caps:
        return []
    resolve = _generator_uid_resolver(system)
    pmin = {}
    for c in system.get("commitment_array", []):
        guid = resolve(c.get("generator"))
        if guid is not None:
            pmin[guid] = _scalar(c.get("pmin"))
    units: dict[str, list] = {}
    for g in system.get("generator_array", []):
        fu = g.get("fuel")
        hr = g.get("heat_rate")
        guid = g.get("uid")
        if fu in caps and hr and pmin.get(guid, 0.0) > 0.0:
            units.setdefault(fu, []).append((int(guid), pmin[guid] * float(hr)))
    return [(fu, caps[fu], units[fu]) for fu in units]


def _repair_fuel_offtakes(u, blocks, meta, fuels, raw, durations, set_u) -> None:
    """Demote ON hours of capped-fuel units (least-committed raw fraction
    first, pins respected) until the forced pmin burn fits the weekly cap."""
    for _, cap_v, units in fuels:
        rate = dict(units)
        cells = [
            (raw.get((g, b), 0.0), g, i, b)
            for g, _ in units
            for i, b in enumerate(blocks)
            if u.get((g, b), 0) == 1
        ]
        forced = sum(rate[g] * durations.get(b, 1.0) for _, g, _, b in cells)
        if forced <= cap_v + 1e-9:
            continue
        for _, g, i, b in sorted(cells):
            if _pin(meta.get(g), i) is not None:
                continue
            set_u(g, i, 0, "fuel")
            forced -= rate[g] * durations.get(b, 1.0)
            if forced <= cap_v + 1e-9:
                break


def _pin(meta_g: dict | None, idx: int) -> int | None:
    """Pinned u value at block-index ``idx`` (None = free).

    Precedence: data-derived forced-OFF (pmax maintenance window below
    pmin) > explicit ``fixed_status`` pin > initial-condition window
    (a unit inside its remaining min-up/min-down window is NOT free —
    making this a pin stops the order-pair / run-fill rules from
    oscillating against the window) > ``must_run``.
    """
    if not meta_g:
        return None
    fo = meta_g.get("forced_off")
    if fo is not None and idx < len(fo) and fo[idx]:
        return 0
    pins = meta_g.get("pins")
    if pins is not None and idx < len(pins):
        v = pins[idx]
        if 0.0 <= v <= 1.0:
            return 1 if v >= 0.5 else 0
    ini = meta_g.get("ini_status")
    if ini == 1 and idx < int(meta_g.get("min_up", 0) - meta_g.get("ini_up", 0.0)):
        return 1
    if ini == 0 and idx < int(meta_g.get("min_down", 0) - meta_g.get("ini_down", 0.0)):
        return 0
    return 1 if meta_g.get("must_run") else None


def _repair_status_rows(u, blocks, meta, srows, raw, set_u) -> None:
    """One pass over the pure-status pampl rows: fix each violated
    (row, block) with single flips.  Preference order — demote a
    negative-coefficient ON unit (indicator/flag semantics) or a
    positive-coefficient ON unit for `<=` overshoot, using the raw
    LP fractional value as the tie-break (flip the unit whose
    relaxation was least committed); promote an OFF unit only as the
    last resort (it commits real capacity the LP did not choose)."""
    eps = 1e-9
    for _, terms, op, rhs in srows:
        for i, b in enumerate(blocks):
            for _ in range(len(terms)):
                lhs = sum(c * u.get((g, b), 0) for c, g in terms)
                need_down = op in ("<=", "=") and lhs > rhs + eps
                need_up = op in (">=", "=") and lhs < rhs - eps
                if not (need_down or need_up):
                    break
                demote, promote = [], []
                for c, g in terms:
                    cur = u.get((g, b), 0)
                    pin = _pin(meta.get(g), i)
                    if pin is not None:
                        continue
                    helps_down = (c > 0 and cur == 1) or (c < 0 and cur == 0)
                    helps_up = (c < 0 and cur == 1) or (c > 0 and cur == 0)
                    helps = helps_down if need_down else helps_up
                    if not helps:
                        continue
                    r = raw.get((g, b), 0.0)
                    if cur == 1:
                        demote.append((r, g))  # least-committed first
                    else:
                        promote.append((-r, g))  # most-committed first
                if demote:
                    _, g = min(demote)
                    set_u(g, i, 0, "urow")
                elif promote:
                    _, g = min(promote)
                    set_u(g, i, 1, "urow")
                else:
                    break  # only pinned candidates — leave for verify


def repair_seed(
    u: dict[tuple[int, int], int],
    blocks: list[int],
    meta: dict[int, dict],
    pairs: list,
    srows: list,
    raw: dict[tuple[int, int], float],
    fuels: list | None = None,
    durations: dict[int, float] | None = None,
) -> dict[str, int]:
    """In-place commitment-feasibility repair; returns per-rule flip counts."""
    stats = {"pin": 0, "min_down": 0, "min_up": 0, "order": 0, "urow": 0, "fuel": 0}
    fuels = fuels or []
    durations = durations or {}
    nb = len(blocks)

    def set_u(g: int, i: int, val: int, rule: str) -> None:
        key = (g, blocks[i])
        if u.get(key, 0) != val:
            u[key] = val
            stats[rule] += 1

    for _ in range(10):
        before = dict(stats)
        for g, m in meta.items():
            # 1. pins / must_run.
            for i in range(nb):
                p = _pin(m, i)
                if p is not None:
                    set_u(g, i, p, "pin")
            seq = [u.get((g, b), 0) for b in blocks]
            # 3. interior min-down gaps → fill ON; short ON runs → extend.
            runs: list[list[int]] = []  # [value, start, length]
            for i, v in enumerate(seq):
                if runs and runs[-1][0] == v:
                    runs[-1][2] += 1
                else:
                    runs.append([v, i, 1])
            for k, (val, st, ln) in enumerate(runs):
                interior = 0 < k < len(runs) - 1
                if not interior:
                    continue
                if val == 0 and m["min_down"] > 1 and ln < m["min_down"]:
                    for i in range(st, st + ln):
                        if _pin(m, i) is None:
                            set_u(g, i, 1, "min_down")
                elif val == 1 and m["min_up"] > 1 and ln < m["min_up"]:
                    for i in range(st + ln, min(st + m["min_up"], nb)):
                        if _pin(m, i) is None:
                            set_u(g, i, 1, "min_up")
        # 4. order pairs: follower ON requires leader ON.
        for lead, follow in pairs:
            ml = meta.get(lead)
            for i, b in enumerate(blocks):
                if u.get((follow, b), 0) == 1 and u.get((lead, b), 0) == 0:
                    if _pin(ml, i) == 0:
                        set_u(follow, i, 0, "order")
                    else:
                        set_u(lead, i, 1, "order")
        # 5. generic pure-status pampl rows (exclusivity, config
        # implications, indicator links).
        _repair_status_rows(u, blocks, meta, srows, raw, set_u)
        # 6. weekly fuel-offtake caps: forced pmin burn must fit.
        _repair_fuel_offtakes(u, blocks, meta, fuels, raw, durations, set_u)
        if stats == before:
            break
    return stats


def verify_seed(
    u: dict[tuple[int, int], int],
    blocks: list[int],
    meta: dict[int, dict],
    pairs: list,
    srows: list,
    fuels: list | None = None,
    durations: dict[int, float] | None = None,
) -> int:
    """Count residual commitment-only violations (0 = strict-feasible)."""
    bad = 0
    nb = len(blocks)
    for g, m in meta.items():
        seq = [u.get((g, b), 0) for b in blocks]
        for i in range(nb):
            p = _pin(m, i)
            if p is not None and seq[i] != p:
                bad += 1
        runs: list[list[int]] = []
        for i, v in enumerate(seq):
            if runs and runs[-1][0] == v:
                runs[-1][2] += 1
            else:
                runs.append([v, i, 1])
        for k, (val, _, ln) in enumerate(runs):
            if 0 < k < len(runs) - 1:
                if val == 0 and m["min_down"] > 1 and ln < m["min_down"]:
                    bad += 1
                elif val == 1 and m["min_up"] > 1 and ln < m["min_up"]:
                    bad += 1
    for lead, follow in pairs:
        for b in blocks:
            if u.get((follow, b), 0) > u.get((lead, b), 0):
                bad += 1
    durations = durations or {}
    for fu_name, cap_v, units in fuels or []:
        forced = sum(
            r * durations.get(b, 1.0)
            for g, r in units
            for b in blocks
            if u.get((g, b), 0) == 1
        )
        if forced > cap_v + 1e-6:
            bad += 1
            _log.warning(
                "fuel %s forced burn %.1f exceeds cap %.1f", fu_name, forced, cap_v
            )
    eps = 1e-9
    for _, terms, op, rhs in srows:
        for b in blocks:
            lhs = sum(c * u.get((g, b), 0) for c, g in terms)
            if op == "<=" and lhs > rhs + eps:
                bad += 1
            elif op == ">=" and lhs < rhs - eps:
                bad += 1
            elif op == "=" and abs(lhs - rhs) > eps:
                bad += 1
    return bad


def _blocks_from_case(mip_json: Path) -> list[int]:
    """The model's FULL block-uid list (``simulation.block_array``), so the
    seed covers blocks where EVERY unit is OFF (the sparse status_sol has no
    rows there — deriving blocks from the parquet alone would hide min-up/
    down windows crossing an all-OFF block).  Empty when the case JSON has
    no block layout (caller falls back to the parquet-derived list)."""
    with mip_json.open() as fh:
        case = json.load(fh)
    return [int(b["uid"]) for b in case.get("simulation", {}).get("block_array", [])]


def _block_durations(mip_json: Path) -> dict[int, float]:
    """block uid → duration [h] from ``simulation.block_array`` (default 1)."""
    with mip_json.open() as fh:
        case = json.load(fh)
    return {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in case.get("simulation", {}).get("block_array", [])
    }


def build_full_seed(
    out_dir: Path,
    mip_json: Path,
    seed_csv: Path,
    *,
    repair: bool = True,
) -> dict:
    """Enumerate every (committable generator, block); absent → u=0; repair.

    ``repair=False`` writes the raw densified seed (A/B experiments: shows
    whether the un-repaired rounding would be accepted).
    Returns a small summary dict (rows, u1, u0, generators, blocks, repairs).
    """
    status = out_dir / "Commitment" / "status_sol.parquet"
    if not status.exists():
        raise FileNotFoundError(status)
    df = pads.dataset(status).to_table(columns=["block", "uid", "value"]).to_pandas()

    system = _load_system(mip_json)
    c2g = _commitment_to_generator(system)
    df["generator_uid"] = df["uid"].map(c2g)
    df = df[df["generator_uid"].notna()].copy()

    # ON set: (generator, block) with any commitment status >= 0.5.
    on = df[df["value"] >= 0.5]
    on_pairs = {(int(g), int(b)) for g, b in zip(on["generator_uid"], on["block"])}

    generators = sorted({int(g) for g in c2g.values()})
    blocks = _blocks_from_case(mip_json) or sorted(
        {int(b) for b in df["block"].unique()}
    )

    u = {(g, b): (1 if (g, b) in on_pairs else 0) for g in generators for b in blocks}

    # Raw fractional LP status per (generator, block) — the repair tie-break
    # (flip the unit whose relaxation was least committed).
    raw: dict[tuple[int, int], float] = {}
    for guid, b, v in zip(df["generator_uid"], df["block"], df["value"]):
        key = (int(guid), int(b))
        raw[key] = max(raw.get(key, 0.0), float(v))

    meta = _commitment_meta(system)
    pairs = _order_pairs(mip_json, system, meta)
    srows = _status_rows(mip_json, system)
    fuels = _fuel_offtakes(system)
    durations = _block_durations(mip_json)
    stats = (
        repair_seed(u, blocks, meta, pairs, srows, raw, fuels, durations)
        if repair
        else {}
    )
    residual = verify_seed(u, blocks, meta, pairs, srows, fuels, durations)

    u1 = 0
    with seed_csv.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["generator_uid", "block_uid", "u"])
        for g in generators:
            for b in blocks:
                val = u[(g, b)]
                u1 += val
                w.writerow([g, b, val])

    total = len(generators) * len(blocks)
    return {
        "rows": total,
        "u1": u1,
        "u0": total - u1,
        "generators": len(generators),
        "blocks": len(blocks),
        "order_pairs": len(pairs),
        "status_rows": len(srows),
        "capped_fuels": len(fuels),
        "repairs": stats,
        "residual": residual,
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="gtopt_build_seed",
        description=(
            "Densify a solved reduced case's Commitment/status_sol.parquet "
            "into a complete, commitment-feasible (generator_uid, block_uid, "
            "u) seed CSV for monolithic_options.mip_start.seed_solution_file."
        ),
        epilog="Exit codes: 0 ok, 1 residual commitment violations remain.",
    )
    ap.add_argument("out_dir", type=Path, metavar="REDUCED_OUT_DIR")
    ap.add_argument("mip_json", type=Path, metavar="MIP_JSON")
    ap.add_argument("seed_csv", type=Path, metavar="OUT_SEED_CSV")
    ap.add_argument(
        "--no-repair",
        action="store_true",
        help="write the raw densified seed without commitment repair "
        "(A/B experiments; residual violations are still counted)",
    )
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO)
    summary = build_full_seed(
        args.out_dir, args.mip_json, args.seed_csv, repair=not args.no_repair
    )
    print(
        f"seed {args.seed_csv.name}: rows={summary['rows']} "
        f"u1={summary['u1']} u0={summary['u0']} "
        f"({summary['generators']} generators × {summary['blocks']} blocks) "
        f"order_pairs={summary['order_pairs']} "
        f"status_rows={summary['status_rows']} repairs={summary['repairs']} "
        f"residual_violations={summary['residual']}"
    )
    return 0 if summary["residual"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
