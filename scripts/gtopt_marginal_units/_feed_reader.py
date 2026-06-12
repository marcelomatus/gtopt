# SPDX-License-Identifier: BSD-3-Clause
"""Read a canonical operation feed (gtopt_canonical_feed) into the
in-memory shape the rest of the package expects.

Thin wrapper over ``gtopt_canonical_feed.read_feed``. Exists so that
``main.py`` can stay agnostic about which package owns the schema.
"""

from __future__ import annotations

from pathlib import Path

from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Cells,
    Topology,
    read_feed,
)


def read_canonical_feed(
    feed_path: Path,
    *,
    drop_lmp: bool = False,
) -> tuple[Topology, Cells]:
    """Load (Topology, Cells) from a canonical feed.

    ``drop_lmp=True`` honours master plan §4.7 R0: in
    ``mode=real-reconstruct`` the script must not read the published
    LMP, since that's the very value being reconstructed.
    """
    topo, cells, _manifest = read_feed(
        Path(feed_path),
        drop_lmp=drop_lmp,
        verify_hashes=True,
        expected_schema_version=SCHEMA_VERSION,
    )
    return topo, cells
