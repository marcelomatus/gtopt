# SPDX-License-Identifier: BSD-3-Clause
"""Canonical-feed writer wrapping ``gtopt_canonical_feed.write_feed``."""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import pandas as pd

from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Cells,
    Manifest,
    Topology,
    write_feed,
)
from gtopt_canonical_feed.cells import (
    COL_BLOCK,
    COL_BUS_UID,
    COL_DATA_SOURCE,
    COL_DATE_UTC,
    COL_DISPATCH,
    COL_GEN_UID,
    COL_HOUR,
    COL_LMP,
    COL_LOAD,
    COL_SCENARIO,
    COL_STAGE,
)


_VERSION = "1.0.0"


def write_canonical_feed(
    out_root: Path,
    *,
    topology: Topology,
    dispatch_long: pd.DataFrame,
    lmp_long: Optional[pd.DataFrame] = None,
    load_long: Optional[pd.DataFrame] = None,
    extras: Optional[dict] = None,
) -> Manifest:
    """Convert (date_utc, hour, *) long-form frames into the canonical
    Cells layout and write the parquet dataset."""
    cells = Cells(
        dispatch=_to_canonical(dispatch_long, COL_GEN_UID, COL_DISPATCH),
        lmp=_to_canonical(lmp_long, COL_BUS_UID, COL_LMP)
        if lmp_long is not None
        else None,
        load=_to_canonical(load_long, COL_BUS_UID, COL_LOAD)
        if load_long is not None
        else None,
    )
    manifest = Manifest.make(
        producer="cen2gtopt",
        producer_version=_VERSION,
        schema_version=SCHEMA_VERSION,
        extras=extras or {},
    )
    return write_feed(out_root, topology, cells, manifest)


def _to_canonical(df: pd.DataFrame, uid_col: str, value_col: str) -> pd.DataFrame:
    """Add the (scenario, stage, block, data_source) cell-key columns
    so the long-form frame matches the canonical schema."""
    out = pd.DataFrame()
    # Cell-key triple — left empty (NA) for real-mode data.
    out[COL_SCENARIO] = pd.NA
    out[COL_STAGE] = pd.NA
    out[COL_BLOCK] = pd.NA
    out[COL_DATE_UTC] = df["date_utc"].astype(str)
    out[COL_HOUR] = pd.to_numeric(df["hour"], errors="coerce").astype("Int64")
    out[COL_DATA_SOURCE] = "real"
    out[uid_col] = df[uid_col].astype(int)
    out[value_col] = df[value_col].astype(float)
    return out
