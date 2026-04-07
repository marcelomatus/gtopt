# SPDX-License-Identifier: BSD-3-Clause
"""Core LP fingerprint computation and comparison logic."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

# Label format: lowercase(class)_variable_uid_ctx0_ctx1_...
# Context field counts map to context types:
#   2 fields = StageContext (scenario, stage)
#   3 fields = BlockContext (scenario, stage, block)
#   4 fields = BlockExContext (scenario, stage, block, extra)
#          or ScenePhaseContext (scene, phase) — but that's also 2 fields
# We use the count after class_variable_uid to determine the type.
_CONTEXT_TYPE_MAP = {
    0: "monostate",
    2: "StageContext",
    3: "BlockContext",
    4: "BlockExContext",
}


@dataclass(frozen=True, order=True)
class FingerprintEntry:
    """One entry in the structural template."""

    class_name: str
    variable_name: str
    context_type: str


@dataclass
class FingerprintStats:
    """Per-class column/row count statistics (informational only)."""

    total_cols: int = 0
    total_rows: int = 0
    cols_by_class: dict[str, int] = field(default_factory=dict)
    rows_by_class: dict[str, int] = field(default_factory=dict)


@dataclass
class LpFingerprint:
    """Complete LP fingerprint."""

    col_template: list[FingerprintEntry] = field(default_factory=list)
    row_template: list[FingerprintEntry] = field(default_factory=list)
    untracked_cols: int = 0
    untracked_rows: int = 0
    col_hash: str = ""
    row_hash: str = ""
    structural_hash: str = ""
    stats: FingerprintStats = field(default_factory=FingerprintStats)


def _hash_template(entries: list[FingerprintEntry]) -> str:
    """Compute SHA-256 hash of a sorted template, matching C++ implementation."""
    data = ""
    for e in entries:
        data += e.class_name + "\0" + e.variable_name + "\0" + e.context_type + "\n"
    return hashlib.sha256(data.encode()).hexdigest()


def _parse_lp_name(name: str) -> tuple[str, str, str] | None:
    """Parse a gtopt LP label into (class_name, variable_name, context_type).

    Label format: lowercase(class)_variable_uid_ctx0_ctx1_...
    Returns None if the name cannot be parsed.
    """
    parts = name.split("_")
    if len(parts) < 3:
        return None

    # First part is lowercase class name
    class_name = parts[0]
    # Last N parts are numeric (uid + context fields)
    # Find where numeric parts start (from the end)
    numeric_start = len(parts)
    for i in range(len(parts) - 1, 0, -1):
        try:
            int(parts[i])
            numeric_start = i
        except ValueError:
            break

    if numeric_start >= len(parts):
        return None

    # Variable name is everything between class and the numeric part
    variable_name = "_".join(parts[1:numeric_start])
    if not variable_name:
        return None

    # Context fields = everything after uid (which is at numeric_start)
    n_context_fields = len(parts) - numeric_start - 1  # subtract uid
    context_type = _CONTEXT_TYPE_MAP.get(n_context_fields, f"Context{n_context_fields}")

    return class_name, variable_name, context_type


def compute_from_names(col_names: list[str], row_names: list[str]) -> LpFingerprint:
    """Compute LP fingerprint from column and row name lists."""
    fp = LpFingerprint()
    fp.stats.total_cols = len(col_names)
    fp.stats.total_rows = len(row_names)

    col_set: set[FingerprintEntry] = set()
    for name in col_names:
        parsed = _parse_lp_name(name)
        if parsed is None:
            fp.untracked_cols += 1
            continue
        cls, var, ctx = parsed
        col_set.add(FingerprintEntry(cls, var, ctx))
        fp.stats.cols_by_class[cls] = fp.stats.cols_by_class.get(cls, 0) + 1

    row_set: set[FingerprintEntry] = set()
    for name in row_names:
        parsed = _parse_lp_name(name)
        if parsed is None:
            fp.untracked_rows += 1
            continue
        cls, var, ctx = parsed
        row_set.add(FingerprintEntry(cls, var, ctx))
        fp.stats.rows_by_class[cls] = fp.stats.rows_by_class.get(cls, 0) + 1

    fp.col_template = sorted(col_set)
    fp.row_template = sorted(row_set)
    fp.col_hash = _hash_template(fp.col_template)
    fp.row_hash = _hash_template(fp.row_template)
    fp.structural_hash = hashlib.sha256(
        (fp.col_hash + fp.row_hash).encode()
    ).hexdigest()

    return fp


def _read_lp_file(path: Path) -> tuple[list[str], list[str]]:
    """Parse an LP file and extract column and row names.

    Returns (col_names, row_names).
    """
    import gzip
    import lzma

    text: str
    p = Path(path)
    if p.suffix == ".gz":
        with gzip.open(p, "rt") as f:
            text = f.read()
    elif p.suffix in (".zst",):
        try:
            import zstandard as zstd

            with open(p, "rb") as fb:
                dctx = zstd.ZstdDecompressor()
                text = dctx.decompress(fb.read()).decode()
        except ImportError as exc:
            raise ImportError("zstandard package required for .zst files") from exc
    elif p.suffix == ".xz":
        with lzma.open(p, "rt") as f:
            text = f.read()
    else:
        with open(p) as f:
            text = f.read()

    col_names: list[str] = []
    row_names: list[str] = []

    # LP file sections
    section = None
    var_re = re.compile(r"[A-Za-z_][A-Za-z0-9_:.]*")

    for line in text.splitlines():
        stripped = line.strip().lower()

        if stripped.startswith("\\"):
            continue

        if stripped in ("minimize", "minimize", "min"):
            section = "objective"
            continue
        if stripped.startswith("subject to") or stripped == "s.t." or stripped == "st":
            section = "constraints"
            continue
        if stripped == "bounds":
            section = "bounds"
            continue
        if stripped in (
            "generals",
            "general",
            "integers",
            "integer",
            "binary",
            "binaries",
        ):
            section = "integers"
            continue
        if stripped == "end":
            break

        if section == "constraints":
            # Row names appear as "name:" at the start of constraint lines
            match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_:.]*)\s*:", line)
            if match:
                row_names.append(match.group(1))

        if section == "bounds":
            # Variable names appear in bounds lines
            for match in var_re.finditer(line):
                name = match.group()
                if name.lower() not in ("inf", "infinity", "free"):
                    col_names.append(name)

    # Deduplicate col_names (same var may appear in bounds multiple times)
    seen: set[str] = set()
    unique_cols: list[str] = []
    for c in col_names:
        if c not in seen:
            seen.add(c)
            unique_cols.append(c)

    return unique_cols, row_names


def compute_from_lp_file(path: Path) -> LpFingerprint:
    """Compute LP fingerprint from an LP file."""
    col_names, row_names = _read_lp_file(path)
    return compute_from_names(col_names, row_names)


def load_fingerprint_json(path: Path) -> LpFingerprint:
    """Load an LP fingerprint from a JSON file (as produced by gtopt)."""
    with open(path) as f:
        data = json.load(f)

    structural = data["structural"]
    stats_data = data.get("stats", {})

    fp = LpFingerprint()
    fp.col_template = sorted(
        FingerprintEntry(e["class"], e["variable"], e["context_type"])
        for e in structural["columns"]["template"]
    )
    fp.row_template = sorted(
        FingerprintEntry(e["class"], e["constraint"], e["context_type"])
        for e in structural["rows"]["template"]
    )
    fp.untracked_cols = structural["columns"].get("untracked_count", 0)
    fp.untracked_rows = structural["rows"].get("untracked_count", 0)
    fp.col_hash = structural["columns"]["hash"]
    fp.row_hash = structural["rows"]["hash"]
    fp.structural_hash = structural["hash"]

    fp.stats.total_cols = stats_data.get("total_cols", 0)
    fp.stats.total_rows = stats_data.get("total_rows", 0)
    fp.stats.cols_by_class = stats_data.get("cols_by_class", {})
    fp.stats.rows_by_class = stats_data.get("rows_by_class", {})

    return fp


def write_fingerprint_json(
    fp: LpFingerprint,
    path: Path,
    scene_uid: int = 0,
    phase_uid: int = 0,
) -> None:
    """Write an LP fingerprint to a JSON file."""
    data = {
        "version": 1,
        "scene_uid": scene_uid,
        "phase_uid": phase_uid,
        "structural": {
            "columns": {
                "template": [
                    {
                        "class": e.class_name,
                        "variable": e.variable_name,
                        "context_type": e.context_type,
                    }
                    for e in fp.col_template
                ],
                "untracked_count": fp.untracked_cols,
                "hash": fp.col_hash,
            },
            "rows": {
                "template": [
                    {
                        "class": e.class_name,
                        "constraint": e.variable_name,
                        "context_type": e.context_type,
                    }
                    for e in fp.row_template
                ],
                "untracked_count": fp.untracked_rows,
                "hash": fp.row_hash,
            },
            "hash": fp.structural_hash,
        },
        "stats": {
            "total_cols": fp.stats.total_cols,
            "total_rows": fp.stats.total_rows,
            "cols_by_class": dict(sorted(fp.stats.cols_by_class.items())),
            "rows_by_class": dict(sorted(fp.stats.rows_by_class.items())),
        },
    }
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


@dataclass
class ComparisonResult:
    """Result of comparing two fingerprints."""

    match: bool
    added_cols: list[FingerprintEntry] = field(default_factory=list)
    removed_cols: list[FingerprintEntry] = field(default_factory=list)
    added_rows: list[FingerprintEntry] = field(default_factory=list)
    removed_rows: list[FingerprintEntry] = field(default_factory=list)
    untracked_cols: int = 0
    untracked_rows: int = 0


def compare_fingerprints(
    actual: LpFingerprint, expected: LpFingerprint
) -> ComparisonResult:
    """Compare two fingerprints, ignoring stats (informational only).

    Only the structural section is compared.
    """
    if actual.structural_hash == expected.structural_hash:
        return ComparisonResult(
            match=True,
            untracked_cols=actual.untracked_cols,
            untracked_rows=actual.untracked_rows,
        )

    actual_col_set = set(actual.col_template)
    expected_col_set = set(expected.col_template)
    actual_row_set = set(actual.row_template)
    expected_row_set = set(expected.row_template)

    return ComparisonResult(
        match=False,
        added_cols=sorted(actual_col_set - expected_col_set),
        removed_cols=sorted(expected_col_set - actual_col_set),
        added_rows=sorted(actual_row_set - expected_row_set),
        removed_rows=sorted(expected_row_set - actual_row_set),
        untracked_cols=actual.untracked_cols,
        untracked_rows=actual.untracked_rows,
    )
