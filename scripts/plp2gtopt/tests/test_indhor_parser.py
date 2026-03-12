"""Tests for IndhorParser and IndhorWriter."""

import pytest
import pandas as pd
from plp2gtopt.indhor_parser import IndhorParser
from plp2gtopt.indhor_writer import IndhorWriter

_SAMPLE_CSV = (
    "Año,Mes,Dia,Hora,Bloque\n"
    "2024,1,1,1,1\n"
    "2024,1,1,2,1\n"
    "2024,1,1,3,2\n"
    "2024,1,1,4,2\n"
    "2024,1,1,5,3\n"
)


def _write_csv(tmp_path, content=_SAMPLE_CSV):
    p = tmp_path / "indhor.csv"
    p.write_text(content, encoding="utf-8")
    return p


def test_parse_creates_dataframe(tmp_path):
    parser = IndhorParser(_write_csv(tmp_path))
    parser.parse()
    assert not parser.is_empty
    assert list(parser.df.columns) == ["year", "month", "day", "hour", "block"]


def test_parse_dtypes(tmp_path):
    parser = IndhorParser(_write_csv(tmp_path))
    parser.parse()
    for col in ("year", "month", "day", "hour", "block"):
        assert parser.df[col].dtype == "int32", f"{col} should be int32"


def test_parse_row_count(tmp_path):
    parser = IndhorParser(_write_csv(tmp_path))
    parser.parse()
    assert len(parser.df) == 5


def test_block_hours_map(tmp_path):
    parser = IndhorParser(_write_csv(tmp_path))
    parser.parse()
    bhm = parser.block_hours_map()
    assert bhm[1] == [1, 2]
    assert bhm[2] == [3, 4]
    assert bhm[3] == [5]


def test_parse_missing_file():
    parser = IndhorParser("/nonexistent/indhor.csv")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_is_empty_before_parse(tmp_path):
    parser = IndhorParser(_write_csv(tmp_path))
    assert parser.is_empty


def test_indhor_written_to_json(tmp_path):
    """IndhorWriter writes parquet and returns correct relative path."""
    p = _write_csv(tmp_path)
    ip = IndhorParser(p)
    ip.parse()

    out_dir = tmp_path / "BlockHourMap"
    writer = IndhorWriter(ip, {"output_format": "parquet", "compression": "gzip"})
    rel = writer.to_parquet(out_dir)
    assert rel == "BlockHourMap/block_hour_map"
    assert (out_dir / "block_hour_map.parquet").exists()
    df = pd.read_parquet(out_dir / "block_hour_map.parquet")
    assert list(df.columns) == ["year", "month", "day", "hour", "block"]
    assert len(df) == 5
