# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.output_format`."""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from gtopt_shared.output_format import (
    OUTPUT_FORMATS,
    format_suffix,
    write_dataframe,
)


def test_output_formats_pair() -> None:
    """``OUTPUT_FORMATS`` lists exactly the two formats gtopt accepts."""
    assert OUTPUT_FORMATS == ("parquet", "csv")


@pytest.mark.parametrize(
    "fmt,expected",
    [("parquet", ".parquet"), ("csv", ".csv")],
)
def test_format_suffix_known(fmt: str, expected: str) -> None:
    assert format_suffix(fmt) == expected


def test_format_suffix_unknown_raises_value_error() -> None:
    """Unknown formats are rejected with a clear message (no silent default)."""
    with pytest.raises(ValueError, match="unsupported output_format"):
        format_suffix("feather")
    with pytest.raises(ValueError, match="unsupported output_format"):
        format_suffix("")


def test_write_dataframe_emits_parquet_with_suffix(tmp_path: Path) -> None:
    df = pd.DataFrame({"scenario": [1, 1], "uid": [1, 2], "value": [10.0, 20.0]})
    out = write_dataframe(df, tmp_path, "demand_sol")
    assert out == tmp_path / "demand_sol.parquet"
    assert out.exists()
    # Round-trip cleanly.
    pd.testing.assert_frame_equal(pd.read_parquet(out), df, check_dtype=False)


def test_write_dataframe_emits_csv_when_requested(tmp_path: Path) -> None:
    df = pd.DataFrame({"scenario": [1], "uid": [42], "value": [3.14]})
    out = write_dataframe(df, tmp_path, "load_sol", output_format="csv")
    assert out == tmp_path / "load_sol.csv"
    text = out.read_text()
    assert "scenario" in text and "42" in text


def test_write_dataframe_propagates_compression_args_for_parquet(
    tmp_path: Path,
) -> None:
    """Compression kwargs are forwarded; CSV ignores them silently."""
    df = pd.DataFrame({"scenario": [1], "uid": [1], "value": [1.0]})
    out = write_dataframe(
        df,
        tmp_path,
        "z",
        output_format="parquet",
        compression="zstd",
        compression_level=3,
    )
    assert out.exists()
    # CSV path should still write fine even when compression kwargs are
    # supplied (ignored, not crashed).
    out_csv = write_dataframe(
        df,
        tmp_path,
        "z_csv",
        output_format="csv",
        compression="zstd",
        compression_level=3,
    )
    assert out_csv.exists()


def test_write_dataframe_unknown_format_raises(tmp_path: Path) -> None:
    df = pd.DataFrame({"scenario": [1], "uid": [1], "value": [1.0]})
    with pytest.raises(ValueError, match="unsupported output_format"):
        write_dataframe(df, tmp_path, "x", output_format="feather")
