# SPDX-License-Identifier: BSD-3-Clause
"""gtopt2pbi — make gtopt Parquet field files Power BI ready (CLI + library).

Walks a directory tree of gtopt field Parquet/CSV files and, in place:

* reshapes wide ``uid:N`` field tables to the tidy long shape
  ``[<index cols>, uid, value]`` (read natively by gtopt; ideal for Power BI);
* re-encodes every Parquet file with the target codec (default ``zstd``).

Hive-partitioned ``*.parquet`` directories (gtopt's solve *output*) are skipped
— they are already long; this tool targets the flat per-field file layout that
``plp2gtopt`` / ``plexos2gtopt`` emit for a case's inputs.

Examples::

    gtopt2pbi support/juan/gtopt_iplp_rerun527
    gtopt2pbi --dry-run support/juan/gtopt_iplp_rerun527/Generator
    gtopt2pbi -c zstd --compression-level 9 mycase/
    python -m gtopt2pbi caseA caseB
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Sequence

import pandas as pd
import pyarrow as pa

# The wide→long reshape is shared with the plp2gtopt conversion pipeline so the
# two stay in lock-step (a wide field table ⇒ a long one; anything else ⇒ None).
from plp2gtopt.base_writer import to_long_layout

_DEFAULT_CODEC = "zstd"
_UNCOMPRESSED_ALIASES = {"none", "", "uncompressed"}


def _codec_kwargs(compression: str, level: Optional[int]) -> dict:
    """Return ``to_parquet`` kwargs for *compression*, falling back to gzip
    when the requested codec is not compiled into the linked Arrow build."""
    codec = (compression or "").lower()
    if codec in _UNCOMPRESSED_ALIASES:
        return {}
    if not pa.Codec.is_available(codec):
        print(
            f"warning: codec '{codec}' unavailable in this Arrow build; "
            "falling back to 'gzip'",
            file=sys.stderr,
        )
        codec = "gzip"
    kwargs: dict = {"compression": codec}
    if level:
        kwargs["compression_level"] = int(level)
    return kwargs


def _is_hive_leaf(path: Path) -> bool:
    """True when *path* lives inside a ``*.parquet/`` directory — i.e. it is a
    gtopt SOLVE-OUTPUT hive leaf (already long), not a flat input field file."""
    return any(parent.suffix == ".parquet" for parent in path.parents)


def relayout_tree(
    root: Path,
    *,
    compression: str = _DEFAULT_CODEC,
    compression_level: Optional[int] = None,
    dry_run: bool = False,
) -> int:
    """Reshape every wide field table under *root* to long + the target codec,
    in place.  Returns the number of files converted.

    Only wide ``uid:N`` field tables are touched (``to_long_layout`` returns a
    frame).  Everything else is left exactly as-is: structural tables
    (block/stage definitions), SDDP cut files, already-long tables, and
    hive-partitioned solve **output**.  This keeps the tool scoped to making a
    case's flat input field files Power-BI friendly without rewriting outputs.
    """
    pq_kwargs = _codec_kwargs(compression, compression_level)
    converted = 0

    for path in sorted(root.rglob("*.parquet")):
        if not path.is_file() or _is_hive_leaf(path):
            continue
        try:
            df = pd.read_parquet(path)
        except (OSError, ValueError) as exc:
            print(f"  skip (unreadable parquet): {path} ({exc})", file=sys.stderr)
            continue
        long_df = to_long_layout(df)
        if long_df is None:
            continue  # not a wide field table — leave untouched
        if dry_run:
            print(f"  [dry-run] reshape: {path}")
        else:
            long_df.to_parquet(path, index=False, **pq_kwargs)
        converted += 1

    for path in sorted(root.rglob("*.csv")):
        if not path.is_file() or _is_hive_leaf(path):
            continue
        try:
            df = pd.read_csv(path)
        except (OSError, ValueError):
            continue
        long_df = to_long_layout(df)
        if long_df is None:
            continue
        if dry_run:
            print(f"  [dry-run] reshape (csv): {path}")
        else:
            long_df.to_csv(path, index=False)
        converted += 1

    return converted


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gtopt2pbi",
        description=(
            "Make gtopt field Parquet/CSV trees Power BI ready: reshape wide "
            "uid:N tables to long [<index>, uid, value] and re-encode Parquet "
            "with a single readable codec (default zstd), in place."
        ),
    )
    parser.add_argument(
        "dirs",
        nargs="+",
        type=Path,
        metavar="DIR",
        help="directory tree(s) of field Parquet/CSV files to convert in place",
    )
    parser.add_argument(
        "-c",
        "--compression",
        default=_DEFAULT_CODEC,
        metavar="CODEC",
        help="target Parquet codec (default: %(default)s)",
    )
    parser.add_argument(
        "--compression-level",
        type=int,
        default=None,
        metavar="N",
        help="codec level, e.g. 1-22 for zstd (default: the codec's own default)",
    )
    parser.add_argument(
        "-n",
        "--dry-run",
        action="store_true",
        help="list what would change without writing any file",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    """CLI entry point.  Returns a process exit code."""
    args = _build_parser().parse_args(argv)

    total = 0
    for directory in args.dirs:
        if not directory.is_dir():
            print(f"error: not a directory: {directory}", file=sys.stderr)
            return 2
        print(f"{'[dry-run] ' if args.dry_run else ''}gtopt2pbi: {directory}")
        total += relayout_tree(
            directory,
            compression=args.compression,
            compression_level=args.compression_level,
            dry_run=args.dry_run,
        )

    verb = "would convert" if args.dry_run else "converted"
    print(
        f"{verb} {total} wide field file(s) to long + '{args.compression}' "
        "(structural tables, cut files and solve output left untouched)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
