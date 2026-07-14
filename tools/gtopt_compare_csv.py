#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare two CSV files with floating-point tolerance.

Designed for comparing gtopt solver output across different LP backends
(CLP, HiGHS, CPLEX) where numerical differences are expected.

Handles:
  - Signed zero: ``-0`` and ``0`` are treated as equal.
  - Near-zero values: absolute differences below *tolerance* pass.
  - The ``kappa`` and ``max_kappa`` columns in ``solution.csv`` are skipped
    (solver metadata).
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

# Columns that are solver-internal metadata and must never be golden-compared.
# ``kappa``/``max_kappa`` are condition numbers that swing with the LP scaling
# / solver build; ``solve_ticks``/``solve_time_s``/``solve_calls`` are run-local
# telemetry (wall time, tick and call counts) that vary run-to-run.  Comparing
# any of these against a checked-in golden is meaningless — a wall-clock time
# can never match a fixture.  (Older goldens predate the solve_* columns; the
# name-based column alignment below already ignores actual-only columns, but
# skipping them here keeps them harmless even once a golden is regenerated with
# them present.)
_SKIP_COLUMNS = frozenset(
    {"kappa", "max_kappa", "solve_ticks", "solve_time_s", "solve_calls"}
)


def _align_columns(
    actual_header: list[str], expected_header: list[str]
) -> tuple[list[tuple[int, int]], list[str]]:
    """Align two flat CSV headers by column NAME.

    Returns ``(col_pairs, missing)`` where ``col_pairs`` is a list of
    ``(actual_idx, expected_idx)`` for every non-skipped column that the
    expected (golden) header declares AND the actual header also carries, and
    ``missing`` lists expected non-skipped column names absent from the actual
    header.

    Name-based alignment (rather than positional ``zip``) lets the actual file
    carry EXTRA diagnostic columns the golden never had (e.g. ``solve_time_s``,
    ``infeasible_count``) without tripping a structural header/field-count
    mismatch — those actual-only columns are simply ignored.  An expected
    column that has vanished from the actual output is a genuine regression and
    surfaces in ``missing``.
    """
    a_index = {name: i for i, name in enumerate(actual_header)}
    col_pairs: list[tuple[int, int]] = []
    missing: list[str] = []
    for e_idx, name in enumerate(expected_header):
        if name in _SKIP_COLUMNS:
            continue
        a_idx = a_index.get(name)
        if a_idx is None:
            missing.append(name)
        else:
            col_pairs.append((a_idx, e_idx))
    return col_pairs, missing


def _is_numeric(value: str) -> bool:
    return _NUMERIC_RE.match(value) is not None


def _parse_float(value: str) -> float:
    """Parse a numeric string, treating '-0' as 0."""
    v = float(value)
    return 0.0 if v == 0.0 else v


def _parse_header(line: str) -> list[str]:
    """Return unquoted column names from a CSV header line."""
    return [f.strip().strip('"') for f in line.split(",")]


def _is_long_layout(header: list[str]) -> bool:
    """Return True for the long-form per-element schema.

    Long form (default since 2026-05-19 when ``output_layout=long``)
    has the bare columns ``uid`` and ``value`` — never coexists with
    a wide-form ``uid:N`` column.
    """
    return "uid" in header and "value" in header


def _is_wide_layout(header: list[str]) -> bool:
    """Return True when the header has any ``uid:N`` column."""
    return any(h.startswith("uid:") for h in header)


def _explode_wide_to_long(lines: list[str]) -> list[str]:
    """Convert a wide-form gtopt CSV to canonical long form.

    Wide:  ``scenario,stage,block,uid:1,uid:2,…`` with one row per
           ``(scenario, stage, block)`` and one float column per uid.
    Long:  ``scenario,stage,block,uid,value`` with one row per
           ``(scenario, stage, block, uid)``; zero-valued cells are
           dropped to mirror how the C++ writer emits the long form.

    The result is sorted by ``(scenario, stage, block, uid)`` so the
    line-by-line comparison stays deterministic regardless of the
    source layout.  Returns a list of CSV lines (header + data) ready
    for the existing comparator.
    """
    if not lines:
        return []
    header = _parse_header(lines[0])
    uid_cols: list[tuple[int, int]] = []
    for idx, name in enumerate(header):
        if name.startswith("uid:"):
            try:
                uid_cols.append((idx, int(name.split(":", 1)[1])))
            except ValueError:
                continue
    if not uid_cols:
        return lines  # nothing to explode
    key_cols = [k for k in ("scenario", "stage", "block") if k in header]
    if not key_cols:
        return lines
    key_idxs = [header.index(k) for k in key_cols]

    out_rows: list[tuple[tuple[int, ...], str]] = []
    for line in lines[1:]:
        if not line.strip():
            continue
        fields = [f.strip() for f in line.split(",")]
        try:
            key_vals = tuple(int(fields[i]) for i in key_idxs)
        except (ValueError, IndexError):
            continue
        for col_idx, uid in uid_cols:
            if col_idx >= len(fields):
                continue
            raw = fields[col_idx]
            if not raw:
                continue
            try:
                val = float(raw)
            except ValueError:
                continue
            # Drop zero-valued cells so the row count matches what the
            # long-form writer would have emitted.
            if val == 0.0:
                continue
            sort_key = (*key_vals, uid)
            # Round-trip via repr-style float formatting so that
            # numerical equality is preserved exactly when the source
            # CSV already happened to be long form.  The downstream
            # comparator does its own tolerance check, so the exact
            # text encoding here does not need to match the C++
            # writer byte-for-byte.
            out_rows.append((sort_key, raw))

    out_rows.sort(key=lambda kv: kv[0])
    header_long = [*key_cols, "uid", "value"]
    out_lines = [",".join(header_long)]
    for key, val_str in out_rows:
        out_lines.append(",".join(str(v) for v in key) + f",{val_str}")
    return out_lines


def _sort_long_lines(lines: list[str]) -> list[str]:
    """Sort an already-long CSV's data rows by ``(scenario, stage, block, uid)``.

    The C++ writer is order-stable but the wide→long conversion above
    sorts on this composite key, so we sort the genuine-long-form side
    on the same key to keep line-by-line comparison aligned.  The
    header is rewritten without surrounding quotes so the byte-for-byte
    header comparison against a wide-side that was exploded by
    :func:`_explode_wide_to_long` (which never quotes) matches.
    """
    if len(lines) <= 1:
        return lines
    header = _parse_header(lines[0])
    key_idxs = [
        header.index(k) for k in ("scenario", "stage", "block", "uid") if k in header
    ]
    if not key_idxs:
        return lines

    def _key(line: str) -> tuple[int, ...]:
        fs = [f.strip() for f in line.split(",")]
        out: list[int] = []
        for i in key_idxs:
            if i >= len(fs):
                return tuple(out)
            try:
                out.append(int(fs[i]))
            except ValueError:
                return tuple(out)
        return tuple(out)

    sorted_data = sorted(lines[1:], key=_key)
    canonical_header = ",".join(header)
    return [canonical_header, *sorted_data]


def _parse_long_keyed_values(
    lines: list[str],
) -> tuple[list[str], dict[tuple[int, ...], float]]:
    """Convert long-form CSV lines into a ``{key_tuple: value}`` dict.

    The key is built from whatever subset of ``(scenario, stage, block,
    uid)`` columns is present in the header — gtopt emits per-stage-only
    files (Battery/capacost_sol etc.) with just ``stage, uid, value`` and
    per-(scene, phase) stage files with the full key.  Returns
    ``(key_cols, dict)`` so the comparator can report keys with the same
    columns the file uses.  ``uid`` is always the last key column when
    present.  Skips the header.  Zero values are kept (so that a wide
    side that exploded into long with zero-drop can still be
    reconstructed correctly: any key absent here means "implicit zero").
    """
    if not lines:
        return [], {}
    header = _parse_header(lines[0])
    key_cols = [k for k in ("scenario", "stage", "block", "uid") if k in header]
    try:
        key_idxs = [header.index(k) for k in key_cols]
        i_va = header.index("value")
    except ValueError:
        return [], {}
    out: dict[tuple[int, ...], float] = {}
    for line in lines[1:]:
        if not line.strip():
            continue
        fs = [f.strip() for f in line.split(",")]
        try:
            key = tuple(int(fs[i]) for i in key_idxs)
            val = _parse_float(fs[i_va])
        except (ValueError, IndexError):
            continue
        out[key] = val
    return key_cols, out


def _close_enough(a: float, b: float, tolerance: float) -> bool:
    """Tolerance check matching the loop in :func:`compare_csv`."""
    diff = abs(a - b)
    if abs(b) < tolerance and diff < tolerance:
        return True
    scale = max(abs(a), abs(b))
    return scale > 0 and diff / scale < tolerance


def _compare_long_keyed(
    actual_lines: list[str],
    expected_lines: list[str],
    *,
    tolerance: float,
    strict: bool,
    verbose: bool,
) -> tuple[list[str], list[str]]:
    """Compare two long-form CSVs by ``(scenario,stage,block,uid)`` key.

    Missing keys default to zero — this is the correct semantics for
    the gtopt long-form encoding, which drops cells whose value is
    zero.  A key present on only one side therefore compares its value
    to ``0.0``.  Used when at least one input is in long form so the
    comparator does not require byte-equal line counts.
    """
    a_key_cols, a_map = _parse_long_keyed_values(actual_lines)
    e_key_cols, e_map = _parse_long_keyed_values(expected_lines)
    errors: list[str] = []
    warnings: list[str] = []
    # Project both maps to the common (intersection-ordered) key
    # columns so a per-stage golden (header ``stage, uid, value``) can
    # still be compared against a per-(scene, stage, block) actual
    # (header ``scenario, stage, block, uid, value``).  The intersection
    # is taken in the natural ``scenario, stage, block, uid`` order so
    # the message labels stay readable.
    common_cols = [
        k
        for k in ("scenario", "stage", "block", "uid")
        if k in a_key_cols and k in e_key_cols
    ]
    if not common_cols:
        # Nothing to key on — degenerate case (e.g. files with no
        # ``stage``/``uid`` columns).  Emit a single header-mismatch
        # error and bail.
        if (
            actual_lines
            and expected_lines
            and _parse_header(actual_lines[0]) != _parse_header(expected_lines[0])
        ):
            errors.append(
                f"Header mismatch:\n"
                f"  Actual:   {actual_lines[0]}\n"
                f"  Expected: {expected_lines[0]}"
            )
        return errors, warnings

    def _project(
        key_cols: list[str], full_map: dict[tuple[int, ...], float]
    ) -> dict[tuple[int, ...], float]:
        if key_cols == common_cols:
            return full_map
        keep = [key_cols.index(c) for c in common_cols]
        out: dict[tuple[int, ...], float] = {}
        for k, v in full_map.items():
            sub = tuple(k[i] for i in keep)
            # Multiple full keys can project onto the same common key
            # (e.g. when the wide-side has no ``scenario`` column but
            # the actual has multiple scenarios) — sum is the right
            # aggregation for emit-and-drop-zeros long form because the
            # opposite side carries the same aggregation.
            out[sub] = out.get(sub, 0.0) + v
        return out

    a_projected = _project(a_key_cols, a_map)
    e_projected = _project(e_key_cols, e_map)
    keys = set(a_projected) | set(e_projected)
    key_label = "(" + ",".join(common_cols) + ")"
    for key in sorted(keys):
        a_val = a_projected.get(key, 0.0)
        e_val = e_projected.get(key, 0.0)
        if a_val == e_val:
            continue
        if _close_enough(a_val, e_val, tolerance):
            if verbose:
                print(
                    f"  Numeric diff at key {key}: actual={a_val!r}, expected={e_val!r}"
                )
            continue
        msg = (
            f"Numeric mismatch at key {key_label}={key}: "
            f"actual={a_val!r}, expected={e_val!r} (diff={abs(a_val - e_val):.6e})"
        )
        if strict:
            errors.append(msg)
        else:
            warnings.append(msg)
    return errors, warnings


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

    Layout-aware: when one input is in the wide gtopt per-element
    layout (``uid:N`` columns) and the other is in the long layout
    (``scenario, stage, block, uid, value``), both are normalised to
    long form before the line-by-line comparison so the comparator
    works regardless of which side flipped its ``output_layout``.
    When both sides are (or normalise to) long form, the comparison
    runs key-by-key on ``(scenario, stage, block, uid)`` and treats
    a missing key as the implicit zero the long encoding drops —
    line-count differences therefore become per-key numeric diffs
    rather than a fatal structural mismatch.
    """
    with open(actual_path, encoding="utf-8") as f:
        actual_lines = f.read().splitlines()
    with open(expected_path, encoding="utf-8") as f:
        expected_lines = f.read().splitlines()

    # Normalise to a common shape when the two sides used different
    # ``output_layout`` settings.  When both are already wide (or both
    # already long, or neither is a per-element file like solution.csv),
    # this is a no-op aside from a stable sort on the long side.
    layout_long = False
    if actual_lines and expected_lines:
        a_hdr = _parse_header(actual_lines[0])
        e_hdr = _parse_header(expected_lines[0])
        a_long = _is_long_layout(a_hdr)
        a_wide = _is_wide_layout(a_hdr)
        e_long = _is_long_layout(e_hdr)
        e_wide = _is_wide_layout(e_hdr)
        if (a_long and e_wide) or (a_wide and e_long):
            if a_wide:
                actual_lines = _explode_wide_to_long(actual_lines)
            else:
                actual_lines = _sort_long_lines(actual_lines)
            if e_wide:
                expected_lines = _explode_wide_to_long(expected_lines)
            else:
                expected_lines = _sort_long_lines(expected_lines)
            layout_long = True
        elif a_long and e_long:
            actual_lines = _sort_long_lines(actual_lines)
            expected_lines = _sort_long_lines(expected_lines)
            layout_long = True

    if layout_long:
        return _compare_long_keyed(
            actual_lines,
            expected_lines,
            tolerance=tolerance,
            strict=strict,
            verbose=verbose,
        )

    errors: list[str] = []
    warnings: list[str] = []

    if len(actual_lines) != len(expected_lines):
        errors.append(
            f"Line count mismatch: actual={len(actual_lines)}, "
            f"expected={len(expected_lines)}"
        )
        return errors, warnings

    col_pairs: list[tuple[int, int]] = []

    for i, (actual_line, expected_line) in enumerate(zip(actual_lines, expected_lines)):
        line_num = i + 1

        # Detect header lines: first line, or lines containing quotes
        is_header = i == 0 or '"' in expected_line
        if is_header:
            # Align by column NAME so an actual file that carries extra
            # diagnostic columns (solve_time_s, infeasible_count, …) the golden
            # never had does not trip a structural header/field-count mismatch.
            col_pairs, missing = _align_columns(
                _parse_header(actual_line), _parse_header(expected_line)
            )
            if missing:
                errors.append(
                    f"Header mismatch at line {line_num}: expected column(s) "
                    f"{missing} missing from actual output\n"
                    f"  Actual:   {actual_line}\n"
                    f"  Expected: {expected_line}"
                )
            continue

        # Data lines — compare only the name-aligned (non-skipped, shared)
        # column pairs; actual-only columns are ignored by construction.
        actual_fields = [f.strip() for f in actual_line.split(",")]
        expected_fields = [f.strip() for f in expected_line.split(",")]

        for a_idx, e_idx in col_pairs:
            if a_idx >= len(actual_fields) or e_idx >= len(expected_fields):
                errors.append(
                    f"Field count mismatch at line {line_num}:\n"
                    f"  Actual:   {actual_line}\n"
                    f"  Expected: {expected_line}"
                )
                break

            av = actual_fields[a_idx]
            ev = expected_fields[e_idx]

            if av == ev:
                continue

            col_num = e_idx + 1

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
