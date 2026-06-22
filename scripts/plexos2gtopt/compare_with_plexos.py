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
import hashlib
import json
import os
import pickle
import re
import sys
import tempfile
import zipfile
from pathlib import Path

# Rich is already a project dependency (see scripts/pyproject.toml).
from rich.console import Console
from rich.table import Table


# Cache schema version — bump on ANY signature / field change to the
# ``compute_plexos_*`` functions so stale pickles are invalidated.
# A schema mismatch silently falls back to re-extraction; do not
# guard with try/except in the call site, the unpickler will surface
# AttributeError / KeyError naturally.
_PLEXOS_CACHE_VERSION = 8  # +sum_f2_dur per line (analytic loss from PLEXOS flows)


def _plexos_cache_path(res_zip: Path, cache_dir: Path | None = None) -> Path:
    """Return the deterministic cache-file path for ``res_zip``.

    The key blends the resolved zip path, its size, its mtime (ns),
    and the cache schema version so re-running against the same
    bundle is O(unpickle) but a touched / re-downloaded zip
    invalidates automatically.

    ``cache_dir`` defaults to ``$XDG_CACHE_HOME/gtopt/plexos_compare``
    (falling back to ``~/.cache/gtopt/plexos_compare``) — a
    cross-tool spot the user can ``rm -rf`` without disturbing
    either the source zip or the gtopt case directory.
    """
    if cache_dir is None:
        base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
        cache_dir = base / "gtopt" / "plexos_compare"
    res_zip = res_zip.resolve()
    st = res_zip.stat()
    key = (
        f"{res_zip}|size={st.st_size}|mtime_ns={st.st_mtime_ns}"
        f"|v{_PLEXOS_CACHE_VERSION}"
    )
    digest = hashlib.sha1(key.encode()).hexdigest()[:16]
    return cache_dir / f"{res_zip.stem}-{digest}.pkl"


def _load_plexos_cache(path: Path) -> dict | None:
    """Best-effort cache reader; returns ``None`` on miss or corruption."""
    if not path.is_file():
        return None
    try:
        with path.open("rb") as f:
            data = pickle.load(f)
    except (OSError, pickle.UnpicklingError, EOFError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("_version") != _PLEXOS_CACHE_VERSION:
        return None
    return data


def _save_plexos_cache(path: Path, data: dict) -> None:
    """Atomic pickle write: tmp + rename so a SIGINT mid-write does not
    leave a half-written cache file the next run would try to load."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".pkl.tmp")
    with tmp.open("wb") as f:
        pickle.dump(data, f, protocol=pickle.HIGHEST_PROTOCOL)
    tmp.replace(path)


def _compute_plexos_all(accdb_path: Path) -> dict:
    """Run every ``compute_plexos_*`` extractor once on the same accdb.

    Returns a dict the cache layer can pickle.  The keys map 1-to-1
    to the variables the caller in ``main()`` consumes.
    ``_version`` is folded in so the cache loader can reject stale
    schemas without unpickling first.
    """
    totals = compute_plexos_energy_totals(accdb_path)
    unit = compute_plexos_per_unit_srmc(accdb_path)
    # System operational cost as Σ_unit (Σ_block gen[u,b] × srmc[u,b] ×
    # dur[b]) — apples-to-apples with gtopt's ``op_cost_usd`` (which is
    # Σ_unit Σ_block gen × srmc × dur from the gtopt parquets).  Each
    # unit's compute_plexos_per_unit_srmc entry stores (total_mwh,
    # weighted_avg_srmc) with the average already MWh-weighted, so the
    # product reconstructs the system-total $.  Stored alongside the
    # PLEXOS pid-119 ``gen_cost_usd`` (which uses the integrated
    # PLEXOS per-block cost — a different accounting basis, kept for
    # historical reference).
    totals["gen_cost_srmc_usd"] = sum(mwh * srmc for mwh, srmc in unit.values())
    return {
        "_version": _PLEXOS_CACHE_VERSION,
        "totals": totals,
        "unit": unit,
        "bus": compute_plexos_per_bus_lmp(accdb_path),
        "line": compute_plexos_per_line(accdb_path),
        "tech": compute_plexos_generation_by_technology(accdb_path),
        "res": compute_plexos_reservoir_volumes(accdb_path),
        "res_wv": compute_plexos_reservoir_water_value(accdb_path),
        "bat": compute_plexos_battery_operation(accdb_path),
        "commit": compute_plexos_commitment(accdb_path),
        # Name→category map: used by ``compute_gtopt_generation_by_technology``
        # to classify gtopt generators using PLEXOS's own taxonomy.
        # Without caching this the gtopt-tech step still hits mdb-export
        # even when the rest of the PLEXOS side is cache-hot.
        "name_to_category": _plexos_name_to_category(accdb_path),
    }


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


def compute_plexos_problem_stats(log_path: Path) -> dict[str, float]:
    """LP/MIP problem size and TOTAL run time from a PLEXOS solver log.

    Parses the matrix-size banner and the model-completion wall time, e.g.::

        Tasks: 1   Cols 3,289,156   Ints 256,158   Rows 4,956,106   Nzs 14,023,279
        <-- Model "PRGdia_Full_Definitivo" Completed. Time: 01:57:14.9

    Returns ``{rows, cols, int_vars, run_s}``.  ``run_s`` is the TOTAL
    PLEXOS run wall time (``Model ... Completed. Time: HH:MM:SS.s`` =
    cumulative elapsed since model start — compilation + solve + solution
    DB write), matching the gtopt total-run measure.  Missing pieces
    default to 0.0.
    """
    text = log_path.read_text(encoding="utf-8", errors="replace")
    stats: dict[str, float] = {
        "rows": 0.0,
        "cols": 0.0,
        "int_vars": 0.0,
        "run_s": 0.0,
    }

    banner = re.search(r"Cols\s+([\d,]+)\s+Ints\s+([\d,]+)\s+Rows\s+([\d,]+)", text)
    if banner:
        stats["cols"] = float(banner.group(1).replace(",", ""))
        stats["int_vars"] = float(banner.group(2).replace(",", ""))
        stats["rows"] = float(banner.group(3).replace(",", ""))

    # Total run wall time: ``Model "<name>" Completed. Time: HH:MM:SS.s``.
    done = re.search(
        r'Model\s+"[^"]*"\s+Completed\.\s+Time:\s*(\d+):(\d+):([\d.]+)', text
    )
    if done:
        h, m, s = done.groups()
        stats["run_s"] = int(h) * 3600 + int(m) * 60 + float(s)

    # Final MIP gap (%) from the objective lines, mirroring the gtopt side.
    objs = parse_plexos_log(log_path)
    bi = objs.get("Best Integer Solution")
    bb = objs.get("Best Bound")
    if bi and bb:
        stats["gap_pct"] = 100.0 * (bi - bb) / abs(bi)
    return stats


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
PLEXOS_PROP_GENERATOR_UNITS_GENERATING = 7  # # of units ON per period (UC)
PLEXOS_PROP_GENERATOR_GENCOST = 119  # $ per block (already integrated)
PLEXOS_PROP_GENERATOR_STARTUP_COST = 120  # Start & Shutdown Cost ($ per block)
PLEXOS_PROP_GENERATOR_SRMC = 137  # $/MWh per period
#
# Collection-80 (Battery class) primary props.
#
# IMPORTANT: PLEXOS publishes battery power on two sides:
#
#   * AC-side (network)         — what the inverter exchanges with
#     the bus.  Includes inverter loss already.  Use these to
#     compare against gtopt's ``Battery/finp_sol`` (charge into the
#     bus as load) and ``Battery/fout_sol`` (discharge out of the
#     battery as generation).
#         520 = Generation (MW, AC discharge to the bus)
#         521 = Load       (MW, AC charge from the bus)
#
#   * DC-side (cell)            — power at the storage cell itself,
#     before/after the inverter.  Used internally by PLEXOS for the
#     SoC update.  Reading these against gtopt's AC-side parquets
#     creates a phantom over-cycling gap of (1 - η_inv)·throughput.
#         523 = Charging    (MW, DC into the cell)
#         524 = Discharging (MW, DC out of the cell)
#
# Per-battery throughput compares MUST use 520 / 521.
PLEXOS_PROP_BATTERY_GENERATION = 520  # MW discharge to bus (AC-side)
PLEXOS_PROP_BATTERY_LOAD = 521  # MW charge from bus (AC-side)
PLEXOS_PROP_BATTERY_CHARGING = 523  # MW into cell (DC-side) — do NOT compare
PLEXOS_PROP_BATTERY_DISCHARGING = 524  # MW out of cell (DC-side) — do NOT compare
#
# Collection-80 (Storage) volume props:
PLEXOS_PROP_STORAGE_SOC = 518  # GWh / m³ per period
PLEXOS_PROP_STORAGE_INITIAL_VOLUME = 645
PLEXOS_PROP_STORAGE_END_VOLUME = 646
# Storage "Shadow Price" (prop 680, Storages collection, unit $/CMD) = the
# marginal value of stored water = PLEXOS's water value.  Pairs with gtopt
# Reservoir/water_value_dual (the storage-balance dual, also $/CMD).
PLEXOS_PROP_STORAGE_SHADOW_PRICE = 680
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
PLEXOS_PROP_NODE_UNSERVED = 1408  # Node Unserved Energy (MW per period) —
#   the consumer-side load-shed; node-level mirrors NODE_LOAD scope.
#   (Region Unserved Energy is prop 1000; node is apples-to-apples here.)
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
    # PLEXOS Node.Load (1373) is GROSS-of-battery-charging: it
    # includes the battery's charging draw at the node where the
    # battery sits.  gtopt's ``Demand/load_sol`` (filtered to
    # input-bundle uids) is consumer-only — the battery's charge
    # path lives in the Battery object's ``finp_sol``.  Compute a
    # ``load_consumer_mwh`` = Node.Load − Battery.Load for an
    # apples-to-apples consumer-demand comparison.
    node_load = _energy_mwh(PLEXOS_PROP_NODE_LOAD)
    battery_load = _energy_mwh(PLEXOS_PROP_BATTERY_LOAD)
    return {
        "load_mwh": max(0.0, node_load - battery_load),
        # PLEXOS Node Unserved Energy (prop 1408) — measured, not assumed.
        # On a feasible CEN PCP solve this is 0, but extracting it keeps the
        # comparison honest (and catches a PLEXOS load-shed if one occurs).
        "unserved_mwh": _energy_mwh(PLEXOS_PROP_NODE_UNSERVED),
        "load_node_mwh": node_load,
        "load_region_mwh": _energy_mwh(PLEXOS_PROP_REGION_LOAD),
        "battery_load_mwh": battery_load,
        "losses_mwh": _energy_mwh(PLEXOS_PROP_REGION_LOSSES),
        "gen_mwh": _energy_mwh(PLEXOS_PROP_REGION_GENERATION),
        "gen_unit_mwh": _energy_mwh(PLEXOS_PROP_GENERATOR_GENERATION),
        "gen_cost_usd": _block_sum(PLEXOS_PROP_GENERATOR_GENCOST),
        # Start & Shutdown Cost (prop 120) — already integrated $ per
        # block per generator.  Summed across all generators and blocks
        # for the system-wide commitment cost component that adds to
        # the dispatch $ (Σ gen×srmc×dt) to form total operational $.
        "startup_cost_usd": _block_sum(PLEXOS_PROP_GENERATOR_STARTUP_COST),
        "block_count": len(durations),
        "hours_covered": sum(durations.values()),
    }


def _sum_real_generator_mwh(
    case_dir: Path,
    bundle: dict,
    durations: dict[int, float],
) -> tuple[float, float]:
    """Split ``Generator/generation_sol.parquet`` MWh into real vs synthetic.

    gtopt's ``expand_batteries`` (C++ ``system.cpp``) appends synthetic
    ``<bat>_gen`` companion generators at LP-build time — they're NOT
    written back into the bundle's ``generator_array``, but their MWh
    rows DO end up in ``Generator/generation_sol.parquet`` with uids
    above the bundle's max ``Generator.uid``.  Splitting on
    bundle-uid membership separates real consumer generation from
    the battery-discharge bus-injection (which equals
    ``Battery/fout_sol`` to the cent on the CEN PCP bundle —
    verified empirically).

    Returns ``(real_gen_mwh, synthetic_bat_gen_mwh)``; their sum is
    the legacy ``gen_mwh`` total kept for backwards-compat reporting.
    """
    import pyarrow.parquet as pq

    f = case_dir / "output" / "Generator" / "generation_sol.parquet"
    if not f.exists():
        return 0.0, 0.0
    bundle_uids = {int(g["uid"]) for g in bundle["system"].get("generator_array", [])}
    df = pq.read_table(f, columns=["uid", "block", "value"]).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations).fillna(0.0)
    mwh = df["value"] * df["duration"]
    is_real = df["uid"].astype(int).isin(bundle_uids)
    return float(mwh.where(is_real, 0.0).sum()), float(mwh.where(~is_real, 0.0).sum())


def _sum_line_losses_extras_mwh(
    case_dir: Path,
    durations: dict[int, float],
) -> float:
    """Sum the opt-in consolidated per-line loss stream from
    ``Line/loss_sol.parquet`` into a single horizon-wide MWh.

    gtopt's ``LineLP::add_to_output`` (``source/line_lp.cpp``) emits
    one consolidated ``Line/loss_sol.parquet`` whose per-(line,
    scenario, stage, block) value is ``LP(lossp) + LP(lossn)`` —
    direction-agnostic total dissipated energy.  This single file
    replaces the earlier paired ``lossp_sol`` / ``lossn_sol`` outputs,
    so consumers no longer have to special-case the missing-direction
    edge case when the LP routes every dispatch one way.

    Emitted ONLY when the run was invoked with the ``extras``
    write-out bit (e.g. ``--write-out all`` or
    ``--write-out solution,dual,reduced_cost,extras``).  Default
    runs do not include this stream — callers must opt in.

    When the parquet exists this is the **most accurate** line-loss
    metric: it sums the exact per-block LP-decision values that
    entered gtopt's bus-balance, with no approximation from the
    R/V²·f² re-derivation or the bus-residual energy balance.

    Returns ``0.0`` when the parquet is absent so callers can fall
    back transparently to the analytic / bus-residual paths.
    """
    import pyarrow.parquet as pq

    f = case_dir / "output" / "Line" / "loss_sol.parquet"
    if not f.exists():
        return 0.0
    df = pq.read_table(f, columns=["block", "value"]).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations).fillna(0.0)
    return float((df["value"] * df["duration"]).sum())


def _compute_line_losses_analytic_mwh(
    case_dir: Path,
    bundle: dict,
    durations: dict[int, float],
) -> float:
    """Analytic line-loss total: ``Σ_line Σ_block (R/V²) · |f|² · dur``.

    The bus-balance residual ``gen − load − bat_in`` would be the
    ideal loss measure, but on the CEN PCP weekly bundle it overstates
    actual line losses by ~75 % (62.7 GWh residual vs ~36 GWh of true
    line PWL loss).  Root cause is a mix of LP slack accounting in
    the over-envelope PWL bands, float-rounding accumulated across
    176 k (gen, block) cells, and any synthetic-element drift not
    captured by the consumer / battery filters.  See
    ``losses_audit.md`` (2026-05-28 task ``a7a0db94…``).

    This helper sidesteps the bus residual entirely by re-deriving
    losses from the physical line definitions: each line carries a
    p.u. resistance ``R`` (and optional ``V`` voltage), and the
    per-block dispatch is split into positive / negative direction
    flow MW in ``Line/{flowp,flown}_sol.parquet``.  The disjoint
    sum ``|f| = flowp + flown`` is the absolute MW carried, and the
    energy dissipated in that block is ``R/V² · |f|² · dur_h``.
    Matches PLEXOS Line.Loss within ±13 % across the 281 lossy CEN
    PCP lines (vs +79 % for the bus-residual approach), and matches
    PLEXOS's own analytic ``Σ R·I²·dt`` derivation to the cent.

    Returns the system-wide line-loss total in MWh.  When no flow
    parquets exist (e.g. an LP-only or write-out-restricted run)
    returns ``0.0`` so callers can fall back to the bus residual.
    """
    import pyarrow.parquet as pq

    # Unified signed flow under ``Line/flow_sol.parquet`` (preferred);
    # fall back to the legacy directional pair ``flowp_sol`` /
    # ``flown_sol`` for older gtopt outputs.  The loss formula uses
    # ``|flow|²``, so signed and directional inputs collapse to the
    # same result once we take the magnitude.
    f_path = case_dir / "output" / "Line" / "flow_sol.parquet"
    fp_path = case_dir / "output" / "Line" / "flowp_sol.parquet"
    fn_path = case_dir / "output" / "Line" / "flown_sol.parquet"
    if not f_path.exists() and (not fp_path.exists() or not fn_path.exists()):
        return 0.0
    # Per-line R/V² constants from the bundle.  Voltage defaults to 1.0
    # (gtopt convention: ``resistance`` is already p.u. normalised when
    # ``voltage`` is absent — matches PLEXOS Line.Resistance on the CEN
    # PCP bundle, where 36 of 317 lines carry no voltage and the rest
    # use 1.0 implicitly).  Lines without ``resistance`` are lossless
    # by definition (treat as zero contribution).
    r_by_uid: dict[int, float] = {}
    v_by_uid: dict[int, float] = {}
    for line in bundle["system"].get("line_array", []):
        uid = int(line["uid"])
        r = line.get("resistance")
        if r is None:
            continue
        r_by_uid[uid] = float(r)
        v = line.get("voltage")
        v_by_uid[uid] = float(v) if v else 1.0
    if not r_by_uid:
        return 0.0
    if f_path.exists():
        flow = pq.read_table(f_path, columns=["uid", "block", "value"]).to_pandas()
        flow["fabs"] = flow["value"].abs()
    else:
        fp = pq.read_table(fp_path, columns=["uid", "block", "value"]).to_pandas()
        fn = pq.read_table(fn_path, columns=["uid", "block", "value"]).to_pandas()
        flow = fp.merge(
            fn, on=["uid", "block"], how="outer", suffixes=("_p", "_n")
        ).fillna(0.0)
        flow["fabs"] = flow["value_p"] + flow["value_n"]
    flow["uid"] = flow["uid"].astype(int)
    flow["dur"] = flow["block"].astype(int).map(durations).fillna(0.0)
    flow["R"] = flow["uid"].map(r_by_uid)
    flow["V"] = flow["uid"].map(v_by_uid)
    flow = flow.dropna(subset=["R"])
    flow["loss_mwh"] = flow["R"] / (flow["V"] ** 2) * flow["fabs"] ** 2 * flow["dur"]
    return float(flow["loss_mwh"].sum())


def _compute_plexos_line_losses_analytic_mwh(
    plexos_line: dict[str, dict[str, float]],
    case_dir: Path,
) -> float:
    """Analytic line-loss total recomputed from **PLEXOS** flows.

    Mirrors :func:`_compute_line_losses_analytic_mwh` (the gtopt side) but
    drives the ``Σ_line (R/V²)·Σ_block |f|²·dur`` formula off PLEXOS's own
    per-line flow series (``sum_f2_dur`` from :func:`compute_plexos_per_line`)
    and the SAME ``R``/``V`` from the gtopt bundle, matched by line name.

    Comparing this against the gtopt analytic isolates **flow-driven** from
    **model-driven** loss differences: identical R/V² on both sides, so any
    gap is purely how the two dispatches route power.  Comparing it against
    PLEXOS's reported ``Region.Losses`` validates the resistance data.

    PLEXOS computes losses **post-solve** (not in the LP objective), so its
    flows are effectively the lossless-dispatch solution; this recomputation
    is the apples-to-apples bridge to gtopt, which prices PWL losses inside
    the LP.  Returns 0.0 when no flows / resistances are available.
    """

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        return 0.0
    bundle = json.loads(bundle_path.read_text())

    # Per-line R/V² keyed by NAME (PLEXOS reports names, not uids).
    rv2_by_name: dict[str, float] = {}
    for line in bundle["system"].get("line_array", []):
        r = line.get("resistance")
        if r is None:
            continue
        v = line.get("voltage")
        vv = float(v) if v else 1.0
        rv2_by_name[str(line["name"])] = float(r) / (vv * vv)
    if not rv2_by_name:
        return 0.0

    total = 0.0
    for name, entry in plexos_line.items():
        rv2 = rv2_by_name.get(name)
        if rv2 is None:
            continue
        total += rv2 * entry.get("sum_f2_dur", 0.0)
    return total


def compute_gtopt_problem_stats(case_dir: Path) -> dict[str, float | str]:
    """LP/MIP problem size and TOTAL run time for the gtopt run.

    Sources, all under ``<case>/output``:
      * newest ``logs/gtopt_*.log`` → first/last ``[YYYY-MM-DD HH:MM:SS.mmm]``
        timestamps (delta = TOTAL run wall time: LP build + solve + write_out)
        and ``avg LP size : N vars, M rows``.
      * newest ``logs/cplex_*.log`` → ``Reduced MIP has B binaries, G
        generals`` (0/absent under an LP relaxation).
      * ``solver_status.json`` → ``solver``, ``method`` labels.

    ``run_s`` is the total process wall time (matches PLEXOS's
    ``Model ... Completed`` total), NOT the solver-only time.  Returns
    ``{rows, cols, int_vars, run_s, solver, method}`` with missing pieces
    defaulting to 0 / "" so the renderer degrades gracefully.
    """
    from datetime import datetime

    out_dir = case_dir / "output"
    stats: dict[str, float | str] = {
        "rows": 0.0,
        "cols": 0.0,
        "int_vars": 0.0,
        "run_s": 0.0,
        "solver": "",
        "method": "",
    }

    status_path = out_dir / "solver_status.json"
    if status_path.is_file():
        try:
            sd = json.loads(status_path.read_text())
            stats["solver"] = str(sd.get("solver", ""))
            stats["method"] = str(sd.get("method", ""))
        except (ValueError, OSError):
            pass

    logs = out_dir / "logs"
    gtopt_logs = sorted(logs.glob("gtopt_*.log")) if logs.is_dir() else []
    if gtopt_logs:
        text = gtopt_logs[-1].read_text(errors="ignore")
        m = re.search(r"avg LP size\s*:\s*([\d,]+)\s*vars,\s*([\d,]+)\s*rows", text)
        if m:
            stats["cols"] = float(m.group(1).replace(",", ""))
            stats["rows"] = float(m.group(2).replace(",", ""))
        # Total run wall time = last − first log timestamp (covers LP build,
        # solve, and write_out — the whole gtopt process).
        ts = re.findall(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", text)
        if len(ts) >= 2:
            fmt = "%Y-%m-%d %H:%M:%S.%f"
            try:
                t0 = datetime.strptime(ts[0], fmt)
                t1 = datetime.strptime(ts[-1], fmt)
                stats["run_s"] = max(0.0, (t1 - t0).total_seconds())
            except ValueError:
                pass

    cplex_logs = sorted(logs.glob("cplex_*.log")) if logs.is_dir() else []
    if cplex_logs:
        text = cplex_logs[-1].read_text(errors="ignore")
        m = re.search(r"Reduced MIP has\s+(\d+)\s+binaries,\s+(\d+)\s+generals", text)
        if m:
            stats["int_vars"] = float(int(m.group(1)) + int(m.group(2)))
        # Final MIP gap = the last "X.XX%" in the CPLEX B&C node table's Gap
        # column (the last incumbent/node line before the run summary).  gtopt
        # typically reaches ~0; PLEXOS stops at its configured gap tolerance.
        gap_hits = re.findall(r"(\d+(?:\.\d+)?)\s*%\s*$", text, re.MULTILINE)
        if gap_hits:
            stats["gap_pct"] = float(gap_hits[-1])
    return stats


def _render_problem_size_compare(
    plexos_ps: dict[str, float],
    gtopt_ps: dict[str, float | str],
    console: Console,
) -> None:
    """Step 3b: problem size + TOTAL run time, PLEXOS vs gtopt."""
    table = Table(title="Problem size & total run time — PLEXOS vs gtopt")
    table.add_column("Metric", style="bold")
    table.add_column("PLEXOS", justify="right")
    table.add_column("gtopt", justify="right")
    table.add_column("PLEXOS / gtopt", justify="right")

    def _row(label: str, p: float, g: float, fmt: str) -> None:
        ratio = f"{p / g:.2f}×" if g else "—"
        table.add_row(label, fmt.format(p), fmt.format(g), ratio)

    _row(
        "Total run time [s]", plexos_ps["run_s"], float(gtopt_ps["run_s"]), "{:>12,.1f}"
    )
    _row("Rows", plexos_ps["rows"], float(gtopt_ps["rows"]), "{:>12,.0f}")
    _row("Cols", plexos_ps["cols"], float(gtopt_ps["cols"]), "{:>12,.0f}")
    _row(
        "Integer vars",
        plexos_ps["int_vars"],
        float(gtopt_ps["int_vars"]),
        "{:>12,.0f}",
    )
    # MIP gap at termination (%): PLEXOS often stops at its gap tolerance;
    # gtopt runs to ~0.  A gap ratio is not meaningful, so no ratio column.
    pgap = plexos_ps.get("gap_pct")
    ggap = gtopt_ps.get("gap_pct")
    if pgap is not None or ggap is not None:
        table.add_row(
            "Final MIP gap [%]",
            f"{pgap:.2f}%" if pgap is not None else "—",
            f"{float(ggap):.2f}%" if ggap is not None else "—",
            "—",
        )
    console.print(table)
    console.print(
        "[dim]Total run time = full wall clock: PLEXOS "
        "``Model … Completed`` (compile + solve + solution-DB write); gtopt "
        "first→last log timestamp (LP build + solve + write_out).  PLEXOS "
        "may interrupt its MIP at gap; gtopt runs to its configured gap — "
        "termination is not identical.[/dim]"
    )


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

    import pyarrow.parquet as pq

    # 1. Bundle JSON → block uid → duration_h.  Bundle filename
    #    convention is either ``plexos_bundle.json`` (legacy) or
    #    ``<stem>.json`` (current plexos2gtopt output).  Look for
    #    either.
    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        if cand.name == "planning.json":
            continue  # gtopt's writer copies; not the source bundle
        if cand.name.endswith(".provenance.json"):
            continue  # plexos2gtopt run-metadata sidecar; not the bundle
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

    # Split Generator/generation_sol into real (bundle-defined) and synthetic
    # (``<bat>_gen`` companion LP-build injections) so the bus residual can
    # be expressed in pure-real-bus terms.  ``gen_mwh`` is kept as the legacy
    # sum for backwards-compat (consumed by per-technology rollups
    # downstream).
    gen_real_mwh, gen_synth_bat_mwh = _sum_real_generator_mwh(
        case_dir, bundle, durations
    )
    gen_total = gen_real_mwh + gen_synth_bat_mwh
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

    # ---- Three line-loss measures + the headline aliasing ----
    #
    # 1. ``losses_line_analytic_mwh`` (PRIMARY for PLEXOS comparison) —
    #    derived from the physical line definition:
    #    ``Σ_line Σ_block (R/V²)·|f|²·dur`` using the flowp/flown sol
    #    parquets that ARE emitted on every run.  Both gtopt's PWL and
    #    PLEXOS's PWL approximate this same underlying convex quadratic,
    #    so the analytic is the apples-to-apples truth both sides
    #    target.  Matches PLEXOS Line.Loss to within ±13 % on the CEN
    #    PCP weekly bundle.  ``losses_mwh`` aliases this when available.
    #
    # 2. ``losses_line_extras_mwh`` (LP-INTERNAL DIAGNOSTIC, OPT-IN) —
    #    direct sum of gtopt's per-(line, block)
    #    ``Line/loss_sol.parquet`` consolidated stream
    #    (``LP(lossp) + LP(lossn)`` merged per cell at LP-build time —
    #    see ``source/line_lp.cpp`` ``LineLP::add_to_output``).  This is
    #    what the LP actually charged itself for losses on the bus
    #    balance — NOT directly the same quantity as PLEXOS's reported
    #    Line.Loss, because gtopt's midpoint-debiased PWL produces a
    #    structurally smaller per-block value than the analytic quadratic
    #    on this CEN PCP bundle (verified LP-relax: ``loss_sol`` summed
    #    to 13 GWh vs 37 GWh analytic vs PLEXOS 35 GWh).  Reported as a
    #    diagnostic field (e.g. detecting LP under-charging that lets
    #    generation under-dispatch the physics) but NOT used as the
    #    headline ``losses_mwh``.  Emitted only when ``--write-out``
    #    includes ``extras``; gated by ``OutputContext::emit_extras``
    #    (``planning_enums.hpp:428``).
    #
    # 3. ``losses_bus_residual_mwh`` (LEGACY, LAST RESORT) — old
    #    energy-balance formula: ``gen_total - load_total - batt_charge
    #    - bes_round_trip``.  Reported so the user can spot residual
    #    drift but no longer the primary loss metric — the audit
    #    (``losses_audit.md`` 2026-05-28 task ``a7a0db94…``) showed
    #    it overstates real line losses by ~75 % on MIP runs of the CEN
    #    PCP bundle due to PWL slack accounting in over-envelope bands
    #    plus float rounding accumulated across 176 k cells.
    #
    # 4. ``bes_round_trip_mwh`` — ``max(0, batt_charge - batt_discharge)``,
    #    the BES round-trip loss + SoC drift over the horizon.  Reported
    #    explicitly so the bus-residual decomposition is visible.
    #
    # ``losses_mwh`` aliases the first non-zero value in the order
    # **analytic → bus-residual** (the LP-internal extras stream is
    # intentionally NOT preferred for the headline because it's a
    # different physical quantity than PLEXOS's Line.Loss — see #2
    # above for the rationale).  Downstream consumers reading the
    # single ``losses_mwh`` key automatically get the most-comparable-
    # to-PLEXOS number without branching.
    losses_line_extras_mwh = _sum_line_losses_extras_mwh(case_dir, durations)
    losses_line_analytic_mwh = _compute_line_losses_analytic_mwh(
        case_dir, bundle, durations
    )
    # ``bes_round_trip_mwh`` is reported as a separate diagnostic ONLY —
    # it is NOT subtracted from ``losses_bus_residual_mwh``.  The
    # round-trip energy ``(bat_charge − bat_discharge)`` is ALREADY
    # captured by the ``− batt_charge`` term: the bus paid
    # ``batt_charge`` MWh to charge the battery and got back
    # ``batt_discharge`` MWh on discharge, so generation has to cover
    # the gap, which it does via ``gen_total``.  Subtracting it again
    # double-counts and made the residual half of the LP's actual
    # ``loss_sol`` output (verified on CEN PCP LP-relax: 7,742 vs
    # 13,058 = ~5,944 MWh round-trip subtracted twice).  Without the
    # extra subtraction, the bus residual matches ``loss_sol`` to
    # float-rounding noise across the 176 k (gen, block) cells.
    bes_round_trip = max(0.0, batt_charge - batt_discharge)
    losses_bus_residual_mwh = max(0.0, gen_total - load_total - batt_charge)
    if losses_line_analytic_mwh > 0.0:
        losses_total = losses_line_analytic_mwh
    else:
        losses_total = losses_bus_residual_mwh

    # Start & Shutdown $ — mirror of PLEXOS prop 120 (Start & Shutdown
    # Cost).  PLEXOS publishes already-integrated per-block $ on each
    # generator; gtopt's equivalent is a multiplication of the
    # commitment LP variables by their declared per-event costs:
    #   startup $   = Σ_{c,t} startup_sol[c,t]  × commitment[c].startup_cost
    #   shutdown $  = Σ_{c,t} shutdown_sol[c,t] × commitment[c].shutdown_cost
    # The startup_sol / shutdown_sol parquets are population counts (0/1
    # event flags per block), so no Δt-weighting is needed.  Keyed via
    # commitment.uid → cost map from planning.json's commitment_array.
    # When the bundle has no commitment array or the parquets are
    # absent (e.g. an LP-relax run without commitment binaries),
    # contributes 0 — the metric still adds cleanly into the operational
    # total.

    startup_cost_usd = 0.0
    shutdown_cost_usd = 0.0
    commit_dir = case_dir / "output" / "Commitment"
    planning_path = case_dir / "output" / "planning.json"
    if commit_dir.is_dir() and planning_path.is_file():
        try:
            with planning_path.open() as f:
                _plan = json.load(f)
            _commit_arr = _plan.get("system", {}).get("commitment_array", [])
            su_by_uid = {
                int(c["uid"]): float(c.get("startup_cost") or 0.0)
                for c in _commit_arr
                if "uid" in c
            }
            sd_by_uid = {
                int(c["uid"]): float(c.get("shutdown_cost") or 0.0)
                for c in _commit_arr
                if "uid" in c
            }
            for fname, cost_map, target in (
                ("startup_sol.parquet", su_by_uid, "startup"),
                ("shutdown_sol.parquet", sd_by_uid, "shutdown"),
            ):
                fpath = commit_dir / fname
                if not fpath.exists():
                    continue
                df = pq.read_table(fpath).to_pandas()
                if df.empty:
                    continue
                df["unit_cost"] = df["uid"].astype(int).map(cost_map).fillna(0.0)
                total = float((df["value"] * df["unit_cost"]).sum())
                if target == "startup":
                    startup_cost_usd = total
                else:
                    shutdown_cost_usd = total
        except (OSError, ValueError, KeyError):
            pass

    return {
        "gen_mwh": gen_total,
        # Split for the bus-balance decomposition (gen_real + synth = gen_total).
        "gen_real_mwh": gen_real_mwh,
        "gen_synth_bat_mwh": gen_synth_bat_mwh,
        "load_mwh": load_total,
        "fail_mwh": fail_total,
        # BESS charging (counted on PLEXOS side as Region.Load - Node.Load
        # contribution, here pulled out as a separate row so the demand
        # comparison stays apples-to-apples on consumer load).
        "battery_charge_mwh": batt_charge,
        "battery_discharge_mwh": batt_discharge,
        "bes_round_trip_mwh": bes_round_trip,
        # Three line-loss measures, ranked by accuracy.  ``losses_mwh``
        # aliases the first non-zero value in the order:
        # extras → analytic → bus-residual.  See the long comment in
        # ``compute_gtopt_energy_totals`` for the trade-offs.
        "losses_line_extras_mwh": losses_line_extras_mwh,
        "losses_line_analytic_mwh": losses_line_analytic_mwh,
        "losses_bus_residual_mwh": losses_bus_residual_mwh,
        # Implicit transmission losses (energy-balance residual).
        "losses_mwh": losses_total,
        "op_cost_usd": op_cost,
        # Per-event commitment costs (PLEXOS prop 120 equivalent).
        # Added to op_cost in the Step 3 rendering for the
        # total-operational-cost line; reported separately too so the
        # dispatch / commitment split is visible.
        "startup_cost_usd": startup_cost_usd,
        "shutdown_cost_usd": shutdown_cost_usd,
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

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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
    # LEFT-merge so generators without a published SRMC entry (hydro
    # / pumped storage / RoR — water value travels via
    # ``Reservoir/water_value_dual`` instead of
    # ``Generator/srmc_sol``) still appear with their dispatch MWh.
    # An inner merge silently zeroed every such generator in the
    # Step 4 / Step 4b dispatch tables, producing the phantom
    # "-100% dispatch on COLBUN_U2 / PEHUENCHE_U2 / RALCO_U2"
    # column even though gtopt actually dispatches them.
    merged = gen.merge(srmc, on=key_cols, suffixes=("_mw", "_srmc"), how="left")
    merged["value_srmc"] = merged["value_srmc"].fillna(0.0)
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
    plexos_only = [(n, p_m, p_s) for n, p_m, p_s, g_m, _ in rows if g_m < 1 < p_m]
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
    gtopt_only = [(n, g_m, g_s) for n, p_m, _, g_m, g_s in rows if p_m < 1 < g_m]
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

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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

    # PLEXOS publishes LMP = 0 on many uncongested buses
    # (typically the solution database omits the marginal price on
    # buses where the LP didn't need one).  Those collide as
    # "Δ = +gtopt_LMP, +0.0%" rows that dominate the top-N table
    # and bury the actually-divergent buses.  Sort common buses
    # into two groups: PLEXOS-priced (|p| > 0.01) and
    # PLEXOS-zero, render PLEXOS-priced first.
    rows: list[tuple[str, float, float, float]] = []
    zero_rows: list[tuple[str, float, float, float]] = []
    for n in common:
        p = plexos_bus[n]
        g = gtopt_bus[n]
        if abs(p) < 0.01:
            zero_rows.append((n, p, g, g - p))
        else:
            rows.append((n, p, g, g - p))
    rows.sort(key=lambda r: abs(r[3]), reverse=True)
    zero_rows.sort(key=lambda r: abs(r[3]), reverse=True)

    t = Table(
        title=(
            f"Per-bus LMP comparison (Step 5) — top {top_n} buses "
            f"by |Δ LMP|  (common buses: {len(common)}, "
            f"PLEXOS-priced: {len(rows)}, PLEXOS-zero: {len(zero_rows)}, "
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

        # Average marginal cost over the PLEXOS-PRICED buses only (``rows``
        # excludes the buses PLEXOS leaves at 0 / doesn't save) — same bus
        # set on both sides, so it's apples-to-apples.
        amc_p = statistics.mean(r[1] for r in rows)
        amc_g = statistics.mean(r[2] for r in rows)
        console.print(
            f"[dim]Avg marginal cost over {len(rows)} PLEXOS-priced buses: "
            f"PLEXOS = {amc_p:.2f} $/MWh, gtopt = {amc_g:.2f} $/MWh "
            f"(Δ = {amc_g - amc_p:+.2f}).[/dim]"
        )
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

    Returns ``{line_name →
        {energy_mwh, peak_flow_mw, max_flow, min_flow, active}}``:

      * ``energy_mwh`` = Σ |MW_block| × duration_block_h over all 111
        blocks.  Uses absolute value so positive- and negative-direction
        flows DON'T cancel — a line carrying ±500 MW alternating shows
        as ~84 GWh transferred over a 168-h week, not ~0.
      * ``peak_flow_mw`` = max ``|MW_block|`` over all blocks
        (instantaneous, NOT duration-weighted).  Pairs with
        ``max_flow`` to flag lines whose dispatch peaks above the
        published rating (= candidate for ``Enforce Limits = 0``
        lift on the gtopt side; see Step 6c cap-violation table).
        Capricornio110->LaNegra110 is the canonical example: cap
        76 MW, PLEXOS peak ≈ 209 MW (2.76×).
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
            "peak_flow_mw": 0.0,
            "sum_f2_dur": 0.0,
            "max_flow": 0.0,
            "min_flow": 0.0,
            "active": 0.0,
        }
        # Build a per-period {abs(ExportLimit) + abs(ImportLimit)} map so
        # the energy / peak aggregation can SKIP periods where the line
        # is effectively outaged (both limits clamped to 0 by PLEXOS).
        # Without this filter, PLEXOS publishes phantom Flow values on
        # outage periods (the unconstrained DC-OPF pre-limit angle-
        # difference solution, NOT physical flow) that inflate
        # ``energy_mwh`` by orders of magnitude.  Verified on CEN PCP
        # 2026-04-07 ``Duqueco220→Temuco220``: PLEXOS reports peak
        # 10,457 MW on 96 outage blocks where Export/Import Limit are
        # both 0 — fictitious 1.5 TWh of "transferred" energy that
        # actually never flowed (Loss = 0 on those same blocks).
        # gtopt's converter HONOURS the PLEXOS Lin_Units derate via
        # per-block ``in_service`` so the gtopt side correctly reports
        # 0 — the divergence is entirely on PLEXOS's reporting side.
        cap_by_period: dict[int, float] = {}
        for fname in ("max_flow", "min_flow"):
            for p, v in fields.get(fname, ()):
                cap_by_period[p] = cap_by_period.get(p, 0.0) + abs(v)

        # Service iff at least one direction has a non-zero limit.  Default
        # to TRUE when no limit data exists (legacy lines that don't ship
        # Export/Import Limit but still get a Flow series).  Precompute the
        # in-service period set so the filters below stay simple inline
        # membership tests (no per-iteration closure).
        no_caps = not cap_by_period
        served_periods = frozenset(p for p, c in cap_by_period.items() if c > 1e-9)

        for fname, rows in fields.items():
            if fname == "flow":
                # Total absolute energy transferred (non-cancelling) —
                # filter to in-service periods only (see comment above).
                entry["energy_mwh"] = sum(
                    abs(v) * durations.get(p, 0)
                    for p, v in rows
                    if no_caps or p in served_periods
                )
                # Peak instantaneous |flow| — pairs with max_flow to
                # detect cap-violating lines (PLEXOS Enforce Limits=0
                # signal).  NOT duration-weighted; also filtered to
                # in-service periods so an outage-block phantom value
                # doesn't masquerade as a real peak.
                entry["peak_flow_mw"] = max(
                    (abs(v) for p, v in rows if no_caps or p in served_periods),
                    default=0.0,
                )
                # Σ |MW|² · dur over in-service periods — the flow² energy
                # term the analytic loss (R/V²·|f|²·dt) multiplies.  Lets the
                # caller recompute PLEXOS line losses from PLEXOS's own flows
                # with the exact same formula gtopt uses on its flows.
                entry["sum_f2_dur"] = sum(
                    (v * v) * durations.get(p, 0)
                    for p, v in rows
                    if no_caps or p in served_periods
                )
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

    Returns ``{line_name →
        {energy_mwh, peak_flow_mw, max_flow, min_flow, active,
         enforce_level}}``:
      * ``energy_mwh`` = Σ |MW_block| × duration_block_h (non-cancelling
        across positive/negative flow directions).
      * ``peak_flow_mw`` = max ``|MW_block|`` over all blocks
        (instantaneous; pairs with ``max_flow`` to flag lines whose
        dispatch peaks above the published rating — i.e. lines
        running under ``enforce_level = 0``).
      * ``max_flow`` =  fmax  (bundle JSON, time-mean of tmax_ab list /
                         scalar; positive direction)
      * ``min_flow`` = -fmax_ba (negative-direction limit, sign-flipped
                         to match PLEXOS Min Flow convention).
      * ``active`` = bundle's ``active`` field if present, else 1.0
        (lines with active=0/False are not in service this period).
      * ``enforce_level`` = bundle's ``enforce_level`` field; mirrors
        PLEXOS ``Enforce Limits`` (0 = no cap, 1 = voltage-conditional
        — treated as 2 in gtopt's DC-OPF, 2 = hard cap, default 2).
        Surfaces which lines were deliberately lifted to explain
        peak > cap cases in the Step 6c cap-violation table.
    """

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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
    enforce_by_name: dict[str, float] = {}
    for ll in bundle["system"]["line_array"]:
        name_by_uid[int(ll["uid"])] = ll["name"]
        fmax_ab = _scalar_or_first(ll.get("tmax_ab", 0.0))
        fmax_ba = _scalar_or_first(ll.get("tmax_ba", fmax_ab))
        # Soft-capped (ex-EL0 / lifted) lines carry ``tmax_normal_*`` +
        # ``overload_penalty``: the writer inflated ``tmax_ab`` to
        # ``hard_factor × rating`` (5× regular, 10× lifted) so the loss
        # PWL stays finite, with the soft threshold at ``soft_factor ×
        # rating`` (2× / 4×).  ``tmax_ab`` is therefore the *emergency*
        # ceiling, not the original rating.  Flag these so the renderer
        # can recover the original cap for the Step 6b / 6c comparison.
        is_soft = "tmax_normal_ab" in ll and "overload_penalty" in ll
        soft_thr = (
            _scalar_or_first(ll.get("tmax_normal_ab", fmax_ab)) if is_soft else fmax_ab
        )
        limits[ll["name"]] = {
            "max_flow": fmax_ab,
            "min_flow": -fmax_ba,
            "is_soft": 1.0 if is_soft else 0.0,
            "soft_threshold": soft_thr,
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
        # PLEXOS ``Enforce Limits``: 0 = no cap, 1 = voltage-conditional,
        # 2 = hard cap (default).  Stored as float for table-rendering.
        raw_enforce = ll.get("enforce_level", 2)
        try:
            enforce_by_name[ll["name"]] = float(raw_enforce)
        except (TypeError, ValueError):
            enforce_by_name[ll["name"]] = 2.0

    # Unified signed flow under ``Line/flow_sol.parquet`` (preferred);
    # fall back to the legacy ``flowp_sol`` for older gtopt outputs.
    # ``|value|`` collapses both inputs to the same magnitude.
    flow_path = case_dir / "output" / "Line" / "flow_sol.parquet"
    if not flow_path.exists():
        flow_path = case_dir / "output" / "Line" / "flowp_sol.parquet"
    energy_mwh: dict[str, float] = {}
    peak_mw: dict[str, float] = {}
    if flow_path.exists():
        df = pq.read_table(flow_path).to_pandas()
        df["duration"] = df["block"].astype(int).map(durations)
        df["name"] = df["uid"].astype(int).map(name_by_uid)
        for name, sub in df.groupby("name", observed=True):
            # Σ |MW| × duration — total energy transferred (sign-blind)
            energy_mwh[str(name)] = float((sub["value"].abs() * sub["duration"]).sum())
            # Peak instantaneous |MW| — for Step 6c cap-violation detection.
            peak_mw[str(name)] = float(sub["value"].abs().max())

    out: dict[str, dict[str, float]] = {}
    for name, lim in limits.items():
        out[name] = {
            "energy_mwh": energy_mwh.get(name, 0.0),
            "peak_flow_mw": peak_mw.get(name, 0.0),
            "max_flow": lim["max_flow"],
            "min_flow": lim["min_flow"],
            "active": active_by_name.get(name, 1.0),
            "enforce_level": enforce_by_name.get(name, 2.0),
            "is_soft": lim["is_soft"],
            "soft_threshold": lim["soft_threshold"],
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

    def _orig_cap(g: dict[str, float], plexos_max: float) -> float:
        """Recover the ORIGINAL line rating for soft-capped lines.

        For EL0 / lifted lines the writer stored ``tmax_ab = hard_factor ×
        rating`` with ``hard_factor ∈ {5 (regular), 10 (lifted)}``.  The
        ratio ``tmax_ab / tmax_normal_ab`` is 2.5 for *both*, so the bundle
        alone is ambiguous — but the original rating is exactly the value
        PLEXOS published as its Export Limit (the converter scaled from
        it).  Snap the factor to whichever of {5, 10} reproduces the PLEXOS
        rating; fall back to the regular ÷5 when no PLEXOS reference.
        Plain (non-soft) lines are returned unchanged.
        """
        hard = float(g.get("max_flow", 0.0))
        if g.get("is_soft", 0.0) < 0.5 or hard <= 0.0:
            return hard
        cands = (hard / 5.0, hard / 10.0)
        if plexos_max and plexos_max > 0.0:
            return min(cands, key=lambda c: abs(c - plexos_max))
        return cands[0]

    energy_rows = []
    limit_rows = []
    active_mismatch = []
    for n in common:
        p = plexos_line[n]
        g = gtopt_line[n]
        p_e = p.get("energy_mwh", 0.0)
        g_e = g.get("energy_mwh", 0.0)
        p_max = p.get("max_flow", 0.0)
        g_hard = g.get("max_flow", 0.0)
        # Step 6b/6c compare the ORIGINAL rating, not the soft-cap
        # emergency ceiling.  Cache it back on the row for Step 6c reuse.
        g_max = _orig_cap(g, p_max)
        g["orig_cap"] = g_max
        p_act = p.get("active", 1.0)
        g_act = g.get("active", 1.0)
        energy_rows.append((n, p_e, g_e, g_e - p_e, p_max, g_hard))
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

    # (2) Limit differences.  For soft-capped (EL0 / lifted) lines the
    # ``gtopt Max`` column reports the recovered ORIGINAL rating (the
    # emergency ceiling tmax_ab = 5×/10× rating is the loss-PWL envelope,
    # not a real limit), so this table now flags genuine rating
    # mismatches instead of the soft-cap headroom.
    t2 = Table(
        title=(
            f"Per-line LIMIT comparison (Step 6b) — top {top_n} by "
            "|Δ rated cap|  (gtopt = original rating; soft-cap headroom excluded)"
        ),
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

    # (4) Step 6c — Cap-violation candidates.
    # Lines whose PEAK instantaneous flow exceeds their rated cap on
    # EITHER side (PLEXOS or gtopt).  These are the lines running
    # under ``Enforce Limits = 0`` (lifted) or — if the gtopt
    # ``enforce_level`` is still 2 — candidates for a manual lift.
    # The canonical example is Capricornio110->LaNegra110:
    # PLEXOS published rating 76 MW, PLEXOS peak ≈ 209 MW (2.76×).
    # The gtopt side ``--lift-line-caps`` machinery encodes this as
    # ``enforce_level = 0``; this table surfaces both the encoded
    # lifts (cross-check) and any unencoded violations (action item).
    cap_rows: list[tuple[str, float, float, float, float, float, float]] = []
    for n in common:
        p = plexos_line[n]
        g = gtopt_line[n]
        p_peak = float(p.get("peak_flow_mw", 0.0))
        g_peak = float(g.get("peak_flow_mw", 0.0))
        p_cap = float(p.get("max_flow", 0.0))
        # Use the recovered ORIGINAL rating (cached in the Step 6b loop)
        # so cap-violation flags overuse against the true rating, not the
        # 5×/10× soft-cap envelope.
        g_cap = float(g.get("orig_cap", g.get("max_flow", 0.0)))
        enforce = float(g.get("enforce_level", 2.0))
        # Ratio = peak / cap on the side with the larger violation.
        p_ratio = (p_peak / p_cap) if p_cap > 0 else 0.0
        g_ratio = (g_peak / g_cap) if g_cap > 0 else 0.0
        max_ratio = max(p_ratio, g_ratio)
        if max_ratio > 1.0 + 1e-3:
            cap_rows.append((n, p_peak, g_peak, p_cap, g_cap, max_ratio, enforce))
    cap_rows.sort(key=lambda r: r[5], reverse=True)

    t4 = Table(
        title=(
            f"Per-line CAP-VIOLATION (Step 6c) — {len(cap_rows)} line(s) "
            f"with peak |flow| > rated cap on EITHER side "
            "(candidates for enforce_level=0 lift)"
        ),
    )
    t4.add_column("Line", style="bold")
    t4.add_column("PLEXOS peak", justify="right")
    t4.add_column("gtopt peak", justify="right")
    t4.add_column("PLEXOS cap", justify="right")
    t4.add_column("gtopt cap", justify="right")
    t4.add_column("max(p/cap)", justify="right")
    t4.add_column("gtopt EL", justify="right")
    if not cap_rows:
        t4.add_row("(no cap violations)", "", "", "", "", "", "")
    else:
        for n, p_pk, g_pk, p_c, g_c, ratio, el in cap_rows[:top_n]:
            # Mark explicit lifts (EL=0) so the reader can distinguish
            # "this was deliberate" from "should we lift this too?"
            el_marker = f"{el:.0f} (lifted)" if el < 1.5 else f"{el:.0f}"
            t4.add_row(
                n,
                f"{p_pk:>9.1f}",
                f"{g_pk:>9.1f}",
                f"{p_c:>9.1f}",
                f"{g_c:>9.1f}",
                f"{ratio:>+6.2f}x",
                el_marker,
            )
        console.print(
            "[dim]Step 6c: ``EL`` = ``Line.enforce_level`` in the gtopt "
            "bundle.  EL = 0 (lifted) means the cap is intentionally not "
            "enforced (loss-PWL segments use a 2× envelope so losses "
            "stay finite); EL = 2 (default) means the cap should bind "
            "— a peak > cap here points to a missing lift or a stale "
            "PLEXOS rating.[/dim]"
        )
    console.print(t4)

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
    cat_by_name: dict[str, str] | None = None,
) -> dict[str, float]:
    """gtopt total generation MWh per technology bucket.

    Reads ``Generator/generation_sol.parquet`` directly (NOT joined
    with SRMC — hydro / renewable units with ``gcost=0`` don't ship
    a per-block SRMC and would be silently dropped by an inner-join).

    Classifies each dispatched unit via PLEXOS category (preferred,
    when a category map or .accdb is supplied) or a name-pattern
    heuristic (BAT_* / _FV / _EO / _U[N] suffix) fallback.

    The pre-computed ``cat_by_name`` form lets a cache-hot run skip
    the ``_plexos_name_to_category`` accdb hit; if both are passed,
    ``cat_by_name`` wins and ``accdb_path`` is ignored.
    """
    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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

    if cat_by_name is None:
        cat_by_name = {}
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
    plexos_total = sum(plexos_tech.values())
    gtopt_total = sum(gtopt_tech.values())
    total_pct = (
        f"{100 * (gtopt_total - plexos_total) / plexos_total:>+6.1f}%"
        if plexos_tech
        else "—"
    )
    table.add_row(
        "TOTAL",
        f"{plexos_total:>13,.0f}",
        f"{gtopt_total:>13,.0f}",
        f"{gtopt_total - plexos_total:>+13,.0f}",
        total_pct,
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


def compute_plexos_reservoir_water_value(
    accdb_path: Path,
) -> dict[str, float]:
    """Per-reservoir water value [$/CMD] from PLEXOS.

    Returns ``{storage_name → avg_water_value}``: the duration-weighted
    average of the Storage ``Shadow Price`` (prop 680, unit ``$/CMD``)
    over the horizon — PLEXOS's marginal value of stored water.  Pairs
    with gtopt ``Reservoir/water_value_dual``.
    """
    import collections
    import csv
    import io
    import subprocess

    durations = _plexos_block_durations_from_accdb(accdb_path)
    if not durations:
        return {}
    total_hours = sum(durations.values()) or 1.0

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    obj_name = {int(o["object_id"]): o["name"] for o in _dump("t_object")}
    mem_child = {
        int(m["membership_id"]): int(m["child_object_id"])
        for m in _dump("t_membership")
    }
    key_oid: dict[int, int] = {}
    for k in _dump("t_key"):
        try:
            if int(k["property_id"]) != PLEXOS_PROP_STORAGE_SHADOW_PRICE:
                continue
            oid = mem_child.get(int(k["membership_id"]))
        except (KeyError, ValueError):
            continue
        if oid is not None:
            key_oid[int(k["key_id"])] = oid

    by_res: dict[str, list[tuple[int, float]]] = collections.defaultdict(list)
    for r in _dump("t_data_0"):
        try:
            oid = key_oid.get(int(r["key_id"]))
            if oid is None:
                continue
            name = obj_name.get(oid)
            if name is None:
                continue
            by_res[name].append((int(r["period_id"]), float(r["value"])))
        except (KeyError, ValueError):
            continue

    out: dict[str, float] = {}
    for name, series in by_res.items():
        weighted = sum(v * durations.get(p, 0.0) for p, v in series)
        out[name] = weighted / total_hours
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

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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


def compute_gtopt_reservoir_water_value(
    case_dir: Path,
) -> dict[str, float]:
    """Per-reservoir water value [$/CMD] from gtopt.

    Duration-weighted average of ``Reservoir/water_value_dual.parquet``
    (the storage-balance shadow price, $ per cumec-day) over the horizon.
    Pairs with PLEXOS Storage ``Shadow Price`` (prop 680).
    """

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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

    f = case_dir / "output" / "Reservoir" / "water_value_dual.parquet"
    if not f.exists():
        return {}
    df = pq.read_table(f).to_pandas()
    df["duration"] = df["block"].astype(int).map(durations).fillna(0.0)
    out: dict[str, float] = {}
    for uid, sub in df.groupby("uid", observed=True):
        name = name_by_uid.get(int(uid))
        if name is None:
            continue
        weighted = float((sub["value"] * sub["duration"]).sum())
        out[name] = weighted / total_hours
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


def _render_water_value_compare(
    plexos_wv: dict[str, float],
    gtopt_wv: dict[str, float],
    console: Console,
) -> None:
    """Step 8b — per-reservoir water-value [$/CMD] comparison.

    The marginal value of stored water: PLEXOS Storage ``Shadow Price``
    (prop 680) vs gtopt ``Reservoir/water_value_dual`` — both in $/CMD, so
    directly comparable.  A large gtopt/PLEXOS ratio flags a mis-scaled FCF
    / boundary-cut terminal term that distorts the hydro-thermal trade-off
    (an inflated water value makes gtopt hoard water → under-use hydro).
    """
    common = sorted(set(plexos_wv) & set(gtopt_wv))
    if not common:
        return
    rows = sorted(common, key=lambda n: -abs(gtopt_wv[n] - plexos_wv[n]))
    t = Table(
        title=(
            f"Water value (Step 8b) — reservoir marginal value of stored "
            f"water [$/CMD]  (common: {len(common)}, "
            f"PLEXOS-only: {len(set(plexos_wv) - set(gtopt_wv))}, "
            f"gtopt-only: {len(set(gtopt_wv) - set(plexos_wv))})"
        ),
    )
    t.add_column("Reservoir", style="bold")
    t.add_column("PLEXOS", justify="right")
    t.add_column("gtopt", justify="right")
    t.add_column("Δ (g−p)", justify="right")
    t.add_column("ratio g/p", justify="right")
    for name in rows:
        p = plexos_wv[name]
        g = gtopt_wv[name]
        ratio = f"{g / p:.2f}x" if abs(p) > 1e-9 else "—"
        t.add_row(
            name,
            f"{p:>12,.1f}",
            f"{g:>12,.1f}",
            f"{g - p:>+12,.1f}",
            f"{ratio:>8}",
        )
    console.print(t)
    console.print(
        "[dim]Water value = marginal value of stored water.  PLEXOS = "
        "Storage Shadow Price (prop 680, $/CMD); gtopt = duration-weighted "
        "Reservoir/water_value_dual.  Ratios ≫ 1 indicate an over-scaled "
        "FCF / boundary-cut term (water hoarding).[/dim]"
    )


def compute_plexos_battery_operation(
    accdb_path: Path,
) -> dict[str, dict[str, float]]:
    """Per-battery total charge / discharge MWh from PLEXOS .accdb.

    Returns ``{battery_name → {charge_mwh, discharge_mwh, net_mwh}}``.

    Uses AC-side properties 520 Generation (discharge to bus) and
    521 Load (charge from bus) to mirror gtopt's
    ``Battery/finp_sol`` and ``Battery/fout_sol``.  Reading the
    DC-side 523/524 here would systematically over-report
    throughput by the inverter loss (~7-10%) and create a phantom
    "battery over-cycling" gap.
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
    key_meta: dict[int, tuple[int, int]] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if (
            pid in (PLEXOS_PROP_BATTERY_LOAD, PLEXOS_PROP_BATTERY_GENERATION)
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
            v * durations.get(p, 0) for p, v in props.get(PLEXOS_PROP_BATTERY_LOAD, [])
        )
        discharge = sum(
            v * durations.get(p, 0)
            for p, v in props.get(PLEXOS_PROP_BATTERY_GENERATION, [])
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

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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


def compute_plexos_commitment(
    accdb_path: Path,
) -> dict[str, dict[str, float]]:
    """Per-generator commitment summary from PLEXOS .accdb.

    Pulls ``Units Generating`` (pid 7) for the unit on/off trajectory and
    ``Start & Shutdown Cost`` (pid 120) for the $ paid for cycling.
    Returns ``{generator_name → {on_hours, startups, shutdowns,
    startup_cost_usd}}`` where:

      * ``on_hours``           = Σ (units_generating × block.duration)
      * ``startups``           = Σ max(0, U_t − U_{t−1})
      * ``shutdowns``          = Σ max(0, U_{t−1} − U_t)
      * ``startup_cost_usd``   = Σ Start & Shutdown Cost (already
                                  block-integrated by PLEXOS)

    Startup / shutdown event counts are derived from ``Units Generating``
    transitions because PLEXOS does not publish per-period ``Units
    Started`` / ``Units Shutdown`` series in the CEN PCP daily bundle;
    only the $ aggregate (pid 120) is shipped.  The transition count is
    a strict lower bound on actual events (back-to-back start+stop in
    the same block is invisible).
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

    want_props = {
        PLEXOS_PROP_GENERATOR_UNITS_GENERATING,
        PLEXOS_PROP_GENERATOR_STARTUP_COST,
    }
    key_meta: dict[int, tuple[int, int]] = {}
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if pid in want_props and mid in mem_child:
            key_meta[kid] = (mem_child[mid], pid)

    by_gen: dict[str, dict[int, list[tuple[int, float]]]] = collections.defaultdict(
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
        by_gen[name][pid].append((pid_period, val))

    out: dict[str, dict[str, float]] = {}
    for name, props in by_gen.items():
        ug = sorted(props.get(PLEXOS_PROP_GENERATOR_UNITS_GENERATING, []))
        on_hours = sum(v * durations.get(p, 0.0) for p, v in ug)
        startups = 0.0
        shutdowns = 0.0
        prev: float | None = None
        for _p, v in ug:
            if prev is not None:
                delta = v - prev
                if delta > 0:
                    startups += delta
                elif delta < 0:
                    shutdowns += -delta
            prev = v
        startup_cost = sum(
            v for _p, v in props.get(PLEXOS_PROP_GENERATOR_STARTUP_COST, [])
        )
        out[name] = {
            "on_hours": float(on_hours),
            "startups": float(startups),
            "shutdowns": float(shutdowns),
            "startup_cost_usd": float(startup_cost),
        }
    return out


def compute_gtopt_commitment(
    case_dir: Path,
) -> dict[str, dict[str, float]]:
    """Per-generator commitment summary from gtopt parquets.

    Reads ``output/Commitment/{status_sol,startup_sol,shutdown_sol}.parquet``
    and the bundle's ``generator_array`` (for uid→name and per-unit startup
    cost) and returns ``{name → {on_hours, startups, shutdowns,
    startup_cost_usd}}``.  ``startup_cost_usd`` is computed as
    ``Σ_t v_{g,t} × startup_cost_g`` so the comparison line lines up with
    PLEXOS's pid-120 dollar value.
    """

    import pyarrow.parquet as pq

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
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
    # Commitment parquets key rows by the **commitment** uid (1..N_cmt),
    # not the generator uid (1..N_gen).  Build the uid → (generator
    # name, startup_cost) map from ``commitment_array`` so we can both
    # group rows by the human-readable generator name and price the
    # startup events at the same $ value plexos2gtopt emitted.
    commit_meta: dict[int, tuple[str, float]] = {}
    for c in bundle["system"].get("commitment_array", []):
        try:
            uid = int(c["uid"])
        except (KeyError, TypeError, ValueError):
            continue
        gen_name = str(c.get("generator", ""))
        raw_sc = c.get("startup_cost")
        try:
            sc = float(raw_sc) if raw_sc is not None else 0.0
        except (TypeError, ValueError):
            sc = 0.0
        commit_meta[uid] = (gen_name, sc)

    name_by_uid = {uid: meta[0] for uid, meta in commit_meta.items()}
    sc_by_uid = {uid: meta[1] for uid, meta in commit_meta.items()}

    commit_dir = case_dir / "output" / "Commitment"
    if not commit_dir.is_dir():
        return {}

    def _by_name(rel: str, weighted: bool) -> dict[str, float]:
        f = commit_dir / rel
        if not f.exists():
            return {}
        df = pq.read_table(f).to_pandas()
        if weighted:
            df["duration"] = df["block"].astype(int).map(durations).fillna(0)
            df["mwh"] = df["value"] * df["duration"]
            col = "mwh"
        else:
            col = "value"
        df["name"] = df["uid"].astype(int).map(name_by_uid)
        return {
            str(name): float(sub[col].sum())
            for name, sub in df.groupby("name", observed=True)
            if name  # drop NaN / empty (uid not in commitment_array)
        }

    on_hours = _by_name("status_sol.parquet", weighted=True)
    startups = _by_name("startup_sol.parquet", weighted=False)
    shutdowns = _by_name("shutdown_sol.parquet", weighted=False)

    # $ paid for startups: Σ_t v_{c,t} · startup_cost_c, with c the
    # commitment uid.  Keyed via commit_meta (commitment uid →
    # generator name, $ start cost) so the cost mapping uses the
    # correct ``commitment_array`` field, not ``generator_array`` (the
    # naming-dialects registry maps ``startup_cost`` onto the
    # Commitment class, not the Generator class).
    su_path = commit_dir / "startup_sol.parquet"
    startup_cost_by_name: dict[str, float] = {}
    if su_path.exists():
        df = pq.read_table(su_path).to_pandas()
        df["sc"] = df["uid"].astype(int).map(sc_by_uid).fillna(0.0)
        df["cost"] = df["value"] * df["sc"]
        df["name"] = df["uid"].astype(int).map(name_by_uid)
        for name, sub in df.groupby("name", observed=True):
            if not name:
                continue
            startup_cost_by_name[str(name)] = float(sub["cost"].sum())

    out: dict[str, dict[str, float]] = {}
    for name in set(on_hours) | set(startups) | set(shutdowns):
        out[name] = {
            "on_hours": on_hours.get(name, 0.0),
            "startups": startups.get(name, 0.0),
            "shutdowns": shutdowns.get(name, 0.0),
            "startup_cost_usd": startup_cost_by_name.get(name, 0.0),
        }
    return out


def _render_commitment_compare(
    plexos_uc: dict[str, dict[str, float]],
    gtopt_uc: dict[str, dict[str, float]],
    console: Console,
    *,
    top_n: int = 30,
) -> None:
    """Step 10 — per-generator commitment + cost-of-commitment comparison."""
    common = sorted(set(plexos_uc) & set(gtopt_uc))
    plexos_only = sorted(set(plexos_uc) - set(gtopt_uc))
    gtopt_only = sorted(set(gtopt_uc) - set(plexos_uc))

    # Aggregate system-level summary first.
    p_on = sum(u["on_hours"] for u in plexos_uc.values())
    g_on = sum(u["on_hours"] for u in gtopt_uc.values())
    p_su = sum(u["startups"] for u in plexos_uc.values())
    g_su = sum(u["startups"] for u in gtopt_uc.values())
    p_sd = sum(u["shutdowns"] for u in plexos_uc.values())
    g_sd = sum(u["shutdowns"] for u in gtopt_uc.values())
    p_cost = sum(u["startup_cost_usd"] for u in plexos_uc.values())
    g_cost = sum(u["startup_cost_usd"] for u in gtopt_uc.values())

    summary = Table(
        title=(
            "Commitment summary (Step 10) — system totals  "
            f"(common: {len(common)}, PLEXOS-only: {len(plexos_only)}, "
            f"gtopt-only: {len(gtopt_only)})"
        ),
    )
    summary.add_column("Metric", style="bold")
    summary.add_column("PLEXOS", justify="right")
    summary.add_column("gtopt", justify="right")
    summary.add_column("Δ (g − p)", justify="right")
    summary.add_column("Δ %", justify="right")

    def _row(label: str, p: float, g: float, fmt: str = "{:,.0f}") -> None:
        d = g - p
        rel = (100.0 * d / p) if p else float("inf") if d else 0.0
        rel_s = f"{rel:+.1f}%" if abs(rel) != float("inf") else "n/a"
        summary.add_row(label, fmt.format(p), fmt.format(g), fmt.format(d), rel_s)

    _row("On-hours (Σ u·Δt)", p_on, g_on)
    _row("Startups (events)", p_su, g_su)
    _row("Shutdowns (events)", p_sd, g_sd)
    _row("Start & Shutdown $", p_cost, g_cost, "${:,.0f}")
    console.print(summary)

    # Per-generator detail: sort by |Δ on_hours| (commitment shape diff).
    rows = []
    for name in common:
        p = plexos_uc[name]
        g = gtopt_uc[name]
        rows.append(
            (
                name,
                p["on_hours"],
                g["on_hours"],
                p["startups"],
                g["startups"],
                p["startup_cost_usd"],
                g["startup_cost_usd"],
            )
        )
    rows.sort(key=lambda r: abs(r[2] - r[1]), reverse=True)

    t = Table(
        title=(f"Per-generator commitment (Step 10) — top {top_n} by |Δ on-hours|"),
    )
    t.add_column("Unit", style="bold")
    t.add_column("PLEXOS on-h", justify="right")
    t.add_column("gtopt on-h", justify="right")
    t.add_column("Δ on-h", justify="right")
    t.add_column("PLEXOS su", justify="right")
    t.add_column("gtopt su", justify="right")
    t.add_column("PLEXOS $", justify="right")
    t.add_column("gtopt $", justify="right")
    for name, pon, gon, psu, gsu, pcost, gcost in rows[:top_n]:
        d_on = gon - pon
        t.add_row(
            name,
            f"{pon:,.0f}",
            f"{gon:,.0f}",
            f"{d_on:+,.0f}",
            f"{psu:,.0f}",
            f"{gsu:,.0f}",
            f"${pcost:,.0f}",
            f"${gcost:,.0f}",
        )
    console.print(t)

    if gtopt_only:
        console.print(
            f"[dim]{len(gtopt_only)} gtopt-only committable unit(s) "
            "(no PLEXOS Units Generating series).[/dim]"
        )
    if plexos_only:
        console.print(
            f"[dim]{len(plexos_only)} PLEXOS-only committed unit(s) "
            "(not in gtopt Commitment/status_sol).[/dim]"
        )
    # Final diagnostic: is gtopt missing startup_cost entirely?
    if g_cost == 0 and p_cost > 0:
        console.print(
            "[yellow]⚠ gtopt paid $0 in startup costs while PLEXOS paid "
            f"${p_cost:,.0f} — the bundle likely has no `start_cost` field "
            "on any generator.  Re-run plexos2gtopt with start-cost wiring "
            "enabled to close this gap.[/yellow]"
        )


def _read_boundary_cut_coeffs(*dirs: str | Path | None) -> dict[str, float]:
    """Per-reservoir ``|water value|`` from ``boundary_cuts.csv``.

    The file has columns ``scene, rhs, <reservoir>...``; each reservoir
    column holds that cut's slope (``-water_value``), so the magnitude is
    the marginal value of stored water ($/storage-unit).  Searches each
    candidate directory in order and returns the first hit's first data row
    as ``{reservoir_name -> |coef|}``; empty dict when no file is found.
    """
    import csv  # local import — matches this module's lazy-csv convention

    for d in dirs:
        if d is None:
            continue
        path = Path(d) / "boundary_cuts.csv"
        if not path.is_file():
            continue
        with path.open(newline="", encoding="utf-8") as fh:
            reader = csv.reader(fh)
            header = next(reader, None)
            first = next(reader, None)
        if not header or not first:
            return {}
        coeffs: dict[str, float] = {}
        for col, val in zip(header, first):
            if col in ("scene", "rhs"):
                continue
            try:
                coeffs[col] = abs(float(val))
            except (TypeError, ValueError):
                continue
        return coeffs
    return {}


def _reservoir_water_cost(
    volumes: dict[str, dict[str, float]], coef: dict[str, float]
) -> float:
    """Terminal water cost on a basis common to gtopt and PLEXOS (PLEXOS
    exposes only NET storage, not per-block extraction):

        sum_r  (eini_r - efin_r) * |coef_r|

    over the boundary-cut reservoirs -- the SIGNED net change x the marginal
    water value (the boundary-cut slope).  A draw-down (efin < eini) is a
    positive cost (a valued asset was consumed); a REFILL (efin > eini) is a
    NEGATIVE cost -- a credit for water banked at its FCF value.  This mirrors
    the cost-to-go the LP itself optimises (`alpha = rhs + sum slope*vol_end`):
    the same single cut both sides use, so the constant `rhs`/`c` intercept
    cancels in the gtopt-vs-PLEXOS comparison and only the slope*vol_end term
    matters.  Clamping refills to zero (the prior `max(0, ...)`) dropped the
    stored-water credit entirely and made a hydro-banking gtopt run look more
    expensive than it is.
    """
    return sum(
        (vol.get("eini", 0.0) - vol.get("efin", 0.0)) * coef[name]
        for name, vol in volumes.items()
        if name in coef
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
    # BESS (Battery Energy Storage System) decomposition.  On the
    # PLEXOS side battery_load_mwh ≡ Region.Load contribution from
    # batteries; gtopt's value lives in ``Battery/finp_sol``.  Surface
    # discharge and round-trip explicitly so the bus residual line
    # below is interpretable — a ``bes_round_trip = batt_charge -
    # batt_discharge`` gap is what makes the bus residual ≠ pure
    # transmission loss.
    _row(
        "BESS charging [MWh]",
        plexos_tot.get("battery_load_mwh", 0.0),
        gtopt_tot.get("battery_charge_mwh", 0.0),
    )
    _row(
        "BESS discharging [MWh]",
        plexos_tot.get("battery_discharge_mwh", 0.0),
        gtopt_tot.get("battery_discharge_mwh", 0.0),
    )
    _row(
        "BESS round-trip loss [MWh]",
        max(
            0.0,
            plexos_tot.get("battery_load_mwh", 0.0)
            - plexos_tot.get("battery_discharge_mwh", 0.0),
        ),
        gtopt_tot.get("bes_round_trip_mwh", 0.0),
    )
    # Line losses — three independent measures on the gtopt side.
    # See compute_gtopt_energy_totals (line ~870) for the trade-offs.
    # ``losses_mwh`` headline aliases analytic (matches PLEXOS Line.Loss
    # to ±13 %); ``losses_line_extras_mwh`` is the LP-internal value
    # the LP charged itself for losses (extras-gated, opt-in via
    # ``--write-out all,extras:Line``); ``losses_bus_residual_mwh`` is
    # the energy-balance residual gen − load − batt_charge (overstates
    # transmission losses because it conflates LP slack + round-trip
    # + float drift across 176 k cells).  PLEXOS publishes
    # Region.Losses (prop 997) which is the analytic equivalent.
    _row(
        "Line losses analytic [MWh]",
        plexos_tot.get("losses_mwh", 0.0),
        gtopt_tot.get("losses_line_analytic_mwh", 0.0),
    )
    # Same R/V²·|f|²·dt formula applied to EACH side's own flows — equal
    # resistances, so the gap is purely how the two dispatches route power
    # (flow-driven), vs the row above where PLEXOS is its reported
    # Region.Losses.  PLEXOS-from-flows ≈ Region.Losses validates the
    # resistance data; gtopt-from-flows is the same number as the row above.
    plexos_from_flows = plexos_tot.get("losses_analytic_from_flows_mwh")
    if plexos_from_flows is not None:
        _row(
            "Line losses analytic-from-flows [MWh]",
            plexos_from_flows,
            gtopt_tot.get("losses_line_analytic_mwh", 0.0),
        )
    extras_g = gtopt_tot.get("losses_line_extras_mwh", 0.0)
    if extras_g > 0.0:
        _row(
            "Line losses LP-internal [MWh]",
            0.0,  # PLEXOS has no equivalent
            extras_g,
        )
    bus_resid_g = gtopt_tot.get("losses_bus_residual_mwh", 0.0)
    if bus_resid_g > 0.0:
        _row(
            "Bus residual (diagnostic) [MWh]",
            0.0,  # PLEXOS has no equivalent
            bus_resid_g,
        )
    _row("Generation [MWh]", plexos_tot["gen_mwh"], gtopt_tot["gen_mwh"])
    _row(
        "Unserved [MWh]",
        plexos_tot.get("unserved_mwh", 0.0),
        gtopt_tot.get("fail_mwh", 0.0),
    )

    # Operational cost — break down into the cost components both PLEXOS
    # and gtopt declare in their objective functions, then sum.
    #
    #   Dispatch $ (Σ gen × srmc × dt)        — pid-137 SRMC weighted by
    #     dispatch, both sides; this is the dispatch-energy cost.  PLEXOS
    #     pid-119 GenCost (already-integrated piecewise) is kept as a
    #     side-channel sanity check.
    #   Start & Shutdown $ (Σ_t v_{c,t} × cost) — pid-120 on PLEXOS;
    #     gtopt computes startup_sol × commitment.startup_cost +
    #     shutdown_sol × commitment.shutdown_cost.  Captures cycling
    #     costs missing from the pure dispatch line.
    #   TOTAL Operational $                    — sum of the above.
    #
    # Each row reports both sides, the Δ (gtopt − PLEXOS), and the
    # relative %.  The breakdown makes it obvious whether a gap is in
    # dispatch (wrong unit dispatched, wrong fuel band) or in commitment
    # (more / fewer starts than PLEXOS).
    p_dispatch = plexos_tot.get("gen_cost_srmc_usd", 0.0)
    p_pid119 = plexos_tot.get("gen_cost_usd", 0.0)
    p_startup = plexos_tot.get("startup_cost_usd", 0.0)
    g_dispatch = gtopt_tot.get("op_cost_usd", 0.0)
    g_startup = gtopt_tot.get("startup_cost_usd", 0.0)
    g_shutdown = gtopt_tot.get("shutdown_cost_usd", 0.0)
    g_commit = g_startup + g_shutdown
    p_total = p_dispatch + p_startup
    g_total = g_dispatch + g_commit

    def _cost_row(label: str, p_val: float, g_val: float) -> None:
        d = g_val - p_val
        rel = (100.0 * d / p_val) if p_val else (0.0 if g_val == 0.0 else float("inf"))
        rel_txt = f"{rel:+.2f}%" if rel != float("inf") else "  +∞%"
        table.add_row(
            label, _format_money(p_val), _format_money(g_val), _format_money(d), rel_txt
        )

    _cost_row("  Dispatch $ (Σ gen×srmc×dt)", p_dispatch, g_dispatch)
    # PLEXOS pid-120 lumps start+shutdown into one stream; gtopt splits
    # them.  Sum gtopt's two to align with PLEXOS's single value.
    _cost_row("  Start & Shutdown $", p_startup, g_commit)
    _cost_row("TOTAL Operational $", p_total, g_total)

    # Water cost (Σ net-drawdown × |boundary water value|) on the common
    # net-storage basis — PLEXOS exposes only net storage, so this is the
    # apples-to-apples water term.  Present only when boundary_cuts.csv was
    # available (the caller stashes water_cost_usd into both totals dicts).
    p_water = plexos_tot.get("water_cost_usd")
    g_water = gtopt_tot.get("water_cost_usd")
    if p_water is not None and g_water is not None:
        _cost_row("  Water $ (Σ Δstorage×wv)", p_water, g_water)
        _cost_row("TOTAL Operational + Water $", p_total + p_water, g_total + g_water)

    console.print(table)
    pid119_note = (
        f"  PLEXOS pid-119 GenCost (integrated piecewise): "
        f"{_format_money(p_pid119)} — kept as a side-channel; "
        "differs from Σ gen×srmc×dt when generators carry a multi-band "
        "offer curve."
        if p_pid119
        else ""
    )
    extra_note = ""
    if g_commit > 0.0 and g_startup > 0.0 and g_shutdown > 0.0:
        extra_note = (
            f"  gtopt split: Σ startup_sol × startup_cost = "
            f"{_format_money(g_startup)}; Σ shutdown_sol × shutdown_cost = "
            f"{_format_money(g_shutdown)}."
        )
    console.print(
        "[dim]Demand/Gen are duration-weighted ∫MW dt across the PLEXOS "
        "t_phase_3 block layout (block durations from t_phase_3).  "
        "Dispatch $ uses gen × SRMC × duration on BOTH sides; Start & "
        "Shutdown $ on PLEXOS is pid-120 Generator.StartShutdownCost "
        "(already integrated $ per block), on gtopt is Σ_t v_{c,t} × "
        "commitment[c].(start|shutdown)_cost." + pid119_note + extra_note + "[/dim]"
    )
    console.print(
        "[dim]Line losses — methodology differs by side.  PLEXOS computes "
        "losses POST-SOLVE (R·I² on the optimised flows; not a term in the "
        "LP objective/balance), so its dispatch is effectively lossless and "
        "its bus residual is ~0.  gtopt models PWL losses INSIDE the LP "
        "(priced in the objective, served from generation) — hence the "
        "'LP-internal' row and the non-zero 'Bus residual' diagnostic, and "
        "why gtopt generation runs higher to cover modelled loss.  Rows: "
        "'analytic' = gtopt Σ(R/V²)|f|²·dt vs PLEXOS reported Region.Losses "
        "(prop 997); 'analytic-from-flows' applies that SAME formula to each "
        "side's own flows (equal R/V²), so its gap is purely flow-driven; "
        "'LP-internal' is the loss the gtopt LP charged itself "
        "(--write-out all,extras:Line).[/dim]"
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

    import pyarrow.parquet as _pq

    plan_path = case_dir / "output" / "planning.json"
    uid_to_name: dict[int, str] = {}
    uid_to_penalty: dict[int, float] = {}
    if plan_path.exists():
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        for uc in plan.get("system", {}).get("user_constraint_array", []):
            uid_to_name[uc["uid"]] = uc.get("name", f"uc_{uc['uid']}")
            raw = uc.get("penalty")
            try:
                uid_to_penalty[uc["uid"]] = float(raw) if raw is not None else 0.0
            except (TypeError, ValueError):
                uid_to_penalty[uc["uid"]] = 0.0

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
        # scalar slack, then price at the per-UC penalty from the
        # planning.json (falls back to ``penalty_per_unit`` only when
        # the UC has no explicit penalty — i.e. it was softened via the
        # global ``--default-uc-penalty`` rather than an explicit
        # per-constraint value in the bundle).
        for uid, slack in table.groupby("uid")["value"].sum().items():
            by_uid[int(uid)] = by_uid.get(int(uid), 0.0) + float(slack)

    breakdown: dict[str, dict] = {}
    for uid, total_slack in by_uid.items():
        name = uid_to_name.get(uid, f"uid={uid}")
        # Per-UC penalty wins; ``penalty_per_unit`` is the diagnostic
        # fallback for UCs that the bundle never priced explicitly.
        # This matches how the LP itself prices the slack on the
        # objective row.
        eff_penalty = uid_to_penalty.get(uid, 0.0)
        if eff_penalty <= 0.0 < penalty_per_unit:
            eff_penalty = penalty_per_unit
        cost = total_slack * eff_penalty
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
            "User-constraint penalty by family (per-UC penalty from "
            f"planning.json; fallback ${penalty_per_unit:,.0f}/unit)"
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
    top_table.add_column("$/unit", justify="right")
    top_table.add_column("Slack", justify="right")
    top_table.add_column("Penalty", justify="right")
    for i, (name, item) in enumerate(list(breakdown.items())[:top_n], 1):
        unit = (item["penalty"] / item["slack"]) if item["slack"] else 0.0
        top_table.add_row(
            str(i),
            name,
            item["category"],
            f"${unit:,.2f}",
            f"{item['slack']:,.2f}",
            _format_money(item["penalty"]),
        )
    console.print(top_table)


# ============================================================
# Step 2b — per-block UC drilldown
# ============================================================
#
# For each top-N UC with non-zero slack, parse its expression, pull
# the per-block dispatch of each referenced generator from both PLEXOS
# (pid 2) and gtopt (generation_sol.parquet), compute the LHS on both
# sides, and emit a per-block table flagging exactly which blocks miss
# the bound (and by how much) on each side.
#
# Expression syntax recognised: ``c1 * generator("NAME1").generation
# {+|-} c2 * generator("NAME2").generation {... } {<=|>=} rhs``.
# Mid-expression sign tokens are absorbed into the coefficient.

_UC_TERM_RE = re.compile(
    r"([+-]?\s*\d+\.?\d*(?:[eE][+-]?\d+)?)\s*\*\s*"
    r'generator\("([^"]+)"\)\.generation'
)
_UC_OP_RHS_RE = re.compile(r"([<>]=)\s*([+-]?\d+\.?\d*(?:[eE][+-]?\d+)?)\s*$")


def _parse_uc_expression(
    expr: str,
) -> tuple[list[tuple[float, str]], str, float] | None:
    """Parse ``c1 * g1 + c2 * g2 + … {<=|>=} rhs`` into
    ``(terms, op, rhs)``.  Returns ``None`` if the expression doesn't
    fit the supported shape (e.g. references something other than
    ``generator(...).generation``)."""
    op_m = _UC_OP_RHS_RE.search(expr)
    if op_m is None:
        return None
    op = op_m.group(1)
    rhs = float(op_m.group(2))
    terms: list[tuple[float, str]] = []
    for m in _UC_TERM_RE.finditer(expr):
        try:
            coeff = float(m.group(1).replace(" ", ""))
        except ValueError:
            return None
        terms.append((coeff, m.group(2)))
    if not terms:
        return None
    return terms, op, rhs


def _pull_plexos_gen_by_period(
    accdb_path: Path, gen_names: set[str]
) -> dict[str, dict[int, float]]:
    """Per-(gen_name, period_id) MW for the requested generators.

    Restricts the t_data_0 scan to property 2 (Generation) and the
    object_ids matching ``gen_names``.  ``period_id`` here matches the
    block layout (mdb-export reports values already aggregated to the
    bundle's block periods)."""
    import collections
    import csv
    import io
    import subprocess

    def _dump(table: str) -> list[dict[str, str]]:
        out = subprocess.check_output(["mdb-export", str(accdb_path), table], text=True)
        return list(csv.DictReader(io.StringIO(out)))

    objects = _dump("t_object")
    name_by_oid = {int(o["object_id"]): o["name"] for o in objects}
    want_oids = {oid for oid, n in name_by_oid.items() if n in gen_names}

    members = _dump("t_membership")
    mem_child = {int(m["membership_id"]): int(m["child_object_id"]) for m in members}

    keys = _dump("t_key")
    want_kids: dict[int, int] = {}  # key_id → object_id
    for k in keys:
        try:
            kid = int(k["key_id"])
            mid = int(k["membership_id"])
            pid = int(k["property_id"])
        except ValueError:
            continue
        if pid != PLEXOS_PROP_GENERATOR_GENERATION:
            continue
        oid = mem_child.get(mid)
        if oid in want_oids:
            want_kids[kid] = oid

    out: dict[str, dict[int, float]] = collections.defaultdict(dict)
    for r in _dump("t_data_0"):
        try:
            kid = int(r["key_id"])
            period = int(r["period_id"])
            val = float(r["value"])
        except (KeyError, ValueError):
            continue
        oid = want_kids.get(kid)
        if oid is None:
            continue
        out[name_by_oid[oid]][period] = val
    return dict(out)


def compute_uc_block_drilldown(
    case_dir: Path,
    accdb_path: Path,
    breakdown: dict[str, dict],
    top_n: int = 5,
) -> dict[str, list[dict[str, float | str]]]:
    """Per-block PLEXOS vs gtopt LHS comparison for the top-N
    slacking UCs.

    Returns ``{uc_name: list of {block, plexos_lhs, gtopt_lhs, slack,
    rhs, op}}``, with one entry per block carrying non-zero slack.
    """

    import pyarrow.parquet as pq

    if not breakdown:
        return {}

    bundle_path: Path | None = None
    for cand in sorted(case_dir.glob("*.json")):
        # Skip the solved planning dump and the conversion-provenance
        # sidecar (neither is the PLEXOS-source case bundle).
        if cand.name == "planning.json" or cand.name.endswith(".provenance.json"):
            continue
        bundle_path = cand
        break
    if bundle_path is None or not bundle_path.is_file():
        return {}

    bundle = json.loads(bundle_path.read_text())
    durations = {
        int(b["uid"]): float(b.get("duration", 1.0))
        for b in bundle["simulation"]["block_array"]
    }
    gens = bundle["system"].get("generator_array", [])
    name_by_uid = {int(g["uid"]): g["name"] for g in gens}
    uid_by_name = {g["name"]: int(g["uid"]) for g in gens}

    ucs = bundle["system"].get("user_constraint_array", [])
    uc_by_name = {u["name"]: u for u in ucs}

    # Parse expressions for the top-N offenders, collecting referenced gen names.
    chosen: list[tuple[str, dict, tuple[list[tuple[float, str]], str, float]]] = []
    referenced: set[str] = set()
    for name in list(breakdown)[:top_n]:
        uc = uc_by_name.get(name)
        if uc is None:
            continue
        parsed = _parse_uc_expression(uc.get("expression", ""))
        if parsed is None:
            continue
        chosen.append((name, breakdown[name], parsed))
        for _coeff, g_name in parsed[0]:
            referenced.add(g_name)

    if not chosen or not referenced:
        return {}

    # PLEXOS side: pull per-period MW for the referenced generators.
    plexos_gen = _pull_plexos_gen_by_period(accdb_path, referenced)

    # gtopt side: per-(gen_uid, block) MW from generation_sol.
    gen_sol_path = case_dir / "output" / "Generator" / "generation_sol.parquet"
    if not gen_sol_path.exists():
        return {}
    gen_sol = pq.read_table(gen_sol_path).to_pandas()
    referenced_uids = {uid_by_name[n] for n in referenced if n in uid_by_name}
    gtopt_gen: dict[str, dict[int, float]] = {}
    for uid_v, sub in gen_sol[gen_sol["uid"].astype(int).isin(referenced_uids)].groupby(
        "uid"
    ):
        gtopt_gen[name_by_uid[int(uid_v)]] = {
            int(r["block"]): float(r["value"]) for _, r in sub.iterrows()
        }

    # gtopt slack per (uc_uid, block).
    slack_path = case_dir / "output" / "UserConstraint" / "slack_sol.parquet"
    slack_pos_path = case_dir / "output" / "UserConstraint" / "slack_pos_sol.parquet"
    slack_neg_path = case_dir / "output" / "UserConstraint" / "slack_neg_sol.parquet"
    slack_by_uc_block: dict[int, dict[int, float]] = {}
    for path in (slack_path, slack_pos_path, slack_neg_path):
        if not path.exists():
            continue
        df = pq.read_table(path).to_pandas()
        for (uc_uid, blk), val in df.groupby(["uid", "block"])["value"].sum().items():
            slack_by_uc_block.setdefault(int(uc_uid), {})[int(blk)] = float(val)

    blocks = sorted(durations)
    out: dict[str, list[dict[str, float | str]]] = {}
    for uc_name, _item, (terms, op, rhs) in chosen:
        uc = uc_by_name[uc_name]
        uc_uid = int(uc["uid"])
        rows: list[dict[str, float | str]] = []
        slack_blk = slack_by_uc_block.get(uc_uid, {})
        for blk in blocks:
            p_lhs = sum(
                coeff * plexos_gen.get(gname, {}).get(blk, 0.0)
                for coeff, gname in terms
            )
            g_lhs = sum(
                coeff * gtopt_gen.get(gname, {}).get(blk, 0.0) for coeff, gname in terms
            )
            slack = slack_blk.get(blk, 0.0)
            if slack < 1e-6 and (
                op == ">=" and p_lhs >= rhs - 1e-6 and g_lhs >= rhs - 1e-6
            ):
                continue
            if slack < 1e-6 and (
                op == "<=" and p_lhs <= rhs + 1e-6 and g_lhs <= rhs + 1e-6
            ):
                continue
            rows.append(
                {
                    "block": blk,
                    "hours": durations[blk],
                    "rhs": rhs,
                    "op": op,
                    "plexos_lhs": p_lhs,
                    "gtopt_lhs": g_lhs,
                    "slack": slack,
                }
            )
        if rows:
            out[uc_name] = rows
    return out


def _render_uc_block_drilldown(
    drilldown: dict[str, list[dict[str, float | str]]],
    case_dir: Path,
    console: Console,
    *,
    max_blocks_per_uc: int = 30,
) -> None:
    """Render Step-2b per-block PLEXOS vs gtopt comparison for each
    UC with non-zero slack or a bound violation on either side."""
    if not drilldown:
        return

    bundle_path = next(
        (
            c
            for c in case_dir.glob("*.json")
            if c.name != "planning.json" and c.is_file()
        ),
        None,
    )
    expr_by_name: dict[str, str] = {}
    if bundle_path is not None:
        bundle = json.loads(bundle_path.read_text())
        for u in bundle["system"].get("user_constraint_array", []):
            expr_by_name[u["name"]] = u.get("expression", "")

    for uc_name, rows in drilldown.items():
        # Sort by slack descending so the worst blocks float to the top.
        rows = sorted(rows, key=lambda r: r["slack"], reverse=True)
        expr = expr_by_name.get(uc_name, "")
        title = (
            f"Per-block UC drilldown (Step 2b) — {uc_name}  "
            f"({len(rows)} block(s) with slack/violation)"
        )
        t = Table(title=title)
        t.add_column("Block", justify="right")
        t.add_column("Hours", justify="right")
        t.add_column("RHS", justify="right")
        t.add_column("op")
        t.add_column("PLEXOS LHS", justify="right")
        t.add_column("gtopt LHS", justify="right")
        t.add_column("gtopt Slack", justify="right")
        t.add_column("P meets?", justify="center")
        t.add_column("G meets?", justify="center")
        for r in rows[:max_blocks_per_uc]:
            # ``op`` is the only string field; the rest are numeric.
            op = str(r["op"])
            block = float(r["block"])
            hours = float(r["hours"])
            rhs = float(r["rhs"])
            plexos_lhs = float(r["plexos_lhs"])
            gtopt_lhs = float(r["gtopt_lhs"])
            slack = float(r["slack"])
            p_meets = (
                (plexos_lhs >= rhs - 1e-6) if op == ">=" else (plexos_lhs <= rhs + 1e-6)
            )
            g_meets = (
                (gtopt_lhs >= rhs - 1e-6) if op == ">=" else (gtopt_lhs <= rhs + 1e-6)
            )
            t.add_row(
                str(int(block)),
                f"{hours:.0f}",
                f"{rhs:.3f}",
                op,
                f"{plexos_lhs:,.3f}",
                f"{gtopt_lhs:,.3f}",
                f"{slack:,.3f}",
                "✓" if p_meets else "✗",
                "✓" if g_meets else "✗",
            )
        console.print(t)
        if expr:
            console.print(f"[dim]expression: {expr}[/dim]")
        if len(rows) > max_blocks_per_uc:
            console.print(
                f"[dim]… {len(rows) - max_blocks_per_uc} more blocks "
                "with slack/violation (truncated).[/dim]"
            )


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
    # Horizon: the converter emits the FULL PCP forward-look horizon (the CEN
    # daily bundle is 1 stage × 168 hourly blocks = 7 days), matching PLEXOS's
    # 168-h horizon — it is NOT a 1-day slice.  So the cost TOTALS are over the
    # same span and directly comparable; the Step-3 "Block count / hours
    # covered" row confirms both cover the same horizon.  (Earlier this scout
    # wrongly divided PLEXOS by 7 on the false premise that gtopt ran 1 day.)
    # A large Δ here is therefore a cost-COMPOSITION signal — PLEXOS's MIP
    # objective vs gtopt's objective, which may include FCF / water-value
    # terminal terms, demand-fail or reserve-shortage penalty tiers, or a
    # different offer-curve integration — NOT a horizon artifact.
    console.print(
        "[dim]Horizon: gtopt and PLEXOS both cover the full PCP forward-look "
        "(see the Step-3 block-count row); cost totals are directly "
        "comparable — no per-day rescaling.[/dim]"
    )
    if abs(rel) < 2.0:
        console.print(
            "[green]Within 2% of PLEXOS MIP — looks healthy. "
            "Proceed to Step 2 (per-unit dispatch diff).[/green]"
        )
    elif delta > 0:
        console.print(
            "[yellow]gtopt objective > PLEXOS by "
            f"{rel:+.1f}% (same horizon).  Drill into cost composition: FCF / "
            "water-value terminal terms, demand_fail_cost, or reserve-shortage "
            "penalty — not a horizon effect.[/yellow]"
        )
    else:
        console.print(
            f"[yellow]gtopt objective < PLEXOS by {rel:+.1f}% (same horizon).  "
            "Likely cost terms gtopt omits that PLEXOS integrates (multi-band "
            "offer curves, start/shutdown); cross-check the Step-3 dispatch $ "
            "and the pid-119 GenCost side-channel.[/yellow]"
        )


# ------------------------------------------------------------------
# Input plumbing helpers — let the user point at the gtopt OUTPUT
# directory and the bundle directory directly, and resolve the PLEXOS
# RES zip from a single ``--date YYYYMMDD`` argument.  The legacy
# ``--gtopt-case`` / ``--plexos-res-zip`` / ``--plexos-log`` flags
# remain fully supported; the new flags layer on top of them so the
# canonical one-liner becomes:
#
#     python -m plexos2gtopt.compare_with_plexos \
#         --date 20260422 \
#         --gtopt-output /home/marce/tmp/.../<run>_out \
#         --gtopt-bundle /home/marce/tmp/.../<run>
#
# Resolution rules (see ``_resolve_inputs``):
#   * ``--gtopt-output`` wins over ``--gtopt-case`` (one-line warning).
#   * ``--date`` resolves the RES zip from the local cache
#     (``~/.cache/gtopt/cen2gtopt/pcp_archive/PCP``) and auto-extracts
#     the nested RES from the outer ``PLEXOS<date>.zip`` if needed.
# ------------------------------------------------------------------

_DEFAULT_PCP_ARCHIVE_REL = "gtopt/cen2gtopt/pcp_archive"


def _default_pcp_archive_dir() -> Path:
    """Return the local cache root used by cen2gtopt PCP archives.

    Honours ``$XDG_CACHE_HOME`` (falls back to ``~/.cache``) so a
    user with a non-standard cache layout still gets the right
    directory.  The PCP outer-zip layout lives at ``<root>/PCP/`` and
    the auto-extracted RES bundles land in ``<root>/_unpacked/``.
    """
    base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    return base / _DEFAULT_PCP_ARCHIVE_REL


def _resolve_res_zip_for_date(date_str: str) -> Path:
    """Return the path to ``RES<date>.zip`` for ``date_str`` (YYYYMMDD).

    Search order:
      1. ``<archive>/_unpacked/PCP_RES_<date>/RES<date>.zip`` —
         a previous auto-extraction.
      2. ``<archive>/PCP/RES<date>.zip`` — a sibling RES zip dropped
         directly in the cache.
      3. ``<archive>/PCP/PLEXOS<date>.zip`` — the outer CEN bundle
         that contains both ``DATOS<date>.zip`` and ``RES<date>.zip``
         as siblings.  Auto-extract the RES one to
         ``<archive>/_unpacked/PCP_RES_<date>/RES<date>.zip`` so the
         second invocation hits step (1).

    Raises ``FileNotFoundError`` with the full list of checked paths
    when none of the candidates resolves — diagnostic over guesswork.
    """
    if not re.fullmatch(r"\d{8}", date_str):
        raise ValueError(
            f"--date must be YYYYMMDD (got {date_str!r}); e.g. --date 20260422"
        )

    archive = _default_pcp_archive_dir()
    pcp_dir = archive / "PCP"
    unpacked_dir = archive / "_unpacked" / f"PCP_RES_{date_str}"
    cached_res = unpacked_dir / f"RES{date_str}.zip"
    sibling_res = pcp_dir / f"RES{date_str}.zip"
    outer_zip = pcp_dir / f"PLEXOS{date_str}.zip"

    if cached_res.is_file():
        return cached_res
    if sibling_res.is_file():
        return sibling_res
    if outer_zip.is_file():
        unpacked_dir.mkdir(parents=True, exist_ok=True)
        inner_name = f"RES{date_str}.zip"
        with zipfile.ZipFile(outer_zip) as outer:
            if inner_name not in outer.namelist():
                raise FileNotFoundError(
                    f"{outer_zip} does not contain {inner_name} "
                    f"(found: {outer.namelist()})"
                )
            outer.extract(inner_name, unpacked_dir)
        if not cached_res.is_file():  # defensive — extract should have produced it
            raise FileNotFoundError(
                f"auto-extracted {inner_name} from {outer_zip} but "
                f"{cached_res} is missing"
            )
        return cached_res

    raise FileNotFoundError(
        "no PLEXOS RES zip found for --date "
        f"{date_str}; checked:\n  - {cached_res}\n  - {sibling_res}\n"
        f"  - {outer_zip} (would auto-extract {f'RES{date_str}.zip'})"
    )


def _find_bundle_json(bundle_dir: Path) -> Path:
    """Return the PLEXOS-source JSON inside ``bundle_dir``.

    Uses the same filtering rules as the in-tree ``compute_gtopt_*``
    helpers: any ``*.json`` that is NOT ``planning.json`` and NOT
    a ``*.provenance.json`` sidecar.  This is the single source of
    truth for the bundle-JSON discovery so the stitched case dir we
    build for ``--gtopt-output`` mirrors the layout the helpers expect.

    Raises ``FileNotFoundError`` listing the inspected directory if
    nothing matches.
    """
    for cand in sorted(bundle_dir.glob("*.json")):
        if cand.name == "planning.json":
            continue
        if cand.name.endswith(".provenance.json"):
            continue
        return cand
    raise FileNotFoundError(
        f"no PLEXOS-source JSON found in {bundle_dir} "
        "(looked for *.json, ignored planning.json and *.provenance.json)"
    )


def _stitch_gtopt_case_dir(
    output_dir: Path,
    bundle_dir: Path | None,
) -> Path:
    """Return a ``case_dir`` Path that satisfies the legacy layout.

    The legacy helpers expect ``<case>/output/solution.csv`` plus
    ``<case>/<bundle>.json`` (the PLEXOS-source JSON used to recover
    block durations, demand uids, etc.).  When the user passes
    ``--gtopt-output`` directly we need to bridge to that contract
    WITHOUT requiring an ``output/`` symlink in the user's workspace.

    Implementation: build a fresh temp directory containing two
    symlinks — ``output -> output_dir`` and ``<bundle>.json ->
    bundle_dir/<bundle>.json``.  This adds zero copies, keeps the
    bundle / output identity stable for cache keys, and lets every
    existing helper run unchanged.

    When ``bundle_dir`` is ``None`` only the ``output`` symlink is
    created — Steps 3-10 then surface the usual
    ``no PLEXOS-source JSON`` error so the user knows to pass
    ``--gtopt-bundle``.
    """
    output_dir = output_dir.resolve()
    if not output_dir.is_dir():
        raise FileNotFoundError(f"--gtopt-output: {output_dir} is not a directory")
    if not (output_dir / "solution.csv").is_file():
        raise FileNotFoundError(
            f"--gtopt-output: {output_dir} has no solution.csv at its root "
            "(expected a gtopt OUTPUT directory; pass the bundle dir with "
            "--gtopt-bundle instead)"
        )

    stitched = Path(tempfile.mkdtemp(prefix="gtopt_compare_case_"))
    (stitched / "output").symlink_to(output_dir, target_is_directory=True)

    if bundle_dir is not None:
        bundle_dir = bundle_dir.resolve()
        if not bundle_dir.is_dir():
            raise FileNotFoundError(f"--gtopt-bundle: {bundle_dir} is not a directory")
        bundle_json = _find_bundle_json(bundle_dir)
        (stitched / bundle_json.name).symlink_to(bundle_json)

    return stitched


def _resolve_inputs(
    args: argparse.Namespace, console: Console
) -> tuple[Path | None, Path | None]:
    """Resolve ``(gtopt_case_dir, plexos_res_zip)`` from CLI args.

    Returns ``(case_dir, res_zip)`` honouring the precedence rules:
      * ``--gtopt-output`` > ``--gtopt-case`` (warn when both given;
        the new flag wins, with the bundle dir grafted in).
      * ``--date`` > ``--plexos-res-zip`` > ``--plexos-log``
        (warn when both ``--date`` and ``--plexos-res-zip`` given).

    Either side may return ``None`` when the user supplied no source —
    e.g. ``--plexos-log`` mode (no .accdb available) still works, and
    omitting the gtopt side prints only the PLEXOS summary.
    """
    # ---- gtopt side ----
    case_dir: Path | None = None
    if args.gtopt_output is not None:
        if args.gtopt_case is not None:
            console.print(
                "[yellow]--gtopt-case ignored: --gtopt-output takes "
                "precedence.[/yellow]"
            )
        bundle = args.gtopt_bundle
        case_dir = _stitch_gtopt_case_dir(args.gtopt_output, bundle)
    elif args.gtopt_case is not None:
        case_dir = args.gtopt_case
        if args.gtopt_bundle is not None:
            console.print(
                "[yellow]--gtopt-bundle ignored: --gtopt-case provides "
                "its own bundle layout.[/yellow]"
            )
    elif args.gtopt_bundle is not None:
        # Bundle without output dir — Step 1 cannot read solution.csv
        # and Steps 3-10 would have nothing to compare.  Surface
        # clearly rather than silently skip.
        console.print(
            "[yellow]--gtopt-bundle given without --gtopt-output (or "
            "--gtopt-case); the gtopt side will be skipped.[/yellow]"
        )

    # ---- PLEXOS side ----
    res_zip: Path | None = args.plexos_res_zip
    if args.date is not None:
        if args.plexos_res_zip is not None:
            console.print(
                "[yellow]--plexos-res-zip ignored: --date takes precedence.[/yellow]"
            )
        res_zip = _resolve_res_zip_for_date(args.date)
        console.print(f"[dim]Resolved PLEXOS RES zip from --date: {res_zip}[/dim]")

    return case_dir, res_zip


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
        help=(
            "gtopt case directory (contains output/solution.csv).  "
            "Legacy layout — prefer --gtopt-output + --gtopt-bundle."
        ),
    )
    parser.add_argument(
        "--gtopt-output",
        type=Path,
        default=None,
        help=(
            "gtopt OUTPUT directory (contains solution.csv at its root "
            "alongside per-class Parquet subdirs Battery/, Generator/, "
            "...).  Combine with --gtopt-bundle to point at the "
            "PLEXOS-source JSON without an output/ symlink dance."
        ),
    )
    parser.add_argument(
        "--gtopt-bundle",
        type=Path,
        default=None,
        help=(
            "gtopt BUNDLE directory (the dir plexos2gtopt emitted; "
            "contains the PLEXOS-source JSON, e.g. PCP_<date>.json).  "
            "Required for Steps 3-10 when --gtopt-output is used."
        ),
    )
    parser.add_argument(
        "--date",
        type=str,
        default=None,
        help=(
            "PCP date as YYYYMMDD; auto-resolves --plexos-res-zip from "
            f"{_default_pcp_archive_dir()}/PCP (auto-extracts the nested "
            "RES<date>.zip from PLEXOS<date>.zip when needed) and caches "
            "it under <archive>/_unpacked/PCP_RES_<date>/."
        ),
    )
    src = parser.add_mutually_exclusive_group(required=False)
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
            "the scout extracts the nested log automatically.  Prefer "
            "--date for the canonical one-liner."
        ),
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=None,
        help=(
            "directory for the pickled PLEXOS-extract cache.  Default: "
            "$XDG_CACHE_HOME/gtopt/plexos_compare (~/.cache/gtopt/"
            "plexos_compare).  One pickle per (RES zip, size, mtime, "
            "schema version); subsequent runs against the same bundle "
            "skip every mdb-export round-trip and the temporary accdb "
            "extraction."
        ),
    )
    parser.add_argument(
        "--no-cache",
        action="store_true",
        help=(
            "ignore any cached PLEXOS extract and force a full "
            "re-extraction.  Use after editing a compute_plexos_* "
            "function in this module without bumping "
            "_PLEXOS_CACHE_VERSION."
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
    parser.add_argument(
        "--width",
        type=int,
        default=180,
        help=(
            "console / Rich table width (default: 180).  CEN PCP line names "
            "(e.g. 'Colbun220->PNegro220') and thermal-variant unit names "
            "(e.g. 'KELAR-TG1+TG2+TV_GNL_E') push past the default 80-120 "
            "terminal width; widening to 180+ keeps the leftmost name "
            "column from ellipsis-truncating."
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    # Force a wide console so long line / unit names (e.g.
    # ``Colbun220->PNegro220`` and ``KELAR-TG1+TG2+TV_GNL_E``)
    # aren't auto-truncated to ``Colbun2…`` / ``KELAR-…``.  Rich's
    # default detects the terminal width (often 80-120 chars) and
    # ellipsis-truncates the leftmost name column to fit; on the
    # CEN PCP comparison the row labels are the diagnostic value,
    # so we widen well past the right-justified numeric columns
    # to avoid hiding them.
    console = Console(width=args.width)

    # Resolve the gtopt case dir and PLEXOS RES zip from the user's
    # mix of legacy + new flags (see ``_resolve_inputs`` for the
    # precedence rules).
    case_dir, res_zip = _resolve_inputs(args, console)

    # At least one PLEXOS source must be available (legacy
    # ``required=True`` mutex relaxed so ``--date`` can satisfy it).
    if args.plexos_log is None and res_zip is None:
        console.print(
            "[red]No PLEXOS source given — pass one of --date, "
            "--plexos-res-zip, or --plexos-log.[/red]"
        )
        return 2

    log_path: Path
    if args.plexos_log is not None:
        log_path = args.plexos_log
    else:
        assert res_zip is not None  # narrowed by the guard above
        log_path = find_plexos_log_in_res_bundle(res_zip)
        console.print(f"[dim]Extracted PLEXOS log: {log_path}[/dim]")

    plexos = parse_plexos_log(log_path)
    if not plexos:
        console.print(
            f"[red]No objective lines parsed from {log_path} — "
            "check the file format.[/red]"
        )
        return 1

    gtopt: dict[str, float] | None = None
    if case_dir is not None:
        try:
            gtopt = parse_gtopt_solution(case_dir)
        except (FileNotFoundError, ValueError) as exc:
            console.print(f"[yellow]gtopt side unavailable: {exc}[/yellow]")

    _render_report(plexos, gtopt, console)

    # ----- Step 3: duration-weighted solution totals -----
    # Both sides must have the .accdb (PLEXOS) and the gtopt
    # outputs.  We need the .accdb to recover the per-block duration
    # from t_phase_3 — same source plexos2gtopt uses to lay out the
    # bundle's block_array.
    accdb_path: Path | None = None
    if case_dir is not None and res_zip is not None:
        console.print()
        try:
            # ----- PLEXOS side: cache the parsed extracts -----
            # The accdb is parsed once, then a pickle of every
            # ``compute_plexos_*`` output is dropped in
            # ``$XDG_CACHE_HOME/gtopt/plexos_compare`` (or
            # ``--cache-dir``).  Subsequent runs against the same
            # RES zip skip mdb-export entirely (~minutes → seconds).
            cache_path = _plexos_cache_path(res_zip, args.cache_dir)
            plexos_data: dict | None = None
            if not args.no_cache:
                plexos_data = _load_plexos_cache(cache_path)
                if plexos_data is not None:
                    console.print(
                        f"[dim]Using cached PLEXOS extract: {cache_path}[/dim]"
                    )
            # Persistent scratch for the .accdb so the Step-2b
            # drilldown can re-read per-period generation lazily
            # (cache-hit doesn't keep the accdb around otherwise).
            # Cleaned up at process exit by the OS-level /tmp policy.
            accdb_persist_dir = Path(tempfile.mkdtemp(prefix="plexos_compare_"))
            accdb_path = _extract_accdb_from_res_zip(res_zip, accdb_persist_dir)
            if plexos_data is None:
                plexos_data = _compute_plexos_all(accdb_path)
                _save_plexos_cache(cache_path, plexos_data)
                console.print(f"[dim]Saved PLEXOS extract to {cache_path}[/dim]")

            # Step 3 — system totals (demand, gen, op cost)
            plexos_tot = plexos_data["totals"]
            gtopt_tot = compute_gtopt_energy_totals(case_dir)
            # Step 4 — per-unit SRMC + dispatch
            plexos_unit = plexos_data["unit"]
            gtopt_unit = compute_gtopt_per_unit_srmc(case_dir)
            # Step 5 — per-bus LMP
            plexos_bus = plexos_data["bus"]
            gtopt_bus = compute_gtopt_per_bus_lmp(case_dir)
            # Step 6 — per-line energy + limits + active
            plexos_line = plexos_data["line"]
            gtopt_line = compute_gtopt_per_line(case_dir)
            # Step 7 — generation by technology
            plexos_tech = plexos_data["tech"]
            gtopt_tech = compute_gtopt_generation_by_technology(
                case_dir, cat_by_name=plexos_data["name_to_category"]
            )
            # Step 8 — reservoir trajectories
            plexos_res = plexos_data["res"]
            gtopt_res = compute_gtopt_reservoir_volumes(case_dir)
            # Step 8b — reservoir water values ($/CMD)
            plexos_res_wv = plexos_data.get("res_wv", {})
            try:
                gtopt_res_wv = compute_gtopt_reservoir_water_value(case_dir)
            except FileNotFoundError:
                gtopt_res_wv = {}
            # Step 9 — battery operation
            plexos_bat = plexos_data["bat"]
            gtopt_bat = compute_gtopt_battery_operation(case_dir)
            # Step 10 — generator commitment + cost of commitment
            plexos_commit = plexos_data.get("commit", {})
            try:
                gtopt_commit = compute_gtopt_commitment(case_dir)
            except FileNotFoundError:
                gtopt_commit = {}
            if plexos_tot and gtopt_tot:
                # Recompute PLEXOS line losses from PLEXOS's own flows with the
                # same R/V²·|f|²·dt formula gtopt uses — isolates flow-driven
                # from model-driven loss differences (PLEXOS models losses
                # post-solve, not in the objective).
                if case_dir is not None:
                    plexos_tot["losses_analytic_from_flows_mwh"] = (
                        _compute_plexos_line_losses_analytic_mwh(
                            plexos_data.get("line", {}), case_dir
                        )
                    )
                # Water cost (Σ net-drawdown × |boundary water value|) on the
                # common net-storage basis, when boundary_cuts.csv is found.
                # The same formula is applied to each side's own eini/efin, so
                # _render_solution_compare can add Water $ / Operational+Water $
                # rows.  boundary_cuts.csv lives in the bundle dir (or the
                # output's parent under the mip-loop layout).
                _water_coef = _read_boundary_cut_coeffs(
                    args.gtopt_bundle,
                    Path(args.gtopt_output).parent if args.gtopt_output else None,
                    case_dir,
                )
                if _water_coef:
                    plexos_tot["water_cost_usd"] = _reservoir_water_cost(
                        plexos_res, _water_coef
                    )
                    gtopt_tot["water_cost_usd"] = _reservoir_water_cost(
                        gtopt_res, _water_coef
                    )
                _render_solution_compare(plexos_tot, gtopt_tot, console)
                # Step 3b — problem size + total run time, PLEXOS vs gtopt.
                if case_dir is not None:
                    gtopt_ps = compute_gtopt_problem_stats(case_dir)
                    plexos_ps = {
                        "rows": 0.0,
                        "cols": 0.0,
                        "int_vars": 0.0,
                        "run_s": 0.0,
                    }
                    if res_zip is not None:
                        try:
                            log_path = find_plexos_log_in_res_bundle(res_zip)
                            plexos_ps = compute_plexos_problem_stats(log_path)
                        except (FileNotFoundError, OSError) as exc:
                            console.print(
                                f"[yellow]Step 3b: could not read PLEXOS log "
                                f"({exc}); showing gtopt only.[/yellow]"
                            )
                    console.print()
                    _render_problem_size_compare(plexos_ps, gtopt_ps, console)
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
            elif plexos_bus and not gtopt_bus:
                console.print()
                console.print(
                    "[yellow]Step 5 (per-bus LMP) skipped: gtopt wrote no "
                    "bus balance duals (output/Bus/balance_dual.parquet is "
                    "empty).  MIP runs do not emit LP duals — re-run with "
                    "--write-out dual on an LP-relaxed solve, or solve the "
                    "LP relaxation, to populate the bus-LMP comparison.[/yellow]"
                )
            if plexos_line and gtopt_line:
                console.print()
                _render_line_compare(plexos_line, gtopt_line, console)
            if plexos_tech and gtopt_tech:
                console.print()
                _render_technology_compare(plexos_tech, gtopt_tech, console)
            if plexos_res and gtopt_res:
                console.print()
                _render_reservoir_compare(plexos_res, gtopt_res, console)
            if plexos_res_wv and gtopt_res_wv:
                console.print()
                _render_water_value_compare(plexos_res_wv, gtopt_res_wv, console)
            elif plexos_res_wv and not gtopt_res_wv:
                console.print()
                console.print(
                    "[yellow]Step 8b (reservoir water value) skipped: gtopt "
                    "wrote no Reservoir/water_value_dual.parquet (MIP runs do "
                    "not emit LP duals — re-run an LP-relaxed solve to "
                    "populate it).[/yellow]"
                )
            if plexos_bat and gtopt_bat:
                console.print()
                _render_battery_compare(plexos_bat, gtopt_bat, console)
            if plexos_commit and gtopt_commit:
                console.print()
                _render_commitment_compare(plexos_commit, gtopt_commit, console)
        except (FileNotFoundError, RuntimeError, ImportError) as exc:
            console.print(f"[yellow]Step 3-10 skipped: {exc}[/yellow]")

    if case_dir is not None and not args.no_uc_drilldown:
        console.print()
        try:
            breakdown = load_uc_penalty_breakdown(case_dir, args.uc_penalty)
        except (FileNotFoundError, ImportError, ValueError) as exc:
            console.print(f"[yellow]UC drilldown unavailable: {exc}[/yellow]")
        else:
            _render_uc_drilldown(breakdown, args.uc_penalty, args.top_uc, console)
            # Step 2b — per-block PLEXOS vs gtopt LHS for the top-N
            # slacking UCs.  Requires the accdb (extracted as a
            # side-effect of Step 3) so we can pull each referenced
            # generator's per-period MW.  Skipped when the accdb isn't
            # available (e.g. ``--plexos-log`` mode).
            if accdb_path is not None and breakdown:
                try:
                    drilldown = compute_uc_block_drilldown(
                        case_dir,
                        accdb_path,
                        breakdown,
                        top_n=args.top_uc,
                    )
                except (FileNotFoundError, RuntimeError, ImportError) as exc:
                    console.print(f"[yellow]Step 2b skipped: {exc}[/yellow]")
                else:
                    if drilldown:
                        console.print()
                        _render_uc_block_drilldown(drilldown, case_dir, console)

    return 0


if __name__ == "__main__":
    sys.exit(main())
