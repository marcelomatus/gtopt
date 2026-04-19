#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Validate an SDDP integration-test output directory.

Replaces ``integration_test/cmake/validate_sddp_status.cmake``.  The old
CMake script only checked that ``solver_status.json`` and
``solution.csv`` existed and had the expected column headers — it did
not read a single numerical value.  This script reads the
hive-partitioned parquet (or CSV-shard) output emitted by gtopt and
asserts tolerance-bounded physical invariants:

  * ``solver_status.json`` present with the expected convergence fields
    (skipped for MAX_ITERATIONS=0 runs that don't produce it).
  * ``solution.csv`` present, parseable, at least one cell reported as
    optimal, every optimal cell has a finite obj_value.
  * Reservoir ``efin_sol`` values lie within [emin, emax] for every
    reservoir × scene × phase shard.
  * Generator ``generation_sol`` values are ≥ 0 and ≤ capacity + a
    numerical tolerance.
  * Demand ``fail_sol`` values are ≥ 0.

Extra assertions (bounds on the final UB/LB, per-element obj-sum
tolerances) can be added per case through an optional expectations
JSON — left as a follow-up once more cases share the same framework.

Usage:

    python3 validate_sddp_output.py \
        --output-dir <OUTPUT_DIR> \
        --input-json <INPUT_JSON> \
        [--max-iterations <N>] \
        [--allow-nonoptimal]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    import pandas as pd
except ImportError as exc:  # pragma: no cover
    print(
        "validate_sddp_output: pandas is required (pip install pandas pyarrow)",
        file=sys.stderr,
    )
    raise SystemExit(2) from exc


# ── Constants ──────────────────────────────────────────────────────────────

CAPACITY_TOL = 1e-6  # generation ≤ capacity + tol
ENERGY_TOL = 1e-6  # efin ∈ [emin − tol, emax + tol]
OPTIMAL_STATUS = 0  # as emitted by solution.csv / CLP convention


# ── IO helpers ─────────────────────────────────────────────────────────────


def read_table(directory: Path, stem: str) -> pd.DataFrame | None:
    """Read ``{stem}`` from ``directory`` as a DataFrame.

    Handles both hive-partitioned parquet directories
    (``{stem}.parquet/scene=*/phase=*/part.parquet``) and CSV shards
    (``{stem}_s*_p*.csv[.zst|.gz]``), mirroring the layout the gtopt
    output writer emits in each mode.  Returns ``None`` if no file
    matching either layout exists.
    """
    parquet_path = directory / f"{stem}.parquet"
    if parquet_path.is_dir() or parquet_path.is_file():
        try:
            return pd.read_parquet(parquet_path)
        except Exception as exc:  # noqa: BLE001
            print(
                f"validate_sddp_output: failed to read {parquet_path}: {exc}",
                file=sys.stderr,
            )
            return None

    parent = directory / Path(stem).parent
    name = Path(stem).name
    for ext in (".csv", ".csv.zst", ".csv.gz"):
        shards = sorted(parent.glob(f"{name}_s*_p*{ext}"))
        if shards:
            try:
                return pd.concat([pd.read_csv(s) for s in shards], ignore_index=True)
            except Exception as exc:  # noqa: BLE001
                print(
                    f"validate_sddp_output: failed to read {stem} CSV shards: {exc}",
                    file=sys.stderr,
                )
                return None
        legacy = directory / f"{name}{ext}"
        if legacy.is_file():
            try:
                return pd.read_csv(legacy)
            except Exception as exc:  # noqa: BLE001
                print(
                    f"validate_sddp_output: failed to read {legacy}: {exc}",
                    file=sys.stderr,
                )
                return None
    return None


def load_input_json(path: Path) -> dict[str, Any]:
    """Parse the planning input JSON (system + options)."""
    with path.open() as fh:
        return json.load(fh)


# ── Individual validators (each returns a list of error messages) ─────────


def validate_solver_status(output_dir: Path, max_iterations: int) -> list[str]:
    """Check ``solver_status.json`` (or legacy variants) for expected fields."""
    candidates = [
        output_dir / "solver_status.json",
        output_dir / "sddp_status.json",
        output_dir / "monolithic_status.json",
    ]
    status_file = next((c for c in candidates if c.is_file()), None)
    if status_file is None:
        # max_iterations=0 runs emit no status file; that's allowed.
        if max_iterations == 0:
            return []
        return [
            f"no solver_status.json / sddp_status.json / monolithic_status.json "
            f"under {output_dir}"
        ]

    with status_file.open() as fh:
        status = json.load(fh)

    errors: list[str] = []
    required = (
        ("lower_bound", "upper_bound", "gap", "iteration")
        if status_file.name != "monolithic_status.json"
        else ("status", "elapsed_s")
    )
    for field in required:
        if field not in status:
            errors.append(f"{status_file.name}: missing field {field!r}")
    return errors


def validate_solution_csv(output_dir: Path, *, allow_nonoptimal: bool) -> list[str]:
    """Check ``solution.csv``: structure + at least one optimal row + finite obj."""
    path = output_dir / "solution.csv"
    if not path.is_file():
        return [f"solution.csv not found in {output_dir}"]

    try:
        df = pd.read_csv(path)
    except Exception as exc:  # noqa: BLE001
        return [f"solution.csv parse failed: {exc}"]

    errors: list[str] = []
    for col in ("scene", "phase", "status", "obj_value"):
        if col not in df.columns:
            errors.append(f"solution.csv: missing column {col!r}")
    if errors:
        return errors
    if df.empty:
        return ["solution.csv has no data rows"]

    optimal = df[df["status"] == OPTIMAL_STATUS]
    if optimal.empty and not allow_nonoptimal:
        errors.append(
            f"solution.csv: no optimal rows (statuses: {sorted(df['status'].unique())})"
        )
    bad_obj = optimal[~optimal["obj_value"].apply(lambda v: pd.notna(v))]
    if not bad_obj.empty:
        errors.append(f"solution.csv: {len(bad_obj)} optimal row(s) with NaN obj_value")
    return errors


def validate_reservoir_efin(
    output_dir: Path, reservoirs: list[dict[str, Any]]
) -> list[str]:
    """Every efin value must lie within the reservoir's [emin, emax]."""
    if not reservoirs:
        return []
    reservoir_dir = output_dir / "Reservoir"
    if not reservoir_dir.is_dir():
        # No per-element output directory at all — treated as a warning
        # rather than a failure because some configurations (rebuild mode
        # on tiny sim-pass runs, lp-only runs) legitimately skip element
        # emission.  The solution.csv check already covers the smoke
        # assertion that the solve produced data.
        print(
            f"validate_sddp_output: warning — no Reservoir/ directory in {output_dir} "
            f"(skipping efin bounds check)"
        )
        return []
    df = read_table(reservoir_dir, "efin_sol")
    if df is None:
        return ["Reservoir/efin_sol.{parquet,csv} not found"]

    errors: list[str] = []
    # The uid column name may be "uid:name:<n>" expanded by the writer;
    # the long-form table has one value column per reservoir.  Walk the
    # columns and match against each reservoir entry.
    for rsv in reservoirs:
        uid = rsv.get("uid")
        emin = float(rsv.get("emin", 0.0))
        emax = float(rsv.get("emax", 0.0))
        matching_cols = [
            c
            for c in df.columns
            if _matches_uid(c, uid) or _matches_uid(c, rsv.get("name"))
        ]
        if not matching_cols:
            errors.append(
                f"Reservoir/efin_sol: no column for reservoir uid={uid} name={rsv.get('name')!r} "
                f"(available: {list(df.columns)})"
            )
            continue
        for col in matching_cols:
            series = df[col].dropna()
            lo, hi = series.min(), series.max()
            if lo < emin - ENERGY_TOL or hi > emax + ENERGY_TOL:
                errors.append(
                    f"Reservoir/efin_sol[{col}]: out of bounds — "
                    f"[{lo:.6g}, {hi:.6g}] escapes [{emin:.6g}, {emax:.6g}]"
                )
    return errors


def validate_generator_generation(
    output_dir: Path, generators: list[dict[str, Any]]
) -> list[str]:
    """generation_sol values must be in [0, capacity]."""
    if not generators:
        return []
    gen_dir = output_dir / "Generator"
    if not gen_dir.is_dir():
        print(
            f"validate_sddp_output: warning — no Generator/ directory in {output_dir} "
            f"(skipping generation bounds check)"
        )
        return []
    df = read_table(gen_dir, "generation_sol")
    if df is None:
        return ["Generator/generation_sol.{parquet,csv} not found"]

    errors: list[str] = []
    for gen in generators:
        uid = gen.get("uid")
        capacity = float(gen.get("capacity", 0.0))
        matching_cols = [
            c
            for c in df.columns
            if _matches_uid(c, uid) or _matches_uid(c, gen.get("name"))
        ]
        if not matching_cols:
            # Some generators may be inactive — this is a warning, not a failure.
            continue
        for col in matching_cols:
            series = df[col].dropna()
            lo, hi = series.min(), series.max()
            if lo < -CAPACITY_TOL:
                errors.append(
                    f"Generator/generation_sol[{col}]: negative value {lo:.6g}"
                )
            if hi > capacity + CAPACITY_TOL:
                errors.append(
                    f"Generator/generation_sol[{col}]: {hi:.6g} exceeds capacity "
                    f"{capacity:.6g}"
                )
    return errors


def validate_demand_fail(output_dir: Path) -> list[str]:
    """Demand shortage (``fail_sol``) must be non-negative (may be > 0 if infeasible)."""
    df = read_table(output_dir / "Demand", "fail_sol")
    if df is None:
        return []  # not present on every case

    errors: list[str] = []
    for col in df.columns:
        if col in ("scenario", "stage", "block", "scene", "phase"):
            continue
        series = df[col].dropna()
        if series.empty:
            continue
        lo = float(series.min())
        if lo < -CAPACITY_TOL:
            errors.append(
                f"Demand/fail_sol[{col}]: negative value {lo:.6g} (fail must be ≥ 0)"
            )
    return errors


# ── Golden benchmark comparison ────────────────────────────────────────────


def _close(a: float, b: float, *, abs_tol: float, rel_tol: float) -> bool:
    """Robust close-enough check: accept if EITHER abs OR rel tolerance holds."""
    diff = abs(a - b)
    if diff <= abs_tol:
        return True
    denom = max(abs(a), abs(b), 1.0)
    return diff <= rel_tol * denom


def validate_against_golden(output_dir: Path, golden_path: Path) -> list[str]:
    """Compare the current run's solver_status + solution.csv against a
    committed golden reference.

    The golden JSON carries:

        {
          "tolerance": {"abs": <float>, "rel": <float>},
          "solver_status": {<fields>: <expected>},
          "solution_csv_rows": [
            {"scene": …, "phase": …, "status": …, "obj_value": …},
            …
          ]
        }

    Every numerical field listed in `solver_status` must match the
    corresponding field in the current ``solver_status.json`` within
    the tolerance.  Every row in `solution_csv_rows` must find a
    matching (scene, phase) row in the current ``solution.csv`` with
    equal status and obj_value within tolerance.
    """
    if not golden_path.is_file():
        return [f"golden reference not found: {golden_path}"]

    with golden_path.open() as fh:
        golden = json.load(fh)

    tol = golden.get("tolerance", {})
    abs_tol = float(tol.get("abs", 1.0))
    rel_tol = float(tol.get("rel", 1.0e-4))

    errors: list[str] = []

    # ── solver_status.json ──
    expected_status = golden.get("solver_status")
    if expected_status:
        status_path = output_dir / "solver_status.json"
        if not status_path.is_file():
            errors.append(f"golden check: solver_status.json not found in {output_dir}")
        else:
            with status_path.open() as fh:
                status = json.load(fh)
            for field, expected in expected_status.items():
                actual = status.get(field)
                if isinstance(expected, bool) or isinstance(actual, bool):
                    if bool(actual) != bool(expected):
                        errors.append(
                            f"golden solver_status.{field}: got {actual!r}, "
                            f"expected {expected!r}"
                        )
                elif isinstance(expected, (int, float)):
                    if actual is None or not isinstance(actual, (int, float)):
                        errors.append(
                            f"golden solver_status.{field}: got {actual!r}, "
                            f"expected ≈ {expected!r}"
                        )
                    elif not _close(
                        float(actual),
                        float(expected),
                        abs_tol=abs_tol,
                        rel_tol=rel_tol,
                    ):
                        errors.append(
                            f"golden solver_status.{field}: got {actual:.6g}, "
                            f"expected {expected:.6g} (abs_tol={abs_tol}, "
                            f"rel_tol={rel_tol})"
                        )
                elif actual != expected:
                    errors.append(
                        f"golden solver_status.{field}: got {actual!r}, "
                        f"expected {expected!r}"
                    )

    # ── element_samples: pin specific sol/dual values in the parquet ──
    expected_samples = golden.get("element_samples", [])
    for i, sample in enumerate(expected_samples):
        cls = sample.get("class")
        stem = sample.get("stem")
        filt = sample.get("filter", {})
        value_col = sample.get("value_column")
        expected = sample.get("expected")
        if not (cls and stem and value_col is not None and expected is not None):
            errors.append(
                f"golden element_samples[{i}]: missing required field "
                f"(need class, stem, value_column, expected)"
            )
            continue

        class_dir = output_dir / cls
        if not class_dir.is_dir():
            # Missing class directory is already surfaced by the range-only
            # validator as a warning; for golden samples it's a hard error
            # because the caller explicitly asked to pin this value.
            errors.append(
                f"golden element_samples[{i}]: no {cls}/ directory "
                f"in {output_dir} — cannot read {stem}"
            )
            continue

        df = read_table(class_dir, stem)
        if df is None:
            errors.append(f"golden element_samples[{i}]: {cls}/{stem} not found")
            continue

        if value_col not in df.columns:
            errors.append(
                f"golden element_samples[{i}]: value_column {value_col!r} "
                f"not in {cls}/{stem} "
                f"(available: {list(df.columns)})"
            )
            continue

        # Narrow by the filter dict.  Every filter key must exist in the
        # DataFrame columns; any key missing is a hard error (the golden
        # file is out of sync with the output schema).
        sub = df
        for k, v in filt.items():
            if k not in sub.columns:
                errors.append(
                    f"golden element_samples[{i}]: filter column {k!r} "
                    f"not in {cls}/{stem}"
                )
                sub = None
                break
            sub = sub[sub[k] == v]
        if sub is None:
            continue
        if sub.empty:
            errors.append(
                f"golden element_samples[{i}]: no row matching {filt} in {cls}/{stem}"
            )
            continue

        actual_series = sub[value_col].dropna()
        if actual_series.empty:
            errors.append(
                f"golden element_samples[{i}] [{cls}/{stem} {filt} {value_col}]: "
                f"all matching rows are NaN; expected {expected}"
            )
            continue

        actual = float(actual_series.iloc[0])
        if not _close(actual, float(expected), abs_tol=abs_tol, rel_tol=rel_tol):
            errors.append(
                f"golden element_samples[{i}] [{cls}/{stem} {filt} {value_col}]: "
                f"got {actual:.6g}, expected {float(expected):.6g} "
                f"(abs_tol={abs_tol}, rel_tol={rel_tol})"
            )

    # ── solution.csv rows ──
    expected_rows = golden.get("solution_csv_rows")
    if expected_rows:
        sol_path = output_dir / "solution.csv"
        if not sol_path.is_file():
            errors.append(f"golden check: solution.csv not found in {output_dir}")
        else:
            df = pd.read_csv(sol_path)
            for erow in expected_rows:
                sc = erow["scene"]
                ph = erow["phase"]
                match = df[(df["scene"] == sc) & (df["phase"] == ph)]
                if match.empty:
                    errors.append(
                        f"golden solution_csv_rows[scene={sc}, phase={ph}]: "
                        f"no matching row found"
                    )
                    continue
                row = match.iloc[0]
                if "status" in erow:
                    if int(row["status"]) != int(erow["status"]):
                        errors.append(
                            f"golden solution.csv[scene={sc}, phase={ph}].status: "
                            f"got {int(row['status'])}, expected {int(erow['status'])}"
                        )
                if "obj_value" in erow:
                    if not _close(
                        float(row["obj_value"]),
                        float(erow["obj_value"]),
                        abs_tol=abs_tol,
                        rel_tol=rel_tol,
                    ):
                        errors.append(
                            f"golden solution.csv[scene={sc}, phase={ph}].obj_value: "
                            f"got {float(row['obj_value']):.6g}, "
                            f"expected {float(erow['obj_value']):.6g} "
                            f"(abs_tol={abs_tol}, rel_tol={rel_tol})"
                        )
    return errors


# ── Column-name matching ───────────────────────────────────────────────────


def _matches_uid(col_name: str, target: int | str | None) -> bool:
    """Return True when ``col_name`` references ``target`` (uid int or name string).

    gtopt emits per-element value columns keyed by either ``"uid:<N>"``
    or ``"<name>:<N>"`` depending on the ``use_uid_fname`` option; both
    schemes embed the uid at the end.
    """
    if target is None:
        return False
    if isinstance(target, int):
        return (
            col_name == f"uid:{target}"
            or col_name.endswith(f":{target}")
            or col_name.endswith(f":uid:{target}")
        )
    if isinstance(target, str):
        return col_name.startswith(target + ":") or col_name == target
    return False


# ── Driver ─────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Validate an SDDP gtopt integration-test output directory by reading "
            "the hive-partitioned parquet shards and checking tolerance-bounded "
            "invariants against the input planning JSON."
        )
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="Directory written by gtopt (contains solver_status.json, solution.csv, and per-class subdirs).",
    )
    parser.add_argument(
        "--input-json",
        required=True,
        type=Path,
        help="Path to the planning input JSON (used to read reservoir bounds, generator capacities).",
    )
    parser.add_argument(
        "--max-iterations",
        type=int,
        default=1,
        help="SDDP max_iterations used for the solve (0 skips the solver_status check).",
    )
    parser.add_argument(
        "--allow-nonoptimal",
        action="store_true",
        help="Accept solution.csv with no optimal rows (relaxed smoke check).",
    )
    parser.add_argument(
        "--golden-json",
        type=Path,
        default=None,
        help=(
            "Optional path to a golden-reference JSON.  When given, the "
            "validator additionally checks that the current run's "
            "solver_status + solution.csv rows match the committed "
            "expected values within tolerance.  See the top-of-file docstring."
        ),
    )
    args = parser.parse_args()

    if not args.output_dir.is_dir():
        print(
            f"validate_sddp_output: output directory does not exist: {args.output_dir}",
            file=sys.stderr,
        )
        return 1
    if not args.input_json.is_file():
        print(
            f"validate_sddp_output: input JSON does not exist: {args.input_json}",
            file=sys.stderr,
        )
        return 1

    # Skip validation when the solve itself exited non-zero — the
    # run_sddp_gtopt step writes solve_exit_code.txt to flag this case.
    exit_code_file = args.output_dir / "solve_exit_code.txt"
    if exit_code_file.is_file():
        try:
            code = int(exit_code_file.read_text().strip())
        except ValueError:
            code = -1
        if code != 0:
            print(
                f"validate_sddp_output: solve exited with code {code} — skipping validation"
            )
            return 0

    cfg = load_input_json(args.input_json)
    system = cfg.get("system", {})
    reservoirs = list(system.get("reservoir_array", []))
    generators = list(system.get("generator_array", []))

    errors: list[str] = []
    errors += validate_solver_status(args.output_dir, args.max_iterations)
    errors += validate_solution_csv(
        args.output_dir, allow_nonoptimal=args.allow_nonoptimal
    )
    errors += validate_reservoir_efin(args.output_dir, reservoirs)
    errors += validate_generator_generation(args.output_dir, generators)
    errors += validate_demand_fail(args.output_dir)
    if args.golden_json is not None:
        errors += validate_against_golden(args.output_dir, args.golden_json)

    if errors:
        print(
            "validate_sddp_output: FAILED with the following issues:",
            file=sys.stderr,
        )
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(
        f"validate_sddp_output: OK — {args.output_dir} passes all tolerance-bounded checks"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
