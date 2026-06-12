# SPDX-License-Identifier: BSD-3-Clause
"""Shared DataFrame layout helpers (issue #507 cross-converter sharing).

gtopt's input reader auto-detects layout (a bare ``uid`` + ``value``
column ⇒ long) and pivots long → wide at load, so writers can emit
either shape.  ``long`` is the default because it is the tidy form
Power BI / Power Query expect (no unpivot needed) and matches gtopt's
own solve-output default.

This module centralises the wide→long reshape primitives that were
previously private to ``plp2gtopt.base_writer`` but consumed across
package boundaries by ``gtopt2pbi`` and any future converter that
emits Parquet/CSV output.

Functions:

* :func:`to_long_layout` — pivot a single DataFrame from
  ``[<index cols>, uid:1, uid:2, …]`` wide form into
  ``[<index cols>, uid, value]`` long form.  Returns ``None`` when
  the input isn't a recognisable wide field table.
* :func:`column_to_uid` — parse the integer uid out of a wide
  column name (``uid:N`` or ``<name>:N``).

``plp2gtopt.base_writer`` keeps the legacy ``to_long_layout`` /
``convert_tree_to_long`` names as backward-compat re-exports for
existing consumers (``gtopt2pbi/main.py``,
``plp2gtopt/tests/test_long_layout.py``).
"""

from __future__ import annotations

from typing import Any, Optional

import numpy as np
import pandas as pd


#: Columns that, when present, identify a row's position in the
#: simulation grid (and so must stay as id_vars during the melt).
INDEX_COLS: tuple[str, ...] = ("scenario", "stage", "block")


def column_to_uid(col: str) -> Optional[int]:
    """Parse the integer uid out of a wide value-column name.

    Accepts ``uid:<N>`` and ``<name>:<N>`` (the two forms
    ``pcol_name`` produces).  Returns ``None`` for anything else,
    so the caller can tell a field table from a structural one.
    """
    if ":" not in col:
        return None
    try:
        return int(col.rsplit(":", 1)[1])
    except ValueError:
        return None


def to_long_layout(df: pd.DataFrame) -> Optional[pd.DataFrame]:
    """Reshape a wide field table into long ``[<index cols>, uid, value]``.

    Index columns are the subset of ``scenario`` / ``stage`` / ``block``
    present; every other column must be a ``uid:N`` / ``name:N`` value
    column.  Returns ``None`` when *df* is not a recognisable wide field
    table (e.g. a block or stage definition table), so structural files
    pass through untouched.  The reshape is dense — every wide cell
    becomes one row — so the gtopt long→wide pivot reconstructs the
    original table exactly.
    """
    if df is None or df.empty:
        return None
    id_vars = [c for c in df.columns if c in INDEX_COLS]
    value_vars = [c for c in df.columns if c not in id_vars]
    if not id_vars or not value_vars:
        return None
    uid_map: dict[Any, int] = {}
    for col in value_vars:
        uid = column_to_uid(str(col))
        if uid is None:
            return None
        uid_map[col] = uid
    long_df = df.melt(
        id_vars=id_vars,
        value_vars=value_vars,
        var_name="_col",
        value_name="value",
    )
    long_df["uid"] = long_df["_col"].map(uid_map).astype(np.int32)
    long_df = long_df.drop(columns="_col")
    for col in id_vars:
        long_df[col] = long_df[col].astype(np.int32)
    return long_df[[*id_vars, "uid", "value"]]
