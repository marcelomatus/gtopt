# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.dataframe`."""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from gtopt_shared.dataframe import (
    INDEX_COLS,
    column_to_uid,
    to_long_layout,
    to_wide_layout,
)


@pytest.mark.parametrize(
    "col,expected",
    [
        ("uid:1", 1),
        ("uid:42", 42),
        ("g_pmax:7", 7),
        ("hydro:99", 99),
        ("uid:", None),
        ("uid:abc", None),
        ("no_colon", None),
        ("scenario", None),
    ],
)
def test_column_to_uid_parses_each_recognised_shape(
    col: str, expected: int | None
) -> None:
    """``uid:N`` / ``<name>:N`` parse; everything else returns ``None``."""
    assert column_to_uid(col) == expected


def test_index_cols_is_the_canonical_triple() -> None:
    """``INDEX_COLS`` exposes the (scenario, stage, block) triple gtopt expects."""
    assert INDEX_COLS == ("scenario", "stage", "block")


def test_to_long_layout_pivots_wide_field_table() -> None:
    """Wide ``[scenario, stage, block, uid:1, uid:2]`` → long ``[..., uid, value]``."""
    wide = pd.DataFrame(
        {
            "scenario": [1, 1, 1, 1],
            "stage": [1, 1, 2, 2],
            "block": [1, 2, 1, 2],
            "uid:1": [10.0, 11.0, 12.0, 13.0],
            "uid:2": [20.0, 21.0, 22.0, 23.0],
        }
    )
    long_df = to_long_layout(wide)
    assert long_df is not None
    assert list(long_df.columns) == ["scenario", "stage", "block", "uid", "value"]
    assert len(long_df) == 8  # 4 rows × 2 value cols
    # Round-trip via pivot → wide → equals input.
    wide_again = long_df.pivot_table(
        index=["scenario", "stage", "block"],
        columns="uid",
        values="value",
    ).reset_index()
    wide_again.columns.name = None
    wide_again = wide_again.rename(columns={1: "uid:1", 2: "uid:2"})
    pd.testing.assert_frame_equal(
        wide_again.reset_index(drop=True),
        wide.reset_index(drop=True),
        check_dtype=False,
    )


def test_to_long_layout_preserves_int32_dtypes() -> None:
    """Index cols + uid land as int32 (gtopt long-layout convention)."""
    wide = pd.DataFrame(
        {
            "scenario": [1, 1],
            "stage": [1, 1],
            "block": [1, 2],
            "uid:7": [100.0, 200.0],
        }
    )
    long_df = to_long_layout(wide)
    assert long_df is not None
    for col in ("scenario", "stage", "block", "uid"):
        assert long_df[col].dtype == np.int32, f"{col} dtype is {long_df[col].dtype}"


def test_to_long_layout_returns_none_for_structural_table() -> None:
    """A block / stage definition table (no ``uid:N`` cols) → ``None``."""
    blocks = pd.DataFrame(
        {"uid": [1, 2, 3], "first_block": [0, 1, 2], "count_block": [1, 1, 1]}
    )
    assert to_long_layout(blocks) is None


def test_to_long_layout_returns_none_for_empty_or_none() -> None:
    """Empty DataFrame or ``None`` → ``None`` (writer skips the file)."""
    assert to_long_layout(None) is None  # type: ignore[arg-type]
    assert to_long_layout(pd.DataFrame()) is None


def test_to_long_layout_rejects_mixed_recognised_and_unrecognised_cols() -> None:
    """If any value column doesn't parse as ``<name>:N``, the table is rejected."""
    wide = pd.DataFrame(
        {
            "scenario": [1, 1],
            "stage": [1, 1],
            "block": [1, 2],
            "uid:1": [10.0, 11.0],
            "comment": ["a", "b"],
        }
    )
    assert to_long_layout(wide) is None


def test_to_wide_layout_round_trips_to_long() -> None:
    """``to_wide_layout(to_long_layout(wide))`` reconstructs the wide table."""
    wide = pd.DataFrame(
        {
            "stage": [1, 1, 2, 2],
            "block": [1, 2, 1, 2],
            "uid:1": [10.0, 11.0, 12.0, 13.0],
            "uid:2": [20.0, 21.0, 22.0, 23.0],
        }
    )
    long_df = to_long_layout(wide)
    assert long_df is not None
    wide_again = to_wide_layout(long_df)
    assert wide_again is not None
    assert set(wide_again.columns) == {"stage", "block", "uid:1", "uid:2"}
    # Same cells (order-insensitive), regardless of column order.
    pd.testing.assert_frame_equal(
        wide_again[["stage", "block", "uid:1", "uid:2"]]
        .sort_values(["stage", "block"])
        .reset_index(drop=True),
        wide.sort_values(["stage", "block"]).reset_index(drop=True),
        check_dtype=False,
    )


def test_to_wide_layout_returns_none_for_non_long_tables() -> None:
    """Missing ``uid``/``value`` or index columns → ``None`` (pass-through)."""
    # Already wide (no uid/value pair).
    assert to_wide_layout(pd.DataFrame({"stage": [1], "uid:1": [3.0]})) is None
    # No index column among scenario/stage/block.
    assert to_wide_layout(pd.DataFrame({"uid": [1], "value": [2.0]})) is None
    # Empty / None.
    assert to_wide_layout(pd.DataFrame()) is None
    assert to_wide_layout(None) is None  # type: ignore[arg-type]


def test_plp2gtopt_re_export_shim_preserves_identity() -> None:
    """``plp2gtopt.base_writer`` re-exports the same layout helpers."""
    # pylint: disable=import-outside-toplevel,protected-access
    import plp2gtopt.base_writer as legacy

    assert legacy.to_long_layout is to_long_layout
    assert legacy.to_wide_layout is to_wide_layout
