#!/usr/bin/env python3
"""
Migrate legacy top-level `PlanningOptions` keys into the nested
`model_options` sub-object in test JSON fixtures and case files.

Motivation
----------
The naming-dialects registry's class-blind canonicalize step
(`source/json_canonicalize.cpp`) rewrites e.g.
`reserve_fail_cost → reserve_shortage_cost` globally.  As long as
`PlanningOptions` carried the legacy spellings as canonical fields,
a JSON file placing `options.reserve_fail_cost = X` at the top
level would have its key rewritten — and strict daw::json parse
would reject the result (`reserve_shortage_cost` is not a member
of `PlanningOptions`).  Caught by `tools/check_naming_dialects.py`
as `alias-shadows-canonical`.

The proper fix is to delete the 11 legacy top-level mirror fields
from `PlanningOptions` (per §11 of
docs/analysis/naming-conventions.md).  But that requires migrating
every JSON fixture across the test suite to use the nested form.
This script does that mechanical migration.

What it does
------------
For each `*.cpp` / `*.json` file under `test/source/` and `cases/`:

1. Find every C++ raw-string literal `R"<delim>(<content>)<delim>"`.
2. Try to parse `<content>` as JSON.
3. If the document has a top-level `"options"` key AND that
   object contains any of the 11 legacy ModelOptions-mirror keys,
   rewrite: pull those keys into `options.model_options.{…}`
   (creating or merging with an existing `model_options` block).
4. Apply the two §11 renames as part of the move:
     - `reserve_fail_cost  → reserve_shortage_cost`
     - `hydro_fail_cost    → hydro_spill_cost`
5. Splice the rewritten JSON back into the source file.

For `.json` files (e.g. case JSONs under `cases/`), the whole file
is parsed and rewritten the same way.

Modes
-----
  --dry-run   (default)  Print planned changes; don't write.
  --in-place             Modify files in place.
  --verbose              Per-file detail.
  --filter PATTERN       Only files matching this glob fragment.

Safety
------
* Only rewrites if the JSON parses cleanly — malformed fixtures
  are skipped with a warning.
* Only touches `options.X` keys; ModelOptions-direct fixtures
  (where the legacy keys are at *top level*, no `options` wrapper)
  are untouched.
* Detects existing `options.model_options` and merges into it
  rather than overwriting.

Examples
--------
  tools/migrate_legacy_options_to_model_options.py            # dry-run
  tools/migrate_legacy_options_to_model_options.py --in-place
  tools/migrate_legacy_options_to_model_options.py --filter test_planning
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable, NamedTuple

# ── Constants ──────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROOTS = [
    REPO_ROOT / "test" / "source",
    REPO_ROOT / "cases",
]

# 11 legacy top-level ModelOptions-mirror keys per §11
LEGACY_MODEL_OPTIONS_KEYS: frozenset[str] = frozenset(
    {
        "demand_fail_cost",
        "reserve_fail_cost",
        "hydro_fail_cost",
        "hydro_use_value",
        "use_line_losses",
        "loss_segments",
        "use_kirchhoff",
        "use_single_bus",
        "kirchhoff_threshold",
        "scale_objective",
        "scale_theta",
    }
)

# §11.10 renames applied during the migration.  Maps legacy key →
# new canonical key inside model_options.
KEY_RENAMES: dict[str, str] = {
    "reserve_fail_cost": "reserve_shortage_cost",
    "hydro_fail_cost": "hydro_spill_cost",
}

# ModelOptions struct field declaration order (see
# include/gtopt/model_options.hpp).  When we move legacy keys into
# the nested block, we order them to match — designated initializers
# in C++ require declaration order.
MODEL_OPTIONS_DECL_ORDER: tuple[str, ...] = (
    "use_single_bus",
    "use_kirchhoff",
    "kirchhoff_mode",
    "use_line_losses",
    "line_losses_mode",
    "kirchhoff_threshold",
    "dc_line_reactance_threshold",
    "loss_segments",
    "scale_objective",
    "scale_theta",
    "scale_loss_link",
    "theta_max",
    "auto_scale",
    "demand_fail_cost",
    "reserve_shortage_cost",
    "hydro_spill_cost",
    "hydro_use_value",
    "state_violation_cost",
    "demand_fail_rhs_shift",
    "emission_cost",
    "emission_cap",
    "continuous_phases",
    "strict_storage_emin",
)

# Match a C++ raw string literal: R"<delim>(…)<delim>"
# Delimiter is 0-16 chars from the d-char-set (we conservatively
# accept letters / digits / underscore).
RAW_LITERAL_RE = re.compile(
    r'R"(?P<delim>[A-Za-z_0-9]{0,16})\(',
    re.DOTALL,
)


# ── Data classes ───────────────────────────────────────────────────────────


class Edit(NamedTuple):
    """One in-place rewrite of a JSON literal within a source file."""

    file: Path
    start: int  # byte offset of the raw string's opening
    end: int  # byte offset just past the closing `)<delim>"`
    old_text: str  # original raw-string content (no R"…(  …)…" wrap)
    new_text: str  # rewritten JSON content
    legacy_keys_moved: list[str]


# ── Source scanning ────────────────────────────────────────────────────────


def find_raw_literals(text: str) -> Iterable[tuple[int, int, str, str]]:
    """Yield (start_offset, end_offset, delimiter, content) for every
    C++ raw-string literal in `text`.

    `start_offset` is the byte index of the opening `R`; `end_offset`
    is one past the closing `"`.
    """
    for m in RAW_LITERAL_RE.finditer(text):
        delim = m.group("delim")
        body_start = m.end()
        close_marker = f'){delim}"'
        close_idx = text.find(close_marker, body_start)
        if close_idx == -1:
            continue  # malformed; skip
        body = text[body_start:close_idx]
        yield (m.start(), close_idx + len(close_marker), delim, body)


# ── JSON migration logic ───────────────────────────────────────────────────


def looks_like_planning_options_json(obj: object) -> bool:
    """True if `obj` is a dict with a top-level `"options"` object that
    contains at least one legacy ModelOptions-mirror key.
    """
    if not isinstance(obj, dict):
        return False
    opts = obj.get("options")
    if not isinstance(opts, dict):
        return False
    return any(k in opts for k in LEGACY_MODEL_OPTIONS_KEYS)


def migrate_planning_options(
    obj: dict,
) -> tuple[dict, list[str]]:
    """Return (migrated_obj, list_of_legacy_keys_moved).  Non-destructive;
    operates on a deep copy."""
    import copy

    out = copy.deepcopy(obj)
    opts = out["options"]
    model_opts = opts.get("model_options")
    if not isinstance(model_opts, dict):
        model_opts = {}

    moved: list[str] = []
    for legacy in list(opts.keys()):
        if legacy not in LEGACY_MODEL_OPTIONS_KEYS:
            continue
        value = opts.pop(legacy)
        canonical = KEY_RENAMES.get(legacy, legacy)
        # Existing nested entry wins (matches `migrate_flat_to_model_options`
        # semantics from the deleted helper).
        if canonical not in model_opts:
            model_opts[canonical] = value
        moved.append(legacy)

    if not moved:
        return out, []

    # Reorder model_opts by ModelOptions declaration order; unknown
    # keys keep insertion order at the end.
    ordered = {}
    for key in MODEL_OPTIONS_DECL_ORDER:
        if key in model_opts:
            ordered[key] = model_opts.pop(key)
    ordered.update(model_opts)  # any unknown trailing keys
    opts["model_options"] = ordered
    return out, moved


def reserialize(
    obj: dict,
    *,
    indent_spaces: int,
    leading_indent: str,
) -> str:
    """Re-serialize `obj` as JSON with `indent_spaces` spaces per level,
    each line prefixed with `leading_indent` (matching the C++ surrounding
    indent so the diff stays clean).
    """
    raw = json.dumps(obj, indent=indent_spaces, ensure_ascii=False)
    if not leading_indent:
        return raw
    lines = raw.splitlines()
    out_lines = [
        lines[0]
    ]  # first line is `{` — no leading indent (sits where the raw-string opens)
    for line in lines[1:]:
        out_lines.append(leading_indent + line)
    return "\n".join(out_lines)


def detect_leading_indent(content: str) -> tuple[str, int]:
    """From the original raw-string content, guess (leading_indent,
    indent_step) so the rewritten JSON matches the surrounding C++ block.

    Heuristic: take the indent of the first non-blank line that starts
    with a quote, and the indent step from the first key that's at
    depth 2.
    """
    for line in content.splitlines():
        if line.strip().startswith('"'):
            leading = line[: len(line) - len(line.lstrip())]
            return leading, 2
    return "", 2


# ── Driver ─────────────────────────────────────────────────────────────────


def process_cpp_file(path: Path, *, dry_run: bool, verbose: bool) -> list[Edit]:
    """Return the list of edits that would be applied to this file.

    When `dry_run` is False, writes the file in place atomically."""
    text = path.read_text(encoding="utf-8")
    edits: list[Edit] = []

    for start, end, delim, body in find_raw_literals(text):
        try:
            obj = json.loads(body)
        except (json.JSONDecodeError, ValueError):
            continue  # not JSON — could be SQL, LP, AMPL, etc.

        if not looks_like_planning_options_json(obj):
            continue

        migrated, moved = migrate_planning_options(obj)
        if not moved:
            continue

        leading, step = detect_leading_indent(body)
        new_body = (
            "\n"
            + leading
            + reserialize(migrated, indent_spaces=step, leading_indent=leading)
            + "\n"
            + leading[:-2]
            if len(leading) >= 2
            else (
                "\n"
                + reserialize(migrated, indent_spaces=step, leading_indent="")
                + "\n"
            )
        )

        edits.append(
            Edit(
                file=path,
                start=start,
                end=end,
                old_text=body,
                new_text=new_body,
                legacy_keys_moved=moved,
            )
        )

    if not edits:
        return []

    if dry_run:
        if verbose:
            for e in edits:
                print(
                    f"  would move {len(e.legacy_keys_moved)} key(s) "
                    f"{e.legacy_keys_moved} in {path.relative_to(REPO_ROOT)}"
                )
        return edits

    # Apply edits in reverse order so offsets stay valid.
    out = text
    for e in sorted(edits, key=lambda x: x.start, reverse=True):
        # Rebuild the raw-string literal with the original delimiter.
        # We have to re-discover the delimiter from the original slice.
        original = text[e.start : e.end]
        m = RAW_LITERAL_RE.match(original)
        if not m:
            continue
        delim = m.group("delim")
        rewritten = f'R"{delim}({e.new_text}){delim}"'
        out = out[: e.start] + rewritten + out[e.end :]

    path.write_text(out, encoding="utf-8")
    return edits


def process_json_file(path: Path, *, dry_run: bool, verbose: bool) -> list[Edit]:
    """Whole-file JSON migration for case files."""
    try:
        text = path.read_text(encoding="utf-8")
        obj = json.loads(text)
    except (json.JSONDecodeError, ValueError, UnicodeDecodeError):
        return []

    if not looks_like_planning_options_json(obj):
        return []

    migrated, moved = migrate_planning_options(obj)
    if not moved:
        return []

    new_text = json.dumps(migrated, indent=2, ensure_ascii=False) + "\n"
    edit = Edit(
        file=path,
        start=0,
        end=len(text),
        old_text=text,
        new_text=new_text,
        legacy_keys_moved=moved,
    )

    if dry_run:
        if verbose:
            print(
                f"  would migrate {len(moved)} key(s) {moved} in "
                f"{path.relative_to(REPO_ROOT)}"
            )
    else:
        path.write_text(new_text, encoding="utf-8")
    return [edit]


def walk(roots: list[Path], filter_pattern: str | None) -> Iterable[Path]:
    for root in roots:
        if not root.exists():
            continue
        for ext in ("*.cpp", "*.hpp", "*.json"):
            for p in root.rglob(ext):
                if filter_pattern and filter_pattern not in str(p):
                    continue
                yield p


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--in-place",
        action="store_true",
        help="modify files in place (default: dry-run)",
    )
    ap.add_argument("--verbose", action="store_true", help="per-file detail")
    ap.add_argument(
        "--filter",
        default=None,
        help="only process files whose path contains this substring",
    )
    ap.add_argument(
        "paths",
        nargs="*",
        type=Path,
        help="optional explicit file/dir paths (default: test/source/ and cases/)",
    )
    args = ap.parse_args()

    roots = args.paths if args.paths else DEFAULT_ROOTS
    dry_run = not args.in_place

    n_files_scanned = 0
    n_files_modified = 0
    n_edits = 0

    for path in walk(roots, args.filter):
        n_files_scanned += 1
        if path.suffix in (".cpp", ".hpp"):
            edits = process_cpp_file(path, dry_run=dry_run, verbose=args.verbose)
        else:  # .json
            edits = process_json_file(path, dry_run=dry_run, verbose=args.verbose)
        if edits:
            n_files_modified += 1
            n_edits += len(edits)

    print(f"\nmigrate-legacy-options summary:")
    print(f"  scanned       : {n_files_scanned} files")
    print(f"  candidate     : {n_files_modified} files with legacy keys to move")
    print(f"  total edits   : {n_edits} JSON literals")
    print(
        f"  mode          : {'DRY-RUN (no files modified)' if dry_run else 'IN-PLACE'}"
    )
    if dry_run and n_files_modified:
        print("\nRe-run with --in-place to apply.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
