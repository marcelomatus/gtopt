# SPDX-License-Identifier: BSD-3-Clause
"""CLI for LP fingerprint: compute, verify, and compare."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from ._fingerprint import (
    ComparisonResult,
    LpFingerprint,
    compare_fingerprints,
    compute_from_lp_file,
    load_fingerprint_json,
    write_fingerprint_json,
)


def _cmd_compute(args: argparse.Namespace) -> int:
    """Compute fingerprint from an LP file."""
    fp = compute_from_lp_file(Path(args.lp_file))

    if args.output:
        write_fingerprint_json(fp, Path(args.output))
        print(f"Fingerprint written to {args.output}")
    else:
        print(f"Structural hash: {fp.structural_hash}")
        print(f"  Columns: {len(fp.col_template)} types, {fp.stats.total_cols} total")
        print(f"  Rows:    {len(fp.row_template)} types, {fp.stats.total_rows} total")
        if fp.untracked_cols or fp.untracked_rows:
            print(f"  Untracked: {fp.untracked_cols} cols, {fp.untracked_rows} rows")

    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    """Verify LP file against a golden fingerprint JSON."""
    actual = compute_from_lp_file(Path(args.lp_file))
    expected = load_fingerprint_json(Path(args.golden))
    result = compare_fingerprints(actual, expected)

    if result.match:
        print(f"PASS: structural fingerprints match ({actual.structural_hash[:16]}...)")
        if result.untracked_cols or result.untracked_rows:
            print(
                f"  Warning: {result.untracked_cols} untracked cols, "
                f"{result.untracked_rows} untracked rows"
            )
        return 0

    print("FAIL: structural fingerprints differ")
    print(f"  Actual:   {actual.structural_hash}")
    print(f"  Expected: {expected.structural_hash}")
    _print_diff(result)
    return 1


def _cmd_compare(args: argparse.Namespace) -> int:
    """Compare two fingerprint JSON files."""
    actual = load_fingerprint_json(Path(args.actual))
    expected = load_fingerprint_json(Path(args.expected))
    result = compare_fingerprints(actual, expected)

    if result.match:
        print(f"PASS: structural fingerprints match ({actual.structural_hash[:16]}...)")
        _print_stats(actual, expected)
        return 0

    print("FAIL: structural fingerprints differ")
    print(f"  Actual:   {actual.structural_hash}")
    print(f"  Expected: {expected.structural_hash}")
    _print_diff(result)
    _print_stats(actual, expected)
    return 1


def _print_diff(result: ComparisonResult) -> None:
    """Print structural differences."""
    if result.added_cols:
        print(f"\n  Added columns ({len(result.added_cols)}):")
        for e in result.added_cols:
            print(f"    + {e.class_name}.{e.variable_name} ({e.context_type})")
    if result.removed_cols:
        print(f"\n  Removed columns ({len(result.removed_cols)}):")
        for e in result.removed_cols:
            print(f"    - {e.class_name}.{e.variable_name} ({e.context_type})")
    if result.added_rows:
        print(f"\n  Added rows ({len(result.added_rows)}):")
        for e in result.added_rows:
            print(f"    + {e.class_name}.{e.variable_name} ({e.context_type})")
    if result.removed_rows:
        print(f"\n  Removed rows ({len(result.removed_rows)}):")
        for e in result.removed_rows:
            print(f"    - {e.class_name}.{e.variable_name} ({e.context_type})")


def _print_stats(actual: LpFingerprint, expected: LpFingerprint) -> None:
    """Print stats comparison (informational)."""
    a_stats = actual.stats
    e_stats = expected.stats
    if (
        a_stats.total_cols != e_stats.total_cols
        or a_stats.total_rows != e_stats.total_rows
    ):
        print(
            f"\n  Stats (informational):"
            f"\n    Cols: {a_stats.total_cols} (actual) vs {e_stats.total_cols} (expected)"
            f"\n    Rows: {a_stats.total_rows} (actual) vs {e_stats.total_rows} (expected)"
        )


def main(argv: list[str] | None = None) -> int:
    """Entry point for the CLI."""
    parser = argparse.ArgumentParser(
        prog="gtopt_check_fingerprint",
        description="LP structural fingerprint tool for gtopt",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # compute
    p_compute = subparsers.add_parser(
        "compute", help="Compute fingerprint from an LP file"
    )
    p_compute.add_argument("lp_file", help="Path to LP file (.lp, .lp.gz, .lp.zst)")
    p_compute.add_argument("-o", "--output", help="Output JSON file path")

    # verify
    p_verify = subparsers.add_parser(
        "verify", help="Verify LP file against a golden fingerprint"
    )
    p_verify.add_argument("--lp-file", required=True, help="Path to LP file")
    p_verify.add_argument("--golden", required=True, help="Path to golden JSON")

    # compare
    p_compare = subparsers.add_parser(
        "compare", help="Compare two fingerprint JSON files"
    )
    p_compare.add_argument("--actual", required=True, help="Actual fingerprint JSON")
    p_compare.add_argument(
        "--expected", required=True, help="Expected fingerprint JSON"
    )

    args = parser.parse_args(argv)

    commands = {
        "compute": _cmd_compute,
        "verify": _cmd_verify,
        "compare": _cmd_compare,
    }
    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
