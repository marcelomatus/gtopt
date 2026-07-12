#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract the reduce-curve quality KPIs from gtopt output directories.

Six KPIs per solved point (see ``KPIS`` below), comparing each reduced
transport point against the full Kirchhoff+losses reference:

  1. obj_gross   — gross objective [$] (Σ obj_value over scene,phase).
  2. opex        — operating cost [$] = Σ of every ``*_cost.parquet``
                   (generation/fuel/vom/startup/shutdown/status, demand
                   fail, reserve shortage, flow costs).
  3. fcf         — future cost / terminal water value [$] = obj_gross −
                   opex (the boundary-cut residual).  Reported as
                   fcf' = fcf − c, subtracting a constant baseline c so
                   the (dominant) terminal-value level does not swamp the
                   comparison; c defaults to the reference point's fcf
                   (so fcf' measures each level's terminal-value error),
                   or an explicit --c-const (e.g. the boundary-cut rhs).
  4. lmp         — bus price [$/MWh] from Bus/balance_dual: the
                   demand-weighted mean plus the min/max over buses
                   (per-bus time-mean), so the spread min..max shows how
                   much locational (congestion) signal each level keeps.
  5. water_value — mean reservoir water value [$/hm³] from
                   Reservoir/water_value_dual (energy-weighted).
  6. dispatch_err — dispatch/commitment fidelity: fraction of total
                   generation energy re-allocated vs the reference,
                   Σ|E_g − E_g^ref| / (2 Σ E_g^ref); plus unserved energy
                   and spill as health checks.

Run:  python reduce_curve_kpis.py [--work ~/tmp/reduce_curve] [--d8 20251019]
Reads every ``<work>/PLEXOS<d8>/output_<tag>/`` dir; writes a table + a
kpis JSON next to the curve report.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import pandas as pd


def _ticks(case_dir: Path, tag: str) -> float | None:
    """Deterministic CPLEX ticks from the point's solve log (contention-proof)."""
    log = case_dir / f"solve_{tag}.log"
    if not log.exists():
        return None
    import re

    total = 0.0
    seen = False
    for line in log.read_text(errors="ignore").splitlines():
        m = re.search(r"GTOPT_SOLVE_EFFORT.*ticks=([\d.eE+]+)", line)
        if m:
            total += float(m.group(1))
            seen = True
    return total if seen else None


def _obj_gross(out: Path) -> float | None:
    csv = out / "solution.csv"
    if not csv.exists():
        return None
    df = pd.read_csv(csv)
    return float(df["obj_value"].sum()) if "obj_value" in df else None


def _sum_all_costs(out: Path) -> float:
    """Σ of every ``*_cost.parquet`` = total operating-cost contribution."""
    total = 0.0
    for p in out.rglob("*_cost.parquet"):
        try:
            total += float(pd.read_parquet(p, columns=["value"])["value"].sum())
        except (KeyError, OSError):
            continue
    return total


def _block_durations(base_json: Path) -> dict[int, float]:
    sim = json.loads(base_json.read_text())["simulation"]
    return {int(b["uid"]): float(b.get("duration", 1.0)) for b in sim["block_array"]}


def _load_long(path: Path) -> pd.DataFrame | None:
    if not path.exists():
        return None
    try:
        return pd.read_parquet(path)
    except OSError:
        return None


def _lmp(
    out: Path, dur: dict[int, float]
) -> tuple[float | None, float | None, float | None]:
    """Demand-weighted mean bus price + min/max over buses [$/MWh]."""
    duals = _load_long(out / "Bus" / "balance_dual.parquet")
    load = _load_long(out / "Demand" / "load_sol.parquet")
    if duals is None or duals.empty:
        return None, None, None
    duals = duals.copy()
    duals["w"] = duals["block"].map(dur).fillna(1.0)
    if load is not None and not load.empty:
        # Weight each (block) price by that block's served load × duration.
        load = load.copy()
        load["w"] = load["block"].map(dur).fillna(1.0)
        blk_load = load.groupby("block").apply(
            lambda g: (g["value"] * g["w"]).sum(), include_groups=False
        )
        duals["lw"] = duals["block"].map(blk_load).fillna(0.0) + 1e-9
        mean = float((duals["value"] * duals["lw"]).sum() / duals["lw"].sum())
    else:
        mean = float((duals["value"] * duals["w"]).sum() / duals["w"].sum())
    # Per-bus time-mean, then min/max across buses (locational spread).
    per_bus = duals.groupby("uid").apply(
        lambda g: (g["value"] * g["w"]).sum() / g["w"].sum(), include_groups=False
    )
    return mean, float(per_bus.min()), float(per_bus.max())


def _water_value(out: Path, dur: dict[int, float]) -> float | None:
    wv = _load_long(out / "Reservoir" / "water_value_dual.parquet")
    if wv is None or wv.empty:
        return None
    wv = wv.copy()
    wv["w"] = wv["block"].map(dur).fillna(1.0)
    return float((wv["value"] * wv["w"]).sum() / wv["w"].sum())


def _gen_energy(out: Path, dur: dict[int, float]) -> pd.Series | None:
    g = _load_long(out / "Generator" / "generation_sol.parquet")
    if g is None or g.empty:
        return None
    g = g.copy()
    g["e"] = g["value"] * g["block"].map(dur).fillna(1.0)
    return g.groupby("uid")["e"].sum()


def _unserved_spill(out: Path, dur: dict[int, float]) -> tuple[float, float]:
    fail = _load_long(out / "Demand" / "fail_sol.parquet")
    unserved = 0.0
    if fail is not None and not fail.empty:
        fail = fail.copy()
        unserved = float((fail["value"] * fail["block"].map(dur).fillna(1.0)).sum())
    spill = 0.0
    sp = _load_long(out / "Reservoir" / "spill_sol.parquet")
    if sp is not None and not sp.empty:
        sp = sp.copy()
        spill = float((sp["value"] * sp["block"].map(dur).fillna(1.0)).sum())
    return unserved, spill


def extract(out: Path, dur: dict[int, float], ref_gen: pd.Series | None) -> dict:
    obj = _obj_gross(out)
    opex = _sum_all_costs(out)
    lmp_mean, lmp_min, lmp_max = _lmp(out, dur)
    wv = _water_value(out, dur)
    gen = _gen_energy(out, dur)
    unserved, spill = _unserved_spill(out, dur)
    disp_err = None
    if gen is not None and ref_gen is not None:
        aligned = gen.reindex(ref_gen.index).fillna(0.0)
        denom = 2.0 * ref_gen.abs().sum()
        disp_err = float((aligned - ref_gen).abs().sum() / denom) if denom else None
    return {
        "obj_gross": obj,
        "opex": opex,
        "fcf": (obj - opex) if obj is not None else None,
        "lmp_mean": lmp_mean,
        "lmp_min": lmp_min,
        "lmp_max": lmp_max,
        "water_value_mean": wv,
        "dispatch_err_frac": disp_err,
        "unserved_mwh": unserved,
        "spill_hm3": spill,
        "total_gen_gwh": (float(gen.sum()) / 1e3) if gen is not None else None,
    }


def _pct(v: float | None, ref: float | None) -> str:
    if v is None or ref is None or ref == 0:
        return "-"
    return f"{100.0 * (v - ref) / abs(ref):+.2f}%"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "reduce_curve"))
    ap.add_argument("--d8", default="20251019")
    ap.add_argument(
        "--c-const",
        type=float,
        default=None,
        help="baseline c subtracted from fcf to report fcf'=fcf-c "
        "(default: the reference point's fcf)",
    )
    args = ap.parse_args()

    case_dir = Path(args.work) / f"PLEXOS{args.d8}"
    base_json = next(
        p
        for p in case_dir.glob(f"DATOS{args.d8}.json")
        if not p.name.endswith(".provenance.json")
    )
    dur = _block_durations(base_json)

    # Solve time / size from the driver report (keyed by tag).
    meta: dict[str, dict] = {}
    report = case_dir.parent / f"reduce_curve_{args.d8}.json"
    if report.exists():
        for p in json.loads(report.read_text()).get("points", []):
            if p.get("tag"):
                meta[p["tag"]] = p

    out_dirs = sorted(case_dir.glob("output_*"))
    ref_dir = case_dir / "output_reference"
    ref_gen = _gen_energy(ref_dir, dur) if ref_dir.exists() else None

    rows: dict[str, dict] = {}
    for out in out_dirs:
        tag = out.name.replace("output_", "")
        if not (out / "solution.csv").exists():
            continue
        rows[tag] = extract(out, dur, ref_gen if tag != "reference" else None)
        m = meta.get(tag, {})
        rows[tag]["solve_secs"] = m.get("solve_secs")
        rows[tag]["buses"] = m.get("buses")
        rows[tag]["lines"] = m.get("lines")
        rows[tag]["ticks"] = _ticks(case_dir, tag)

    ref = rows.get("reference", {})
    c = args.c_const if args.c_const is not None else ref.get("fcf")
    for r in rows.values():
        r["fcf_prime"] = (
            (r["fcf"] - c) if (r.get("fcf") is not None and c is not None) else None
        )
    (case_dir / "kpis.json").write_text(
        json.dumps({"c_const": c, "points": rows}, indent=2)
    )

    order = ["reference"] + sorted(
        (t for t in rows if t != "reference"),
        key=lambda t: int(t[1:]) if t[1:].isdigit() else 999,
    )
    cs = f"{c:.5g}" if c is not None else "n/a"
    ref_ticks = ref.get("ticks")
    print(f"\nc = {cs}  (fcf' = fcf − c);  speedup uses deterministic ticks")
    print(
        f"\n{'point':9s} {'buses':>5s} {'lines':>5s} {'ticks':>8s} {'tick_spd':>8s} "
        f"{'secs':>5s} {'Δobj':>8s} {'opex':>12s} {'fcf_prime':>12s} "
        f"{'lmp_min':>8s} {'lmp_mean':>8s} {'lmp_max':>8s} "
        f"{'wval':>8s} {'disp_err':>8s} {'unsv_GWh':>9s}"
    )
    for tag in order:
        if tag not in rows:
            continue
        r = rows[tag]
        tk = r.get("ticks")
        spd = f"{ref_ticks / tk:.2f}x" if (ref_ticks and tk) else "-"
        print(
            f"{tag:9s} "
            f"{(r.get('buses') or 0):5d} {(r.get('lines') or 0):5d} "
            f"{(tk if tk is not None else float('nan')):8.0f} {spd:>8s} "
            f"{(r.get('solve_secs') if r.get('solve_secs') is not None else float('nan')):5.0f} "
            f"{_pct(r['obj_gross'], ref.get('obj_gross')):>8s} "
            f"{(r['opex'] or float('nan')):12.5g} "
            f"{(r['fcf_prime'] if r['fcf_prime'] is not None else float('nan')):12.5g} "
            f"{(r['lmp_min'] or float('nan')):8.2f} "
            f"{(r['lmp_mean'] or float('nan')):8.2f} "
            f"{(r['lmp_max'] or float('nan')):8.2f} "
            f"{(r['water_value_mean'] or float('nan')):8.4g} "
            f"{(r['dispatch_err_frac'] or float('nan')):8.4f} "
            f"{((r['unserved_mwh'] or 0) / 1e3):9.3f}"
        )
    print(f"\nkpis: {case_dir / 'kpis.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
