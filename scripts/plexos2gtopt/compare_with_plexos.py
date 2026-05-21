#!/usr/bin/env python3
"""Compare a gtopt CEN PCP run against the PLEXOS reference solution.

Step 1 (this module) — **cost totals only**, the cheapest sanity
check.  Higher-fidelity per-element diffs (generator dispatch, bus LMP,
line flow, storage SoC) are deferred to Step 2+ once Step 1 confirms
the two runs are within a sane range.

Inputs:
  * ``--gtopt-case <dir>`` — a gtopt case directory containing
    ``output/solution.csv`` (one row per ``(scene, phase)`` with the
    ``obj_value`` column).
  * ``--plexos-log <path>`` — the PLEXOS solver log
    (``Model ( <name> ) Log.txt``) shipped inside the RES bundle's
    nested ``Solution.zip``.  Carries the ``Best Integer Solution`` /
    ``Best Bound`` lines this scout pattern-matches against.

For the CEN PCP RES bundle the log lives at
``RES{date}.zip.xz → res.zip → Model {model} Solution/Model {model}
Solution.zip → Model ( {model} ) Log.txt``.  Pass
``--plexos-res-zip <path>`` to have this scout do the two-level unzip
in a temp dir and find the log automatically.

Output: a single Rich table with both objective values, the absolute
and relative deltas, and a one-line interpretation hint.  Exit 0
always (this is a diagnostic, not a gate).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
import zipfile
from pathlib import Path

# Rich is already a project dependency (see scripts/pyproject.toml).
from rich.console import Console
from rich.table import Table


# Matches PLEXOS log lines like
#   ``    Best Integer Solution:.....         2.8130634809e+007``
# and the matching ``Best Bound`` / ``Linear Relaxation`` lines.  The
# cost value is the last whitespace-separated token on the line.
# PLEXOS indents these report lines by 4 spaces, so we match anywhere
# in the line (no ``^`` anchor).
_LOG_LINE_RE = re.compile(
    r"(?P<label>Best (?:Integer Solution|Bound|Relaxation)):.*?(?P<value>\S+)\s*$",
    re.MULTILINE,
)


def parse_plexos_log(log_path: Path) -> dict[str, float]:
    """Pull ``Best Integer Solution`` / ``Best Bound`` / ``Linear Relaxation``
    objective values from a PLEXOS solver log.

    Returns a dict keyed by the label (e.g. ``"Best Integer Solution"``).
    Values that PLEXOS prints as ``N/A`` (no MIP relaxation, infeasible,
    etc.) are silently skipped — only numeric rows are returned.
    """
    text = log_path.read_text(encoding="utf-8", errors="replace")
    out: dict[str, float] = {}
    for m in _LOG_LINE_RE.finditer(text):
        try:
            out[m.group("label")] = float(m.group("value"))
        except ValueError:
            # ``N/A`` and friends fall through here — drop silently.
            continue
    return out


def find_plexos_log_in_res_bundle(res_zip: Path) -> Path:
    """Extract the nested log file from a CEN PCP RES bundle.

    The CEN bundle nests one solution zip inside another (and the outer
    is sometimes .zip.xz).  Returns the path to the extracted log file
    inside a ``tempfile.mkdtemp`` so the caller can read it.  No cleanup
    here — diagnostic scout, the temp dir is < 100 MB and lives in
    ``/tmp``.
    """
    # If the input is .zip.xz, decompress first.
    if res_zip.suffix == ".xz":
        import lzma

        scratch = Path(tempfile.mkdtemp(prefix="plexos_res_"))
        plain_zip = scratch / res_zip.with_suffix("").name
        with lzma.open(res_zip, "rb") as src, plain_zip.open("wb") as dst:
            dst.write(src.read())
        res_zip = plain_zip

    scratch = Path(tempfile.mkdtemp(prefix="plexos_res_inner_"))
    with zipfile.ZipFile(res_zip) as outer:
        # The CEN naming convention: ``Model <name> Solution/<...>.zip``
        # plus ``...Log.txt`` directly.  Look for the inner zip first.
        nested = next(
            (n for n in outer.namelist() if n.endswith("Solution.zip")),
            None,
        )
        if nested is None:
            # Single-zip case: the log is directly inside.
            log_name = next(
                (n for n in outer.namelist() if n.endswith("Log.txt")),
                None,
            )
            if log_name is None:
                raise FileNotFoundError(f"no Log.txt found in {res_zip}")
            outer.extract(log_name, scratch)
            return scratch / log_name

        # Two-zip case (the CEN PCP daily bundles): extract inner zip,
        # then pull the Log.txt out of it.
        outer.extract(nested, scratch)
        with zipfile.ZipFile(scratch / nested) as inner:
            log_name = next(
                (n for n in inner.namelist() if n.endswith("Log.txt")),
                None,
            )
            if log_name is None:
                raise FileNotFoundError(f"no Log.txt found inside {nested}")
            inner.extract(log_name, scratch)
            return scratch / log_name


def parse_gtopt_solution(case_dir: Path) -> dict[str, float]:
    """Read ``<case>/output/solution.csv`` and return summary objectives.

    The gtopt solution file has one row per ``(scene, phase)`` cell.
    For a single-scene single-phase run this collapses to one number.
    Returns ``{"sum_obj": ..., "max_obj": ..., "rows": ...}``.

    A missing solution.csv raises ``FileNotFoundError`` — let the
    caller decide whether to skip silently or fail.
    """
    sol_path = case_dir / "output" / "solution.csv"
    if not sol_path.exists():
        raise FileNotFoundError(
            f"no gtopt solution at {sol_path} — did the run complete?"
        )

    # Hand-rolled CSV read so this module has zero hard deps beyond
    # rich (which is already in pyproject).  Pandas would be cleaner
    # but pulls a lot in for one column.
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    for line in sol_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        cells = [c.strip() for c in line.split(",")]
        if header is None:
            header = cells
            continue
        rows.append(dict(zip(header, cells, strict=False)))

    if not rows:
        raise ValueError(f"{sol_path}: no data rows")
    if "obj_value" not in (header or []):
        raise ValueError(f"{sol_path}: missing 'obj_value' column (have: {header})")

    obj_values = [float(r["obj_value"]) for r in rows]
    return {
        "sum_obj": sum(obj_values),
        "max_obj": max(obj_values),
        "rows": float(len(obj_values)),
    }


def _format_money(value: float) -> str:
    """Money column format: $X,XXX,XXX.XX with comma separators."""
    return f"${value:,.2f}"


# ------------------------------------------------------------------
# Step 3 — Per-solution comparison (demand, generation, op cost)
# ------------------------------------------------------------------
#
# Both PLEXOS .accdb and gtopt parquet store per-block MW (instantaneous)
# values, NOT per-block MWh.  Each block has a variable duration (1..8 h
# on CEN PCP daily-week; mapped to PLEXOS's t_phase_3 layout).  True
# energy in MWh = sum_{block}(MW_block × duration_block_h).  Without
# duration weighting any comparison is off by the factor (avg-block-
# duration).  This is the calculation the Step 1 cost-totals
# section pointedly does NOT do, and the user-frustrating root cause
# of the apparent 40-50% demand-and-gen mismatch.
#
# PLEXOS bookkeeping (verified via t_unit + t_property on
# CEN PCP DATOS20260422):
#   prop 966 / 970  Region Load / Generation      unit = MW (interval)
#   prop 2          Generator-class Generation    unit = MW (interval)
#   prop 119        Generator Generation Cost     unit = $  (block-$,
#                                                  NOT $/h — already
#                                                  block-integrated;
#                                                  do NOT × duration)


# ============================================================
# PLEXOS solution-database property IDs (t_property.property_id)
# ------------------------------------------------------------
# Centralised so the comparison code self-documents and the
# Region.Load vs Node.Load scoping risk is impossible to repeat.
# Confirmed against CEN PCP DATOS20260422 (see
# ~/.claude/.../memory/feedback_plexos_demand_scope.md).
#
# Collection-2 (Generator class) primary props:
PLEXOS_PROP_GENERATOR_GENERATION = 2  # MW per period
PLEXOS_PROP_GENERATOR_GENCOST = 119  # $ per block (already integrated)
PLEXOS_PROP_GENERATOR_SRMC = 137  # $/MWh per period
#
# Collection-80 (Battery class) primary props:
PLEXOS_PROP_BATTERY_LOAD = 521  # MW charging
PLEXOS_PROP_BATTERY_CHARGING = 523  # MW (named "Charging")
PLEXOS_PROP_BATTERY_DISCHARGING = 524  # MW (named "Discharging")
#
# Collection-80 (Storage) volume props:
PLEXOS_PROP_STORAGE_SOC = 518  # GWh / m³ per period
PLEXOS_PROP_STORAGE_INITIAL_VOLUME = 645
PLEXOS_PROP_STORAGE_END_VOLUME = 646
#
# Collection-200 (Region) — system-level aggregates:
PLEXOS_PROP_REGION_LOAD = 966  # GROSS demand: consumer + batt + losses
PLEXOS_PROP_REGION_GENERATION = 970  # gross injection (matches Region.Load)
PLEXOS_PROP_REGION_LOSSES = 997  # transmission losses (MW)
#
# Collection-281 (Node) — per-node aggregates:
PLEXOS_PROP_NODE_LOAD = 1373  # consumer demand only (apples-to-apples
#                              vs gtopt's demand_array)
PLEXOS_PROP_NODE_GENERATION = 1377
PLEXOS_PROP_NODE_PRICE = 1412  # LMP $/MWh
#
# Collection-303 (Line / transmission):
PLEXOS_PROP_LINE_FLOW = 1462  # MW
PLEXOS_PROP_LINE_EXPORT_LIMIT = 1465  # MW positive-direction cap
PLEXOS_PROP_LINE_IMPORT_LIMIT = 1466  # MW negative-direction cap
# ============================================================


def _read_plexos_table(
    accdb_path: Path,
    table: str,
    *,
    case_dir: Path | None = None,
) -> str:
    """Return the CSV text for ``table`` from a PLEXOS .accdb.

    Tries three sources in order:
      1. ``case_dir / "plexos_cache" / "<table>.csv.zst"`` — the
         pre-extracted cache plexos2gtopt drops during conversion.
         Decompressed via the ``zstd -d`` CLI (avoids hard
         python-zstandard dep).
      2. ``<accdb_dir> / "plexos_cache" / "<table>.csv.zst"``
         when the .accdb itself is under a cached layout.
      3. Live ``mdb-export <accdb> <table>`` fallback — slow,
         used when the cache is absent or unreadable.

    Returns the raw CSV text (headers + rows) ready to pass into
    ``csv.DictReader(io.StringIO(...))``.
    """
    import subprocess

    candidates: list[Path] = []
    if case_dir is not None:
        candidates.append(case_dir / "plexos_cache" / f"{table}.csv.zst")
    if accdb_path is not None:
        candidates.append(accdb_path.parent / "plexos_cache" / f"{table}.csv.zst")

    for cache in candidates:
        if not cache.exists():
            continue
        try:
            proc = subprocess.run(
                ["zstd", "-d", "-q", "-c", str(cache)],
                capture_output=True,
                check=True,
                text=True,
                timeout=60,
            )
            return proc.stdout
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
            # Cache file is corrupt or zstd unavailable — fall through
            # to the live extraction below.
            continue

    # Live fallback.
    return subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)


def _plexos_block_durations_from_accdb(
    accdb_path: Path,
) -> dict[int, int]:
    """Return ``{period_id → duration_h}`` from ``t_phase_3``.

    Each row of ``t_phase_3`` says "calendar hour ``interval_id``
    belongs to LP block ``period_id``".  Counting rows per period_id
    gives the block duration in hours.

    Returns ``{}`` when ``mdb-export`` is unavailable or ``t_phase_3``
    is missing — caller should fall back to assuming 1h blocks.
    """
    import subprocess

    try:
        result = subprocess.run(
            ["mdb-export", str(accdb_path), "t_phase_3"],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        # mdb-export missing or accdb unreadable.
        raise RuntimeError(f"cannot read t_phase_3 from {accdb_path}: {exc}") from exc

    import csv
    import io
    import collections

    counts: dict[int, int] = collections.Counter()
    reader = csv.DictReader(io.StringIO(result.stdout))
    for row in reader:
        # PLEXOS calls them period_id in this table; one row per
        # (period_id, interval_id) pair.
        try:
            counts[int(row["period_id"])] += 1
        except (KeyError, ValueError):
            continue
    return dict(counts)


def _extract_accdb_from_res_zip(res_zip: Path, scratch: Path) -> Path:
    """Unzip the outer + inner RES so we get the .accdb file.

    Returns the resolved path to the unpacked .accdb.  Same two-level
    unzip pattern as :func:`find_plexos_log_in_res_bundle`.
    """
    import lzma

    outer_zip = res_zip
    if res_zip.suffix == ".xz":
        outer_zip = scratch / res_zip.with_suffix("").name
        with lzma.open(res_zip, "rb") as fin, outer_zip.open("wb") as fout:
            fout.write(fin.read())

    with zipfile.ZipFile(outer_zip) as outer:
        nested = next(
            (n for n in outer.namelist() if n.endswith(".accdb")),
            None,
        )
        if nested is None:
            raise FileNotFoundError(f"no .accdb inside {outer_zip}")
        outer.extract(nested, scratch)
        return scratch / nested


def compute_plexos_energy_totals(
    accdb_path: Path,
) -> dict[str, float]:
    """Read PLEXOS Region.Load / Region.Generation / Generator.GenCost
    and return duration-weighted system totals.

    Returns ``{}`` if the .accdb can't be read.
    """
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}

    def _dump(table: str) -> list[dict[str, str]]:
        import csv
        import io

        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")

    # property_id → set of key_id
    key_by_prop: dict[int, set[int]] = {}
    for r in keys:
        try:
            pid = int(r["property_id"])
            kid = int(r["key_id"])
        except ValueError:
            continue
        key_by_prop.setdefault(pid, set()).add(kid)

    def _energy_mwh(prop_id: int) -> float:
        kset = key_by_prop.get(prop_id, set())
        total = 0.0
        for r in data0:
            try:
                kid = int(r["key_id"])
            except ValueError:
                continue
            if kid not in kset:
                continue
            try:
                period = int(r["period_id"])
                value = float(r["value"])
            except (KeyError, ValueError):
                continue
            total += value * durations.get(period, 0)
        return total

    def _block_sum(prop_id: int) -> float:
        """Unweighted per-block sum.  Use for already-integrated $
        properties like prop 119 (Generation Cost)."""
        kset = key_by_prop.get(prop_id, set())
        return sum(
            float(r["value"])
            for r in data0
            if r.get("key_id", "").isdigit() and int(r["key_id"]) in kset
        )

    # Demand scope:  ``load_mwh`` is PLEXOS prop 1373 Node.Load —
    # consumer demand only — to make an apples-to-apples comparison
    # with gtopt's ``demand_array`` (which is also consumer-only;
    # batteries live in a separate Battery object, losses are zero
    # under DC OPF).  Region.Load (prop 966) is the GROSS load
    # including battery charging + transmission losses — comparing
    # gtopt against that shows a phantom 7-8% deficit that is a
    # definitional artifact, not a data error.  See
    # ~/.claude/.../memory/feedback_plexos_demand_scope.md.
    return {
        "load_mwh": _energy_mwh(PLEXOS_PROP_NODE_LOAD),
        "load_region_mwh": _energy_mwh(PLEXOS_PROP_REGION_LOAD),
        "battery_load_mwh": _energy_mwh(PLEXOS_PROP_BATTERY_LOAD),
        "losses_mwh": _energy_mwh(PLEXOS_PROP_REGION_LOSSES),
        "gen_mwh": _energy_mwh(PLEXOS_PROP_REGION_GENERATION),
        "gen_unit_mwh": _energy_mwh(PLEXOS_PROP_GENERATOR_GENERATION),
        "gen_cost_usd": _block_sum(PLEXOS_PROP_GENERATOR_GENCOST),
        "block_count": len(durations),
        "hours_covered": sum(durations.values()),
    }


def _sum_consumer_demand_field_mwh(
    case_dir: Path,
    bundle: dict,
    durations: dict[int, float],
    parquet_rel: str,
) -> float:
    """Sum a ``Demand/<field>_sol`` parquet restricted to **real
    consumer demands** (uids ≤ max input demand uid).

    Filters out the synthetic ``<bat>_dem`` Demand objects that
    gtopt's ``expand_batteries`` (C++ ``system.cpp``) appends to the
    demand_array at LP-build time.  Used for both ``load_sol``
    (consumer served energy) and ``fail_sol`` (consumer unserved
    energy) — without the filter both rows double-count the
    battery charging path (the synthetic demand's load_sol equals
    the battery's finp_sol).
    """
    import pyarrow.parquet as pq

    f = case_dir / "output" / parquet_rel
    if not f.exists():
        return 0.0
    consumer_uids = {int(d["uid"]) for d in bundle["system"].get("demand_array", [])}
    if not consumer_uids:
        return 0.0
    df = pq.read_table(f).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations).fillna(0.0)
    mask = df["uid"].astype(int).isin(consumer_uids)
    return float((df.loc[mask, "value"] * df.loc[mask, "duration"]).sum())


def _sum_consumer_fail_mwh(
    case_dir: Path,
    bundle: dict,
    durations: dict[int, float],
) -> float:
    """Sum ``Demand/fail_sol`` MWh restricted to **real consumer demands**.

    gtopt's ``expand_batteries`` (C++ ``system.cpp``) auto-instantiates
    a synthetic ``<bat>_dem`` Demand per Battery, with
    ``fcost = 0`` by design — the LP is intentionally free to leave
    those demands unserved (i.e. the battery simply doesn't charge
    that block).  The fail_sol parquet pools real consumer fail AND
    those synthetic battery fails together; the synthetics
    overwhelm the real numbers (~7.5 GWh on CEN PCP) and turn the
    unserved-MWh metric into noise.

    Filter by uid range: bundle's input ``demand_array`` covers
    uids 1…N (N=127 on CEN PCP).  Synthetic ``<bat>_dem`` entries
    are appended by gtopt at uid > N.  Sum only fail rows where
    uid ≤ N.
    """
    import pyarrow.parquet as pq

    f = case_dir / "output" / "Demand" / "fail_sol.parquet"
    if not f.exists():
        return 0.0
    consumer_uids = {int(d["uid"]) for d in bundle["system"].get("demand_array", [])}
    if not consumer_uids:
        return 0.0
    df = pq.read_table(f).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations).fillna(0.0)
    mask = df["uid"].astype(int).isin(consumer_uids)
    return float((df.loc[mask, "value"] * df.loc[mask, "duration"]).sum())


def compute_gtopt_energy_totals(case_dir: Path) -> dict[str, float]:
    """Sum gtopt's per-block MW dispatch into MWh using bundle's
    block durations.  Reads parquet directly via pyarrow (already a
    transitive dep of gtopt).
    """
    import json

    import pyarrow.parquet as pq

    # 1. Bundle JSON → block uid → duration_h.  Bundle filename
    #    convention is either ``plexos_bundle.json`` (legacy) or
    #    ``<stem>.json`` (current plexos2gtopt output).  Look for
    #    either.
    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue  # gtopt's writer copies; not the source bundle
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(
            f"no PLEXOS-source JSON found in {case_dir} "
            "(looked for *.json, ignored planning.json)"
        )

    bundle = json.loads(bundle_path.read_text())
    block_array = bundle["simulation"]["block_array"]
    durations = {int(b["uid"]): float(b.get("duration", 1.0)) for b in block_array}

    def _sum_mwh(rel_path: str) -> float:
        # gtopt writes parquet as a **partitioned directory**
        # (``scene=0/phase=0/...``), so ``f`` is a directory not a
        # file.  ``pq.read_table`` handles both layouts transparently.
        f = case_dir / "output" / rel_path
        if not f.exists():
            return 0.0
        tbl = pq.read_table(f, columns=["block", "value"])
        df = tbl.to_pandas()
        df["duration"] = df["block"].astype(int).map(durations)
        return float((df["value"] * df["duration"]).sum())

    # Operational cost computed from per-unit dispatch and per-unit
    # SRMC (short-run marginal cost in $/MWh):
    #   op_cost = sum_{u, b} (gen[u,b] MW) × (srmc[u,b] $/MWh) × dur[b] h
    # Both files have key columns (scene, phase, scenario, stage,
    # block, uid).  Inner-join keeps only (unit, block) pairs that
    # appear in both — i.e. units with non-zero dispatch and a known
    # marginal cost.
    op_cost = 0.0
    gen_path = case_dir / "output" / "Generator" / "generation_sol.parquet"
    srmc_path = case_dir / "output" / "Generator" / "srmc_sol.parquet"
    if gen_path.exists() and srmc_path.exists():
        key = ["scene", "phase", "scenario", "stage", "block", "uid"]
        gen_df = pq.read_table(gen_path).to_pandas()
        srmc_df = pq.read_table(srmc_path).to_pandas()
        merged = gen_df.merge(srmc_df, on=key, suffixes=("_mw", "_srmc"))
        merged["duration"] = merged["block"].astype(int).map(durations)
        op_cost = float(
            (merged["value_mw"] * merged["value_srmc"] * merged["duration"]).sum()
        )

    gen_total = _sum_mwh("Generator/generation_sol.parquet")
    # ``Demand/load_sol.parquet`` pools real-consumer served load AND
    # the synthetic ``<bat>_dem`` load (= battery charging, double-
    # counted with Battery/finp_sol).  Filter out the synthetic
    # rows so the consumer-demand metric is apples-to-apples with
    # PLEXOS Node.Load.
    load_total = _sum_consumer_demand_field_mwh(
        case_dir, bundle, durations, "Demand/load_sol.parquet"
    )
    # ``Demand/fail_sol.parquet`` aggregates BOTH real-consumer
    # demand failure AND the synthetic ``<bat>_dem`` Demand objects
    # gtopt's ``expand_batteries`` auto-instantiates (uid > max
    # bundle demand uid).  Those synthetic demands have ``fcost=0``
    # by design — the LP is FREE to leave them unserved (i.e. the
    # battery just doesn't charge), so their "fail" is normal
    # behaviour, not a demand-shortage signal.  Filter them out
    # so the unserved-MWh metric reflects only real consumer
    # demand failure.
    fail_total = _sum_consumer_demand_field_mwh(
        case_dir, bundle, durations, "Demand/fail_sol.parquet"
    )
    batt_charge = _sum_mwh("Battery/finp_sol.parquet")
    batt_discharge = _sum_mwh("Battery/fout_sol.parquet")
    # Implicit transmission losses: gtopt doesn't write a per-line
    # loss parquet — losses are baked into the bus-balance equation
    # via the piecewise-linear segment columns.  Recover by
    # energy balance:
    #   Generation + battery_discharge = load_served + battery_charge
    #     + losses
    # → losses = Gen + Disch − Load − Charge.  Floor at 0 to absorb
    # the rare per-block rounding artifact that would otherwise show
    # a tiny negative.
    losses_total = max(0.0, gen_total + batt_discharge - load_total - batt_charge)
    return {
        "gen_mwh": gen_total,
        "load_mwh": load_total,
        "fail_mwh": fail_total,
        # BESS charging (counted on PLEXOS side as Region.Load - Node.Load
        # contribution, here pulled out as a separate row so the demand
        # comparison stays apples-to-apples on consumer load).
        "battery_charge_mwh": batt_charge,
        "battery_discharge_mwh": batt_discharge,
        # Implicit transmission losses (energy-balance residual).
        "losses_mwh": losses_total,
        "op_cost_usd": op_cost,
        "block_count": len(block_array),
        "hours_covered": sum(durations.values()),
    }


def compute_plexos_per_unit_srmc(
    accdb_path: Path,
) -> dict[str, tuple[float, float]]:
    """Per-unit dispatch-weighted average SRMC from PLEXOS .accdb.

    Returns ``{unit_name → (total_MWh, dispatch_weighted_avg_srmc_$/MWh)}``.

    Uses:
      * prop 2   "Generation"  (MW per unit per period — for weighting)
      * prop 137 "SRMC"        ($/MWh per unit per period)
      * t_phase_3 period→duration map

    Units that never dispatch (Σ MWh = 0) are dropped — their SRMC is
    undefined.
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")
    members = _dump("t_membership")
    objects = _dump("t_object")

    # Resolve (key_id → unit_name) by walking key.membership_id →
    # member.child_object_id → object.name.  Built once, used twice
    # (Generation + SRMC).
    obj_name = {int(o["object_id"]): o["name"] for o in objects}
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}
    key_obj: dict[int, int] = {}
    key_prop: dict[int, int] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if mid in mem_child:
            key_obj[kid] = mem_child[mid]
            key_prop[kid] = pid

    # Per-(unit_name, period_id) accumulation.
    by_unit_period: dict[str, dict[int, dict[str, float]]] = collections.defaultdict(
        lambda: collections.defaultdict(lambda: {"mw": 0.0, "srmc": 0.0})
    )
    for r in data0:
        try:
            kid = int(r["key_id"])
            pid_period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        prop = key_prop.get(kid)
        if prop not in (
            PLEXOS_PROP_GENERATOR_GENERATION,
            PLEXOS_PROP_GENERATOR_SRMC,
        ):
            continue
        oid = key_obj.get(kid)
        if oid is None:
            continue
        name = obj_name.get(oid)
        if name is None:
            continue
        slot = by_unit_period[name][pid_period]
        if prop == PLEXOS_PROP_GENERATOR_GENERATION:
            slot["mw"] = val
        elif prop == PLEXOS_PROP_GENERATOR_SRMC:
            slot["srmc"] = val

    out: dict[str, tuple[float, float]] = {}
    for name, periods in by_unit_period.items():
        total_mwh = 0.0
        weighted_srmc = 0.0
        for pid_period, v in periods.items():
            dur = durations.get(pid_period, 0)
            mwh = v["mw"] * dur
            total_mwh += mwh
            weighted_srmc += v["srmc"] * mwh
        if total_mwh > 0:
            out[name] = (total_mwh, weighted_srmc / total_mwh)
    return out


def compute_gtopt_per_unit_srmc(
    case_dir: Path,
) -> dict[str, tuple[float, float]]:
    """Per-unit dispatch-weighted average SRMC from gtopt parquet output.

    Returns ``{unit_name → (total_MWh, dispatch_weighted_avg_srmc)}``
    by joining ``Generator/generation_sol.parquet`` ×
    ``Generator/srmc_sol.parquet`` on the (scene, phase, scenario,
    stage, block, uid) key and weighting by per-block duration.
    """
    import json

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid = {
        int(g["uid"]): g["name"] for g in bundle["system"]["generator_array"]
    }

    gen_path = case_dir / "output" / "Generator" / "generation_sol.parquet"
    srmc_path = case_dir / "output" / "Generator" / "srmc_sol.parquet"
    if not (gen_path.exists() and srmc_path.exists()):
        return {}

    gen = pq.read_table(gen_path).to_pandas()
    srmc = pq.read_table(srmc_path).to_pandas()
    key_cols = ["scene", "phase", "scenario", "stage", "block", "uid"]
    merged = gen.merge(srmc, on=key_cols, suffixes=("_mw", "_srmc"))
    merged["duration"] = merged["block"].astype(int).map(durations)
    merged["mwh"] = merged["value_mw"] * merged["duration"]
    merged["name"] = merged["uid"].astype(int).map(name_by_uid)

    out: dict[str, tuple[float, float]] = {}
    grouped = merged.groupby("name", observed=True)
    for name, df in grouped:
        if name is None:
            continue
        total_mwh = df["mwh"].sum()
        if total_mwh <= 0:
            continue
        weighted_srmc = (df["value_srmc"] * df["mwh"]).sum() / total_mwh
        out[str(name)] = (float(total_mwh), float(weighted_srmc))
    return out


def _render_srmc_compare(
    plexos_unit: dict[str, tuple[float, float]],
    gtopt_unit: dict[str, tuple[float, float]],
    console: Console,
    *,
    top_n: int = 30,
    min_avg_mwh: float = 0.0,
) -> None:
    """Step 4 — per-unit SRMC + dispatch comparison.

    Three sub-tables:
      1. Units dispatched in BOTH, sorted by |SRMC delta|.
      2. Top PLEXOS-dispatched units that gtopt skips (dispatch
         mis-selection, not cost error).
      3. Top gtopt-dispatched units that PLEXOS skips (the
         "fake cheap" / under-constrained pickups).
    """
    all_names = set(plexos_unit) | set(gtopt_unit)
    rows = []
    for n in all_names:
        p_mwh, p_srmc = plexos_unit.get(n, (0.0, 0.0))
        g_mwh, g_srmc = gtopt_unit.get(n, (0.0, 0.0))
        rows.append((n, p_mwh, p_srmc, g_mwh, g_srmc))

    # (1) Both dispatched
    both = [
        (n, p_m, p_s, g_m, g_s) for n, p_m, p_s, g_m, g_s in rows if p_m > 1 and g_m > 1
    ]
    both.sort(key=lambda r: abs(r[4] - r[2]), reverse=True)
    both_filtered = [r for r in both if (r[1] + r[3]) / 2 >= min_avg_mwh]

    t1 = Table(
        title=(
            f"Per-unit SRMC comparison (Step 4) — top {top_n} co-dispatched "
            f"units by |Δ SRMC| (avg MWh ≥ {min_avg_mwh:g})"
        ),
    )
    t1.add_column("Unit", style="bold")
    t1.add_column("PLEXOS $/MWh", justify="right")
    t1.add_column("gtopt $/MWh", justify="right")
    t1.add_column("Δ $/MWh", justify="right")
    t1.add_column("Δ %", justify="right")
    t1.add_column("PLEXOS MWh", justify="right")
    t1.add_column("gtopt MWh", justify="right")
    for n, p_m, p_s, g_m, g_s in both_filtered[:top_n]:
        delta = g_s - p_s
        pct = (100.0 * delta / p_s) if p_s else 0.0
        t1.add_row(
            n,
            f"{p_s:>10.2f}",
            f"{g_s:>10.2f}",
            f"{delta:>+8.2f}",
            f"{pct:>+6.1f}%",
            f"{p_m:>10,.0f}",
            f"{g_m:>10,.0f}",
        )
    if len(both_filtered) > top_n:
        t1.caption = f"… and {len(both_filtered) - top_n} more co-dispatched units."
    console.print(t1)

    # (2) PLEXOS-only
    plexos_only = [(n, p_m, p_s) for n, p_m, p_s, g_m, _ in rows if p_m > 1 and g_m < 1]
    plexos_only.sort(key=lambda r: r[1], reverse=True)
    t2 = Table(
        title=(
            f"PLEXOS dispatches, gtopt does NOT — top {top_n} by MWh "
            f"({len(plexos_only)} units total)"
        ),
    )
    t2.add_column("Unit", style="bold")
    t2.add_column("PLEXOS MWh", justify="right")
    t2.add_column("PLEXOS $/MWh", justify="right")
    for n, p_m, p_s in plexos_only[:top_n]:
        t2.add_row(n, f"{p_m:>10,.0f}", f"{p_s:>10.2f}")
    console.print(t2)

    # (3) gtopt-only
    gtopt_only = [(n, g_m, g_s) for n, p_m, _, g_m, g_s in rows if g_m > 1 and p_m < 1]
    gtopt_only.sort(key=lambda r: r[1], reverse=True)
    t3 = Table(
        title=(
            f"gtopt dispatches, PLEXOS does NOT — top {top_n} by MWh "
            f"({len(gtopt_only)} units total)"
        ),
    )
    t3.add_column("Unit", style="bold")
    t3.add_column("gtopt MWh", justify="right")
    t3.add_column("gtopt $/MWh", justify="right")
    for n, g_m, g_s in gtopt_only[:top_n]:
        t3.add_row(n, f"{g_m:>10,.0f}", f"{g_s:>10.2f}")
    console.print(t3)
    console.print(
        "[dim]SRMC = dispatch-weighted average $/MWh per unit.  "
        "Matching values in column 1 vs 2 confirm gcost is published "
        "correctly; large dispatch differences (table 2/3) usually "
        "trace to missing constraints (fuel offtake caps, "
        "transmission, reserves) rather than wrong unit costs.[/dim]"
    )


def compute_plexos_per_bus_lmp(
    accdb_path: Path,
) -> dict[str, float]:
    """Per-bus time-weighted average LMP from PLEXOS .accdb.

    Returns ``{node_name → avg_lmp_$/MWh}`` using prop 1412 "Price"
    on the Node collection (collection_id=281), weighted by
    t_phase_3 block durations.
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}
    total_hours = sum(durations.values())

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")
    members = _dump("t_membership")
    objects = _dump("t_object")

    obj_name = {int(o["object_id"]): o["name"] for o in objects}
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}
    key_obj: dict[int, int] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if pid == PLEXOS_PROP_NODE_PRICE and mid in mem_child:
            key_obj[kid] = mem_child[mid]

    by_bus: dict[str, dict[int, float]] = collections.defaultdict(dict)
    for r in data0:
        try:
            kid = int(r["key_id"])
            pid_period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        oid = key_obj.get(kid)
        if oid is None:
            continue
        name = obj_name.get(oid)
        if name is None:
            continue
        by_bus[name][pid_period] = val

    out: dict[str, float] = {}
    for name, periods in by_bus.items():
        weighted = sum(v * durations.get(p, 0) for p, v in periods.items())
        if total_hours > 0:
            out[name] = weighted / total_hours
    return out


def compute_gtopt_per_bus_lmp(
    case_dir: Path,
) -> dict[str, float]:
    """Per-bus time-weighted average LMP from gtopt
    ``Bus/balance_dual.parquet``.

    Returns ``{bus_name → avg_lmp_$/MWh}`` averaged over the 168 h
    horizon (Σ value·duration / Σ duration).  Bus names come from
    the bundle JSON's bus_array.
    """
    import json

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid = {int(b["uid"]): b["name"] for b in bundle["system"]["bus_array"]}
    total_hours = sum(durations.values())

    lmp_path = case_dir / "output" / "Bus" / "balance_dual.parquet"
    if not lmp_path.exists():
        return {}
    df = pq.read_table(lmp_path).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations)
    df["name"] = df["uid"].astype(int).map(name_by_uid)

    out: dict[str, float] = {}
    for name, sub in df.groupby("name", observed=True):
        weighted = (sub["value"] * sub["duration"]).sum()
        if total_hours > 0:
            out[str(name)] = float(weighted / total_hours)
    return out


def _render_lmp_compare(
    plexos_bus: dict[str, float],
    gtopt_bus: dict[str, float],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 5 — per-bus LMP (locational marginal price) comparison.

    Two sub-tables:
      1. Top N buses by |Δ LMP| (in both PLEXOS and gtopt).
      2. Bus-coverage summary (counts of present-on-both, only-PLEXOS,
         only-gtopt).
    """
    common = set(plexos_bus) & set(gtopt_bus)
    plexos_only = set(plexos_bus) - common
    gtopt_only = set(gtopt_bus) - common

    rows = []
    for n in common:
        p = plexos_bus[n]
        g = gtopt_bus[n]
        rows.append((n, p, g, g - p))
    rows.sort(key=lambda r: abs(r[3]), reverse=True)

    t = Table(
        title=(
            f"Per-bus LMP comparison (Step 5) — top {top_n} buses "
            f"by |Δ LMP|  (common buses: {len(common)}, "
            f"PLEXOS-only: {len(plexos_only)}, gtopt-only: {len(gtopt_only)})"
        ),
    )
    t.add_column("Bus", style="bold")
    t.add_column("PLEXOS $/MWh", justify="right")
    t.add_column("gtopt $/MWh", justify="right")
    t.add_column("Δ $/MWh", justify="right")
    t.add_column("Δ %", justify="right")
    for name, p, g, d in rows[:top_n]:
        pct = (100.0 * d / p) if p else 0.0
        t.add_row(
            name,
            f"{p:>10.2f}",
            f"{g:>10.2f}",
            f"{d:>+8.2f}",
            f"{pct:>+6.1f}%",
        )
    console.print(t)

    # Summary line: mean and median diffs
    if rows:
        deltas = [r[3] for r in rows]
        import statistics

        console.print(
            f"[dim]LMP diff over {len(rows)} common buses: "
            f"mean Δ = {statistics.mean(deltas):+.2f} $/MWh, "
            f"median Δ = {statistics.median(deltas):+.2f} $/MWh, "
            f"mean |Δ| = {statistics.mean(abs(d) for d in deltas):.2f}, "
            f"P90 |Δ| = "
            f"{sorted(abs(d) for d in deltas)[int(0.9 * len(deltas))]:.2f}.  "
            f"LMP = time-weighted average ∫λ dt / Σ dt over the 168 h "
            f"horizon.[/dim]"
        )


def compute_plexos_per_line(
    accdb_path: Path,
) -> dict[str, dict[str, float]]:
    """Per-line **total absolute energy transferred** and limits from
    PLEXOS .accdb.

    Returns ``{line_name → {energy_mwh, max_flow, min_flow, active}}``:

      * ``energy_mwh`` = Σ |MW_block| × duration_block_h over all 111
        blocks.  Uses absolute value so positive- and negative-direction
        flows DON'T cancel — a line carrying ±500 MW alternating shows
        as ~84 GWh transferred over a 168-h week, not ~0.
      * ``max_flow`` / ``min_flow`` = time-weighted average of
        prop 709 / 710 (PLEXOS MW limits).
      * ``active`` = 1.0 if the line has any flow data on prop 708
        (energy > 0 OR limit set), 0.0 if PLEXOS didn't even
        ship a Flow series for the line — the closest signal we have
        for "in service this period" from the solution database.

    Uses PLEXOS Line collection (collection_id=303) properties:
       1462 Flow          (MW per line per period)
       1465 Export Limit  (MW positive-direction cap)
       1466 Import Limit  (MW negative-direction cap)
    Note: PLEXOS uses Import/Export Limit (signed by direction)
    while gtopt uses (tmax_ab, tmax_ba) — we map Export → max_flow
    and -Import → min_flow to match the gtopt sign convention.
    Collection 101 is Waterways (hydro), 303 is the transmission
    Line proper — picking 101 here returns waterway flows by
    mistake.
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}
    total_hours = sum(durations.values())

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")
    members = _dump("t_membership")
    objects = _dump("t_object")

    obj_name = {int(o["object_id"]): o["name"] for o in objects}
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}
    key_meta: dict[int, tuple[int, int]] = {}  # key_id -> (oid, pid)
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if (
            pid
            in (
                PLEXOS_PROP_LINE_FLOW,
                PLEXOS_PROP_LINE_EXPORT_LIMIT,
                PLEXOS_PROP_LINE_IMPORT_LIMIT,
            )
            and mid in mem_child
        ):
            key_meta[kid] = (mem_child[mid], pid)

    by_line: dict[str, dict[str, list[tuple[int, float]]]] = collections.defaultdict(
        lambda: collections.defaultdict(list)
    )
    pid_field = {
        PLEXOS_PROP_LINE_FLOW: "flow",
        PLEXOS_PROP_LINE_EXPORT_LIMIT: "max_flow",
        PLEXOS_PROP_LINE_IMPORT_LIMIT: "min_flow",
    }
    for r in data0:
        try:
            kid = int(r["key_id"])
            pid_period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        meta = key_meta.get(kid)
        if meta is None:
            continue
        oid, pid = meta
        name = obj_name.get(oid)
        if name is None:
            continue
        by_line[name][pid_field[pid]].append((pid_period, val))

    out: dict[str, dict[str, float]] = {}
    for name, fields in by_line.items():
        entry: dict[str, float] = {
            "energy_mwh": 0.0,
            "max_flow": 0.0,
            "min_flow": 0.0,
            "active": 0.0,
        }
        for fname, rows in fields.items():
            if fname == "flow":
                # Total absolute energy transferred (non-cancelling).
                entry["energy_mwh"] = sum(abs(v) * durations.get(p, 0) for p, v in rows)
                entry["active"] = 1.0 if rows else 0.0
            elif fname == "min_flow":
                # PLEXOS Import Limit is published as a positive MW
                # value (max amount in the negative direction); flip
                # sign so it matches gtopt's signed convention
                # min_flow = -fmax_ba.
                weighted = sum(v * durations.get(p, 0) for p, v in rows)
                if total_hours > 0:
                    entry[fname] = -weighted / total_hours
            else:
                # max_flow: time-weighted average in MW.
                weighted = sum(v * durations.get(p, 0) for p, v in rows)
                if total_hours > 0:
                    entry[fname] = weighted / total_hours
        out[name] = entry
    return out


def compute_gtopt_per_line(
    case_dir: Path,
) -> dict[str, dict[str, float]]:
    """Per-line total absolute energy + capacity + active-flag from
    gtopt parquet + bundle.

    Returns ``{line_name → {energy_mwh, max_flow, min_flow, active}}``:
      * ``energy_mwh`` = Σ |MW_block| × duration_block_h (non-cancelling
        across positive/negative flow directions).
      * ``max_flow`` =  fmax  (bundle JSON, time-mean of tmax_ab list /
                         scalar; positive direction)
      * ``min_flow`` = -fmax_ba (negative-direction limit, sign-flipped
                         to match PLEXOS Min Flow convention).
      * ``active`` = bundle's ``active`` field if present, else 1.0
        (lines with active=0/False are not in service this period).
    """
    import json

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid: dict[int, str] = {}
    limits: dict[str, dict[str, float]] = {}

    def _scalar_or_first(v: object) -> float:
        """tmax fields can be scalar, list, or nested list-of-list
        (per-block profile).  Return a representative scalar."""
        if isinstance(v, (int, float)):
            return float(v)
        if isinstance(v, list):
            # Flatten if nested
            flat: list[float] = []
            stack = [v]
            while stack:
                cur = stack.pop()
                for x in cur:
                    if isinstance(x, list):
                        stack.append(x)
                    else:
                        flat.append(float(x))
            return sum(flat) / len(flat) if flat else 0.0
        return 0.0

    active_by_name: dict[str, float] = {}
    for ll in bundle["system"]["line_array"]:
        name_by_uid[int(ll["uid"])] = ll["name"]
        fmax_ab = _scalar_or_first(ll.get("tmax_ab", 0.0))
        fmax_ba = _scalar_or_first(ll.get("tmax_ba", fmax_ab))
        limits[ll["name"]] = {
            "max_flow": fmax_ab,
            "min_flow": -fmax_ba,
        }
        # gtopt's ``active`` is either an int 0/1 or omitted (defaults
        # to 1 = in service).  Coerce both to a 0.0/1.0 float so the
        # comparison renderer can subtract uniformly.
        raw_active = ll.get("active", 1)
        if isinstance(raw_active, list) and raw_active:
            raw_active = raw_active[0]
        try:
            active_by_name[ll["name"]] = 1.0 if float(raw_active) != 0.0 else 0.0
        except (TypeError, ValueError):
            active_by_name[ll["name"]] = 1.0

    flow_path = case_dir / "output" / "Line" / "flowp_sol.parquet"
    energy_mwh: dict[str, float] = {}
    if flow_path.exists():
        df = pq.read_table(flow_path).to_pandas()
        df["duration"] = df["block"].astype(int).map(durations)
        df["name"] = df["uid"].astype(int).map(name_by_uid)
        for name, sub in df.groupby("name", observed=True):
            # Σ |MW| × duration — total energy transferred (sign-blind)
            energy_mwh[str(name)] = float((sub["value"].abs() * sub["duration"]).sum())

    out: dict[str, dict[str, float]] = {}
    for name, lim in limits.items():
        out[name] = {
            "energy_mwh": energy_mwh.get(name, 0.0),
            "max_flow": lim["max_flow"],
            "min_flow": lim["min_flow"],
            "active": active_by_name.get(name, 1.0),
        }
    return out


def _render_line_compare(
    plexos_line: dict[str, dict[str, float]],
    gtopt_line: dict[str, dict[str, float]],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 6 — per-line energy + limit + active comparison.

    Three sub-tables:
      1. Top N lines by |Δ total energy transferred (MWh)| over the
         168 h horizon.  Uses Σ |MW|·dt so positive- and
         negative-direction flows do NOT cancel — gives a true
         "how much did this corridor work" measure.
      2. Top N lines where max_flow limits differ — indicates a
         bundle capacity mismatch.
      3. Lines whose active-status differs: PLEXOS solved with the
         line in service but gtopt's bundle has ``active=0`` (or
         vice versa).  Empty when both agree on every line.
    """
    common = set(plexos_line) & set(gtopt_line)
    plexos_only = set(plexos_line) - common
    gtopt_only = set(gtopt_line) - common

    energy_rows = []
    limit_rows = []
    active_mismatch = []
    for n in common:
        p = plexos_line[n]
        g = gtopt_line[n]
        p_e = p.get("energy_mwh", 0.0)
        g_e = g.get("energy_mwh", 0.0)
        p_max = p.get("max_flow", 0.0)
        g_max = g.get("max_flow", 0.0)
        p_act = p.get("active", 1.0)
        g_act = g.get("active", 1.0)
        energy_rows.append((n, p_e, g_e, g_e - p_e, p_max, g_max))
        limit_rows.append((n, p_max, g_max, g_max - p_max))
        if p_act != g_act:
            active_mismatch.append((n, p_act, g_act))

    energy_rows.sort(key=lambda r: abs(r[3]), reverse=True)
    limit_rows.sort(key=lambda r: abs(r[3]), reverse=True)

    # (1) Energy-transferred differences
    t1 = Table(
        title=(
            f"Per-line ENERGY comparison (Step 6) — top {top_n} by "
            f"|Δ Σ|MW|·dt|  (common: {len(common)}, "
            f"PLEXOS-only: {len(plexos_only)}, gtopt-only: {len(gtopt_only)})"
        ),
    )
    t1.add_column("Line", style="bold")
    t1.add_column("PLEXOS MWh", justify="right")
    t1.add_column("gtopt MWh", justify="right")
    t1.add_column("Δ MWh", justify="right")
    t1.add_column("Δ %", justify="right")
    t1.add_column("PLEXOS max", justify="right")
    t1.add_column("gtopt max", justify="right")
    for n, p_e, g_e, d_e, p_m, g_m in energy_rows[:top_n]:
        pct = (100.0 * d_e / p_e) if p_e else 0.0
        t1.add_row(
            n,
            f"{p_e:>11,.0f}",
            f"{g_e:>11,.0f}",
            f"{d_e:>+10,.0f}",
            f"{pct:>+6.1f}%",
            f"{p_m:>9.1f}",
            f"{g_m:>9.1f}",
        )
    console.print(t1)

    # (2) Limit differences
    t2 = Table(
        title=(f"Per-line LIMIT comparison (Step 6b) — top {top_n} by |Δ max_flow|"),
    )
    t2.add_column("Line", style="bold")
    t2.add_column("PLEXOS Max", justify="right")
    t2.add_column("gtopt Max", justify="right")
    t2.add_column("Δ Max", justify="right")
    t2.add_column("Δ %", justify="right")
    nonzero = [r for r in limit_rows if abs(r[3]) > 1e-6][:top_n]
    if not nonzero:
        t2.add_row("(all limits match within 1e-6)", "", "", "", "")
    else:
        for n, p_m, g_m, d_m in nonzero:
            pct = (100.0 * d_m / p_m) if p_m else 0.0
            t2.add_row(
                n,
                f"{p_m:>9.1f}",
                f"{g_m:>9.1f}",
                f"{d_m:>+8.1f}",
                f"{pct:>+6.1f}%",
            )
    console.print(t2)

    # (3) Active-status mismatch
    t3 = Table(
        title=(
            f"Per-line ACTIVE status — {len(active_mismatch)} mismatch(es) "
            f"of {len(common)} common lines"
        ),
    )
    t3.add_column("Line", style="bold")
    t3.add_column("PLEXOS active", justify="right")
    t3.add_column("gtopt active", justify="right")
    if not active_mismatch:
        t3.add_row("(all lines agree on in-service status)", "", "")
    else:
        for n, p_a, g_a in active_mismatch[:top_n]:
            t3.add_row(n, f"{p_a:>3.0f}", f"{g_a:>3.0f}")
    console.print(t3)

    if energy_rows:
        deltas = [r[3] for r in energy_rows]
        import statistics

        console.print(
            f"[dim]Energy stats over {len(energy_rows)} common lines: "
            f"mean Δ = {statistics.mean(deltas):+.0f} MWh, "
            f"median Δ = {statistics.median(deltas):+.0f} MWh, "
            f"mean |Δ| = {statistics.mean(abs(d) for d in deltas):.0f}, "
            f"P90 |Δ| = "
            f"{sorted(abs(d) for d in deltas)[int(0.9 * len(deltas))]:.0f}.  "
            f"Energy = Σ|MW|·dt over 168 h (non-cancelling).[/dim]"
        )


def _render_unit_generation_compare(
    plexos_unit: dict[str, tuple[float, float]],
    gtopt_unit: dict[str, tuple[float, float]],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 4b — per-generator total generation MWh, sorted by
    |Δ MWh|.  Companion to Step 4 (which sorts by Δ SRMC).
    """
    all_names = set(plexos_unit) | set(gtopt_unit)
    rows = []
    for n in all_names:
        p_mwh, _ = plexos_unit.get(n, (0.0, 0.0))
        g_mwh, _ = gtopt_unit.get(n, (0.0, 0.0))
        rows.append((n, p_mwh, g_mwh, g_mwh - p_mwh))
    rows.sort(key=lambda r: abs(r[3]), reverse=True)

    t = Table(
        title=(f"Per-generator DISPATCH comparison (Step 4b) — top {top_n} by |Δ MWh|"),
    )
    t.add_column("Unit", style="bold")
    t.add_column("PLEXOS MWh", justify="right")
    t.add_column("gtopt MWh", justify="right")
    t.add_column("Δ MWh", justify="right")
    t.add_column("Δ %", justify="right")
    for name, p, g, d in rows[:top_n]:
        pct = (100.0 * d / p) if p else 0.0
        t.add_row(
            name,
            f"{p:>11,.0f}",
            f"{g:>11,.0f}",
            f"{d:>+11,.0f}",
            f"{pct:>+6.1f}%",
        )
    console.print(t)
    import statistics

    deltas = [r[3] for r in rows]
    if deltas:
        console.print(
            f"[dim]Dispatch stats over {len(rows)} generators: "
            f"mean |Δ| = {statistics.mean(abs(d) for d in deltas):,.0f} MWh, "
            f"P90 |Δ| = "
            f"{sorted(abs(d) for d in deltas)[int(0.9 * len(deltas))]:,.0f} "
            f"MWh, total |Δ| = "
            f"{sum(abs(d) for d in deltas):,.0f} MWh.[/dim]"
        )


# ------------------------------------------------------------------
# Step 7 — Generation by technology + Step 8 reservoir trajectories
# + Step 9 battery operation
# ------------------------------------------------------------------


def _plexos_name_to_category(accdb_path: Path) -> dict[str, str]:
    """Return ``{generator_name → PLEXOS category name}``.

    PLEXOS t_object.category_id → t_category.name.  Categories on the
    CEN PCP daily bundle include "Solar Farms", "Wind Farms",
    "Hydro Gen Group A/B/C", "Thermal Gen N. Zone", "Thermal Gen
    S. Zone", "Termicas Ficticias".  Used to classify gtopt
    generators by joining on name (gtopt's bundle doesn't preserve
    category).
    """
    import csv
    import io
    import subprocess

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    cats = _dump("t_category")
    objs = _dump("t_object")
    cat_by_id = {int(c["category_id"]): c.get("name", "?") for c in cats}
    out: dict[str, str] = {}
    for o in objs:
        if o.get("class_id") != "2":  # Generator
            continue
        try:
            cid = int(o.get("category_id") or 0)
        except ValueError:
            continue
        out[o["name"]] = cat_by_id.get(cid, "Uncategorized")
    return out


def _classify_tech(category: str) -> str:
    """Bucket a PLEXOS-style category into a coarse technology label.

    Returns one of: ``thermal``, ``hydro``, ``solar``, ``wind``,
    ``other``.
    """
    if not category:
        return "other"
    c = category.lower()
    if "solar" in c or "fv" in c or "csp" in c:
        return "solar"
    if "wind" in c or "eolic" in c or "eolico" in c:
        return "wind"
    if "hydro" in c or "hidro" in c:
        return "hydro"
    if "thermal" in c or "termic" in c:
        return "thermal"
    return "other"


def compute_plexos_generation_by_technology(
    accdb_path: Path,
) -> dict[str, float]:
    """PLEXOS total generation MWh per technology bucket.

    Uses prop 2 (Generator class Generation) summed per-unit ×
    block duration, then bucketed by PLEXOS category name.
    """
    per_unit = compute_plexos_per_unit_srmc(accdb_path)
    cat_by_name = _plexos_name_to_category(accdb_path)
    out: dict[str, float] = {}
    for name, (mwh, _srmc) in per_unit.items():
        tech = _classify_tech(cat_by_name.get(name, ""))
        out[tech] = out.get(tech, 0.0) + mwh
    return out


def compute_gtopt_generation_by_technology(
    case_dir: Path,
    *,
    accdb_path: Path | None = None,
) -> dict[str, float]:
    """gtopt total generation MWh per technology bucket.

    Reads ``Generator/generation_sol.parquet`` directly (NOT joined
    with SRMC — hydro / renewable units with ``gcost=0`` don't ship
    a per-block SRMC and would be silently dropped by an inner-join).

    Classifies each dispatched unit via PLEXOS category (preferred,
    when an .accdb is supplied) or a name-pattern heuristic
    (BAT_* / _FV / _EO / _U[N] suffix) fallback.
    """
    import json
    import re

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid = {
        int(g["uid"]): g["name"] for g in bundle["system"]["generator_array"]
    }

    gen_path = case_dir / "output" / "Generator" / "generation_sol.parquet"
    if not gen_path.exists():
        return {}

    df = pq.read_table(gen_path).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations)
    df["mwh"] = df["value"] * df["duration"]
    df["name"] = df["uid"].astype(int).map(name_by_uid)

    cat_by_name: dict[str, str] = {}
    if accdb_path is not None:
        cat_by_name = _plexos_name_to_category(accdb_path)

    def _name_to_tech(name: str) -> str:
        if name in cat_by_name:
            return _classify_tech(cat_by_name[name])
        # Heuristic fallback.
        if name.startswith("BAT_"):
            return "battery"
        if "_FV" in name or "_CS" in name:
            return "solar"
        if "_EO" in name or "_EOL" in name:
            return "wind"
        if re.search(r"_U\d+$", name):
            return "hydro"
        return "thermal"

    out: dict[str, float] = {}
    for name, sub in df.groupby("name", observed=True):
        tech = _name_to_tech(str(name))
        out[tech] = out.get(tech, 0.0) + float(sub["mwh"].sum())
    return out


def _render_technology_compare(
    plexos_tech: dict[str, float],
    gtopt_tech: dict[str, float],
    console: Console,
) -> None:
    """Step 7 — generation by technology bucket."""
    techs = sorted(set(plexos_tech) | set(gtopt_tech))
    table = Table(
        title="Generation by technology (Step 7) — duration-weighted MWh",
    )
    table.add_column("Technology", style="bold")
    table.add_column("PLEXOS MWh", justify="right")
    table.add_column("gtopt MWh", justify="right")
    table.add_column("Δ (g − p)", justify="right")
    table.add_column("Δ %", justify="right")
    table.add_column("PLEXOS share", justify="right")
    table.add_column("gtopt share", justify="right")

    p_total = sum(plexos_tech.values()) or 1.0
    g_total = sum(gtopt_tech.values()) or 1.0
    for tech in techs:
        p = plexos_tech.get(tech, 0.0)
        g = gtopt_tech.get(tech, 0.0)
        d = g - p
        pct = (100.0 * d / p) if p else 0.0
        table.add_row(
            tech,
            f"{p:>13,.0f}",
            f"{g:>13,.0f}",
            f"{d:>+13,.0f}",
            f"{pct:>+6.1f}%",
            f"{100 * p / p_total:>5.1f}%",
            f"{100 * g / g_total:>5.1f}%",
        )
    table.add_row(
        "TOTAL",
        f"{sum(plexos_tech.values()):>13,.0f}",
        f"{sum(gtopt_tech.values()):>13,.0f}",
        f"{sum(gtopt_tech.values()) - sum(plexos_tech.values()):>+13,.0f}",
        f"{100 * (sum(gtopt_tech.values()) - sum(plexos_tech.values())) / sum(plexos_tech.values()):>+6.1f}%"
        if plexos_tech
        else "—",
        "100.0%",
        "100.0%",
    )
    console.print(table)


def compute_plexos_reservoir_volumes(
    accdb_path: Path,
) -> dict[str, dict[str, float]]:
    """Per-reservoir Initial/End Volume + average Volume from PLEXOS.

    Returns ``{reservoir_name → {eini, efin, vol_avg}}``.  Uses
    PLEXOS Storage class props 645 Initial Volume, 646 End Volume,
    519 Available SoC (proxy for instantaneous volume).
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}
    total_hours = sum(durations.values())

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")
    members = _dump("t_membership")
    objects = _dump("t_object")

    obj_name = {int(o["object_id"]): o["name"] for o in objects}
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}
    # Storage props in PLEXOS:
    #   645 = Initial Volume
    #   646 = End Volume
    # SoC tracking: prop 518 (per-period instantaneous volume)
    key_meta: dict[int, tuple[int, int]] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if (
            pid
            in (
                PLEXOS_PROP_STORAGE_INITIAL_VOLUME,
                PLEXOS_PROP_STORAGE_END_VOLUME,
                PLEXOS_PROP_STORAGE_SOC,
            )
            and mid in mem_child
        ):
            key_meta[kid] = (mem_child[mid], pid)

    by_res: dict[str, dict[int, list[tuple[int, float]]]] = collections.defaultdict(
        lambda: collections.defaultdict(list)
    )
    for r in data0:
        try:
            kid = int(r["key_id"])
            pid_period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        meta = key_meta.get(kid)
        if meta is None:
            continue
        oid, pid = meta
        name = obj_name.get(oid)
        if name is None:
            continue
        by_res[name][pid].append((pid_period, val))

    out: dict[str, dict[str, float]] = {}
    for name, props in by_res.items():
        entry = {"eini": 0.0, "efin": 0.0, "vol_avg": 0.0}
        # Initial Volume: take period-1 value (constant property)
        if (
            PLEXOS_PROP_STORAGE_INITIAL_VOLUME in props
            and props[PLEXOS_PROP_STORAGE_INITIAL_VOLUME]
        ):
            entry["eini"] = props[PLEXOS_PROP_STORAGE_INITIAL_VOLUME][0][1]
        if (
            PLEXOS_PROP_STORAGE_END_VOLUME in props
            and props[PLEXOS_PROP_STORAGE_END_VOLUME]
        ):
            entry["efin"] = props[PLEXOS_PROP_STORAGE_END_VOLUME][-1][1]
        if PLEXOS_PROP_STORAGE_SOC in props:
            weighted = sum(
                v * durations.get(p, 0) for p, v in props[PLEXOS_PROP_STORAGE_SOC]
            )
            if total_hours > 0:
                entry["vol_avg"] = weighted / total_hours
        out[name] = entry
    return out


def compute_gtopt_reservoir_volumes(
    case_dir: Path,
) -> dict[str, dict[str, float]]:
    """Per-reservoir Initial/End Volume + average from gtopt parquets.

    Reads ``Reservoir/eini_sol.parquet`` and ``Reservoir/efin_sol.parquet``;
    name resolution via bundle JSON's ``reservoir_array``.  ``vol_avg``
    is the duration-weighted average of efin (end-of-block volume)
    since gtopt doesn't write a separate "instantaneous" series.
    """
    import json

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid: dict[int, str] = {
        int(r["uid"]): r["name"] for r in bundle["system"].get("reservoir_array", [])
    }
    total_hours = sum(durations.values()) or 1.0

    def _read(rel: str) -> dict[int, list[tuple[int, float, float]]]:
        f = case_dir / "output" / "Reservoir" / rel
        if not f.exists():
            return {}
        df = pq.read_table(f).to_pandas()
        df["duration"] = df["block"].astype(int).map(durations).fillna(0)
        out: dict[int, list[tuple[int, float, float]]] = {}
        for uid, sub in df.groupby("uid", observed=True):
            out[int(uid)] = list(
                zip(sub["block"], sub["value"], sub["duration"], strict=False)
            )
        return out

    eini = _read("eini_sol.parquet")
    efin = _read("efin_sol.parquet")

    out: dict[str, dict[str, float]] = {}
    for uid, name in name_by_uid.items():
        entry = {"eini": 0.0, "efin": 0.0, "vol_avg": 0.0}
        if uid in eini and eini[uid]:
            entry["eini"] = float(eini[uid][0][1])
        if uid in efin and efin[uid]:
            entry["efin"] = float(efin[uid][-1][1])
            # Duration-weighted average of efin (end-of-block volume).
            weighted = sum(v * d for _, v, d in efin[uid])
            entry["vol_avg"] = weighted / total_hours
        out[name] = entry
    return out


def _render_reservoir_compare(
    plexos_res: dict[str, dict[str, float]],
    gtopt_res: dict[str, dict[str, float]],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 8 — per-reservoir initial/end/average volume comparison."""
    common = sorted(set(plexos_res) & set(gtopt_res))
    rows = []
    for name in common:
        p = plexos_res[name]
        g = gtopt_res[name]
        rows.append(
            (
                name,
                p.get("eini", 0.0),
                g.get("eini", 0.0),
                p.get("efin", 0.0),
                g.get("efin", 0.0),
                abs(g.get("efin", 0.0) - p.get("efin", 0.0)),
            )
        )
    rows.sort(key=lambda r: r[5], reverse=True)

    t = Table(
        title=(
            f"Reservoir trajectory (Step 8) — top {top_n} by |Δ efin|  "
            f"(common: {len(common)}, "
            f"PLEXOS-only: {len(set(plexos_res) - set(gtopt_res))}, "
            f"gtopt-only: {len(set(gtopt_res) - set(plexos_res))})"
        ),
    )
    t.add_column("Reservoir", style="bold")
    t.add_column("PLEXOS eini", justify="right")
    t.add_column("gtopt eini", justify="right")
    t.add_column("PLEXOS efin", justify="right")
    t.add_column("gtopt efin", justify="right")
    t.add_column("Δ efin", justify="right")
    for name, p_i, g_i, p_f, g_f, _ in rows[:top_n]:
        t.add_row(
            name,
            f"{p_i:>10,.1f}",
            f"{g_i:>10,.1f}",
            f"{p_f:>10,.1f}",
            f"{g_f:>10,.1f}",
            f"{g_f - p_f:>+10,.1f}",
        )
    console.print(t)
    console.print(
        "[dim]Volumes in PLEXOS native units (typically GWh for "
        "storages, m³ for waterways).  PLEXOS eini = prop 645 "
        "Initial Volume, efin = prop 646 End Volume.  gtopt: "
        "Reservoir/eini_sol[first block] and efin_sol[last block].[/dim]"
    )


def compute_plexos_battery_operation(
    accdb_path: Path,
) -> dict[str, dict[str, float]]:
    """Per-battery total charge / discharge MWh from PLEXOS .accdb.

    Returns ``{battery_name → {charge_mwh, discharge_mwh,
    net_mwh, eini, efin}}``.  Uses Battery-class properties
    523 Charging (MW load), 524 Discharging (MW gen), and the
    Storage props 645 Initial / 646 End Volume.
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    keys = _dump("t_key")
    data0 = _dump("t_data_0")
    members = _dump("t_membership")
    objects = _dump("t_object")

    obj_name = {int(o["object_id"]): o["name"] for o in objects}
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}
    # Battery props: 523 Charging, 524 Discharging
    key_meta: dict[int, tuple[int, int]] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if (
            pid in (PLEXOS_PROP_BATTERY_CHARGING, PLEXOS_PROP_BATTERY_DISCHARGING)
            and mid in mem_child
        ):
            key_meta[kid] = (mem_child[mid], pid)

    by_bat: dict[str, dict[int, list[tuple[int, float]]]] = collections.defaultdict(
        lambda: collections.defaultdict(list)
    )
    for r in data0:
        try:
            kid = int(r["key_id"])
            pid_period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        meta = key_meta.get(kid)
        if meta is None:
            continue
        oid, pid = meta
        name = obj_name.get(oid)
        if name is None:
            continue
        by_bat[name][pid].append((pid_period, val))

    out: dict[str, dict[str, float]] = {}
    for name, props in by_bat.items():
        charge = sum(
            v * durations.get(p, 0)
            for p, v in props.get(PLEXOS_PROP_BATTERY_CHARGING, [])
        )
        discharge = sum(
            v * durations.get(p, 0)
            for p, v in props.get(PLEXOS_PROP_BATTERY_DISCHARGING, [])
        )
        out[name] = {
            "charge_mwh": charge,
            "discharge_mwh": discharge,
            "net_mwh": discharge - charge,
        }
    return out


def compute_gtopt_battery_operation(
    case_dir: Path,
) -> dict[str, dict[str, float]]:
    """Per-battery total charge / discharge MWh from gtopt parquets.

    Reads ``Battery/finp_sol.parquet`` (charging MW) and
    ``Battery/fout_sol.parquet`` (discharging MW), duration-weighted.
    Returns ``{name → {charge_mwh, discharge_mwh, net_mwh}}``.
    """
    import json

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in case_dir.glob("*.json"):
        if cand.name == "planning.json":
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        raise FileNotFoundError(f"no PLEXOS-source JSON in {case_dir}")

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    name_by_uid: dict[int, str] = {
        int(b["uid"]): b["name"] for b in bundle["system"].get("battery_array", [])
    }

    def _sum_mwh(rel: str) -> dict[str, float]:
        f = case_dir / "output" / "Battery" / rel
        if not f.exists():
            return {}
        df = pq.read_table(f).to_pandas()
        df["duration"] = df["block"].astype(int).map(durations).fillna(0)
        df["name"] = df["uid"].astype(int).map(name_by_uid)
        out: dict[str, float] = {}
        for name, sub in df.groupby("name", observed=True):
            out[str(name)] = float((sub["value"] * sub["duration"]).sum())
        return out

    charge = _sum_mwh("finp_sol.parquet")
    discharge = _sum_mwh("fout_sol.parquet")

    out: dict[str, dict[str, float]] = {}
    for name in set(charge) | set(discharge):
        c = charge.get(name, 0.0)
        d = discharge.get(name, 0.0)
        out[name] = {
            "charge_mwh": c,
            "discharge_mwh": d,
            "net_mwh": d - c,
        }
    return out


def _render_battery_compare(
    plexos_bat: dict[str, dict[str, float]],
    gtopt_bat: dict[str, dict[str, float]],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 9 — per-battery charge / discharge MWh comparison."""
    common = sorted(set(plexos_bat) & set(gtopt_bat))
    rows = []
    for name in common:
        p = plexos_bat[name]
        g = gtopt_bat[name]
        rows.append(
            (
                name,
                p.get("charge_mwh", 0.0),
                g.get("charge_mwh", 0.0),
                p.get("discharge_mwh", 0.0),
                g.get("discharge_mwh", 0.0),
            )
        )
    # Sort by total absolute throughput diff
    rows.sort(
        key=lambda r: abs((r[2] - r[1]) + (r[4] - r[3])),
        reverse=True,
    )

    t = Table(
        title=(
            f"Battery operation (Step 9) — top {top_n} by |Δ throughput|  "
            f"(common: {len(common)}, "
            f"PLEXOS-only: {len(set(plexos_bat) - set(gtopt_bat))}, "
            f"gtopt-only: {len(set(gtopt_bat) - set(plexos_bat))})"
        ),
    )
    t.add_column("Battery", style="bold")
    t.add_column("PLEXOS charge", justify="right")
    t.add_column("gtopt charge", justify="right")
    t.add_column("PLEXOS disch", justify="right")
    t.add_column("gtopt disch", justify="right")
    for name, p_c, g_c, p_d, g_d in rows[:top_n]:
        t.add_row(
            name,
            f"{p_c:>10,.0f}",
            f"{g_c:>10,.0f}",
            f"{p_d:>10,.0f}",
            f"{g_d:>10,.0f}",
        )
    console.print(t)
    # System totals
    p_charge = sum(b.get("charge_mwh", 0.0) for b in plexos_bat.values())
    g_charge = sum(b.get("charge_mwh", 0.0) for b in gtopt_bat.values())
    p_disch = sum(b.get("discharge_mwh", 0.0) for b in plexos_bat.values())
    g_disch = sum(b.get("discharge_mwh", 0.0) for b in gtopt_bat.values())
    console.print(
        f"[dim]System totals: PLEXOS charge {p_charge:,.0f} MWh / "
        f"discharge {p_disch:,.0f} MWh.  gtopt charge {g_charge:,.0f} / "
        f"discharge {g_disch:,.0f} MWh.[/dim]"
    )


def _render_solution_compare(
    plexos_tot: dict[str, float],
    gtopt_tot: dict[str, float],
    console: Console,
) -> None:
    """Step 3 table: solution-level MWh + operational $ comparison."""
    table = Table(
        title=("Solution totals — PLEXOS vs gtopt (Step 3 duration-weighted MWh)"),
    )
    table.add_column("Metric", style="bold")
    table.add_column("PLEXOS", justify="right")
    table.add_column("gtopt", justify="right")
    table.add_column("Δ (g − p)", justify="right")
    table.add_column("Δ %", justify="right")

    def _row(label: str, p: float, g: float, fmt: str = "{:>14,.1f}") -> None:
        delta = g - p
        rel = (100.0 * delta / p) if p else 0.0
        table.add_row(
            label,
            fmt.format(p),
            fmt.format(g),
            fmt.format(delta),
            f"{rel:+.2f}%",
        )

    _row(
        "Block count", plexos_tot["block_count"], gtopt_tot["block_count"], "{:>14,.0f}"
    )
    _row(
        "Hours covered",
        plexos_tot["hours_covered"],
        gtopt_tot["hours_covered"],
        "{:>14,.0f}",
    )
    # Consumer demand (Node.Load on PLEXOS side, demand_array on gtopt
    # side).  See feedback_plexos_demand_scope.md for why this is
    # Node.Load not Region.Load.
    _row(
        "Consumer demand [MWh]",
        plexos_tot["load_mwh"],
        gtopt_tot["load_mwh"],
    )
    # BESS (Battery Energy Storage System) charging counted separately
    # — on the PLEXOS side it's bundled into Region.Load; on the gtopt
    # side it lives in the Battery object's ``finp_sol`` parquet.
    _row(
        "BESS charging [MWh]",
        plexos_tot.get("battery_load_mwh", 0.0),
        gtopt_tot.get("battery_charge_mwh", 0.0),
    )
    # Transmission losses — PLEXOS publishes Region.Losses (prop 997)
    # directly; gtopt's value is the energy-balance residual
    # `gen + batt_discharge − load − batt_charge` since gtopt's
    # piecewise loss model writes losses into bus balance rather
    # than emitting a per-line loss parquet.
    _row(
        "Transmission losses [MWh]",
        plexos_tot.get("losses_mwh", 0.0),
        gtopt_tot.get("losses_mwh", 0.0),
    )
    _row("Generation [MWh]", plexos_tot["gen_mwh"], gtopt_tot["gen_mwh"])
    _row("Unserved [MWh]", 0.0, gtopt_tot.get("fail_mwh", 0.0))

    # Operational cost: PLEXOS via prop 119 (block-$ already
    # integrated); gtopt via Σ(gen × srmc × duration).  Same intent
    # — total operational dispatch $.
    p_cost = plexos_tot.get("gen_cost_usd", 0.0)
    g_cost = gtopt_tot.get("op_cost_usd", 0.0)
    delta_cost = g_cost - p_cost
    rel_cost = (100.0 * delta_cost / p_cost) if p_cost else 0.0
    table.add_row(
        "Operational $ (∫gen·srmc dt)",
        _format_money(p_cost),
        _format_money(g_cost),
        _format_money(delta_cost),
        f"{rel_cost:+.2f}%",
    )

    console.print(table)
    console.print(
        "[dim]Demand/Gen are duration-weighted ∫MW dt across the "
        "111-block PLEXOS layout (block durations from t_phase_3).  "
        "PLEXOS GenCost is per-block $-integrated already (no "
        "duration weighting).  Step 4 (TBD): per-generator dispatch "
        "diff + per-bus LMP diff.[/dim]"
    )


# Heuristic categorisation of PLEXOS-translated user-constraint names.
# Each entry: (regex, label).  First match wins; unmatched names fall
# into "other".  These cover the families gtopt translates from the
# PLEXOS CEN PCP daily bundle — keep in sync with the bundle's actual
# constraint naming if a future case ships a new family.
_UC_CATEGORY_PATTERNS = (
    (re.compile(r"^SD_\d+_"), "security/contingency"),
    (re.compile(r"(Up|Dn|Down)?MinProvision$"), "reserve min provision"),
    (re.compile(r"_CTF_(LW|RS)$|_CSF_(LW|RS)$|_CPF_(LW|RS)$"), "N-1 reserve"),
    (re.compile(r"^Inertia"), "inertia commitment"),
    (re.compile(r"priority\d*$"), "priority dispatch"),
    (re.compile(r"min$"), "minimum dispatch"),
    (re.compile(r"^Almacenamiento_"), "battery balance"),
    (re.compile(r"_Order$"), "dispatch order"),
    (re.compile(r"^DAM_|^DAR_"), "discharge limit"),
)


def _categorize_uc(name: str) -> str:
    """Bucket a user-constraint name into a PLEXOS family.  Returns
    ``"other"`` when no heuristic matches.
    """
    for regex, label in _UC_CATEGORY_PATTERNS:
        if regex.search(name):
            return label
    return "other"


def load_uc_penalty_breakdown(
    case_dir: Path, penalty_per_unit: float
) -> dict[str, dict]:
    """Load gtopt's UserConstraint slack outputs and aggregate the
    penalty cost per constraint.

    Reads ``<case>/output/UserConstraint/{slack_sol, slack_pos_sol,
    slack_neg_sol}.parquet`` (any subset that exists) and resolves
    each ``uid`` to the constraint's name via the merged
    ``output/planning.json``.  Returns
    ``{name: {"penalty": ..., "category": ..., "uid": ...}}`` sorted by
    descending penalty.  Missing files / empty parquet → empty dict.
    """
    # Local imports keep the Step-1 path's startup time low.
    import json as _json

    import pyarrow.parquet as _pq

    plan_path = case_dir / "output" / "planning.json"
    uid_to_name: dict[int, str] = {}
    if plan_path.exists():
        plan = _json.loads(plan_path.read_text(encoding="utf-8"))
        for uc in plan.get("system", {}).get("user_constraint_array", []):
            uid_to_name[uc["uid"]] = uc.get("name", f"uc_{uc['uid']}")

    uc_dir = case_dir / "output" / "UserConstraint"
    if not uc_dir.exists():
        return {}

    by_uid: dict[int, float] = {}
    for fname in (
        "slack_sol.parquet",
        "slack_pos_sol.parquet",
        "slack_neg_sol.parquet",
    ):
        path = uc_dir / fname
        if not path.exists():
            continue
        table = _pq.read_table(path).to_pandas()
        if table.empty:
            continue
        # Each row carries the per-block slack magnitude; sum within a
        # constraint (across scene, phase, stage, block) to a single
        # MWh-equivalent slack, then price at the global UC penalty.
        for uid, slack in table.groupby("uid")["value"].sum().items():
            by_uid[int(uid)] = by_uid.get(int(uid), 0.0) + float(slack)

    breakdown: dict[str, dict] = {}
    for uid, total_slack in by_uid.items():
        name = uid_to_name.get(uid, f"uid={uid}")
        cost = total_slack * penalty_per_unit
        breakdown[name] = {
            "penalty": cost,
            "category": _categorize_uc(name),
            "uid": uid,
            "slack": total_slack,
        }

    return dict(sorted(breakdown.items(), key=lambda kv: -kv[1]["penalty"]))


def _render_uc_drilldown(
    breakdown: dict[str, dict],
    penalty_per_unit: float,
    top_n: int,
    console: Console,
) -> None:
    """Render the Step-2 user-constraint penalty breakdown."""
    if not breakdown:
        console.print(
            "[dim]No user-constraint slack found "
            "(gtopt UserConstraint dir empty or unknown penalty).[/dim]"
        )
        return

    total = sum(item["penalty"] for item in breakdown.values())

    # Aggregate by category for the family-level table.
    by_cat: dict[str, float] = {}
    for item in breakdown.values():
        by_cat[item["category"]] = by_cat.get(item["category"], 0.0) + item["penalty"]
    by_cat_sorted = sorted(by_cat.items(), key=lambda kv: -kv[1])

    cat_table = Table(
        title=(
            f"User-constraint penalty by family (unit penalty ${penalty_per_unit:,.0f})"
        )
    )
    cat_table.add_column("Family", style="bold")
    cat_table.add_column("Total penalty", justify="right")
    cat_table.add_column("Share", justify="right")
    cat_table.add_column("# constraints", justify="right")
    for cat, cost in by_cat_sorted:
        share = 100.0 * cost / total if total else 0.0
        count = sum(1 for item in breakdown.values() if item["category"] == cat)
        cat_table.add_row(
            cat,
            _format_money(cost),
            f"{share:5.1f}%",
            str(count),
        )
    cat_table.add_row(
        "[bold]TOTAL[/bold]",
        _format_money(total),
        "100.0%",
        str(len(breakdown)),
    )
    console.print(cat_table)

    top_table = Table(title=f"Top {top_n} offending constraints")
    top_table.add_column("#", justify="right")
    top_table.add_column("Constraint name", style="bold")
    top_table.add_column("Family")
    top_table.add_column("Penalty", justify="right")
    top_table.add_column("Slack", justify="right")
    for i, (name, item) in enumerate(list(breakdown.items())[:top_n], 1):
        top_table.add_row(
            str(i),
            name,
            item["category"],
            _format_money(item["penalty"]),
            f"{item['slack']:,.2f}",
        )
    console.print(top_table)


def _render_report(
    plexos: dict[str, float],
    gtopt: dict[str, float] | None,
    console: Console,
) -> None:
    """Print the side-by-side cost-totals table."""
    table = Table(title="Cost totals — PLEXOS vs gtopt (Step 1 scout)")
    table.add_column("Metric", style="bold")
    table.add_column("Value", justify="right")
    table.add_column("Source")

    plexos_obj = plexos.get("Best Integer Solution")
    plexos_bound = plexos.get("Best Bound")

    if plexos_obj is not None:
        table.add_row(
            "PLEXOS MIP objective",
            _format_money(plexos_obj),
            "Best Integer Solution (log.txt)",
        )
    if plexos_bound is not None:
        table.add_row(
            "PLEXOS best bound",
            _format_money(plexos_bound),
            "Best Bound (log.txt)",
        )
    if plexos_obj and plexos_bound:
        gap = plexos_obj - plexos_bound
        rel = 100.0 * gap / abs(plexos_obj) if plexos_obj else 0.0
        table.add_row(
            "PLEXOS gap",
            f"{_format_money(gap)}  ({rel:.2f}%)",
            "computed",
        )

    if gtopt is not None:
        table.add_row(
            "gtopt sum(obj_value)",
            _format_money(gtopt["sum_obj"]),
            f"solution.csv ({int(gtopt['rows'])} rows)",
        )
        if plexos_obj:
            delta = gtopt["sum_obj"] - plexos_obj
            rel = 100.0 * delta / abs(plexos_obj)
            table.add_row(
                "Δ gtopt − PLEXOS",
                f"{_format_money(delta)}  ({rel:+.1f}%)",
                "computed",
            )

    console.print(table)

    # Interpretation hint — single line.
    if gtopt is None:
        console.print(
            "[dim]No gtopt run available yet — re-run with "
            "[bold]--gtopt-case <dir>[/bold] once a CEN PCP solve "
            "is in hand.[/dim]"
        )
        return
    if plexos_obj is None:
        return
    delta = gtopt["sum_obj"] - plexos_obj
    rel = 100.0 * delta / abs(plexos_obj)
    # ⚠ Horizon-mismatch caveat: the CEN PCP daily bundle reports
    # PLEXOS totals over its full 7-day PCP forward-look (4-stage
    # MT chained schedule), whereas plexos2gtopt currently emits a
    # 1-stage 24-block JSON.  A like-for-like cost diff must
    # divide PLEXOS by ~7 (or extract day-1 only via
    # cen2gtopt.pcp_solution.extract_property against the .accdb).
    # Until that's wired into this scout, the raw Δ below is
    # apples-to-oranges and the user must apply the 1/7 mental
    # correction.  Tracked for follow-up.
    console.print(
        "[dim]⚠ Horizon caveat: PLEXOS log reports totals across "
        "its full 7-day PCP forward-look; gtopt currently runs 1 day.  "
        "Divide PLEXOS by ~7 for a per-day comparison until this "
        "scout learns to extract per-day totals from the .accdb.[/dim]"
    )
    if abs(rel) < 2.0:
        console.print(
            "[green]Within 2% of PLEXOS MIP — looks healthy. "
            "Proceed to Step 2 (per-unit dispatch diff).[/green]"
        )
    elif delta > 0:
        console.print(
            "[yellow]gtopt costs > PLEXOS by "
            f"{rel:+.1f}%.  Likely demand_fail_cost or reserve-"
            "shortage penalty firing because a commitment or "
            "reserve constraint is overconstrained.[/yellow]"
        )
    else:
        console.print(
            f"[yellow]gtopt costs < PLEXOS by {rel:+.1f}% (raw).  "
            "After the horizon correction (PLEXOS/7 ≈ "
            f"${plexos_obj / 7.0:,.0f}/day) the corrected delta is "
            f"{100.0 * (gtopt['sum_obj'] - plexos_obj / 7.0) / (plexos_obj / 7.0):+.1f}%.  "
            "Expected when gtopt is LP-relaxed vs PLEXOS MIP with "
            "commitment / startup costs.[/yellow]"
        )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="compare_with_plexos",
        description=(
            "PLEXOS vs gtopt comparison scout (Step 1 cost totals + "
            "Step 2 user-constraint penalty breakdown) for the CEN PCP "
            "daily case."
        ),
    )
    parser.add_argument(
        "--gtopt-case",
        type=Path,
        default=None,
        help="gtopt case directory (contains output/solution.csv)",
    )
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--plexos-log",
        type=Path,
        default=None,
        help="path to a PLEXOS solver log file (Log.txt)",
    )
    src.add_argument(
        "--plexos-res-zip",
        type=Path,
        default=None,
        help=(
            "path to a CEN PCP RES bundle (RES*.zip or RES*.zip.xz); "
            "the scout extracts the nested log automatically"
        ),
    )
    parser.add_argument(
        "--uc-penalty",
        type=float,
        default=10000.0,
        help=(
            "per-unit slack penalty applied by gtopt to soft user "
            "constraints (must match the `--default-uc-penalty` "
            "passed to plexos2gtopt; default: 10000)"
        ),
    )
    parser.add_argument(
        "--top-uc",
        type=int,
        default=15,
        help="number of top-offending user constraints to list (default: 15)",
    )
    parser.add_argument(
        "--no-uc-drilldown",
        action="store_true",
        help="skip the Step-2 user-constraint penalty breakdown",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    console = Console()

    log_path: Path
    if args.plexos_log is not None:
        log_path = args.plexos_log
    else:
        log_path = find_plexos_log_in_res_bundle(args.plexos_res_zip)
        console.print(f"[dim]Extracted PLEXOS log: {log_path}[/dim]")

    plexos = parse_plexos_log(log_path)
    if not plexos:
        console.print(
            f"[red]No objective lines parsed from {log_path} — "
            "check the file format.[/red]"
        )
        return 1

    gtopt: dict[str, float] | None = None
    if args.gtopt_case is not None:
        try:
            gtopt = parse_gtopt_solution(args.gtopt_case)
        except (FileNotFoundError, ValueError) as exc:
            console.print(f"[yellow]gtopt side unavailable: {exc}[/yellow]")

    _render_report(plexos, gtopt, console)

    # ----- Step 3: duration-weighted solution totals -----
    # Both sides must have the .accdb (PLEXOS) and the gtopt
    # outputs.  We need the .accdb to recover the per-block duration
    # from t_phase_3 — same source plexos2gtopt uses to lay out the
    # bundle's block_array.
    if args.gtopt_case is not None and args.plexos_res_zip is not None:
        console.print()
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                accdb_path = _extract_accdb_from_res_zip(
                    args.plexos_res_zip, Path(tmpdir)
                )
                # Step 3 — system totals (demand, gen, op cost)
                plexos_tot = compute_plexos_energy_totals(accdb_path)
                gtopt_tot = compute_gtopt_energy_totals(args.gtopt_case)
                # Step 4 — per-unit SRMC + dispatch
                plexos_unit = compute_plexos_per_unit_srmc(accdb_path)
                gtopt_unit = compute_gtopt_per_unit_srmc(args.gtopt_case)
                # Step 5 — per-bus LMP
                plexos_bus = compute_plexos_per_bus_lmp(accdb_path)
                gtopt_bus = compute_gtopt_per_bus_lmp(args.gtopt_case)
                # Step 6 — per-line energy + limits + active
                plexos_line = compute_plexos_per_line(accdb_path)
                gtopt_line = compute_gtopt_per_line(args.gtopt_case)
                # Step 7 — generation by technology
                plexos_tech = compute_plexos_generation_by_technology(accdb_path)
                gtopt_tech = compute_gtopt_generation_by_technology(
                    args.gtopt_case, accdb_path=accdb_path
                )
                # Step 8 — reservoir trajectories
                plexos_res = compute_plexos_reservoir_volumes(accdb_path)
                gtopt_res = compute_gtopt_reservoir_volumes(args.gtopt_case)
                # Step 9 — battery operation
                plexos_bat = compute_plexos_battery_operation(accdb_path)
                gtopt_bat = compute_gtopt_battery_operation(args.gtopt_case)
            if plexos_tot and gtopt_tot:
                _render_solution_compare(plexos_tot, gtopt_tot, console)
            else:
                console.print(
                    "[yellow]Step 3 skipped: could not compute "
                    "duration-weighted totals (missing .accdb or "
                    "gtopt parquets).[/yellow]"
                )
            if plexos_unit and gtopt_unit:
                console.print()
                _render_srmc_compare(plexos_unit, gtopt_unit, console)
                console.print()
                _render_unit_generation_compare(plexos_unit, gtopt_unit, console)
            if plexos_bus and gtopt_bus:
                console.print()
                _render_lmp_compare(plexos_bus, gtopt_bus, console)
            if plexos_line and gtopt_line:
                console.print()
                _render_line_compare(plexos_line, gtopt_line, console)
            if plexos_tech and gtopt_tech:
                console.print()
                _render_technology_compare(plexos_tech, gtopt_tech, console)
            if plexos_res and gtopt_res:
                console.print()
                _render_reservoir_compare(plexos_res, gtopt_res, console)
            if plexos_bat and gtopt_bat:
                console.print()
                _render_battery_compare(plexos_bat, gtopt_bat, console)
        except (FileNotFoundError, RuntimeError, ImportError) as exc:
            console.print(f"[yellow]Step 3-9 skipped: {exc}[/yellow]")

    if args.gtopt_case is not None and not args.no_uc_drilldown:
        console.print()
        try:
            breakdown = load_uc_penalty_breakdown(args.gtopt_case, args.uc_penalty)
        except (FileNotFoundError, ImportError, ValueError) as exc:
            console.print(f"[yellow]UC drilldown unavailable: {exc}[/yellow]")
        else:
            _render_uc_drilldown(breakdown, args.uc_penalty, args.top_uc, console)

    return 0


if __name__ == "__main__":
    sys.exit(main())
