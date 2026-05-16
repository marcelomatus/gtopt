#!/usr/bin/env python3
"""Validate gtopt's per-case solution output against an expected layout.

This is the Python port of the legacy
``integration_test/cmake/validate_solution.cmake`` script.  CMake's
``cmake -P`` flow worked, but had two recurring pain points:

* **Boolean coercion of "0".**  CMake treats the string ``"0"`` as
  false in ``if(NOT …)`` context, which made the obj_value-is-numeric
  check mis-fire for any case whose literature-derived optimum was
  zero (e.g. the new ``hydro_valley_sddpjl`` benchmark).  This script
  uses ``float()`` instead — no regex, no coercion.
* **Per-row CSV parsing via string replace.**  CMake's
  ``string(REPLACE "," ";" …)`` cannot handle quoted fields, embedded
  commas, or Windows line endings.  The Python ``csv`` module
  handles all of that.

Validations performed (mirror the .cmake script, kept identical so
``add_e2e_case`` produces the same e2e_<case>_validate_solution
test):

1. ``OUTPUT_DIR`` and ``EXPECTED_DIR`` both exist.
2. ``<OUTPUT_DIR>/solution.csv`` exists, has a header + ≥ 1 data row.
3. Header contains ``status`` and ``obj_value`` columns.
4. Each data row:
   * ``status`` is exactly ``"0"`` (optimal),
   * ``obj_value`` parses as a finite float.
5. Every ``*.csv`` reachable under ``EXPECTED_DIR`` is also present
   at the same relative path under ``OUTPUT_DIR`` (file-existence
   check only — value comparison lives in
   ``tools/gtopt_compare_csv.py``).

Exit code: 0 on success, 1 on any validation failure.

Usage
-----

::

    python tools/validate_solution.py \\
        --output-dir <case_build_output> \\
        --expected-dir <cases/<case>/output>

The previous CMake invocation::

    cmake -DOUTPUT_DIR=... -DEXPECTED_DIR=... -P validate_solution.cmake

is replaced in ``add_e2e_case.cmake`` with a ``python`` ``add_test``
call.  Both forms behave identically (same failure messages, same
exit codes).
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import NoReturn


REQUIRED_COLUMNS = ("status", "obj_value")


def _die(msg: str) -> NoReturn:
    """Print ``msg`` to stderr and exit with code 1.

    Mirrors the CMake ``message(FATAL_ERROR ...)`` semantics — the
    test harness treats stderr + non-zero exit as failure.
    """
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def _parse_solution_csv(solution_file: Path) -> tuple[list[str], list[dict[str, str]]]:
    """Read solution.csv and return (header, rows-as-dicts).

    Uses :mod:`csv` so quoted fields, embedded commas, and CRLF
    endings are handled identically to the C++ writer that produced
    the file.
    """
    with solution_file.open(newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        try:
            header = [col.strip() for col in next(reader)]
        except StopIteration:
            _die(f"solution.csv is empty: {solution_file}")
        rows: list[dict[str, str]] = []
        for raw in reader:
            stripped = [v.strip() for v in raw]
            if not any(stripped):  # skip blank lines (mirror CMake `continue()`)
                continue
            rows.append(dict(zip(header, stripped, strict=False)))
    return header, rows


def _validate_solution_csv(output_dir: Path) -> None:
    """Validate ``<output_dir>/solution.csv`` row-by-row."""
    solution_file = output_dir / "solution.csv"
    if not solution_file.is_file():
        _die(f"solution.csv not found in output directory: {output_dir}")

    header, rows = _parse_solution_csv(solution_file)

    if not rows:
        _die("solution.csv has no data rows (header only)")

    missing = [c for c in REQUIRED_COLUMNS if c not in header]
    if missing:
        _die(f"required column(s) {missing} not found in solution.csv header: {header}")

    for i, row in enumerate(rows, start=1):
        status_val = row.get("status", "")
        if status_val != "0":
            _die(f"Solution row {i}: status is not optimal (status={status_val!r})")

        obj_val = row.get("obj_value", "")
        try:
            obj_float = float(obj_val)
        except ValueError:
            _die(f"Solution row {i}: obj_value is not a valid number: {obj_val!r}")
        # Reject NaN / ±inf — solver should always emit a finite
        # objective on `status=0`.  CMake regex `^-?[0-9]+(...)$`
        # rejected these implicitly; keep parity here.
        if not math.isfinite(obj_float):
            _die(f"Solution row {i}: obj_value is not finite: {obj_val!r}")

        scene = row.get("scene", "?")
        phase = row.get("phase", "?")
        print(f"  scene={scene} phase={phase} status={status_val} obj_value={obj_val}")


def _validate_expected_files(output_dir: Path, expected_dir: Path) -> None:
    """Every ``*.csv`` under ``expected_dir`` must also live under
    ``output_dir`` (same relative path).  Value comparison is done
    by the separate ``compare_csv`` test step."""
    for expected in sorted(expected_dir.rglob("*.csv")):
        rel = expected.relative_to(expected_dir)
        actual = output_dir / rel
        if not actual.is_file():
            _die(f"Expected output file missing: {rel}")
        print(f"  Found: {rel}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Validate gtopt's per-case solution output against an "
            "expected directory layout."
        ),
        # When called from add_test() the binary path may contain a
        # generator-expression suffix; argparse passes those through
        # untouched.  This is the same contract the .cmake script had.
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="Directory the gtopt run wrote its results into.",
    )
    parser.add_argument(
        "--expected-dir",
        required=True,
        type=Path,
        help=(
            "Reference output directory shipped with the case; every "
            "CSV reachable below it must also exist at the same "
            "relative path under --output-dir."
        ),
    )
    args = parser.parse_args(argv)

    if not args.output_dir.is_dir():
        _die(f"Output directory does not exist: {args.output_dir}")
    if not args.expected_dir.is_dir():
        _die(f"Expected directory does not exist: {args.expected_dir}")

    _validate_solution_csv(args.output_dir)
    _validate_expected_files(args.output_dir, args.expected_dir)

    print("Solution validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
