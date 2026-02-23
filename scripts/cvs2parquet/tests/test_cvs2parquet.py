"""Tests for cvs2parquet.py."""

from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from cvs2parquet.cvs2parquet import _infer_schema, csv_to_parquet

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_csv(tmp_path: Path, name: str, content: str) -> Path:
    p = tmp_path / name
    p.write_text(content)
    return p


# ---------------------------------------------------------------------------
# _infer_schema
# ---------------------------------------------------------------------------


def test_infer_schema_int32_columns():
    df = pd.DataFrame({"stage": [1], "block": [2], "scenario": [3], "val": [1.0]})
    schema = _infer_schema(df)
    assert schema.field("stage").type == pa.int32()
    assert schema.field("block").type == pa.int32()
    assert schema.field("scenario").type == pa.int32()
    assert schema.field("val").type == pa.float64()


def test_infer_schema_no_index_cols():
    df = pd.DataFrame({"a": [1.0], "b": [2.0]})
    schema = _infer_schema(df)
    assert schema.field("a").type == pa.float64()
    assert schema.field("b").type == pa.float64()


# ---------------------------------------------------------------------------
# csv_to_parquet – pandas path (use_schema=False)
# ---------------------------------------------------------------------------


def test_csv_to_parquet_basic(tmp_path):
    csv_path = _write_csv(
        tmp_path, "data.csv", "stage,block,val\n1,1,100.0\n2,2,200.0\n"
    )
    out_path = tmp_path / "data.parquet"
    csv_to_parquet(str(csv_path), str(out_path))

    df = pd.read_parquet(out_path)
    assert list(df.columns) == ["stage", "block", "val"]
    assert df["stage"].dtype == "int32"
    assert df["block"].dtype == "int32"
    assert df["val"].dtype == "float64"
    assert len(df) == 2


def test_csv_to_parquet_no_index_cols(tmp_path):
    """Columns not in _INT32_COLS are cast to float64."""
    csv_path = _write_csv(tmp_path, "vals.csv", "uid:1,uid:2\n10.0,20.0\n30.0,40.0\n")
    out_path = tmp_path / "vals.parquet"
    csv_to_parquet(str(csv_path), str(out_path))

    df = pd.read_parquet(out_path)
    assert df["uid:1"].dtype == "float64"
    assert df["uid:2"].dtype == "float64"


def test_csv_to_parquet_roundtrip_values(tmp_path):
    csv_path = _write_csv(
        tmp_path, "rt.csv", "stage,block,scenario,power\n1,1,1,50.5\n2,3,2,75.0\n"
    )
    out_path = tmp_path / "rt.parquet"
    csv_to_parquet(str(csv_path), str(out_path))

    df = pd.read_parquet(out_path)
    assert df.iloc[0]["stage"] == 1
    assert df.iloc[1]["power"] == pytest.approx(75.0)


# ---------------------------------------------------------------------------
# csv_to_parquet – PyArrow schema path (use_schema=True)
# ---------------------------------------------------------------------------


def test_csv_to_parquet_schema_path(tmp_path):
    csv_path = _write_csv(tmp_path, "s.csv", "stage,block,val\n1,2,3.0\n")
    out_path = tmp_path / "s.parquet"
    csv_to_parquet(str(csv_path), str(out_path), use_schema=True)

    table = pq.read_table(out_path)
    assert table.schema.field("stage").type == pa.int32()
    assert table.schema.field("block").type == pa.int32()
    assert table.schema.field("val").type == pa.float64()
