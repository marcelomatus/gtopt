# SPDX-License-Identifier: BSD-3-Clause
"""Cells — the long-form per-cell data tables.

Each table is keyed on the cell tuple plus the relevant entity uid
(generator / bus / line). Stored on disk as parquet under
``<feed>/cells/<table>.parquet``. Tables are written in long form
(one row per (cell, uid)) per the python-reviewer recommendation.

The Cells container is a thin holder of pandas DataFrames; the
actual schema is enforced via the COL_* constants and dtype
coercion in feed_io.write_feed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import pandas as pd


# ---------------------------------------------------------------------------
# Cell-key columns. Either the gtopt triple (scenario, stage, block) or the
# real-operation pair (date_utc, hour) is populated per row; the unused side
# is NA. data_source tags the producer.
# ---------------------------------------------------------------------------
COL_SCENARIO = "scenario"
COL_STAGE = "stage"
COL_BLOCK = "block"
COL_DATE_UTC = "date_utc"
COL_HOUR = "hour"
COL_DATA_SOURCE = "data_source"  # "simulated" | "real"

CELL_KEY_COLS = [
    COL_SCENARIO,
    COL_STAGE,
    COL_BLOCK,
    COL_DATE_UTC,
    COL_HOUR,
    COL_DATA_SOURCE,
]

# Per-table value columns (the long-form `value` column is named after the
# physical quantity, so consumers can join multiple cell tables on cell-key
# without aliasing).
COL_GEN_UID = "gen_uid"
COL_BUS_UID = "bus_uid"
COL_LINE_UID = "line_uid"

COL_DISPATCH = "dispatch"
COL_COMMITMENT = "commitment"
COL_LMP = "lmp"
COL_FLOW = "flow"
COL_FLOW_DUAL = "flow_dual"
COL_RESTRICTION_DECLARED = "restriction_declared"
COL_LOAD = "load"
COL_ENS = "ens"


@dataclass(slots=True)
class Cells:
    """Container for the per-cell long-form frames. Any frame may be
    None if the producer did not populate it.

    The cell-key columns appear on every frame. The value column is
    named for the physical quantity (e.g. ``dispatch`` on the
    dispatch frame, ``lmp`` on the LMP frame).
    """

    dispatch: pd.DataFrame = field(
        default_factory=lambda: _empty_with_cols(
            CELL_KEY_COLS + [COL_GEN_UID, COL_DISPATCH]
        )
    )
    commitment: Optional[pd.DataFrame] = None
    lmp: Optional[pd.DataFrame] = None
    flow: Optional[pd.DataFrame] = None
    flow_dual: Optional[pd.DataFrame] = None
    line_restriction: Optional[pd.DataFrame] = None
    load: Optional[pd.DataFrame] = None
    ens: Optional[pd.DataFrame] = None

    def has_lmp(self) -> bool:
        return self.lmp is not None and not self.lmp.empty

    def has_flow(self) -> bool:
        return self.flow is not None and not self.flow.empty

    def has_flow_dual(self) -> bool:
        return self.flow_dual is not None and not self.flow_dual.empty

    def has_line_restriction(self) -> bool:
        return self.line_restriction is not None and not self.line_restriction.empty

    def cell_keys(self) -> pd.DataFrame:
        """Distinct cell keys across all populated frames."""
        if self.dispatch.empty:
            return _empty_with_cols(CELL_KEY_COLS)
        return self.dispatch[CELL_KEY_COLS].drop_duplicates().reset_index(drop=True)


def _empty_with_cols(cols: list[str]) -> pd.DataFrame:
    return pd.DataFrame({c: pd.Series(dtype="object") for c in cols})
