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
    members = {int(r["membership_id"]): r for r in _dump("t_membership")}

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

    return {
        "load_mwh": _energy_mwh(966),  # Region Load
        "gen_mwh": _energy_mwh(970),  # Region Generation
        "gen_unit_mwh": _energy_mwh(2),  # Generator-class Generation
        "gen_cost_usd": _block_sum(119),  # Generation Cost (block-$)
        "block_count": len(durations),
        "hours_covered": sum(durations.values()),
    }


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

    return {
        "gen_mwh": _sum_mwh("Generator/generation_sol.parquet"),
        "load_mwh": _sum_mwh("Demand/load_sol.parquet"),
        "fail_mwh": _sum_mwh("Demand/fail_sol.parquet"),
        "op_cost_usd": op_cost,
        "block_count": len(block_array),
        "hours_covered": sum(durations.values()),
    }


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
    _row("Demand [MWh]", plexos_tot["load_mwh"], gtopt_tot["load_mwh"])
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
                plexos_tot = compute_plexos_energy_totals(accdb_path)
                gtopt_tot = compute_gtopt_energy_totals(args.gtopt_case)
            if plexos_tot and gtopt_tot:
                _render_solution_compare(plexos_tot, gtopt_tot, console)
            else:
                console.print(
                    "[yellow]Step 3 skipped: could not compute "
                    "duration-weighted totals (missing .accdb or "
                    "gtopt parquets).[/yellow]"
                )
        except (FileNotFoundError, RuntimeError, ImportError) as exc:
            console.print(f"[yellow]Step 3 skipped: {exc}[/yellow]")

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
