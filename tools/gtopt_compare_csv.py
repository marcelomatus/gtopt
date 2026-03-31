#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare two CSV files with floating-point tolerance.

Designed for comparing gtopt solver output across different LP backends
(CLP, HiGHS, CPLEX) where numerical differences are expected.

Handles:
  - Signed zero: ``-0`` and ``0`` are treated as equal.
  - Near-zero values: absolute differences below *tolerance* pass.
  - The ``kappa`` column in ``solution.csv`` is skipped (solver metadata).
  - Numeric differences within *tolerance* are accepted silently.
  - Larger numeric differences (degenerate LP solutions) are logged as
    warnings but accepted by default.  Use ``--strict`` to treat them
    as errors.

Exit codes:
  0  Files match (within tolerance, or numeric-only diffs in default mode).
  1  Files differ (structural, header, non-numeric, or strict numeric).
  2  Usage / file-not-found error.
"""

from __future__ import annotations

import argparse
import re
import sys

_NUMERIC_RE = re.compile(r"^-?[0-9]+(\.[0-9]+(e[+-]?[0-9]+)?)?$", re.IGNORECASE)

# Columns that are solver-internal metadata and should be skipped.
_SKIP_COLUMNS = frozenset({"kappa", "max_kappa"})


def _is_numeric(value: str) -> bool:
    return _NUMERIC_RE.match(value) is not None


def _parse_float(value: str) -> float:
    """Parse a numeric string, treating '-0' as 0."""
    v = float(value)
    return 0.0 if v == 0.0 else v


def _parse_header(line: str) -> list[str]:
    """Return unquoted column names from a CSV header line."""
    return [f.strip().strip('"') for f in line.split(",")]


def compare_csv(
    actual_path: str,
    expected_path: str,
    tolerance: float = 1e-6,
    *,
    strict: bool = False,
    verbose: bool = False,
) -> tuple[list[str], list[str]]:
    """Compare two CSV files.

    Returns ``(errors, warnings)`` where *errors* are hard failures
    (structural, header, non-numeric mismatches, or strict-mode numeric)
    and *warnings* are numeric differences beyond tolerance that are
    accepted in non-strict mode.
    """
    with open(actual_path, encoding="utf-8") as f:
        actual_lines = f.read().splitlines()
    with open(expected_path, encoding="utf-8") as f:
        expected_lines = f.read().splitlines()

    errors: list[str] = []
    warnings: list[str] = []

    if len(actual_lines) != len(expected_lines):
        errors.append(
            f"Line count mismatch: actual={len(actual_lines)}, "
            f"expected={len(expected_lines)}"
        )
        return errors, warnings

    skip_cols: set[int] = set()

    for i, (actual_line, expected_line) in enumerate(zip(actual_lines, expected_lines)):
        line_num = i + 1

        # Detect header lines: first line, or lines containing quotes
        is_header = i == 0 or '"' in expected_line
        if is_header:
            if actual_line != expected_line:
                errors.append(
                    f"Header mismatch at line {line_num}:\n"
                    f"  Actual:   {actual_line}\n"
                    f"  Expected: {expected_line}"
                )
            # Detect columns to skip (e.g. kappa)
            headers = _parse_header(expected_line)
            for col_idx, name in enumerate(headers):
                if name in _SKIP_COLUMNS:
                    skip_cols.add(col_idx)
            continue

        # Data lines
        actual_fields = [f.strip() for f in actual_line.split(",")]
        expected_fields = [f.strip() for f in expected_line.split(",")]

        if len(actual_fields) != len(expected_fields):
            errors.append(
                f"Field count mismatch at line {line_num}:\n"
                f"  Actual:   {actual_line}\n"
                f"  Expected: {expected_line}"
            )
            continue

        for j, (av, ev) in enumerate(zip(actual_fields, expected_fields)):
            if j in skip_cols:
                continue

            if av == ev:
                continue

            col_num = j + 1

            # Normalize signed zero
            av_norm = "0" if av == "-0" else av
            ev_norm = "0" if ev == "-0" else ev
            if av_norm == ev_norm:
                continue

            # Check if both are numeric
            if _is_numeric(av) and _is_numeric(ev):
                a_val = _parse_float(av)
                e_val = _parse_float(ev)
                diff = abs(a_val - e_val)

                # Absolute tolerance for near-zero expected values
                if abs(e_val) < tolerance and diff < tolerance:
                    if verbose:
                        print(
                            f"  Near-zero at line {line_num}, col {col_num}: "
                            f"actual={av}, expected={ev}"
                        )
                    continue

                # Relative tolerance for larger values
                scale = max(abs(a_val), abs(e_val))
                if scale > 0 and diff / scale < tolerance:
                    if verbose:
                        print(
                            f"  Numeric diff at line {line_num}, col {col_num}: "
                            f"actual={av}, expected={ev}"
                        )
                    continue

                # Beyond tolerance — warning or error depending on mode
                msg = (
                    f"Numeric mismatch at line {line_num}, col {col_num}: "
                    f"actual={av}, expected={ev} (diff={diff:.6e})"
                )
                if strict:
                    errors.append(msg)
                else:
                    warnings.append(msg)
            else:
                # Non-numeric: require exact match
                errors.append(
                    f"Value mismatch at line {line_num}, col {col_num}:\n"
                    f"  Actual:   '{av}'\n"
                    f"  Expected: '{ev}'"
                )

    return errors, warnings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare two CSV files with floating-point tolerance."
    )
    parser.add_argument("actual", help="Path to the actual (solver output) CSV file.")
    parser.add_argument("expected", help="Path to the expected (reference) CSV file.")
    parser.add_argument(
        "-t",
        "--tolerance",
        type=float,
        default=1e-6,
        help="Numeric tolerance (default: 1e-6).",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Treat numeric differences beyond tolerance as errors.",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Show accepted numeric diffs."
    )
    args = parser.parse_args(argv)

    for path, label in [(args.actual, "Actual"), (args.expected, "Expected")]:
        try:
            with open(path, encoding="utf-8"):
                pass
        except FileNotFoundError:
            print(f"{label} file not found: {path}", file=sys.stderr)
            return 2

    errors, warnings = compare_csv(
        args.actual,
        args.expected,
        tolerance=args.tolerance,
        strict=args.strict,
        verbose=args.verbose,
    )

    for warn in warnings:
        print(f"WARN: {warn}", file=sys.stderr)

    if errors:
        for err in errors:
            print(f"FAIL: {err}", file=sys.stderr)
        return 1

    if warnings:
        print(f"OK (with {len(warnings)} numeric warnings): {args.actual}")
    else:
        print(f"OK: {args.actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
