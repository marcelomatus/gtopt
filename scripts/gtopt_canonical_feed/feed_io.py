# SPDX-License-Identifier: BSD-3-Clause
"""Parquet-dataset I/O for the canonical operation feed.

On-disk layout:

    <root>/
      topology/bus.parquet
      topology/generator.parquet
      topology/line.parquet
      cells/dispatch.parquet
      cells/commitment.parquet           # optional
      cells/lmp.parquet                  # optional
      cells/flow.parquet                 # optional
      cells/flow_dual.parquet            # optional
      cells/line_restriction.parquet     # optional
      cells/load.parquet                 # optional
      cells/ens.parquet                  # optional
      manifest.json
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import pandas as pd

from gtopt_canonical_feed.cells import Cells
from gtopt_canonical_feed.errors import (
    FeedHashError,
    FeedSchemaError,
    SchemaVersionError,
)
from gtopt_canonical_feed.manifest import Manifest, sha256_of
from gtopt_canonical_feed.topology import Topology


_TOPOLOGY_FILES = (
    "topology/bus.parquet",
    "topology/generator.parquet",
    "topology/line.parquet",
)
_CELL_FRAMES = (
    ("dispatch", "cells/dispatch.parquet", True),
    ("commitment", "cells/commitment.parquet", False),
    ("lmp", "cells/lmp.parquet", False),
    ("flow", "cells/flow.parquet", False),
    ("flow_dual", "cells/flow_dual.parquet", False),
    ("line_restriction", "cells/line_restriction.parquet", False),
    ("load", "cells/load.parquet", False),
    ("ens", "cells/ens.parquet", False),
)

# Compression: zstd by default per CLAUDE.md guidance for file I/O.
_PARQUET_COMPRESSION = "zstd"


def write_feed(
    root: Path,
    topology: Topology,
    cells: Cells,
    manifest: Manifest,
) -> Manifest:
    """Write a canonical feed dataset to ``root``.

    Returns the manifest with file_hashes and row_counts populated.
    The caller is responsible for creating ``manifest`` with the
    correct producer / schema_version (use ``Manifest.make`` plus
    the package's SCHEMA_VERSION).
    """
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / "topology").mkdir(exist_ok=True)
    (root / "cells").mkdir(exist_ok=True)

    # Topology — three small frames, always written.
    buses_df, generators_df, lines_df = topology.to_frames()
    _write_parquet(
        root / "topology/bus.parquet", buses_df, manifest, "topology/bus.parquet"
    )
    _write_parquet(
        root / "topology/generator.parquet",
        generators_df,
        manifest,
        "topology/generator.parquet",
    )
    _write_parquet(
        root / "topology/line.parquet", lines_df, manifest, "topology/line.parquet"
    )

    # Cells — dispatch is required; the rest are optional and only written
    # when the corresponding attribute is non-None and non-empty.
    for attr, rel_path, required in _CELL_FRAMES:
        df = getattr(cells, attr)
        if df is None or (hasattr(df, "empty") and df.empty):
            if required:
                raise FeedSchemaError(f"required cells frame is empty: {attr}")
            continue
        _write_parquet(root / rel_path, df, manifest, rel_path)

    manifest.write(root)
    return manifest


def read_feed(
    root: Path,
    *,
    drop_lmp: bool = False,
    verify_hashes: bool = True,
    expected_schema_version: Optional[str] = None,
) -> tuple[Topology, Cells, Manifest]:
    """Read a canonical feed dataset.

    Args:
        root: dataset directory.
        drop_lmp: when True, skip ``cells/lmp.parquet`` even if present.
            Used by ``mode=real-reconstruct`` where the LMP must be
            reconstructed (§4.7) and reading the published value would
            contaminate the audit.
        verify_hashes: re-compute sha256 of every parquet file and
            compare against ``manifest.file_hashes``. On mismatch,
            raises ``FeedHashError``.
        expected_schema_version: when provided, raises
            ``SchemaVersionError`` if the manifest's schema_version
            does not match. Callers from gtopt_marginal_units pass the
            package's SCHEMA_VERSION here.
    """
    root = Path(root)
    manifest = Manifest.read(root)
    if (
        expected_schema_version is not None
        and manifest.schema_version != expected_schema_version
    ):
        raise SchemaVersionError(
            f"feed schema_version={manifest.schema_version!r} but reader expected "
            f"{expected_schema_version!r} (root={root})"
        )

    if verify_hashes:
        _verify_hashes(root, manifest)

    # Topology — required.
    buses_df = _read_parquet_required(root / "topology/bus.parquet")
    generators_df = _read_parquet_required(root / "topology/generator.parquet")
    lines_df = _read_parquet_required(root / "topology/line.parquet")
    topology = Topology.from_frames(buses_df, generators_df, lines_df)

    # Cells.
    cells = Cells(dispatch=_read_parquet_required(root / "cells/dispatch.parquet"))
    for attr, rel_path, _required in _CELL_FRAMES[1:]:  # skip dispatch (already done)
        path = root / rel_path
        if not path.exists():
            continue
        if attr == "lmp" and drop_lmp:
            continue
        setattr(cells, attr, _read_parquet_optional(path))

    return topology, cells, manifest


def verify_feed(root: Path) -> Manifest:
    """Verify a feed's manifest hashes without loading any frames.

    Useful as a smoke check before reading; raises FeedHashError on
    any mismatch. Returns the manifest on success.
    """
    root = Path(root)
    manifest = Manifest.read(root)
    _verify_hashes(root, manifest)
    return manifest


def _write_parquet(
    path: Path, df: pd.DataFrame, manifest: Manifest, rel_path: str
) -> None:
    df.to_parquet(path, compression=_PARQUET_COMPRESSION, index=False)
    manifest.file_hashes[rel_path] = sha256_of(path)
    manifest.row_counts[rel_path] = int(len(df))


def _read_parquet_required(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FeedSchemaError(f"required parquet missing: {path}")
    return pd.read_parquet(path)


def _read_parquet_optional(path: Path) -> Optional[pd.DataFrame]:
    if not path.exists():
        return None
    return pd.read_parquet(path)


def _verify_hashes(root: Path, manifest: Manifest) -> None:
    for rel_path, expected in manifest.file_hashes.items():
        path = root / rel_path
        if not path.exists():
            raise FeedHashError(f"manifest references missing file: {rel_path}")
        actual = sha256_of(path)
        if actual != expected:
            raise FeedHashError(
                f"sha256 mismatch on {rel_path}: manifest={expected[:12]}…, file={actual[:12]}…"
            )
