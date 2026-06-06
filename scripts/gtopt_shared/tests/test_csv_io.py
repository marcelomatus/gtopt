# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``gtopt_shared.csv_io.write_csv``."""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.csv as pa_csv
import pytest

from gtopt_shared.csv_io import write_csv


def _read_back(path: Path) -> pa.Table:
    return pa_csv.read_csv(str(path))


def test_write_csv_from_dataframe(tmp_path: Path) -> None:
    df = pd.DataFrame({"scenario": [0, 0, 1], "value": [1.0, 2.0, 3.5]})
    out = tmp_path / "ok.csv"
    write_csv(df, out)

    back = _read_back(out)
    assert back.column_names == ["scenario", "value"]
    assert back.column("scenario").to_pylist() == [0, 0, 1]
    assert back.column("value").to_pylist() == [1.0, 2.0, 3.5]


def test_write_csv_from_pyarrow_table(tmp_path: Path) -> None:
    table = pa.table({"k": [10, 20], "v": [-1.5, 2.5]})
    out = tmp_path / "table.csv"
    write_csv(table, out)

    back = _read_back(out)
    assert back.column("k").to_pylist() == [10, 20]
    assert back.column("v").to_pylist() == [-1.5, 2.5]


def test_write_csv_from_row_iterable_with_headers(tmp_path: Path) -> None:
    headers = ["iteration", "scene", "rhs", "rA", "rB"]
    rows = [[1, 0, 100.5, 0.25, 0.75]]
    out = tmp_path / "rows.csv"
    write_csv(rows, out, headers=headers)

    back = _read_back(out)
    assert back.column_names == headers
    assert back.column("iteration").to_pylist() == [1]
    assert back.column("rB").to_pylist() == [0.75]


def test_write_csv_rejects_table_with_headers(tmp_path: Path) -> None:
    table = pa.table({"x": [1.0]})
    with pytest.raises(TypeError, match="pyarrow.Table"):
        write_csv(table, tmp_path / "x.csv", headers=["x"])


def test_write_csv_rejects_dataframe_with_headers(tmp_path: Path) -> None:
    df = pd.DataFrame({"x": [1.0]})
    with pytest.raises(TypeError, match="pandas.DataFrame"):
        write_csv(df, tmp_path / "x.csv", headers=["x"])


def test_write_csv_rejects_rows_without_headers(tmp_path: Path) -> None:
    with pytest.raises(TypeError, match="headers is required"):
        write_csv([[1, 2, 3]], tmp_path / "x.csv")


def test_write_csv_rejects_row_length_mismatch(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="row 1 has 2 values"):
        write_csv(
            [[1, 2, 3], [4, 5]],
            tmp_path / "x.csv",
            headers=["a", "b", "c"],
        )
