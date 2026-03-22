# SPDX-License-Identifier: BSD-3-Clause
"""Tests for BaseParser vectorized parsing helpers."""

import numpy as np
import pytest

from plp2gtopt.base_parser import BaseParser


class _ConcreteParser(BaseParser):
    """Minimal concrete parser for testing base class methods."""

    def parse(self, parsers=None):
        pass


@pytest.fixture()
def parser(tmp_path):
    """Return a parser with a dummy file path."""
    dummy = tmp_path / "dummy.dat"
    dummy.write_text("1 2 3\n")
    return _ConcreteParser(dummy)


# ── _parse_numeric_block ──


def test_parse_numeric_block_basic(parser):
    """Standard case: extract int and float columns."""
    lines = ["0 10 1.5 2.5 3.5", "1 20 4.5 5.5 6.5", "2 30 7.5 8.5 9.5"]
    next_idx, cols = parser._parse_numeric_block(
        lines, 0, 3, int_cols=(1,), float_cols=(3, 4)
    )
    assert next_idx == 3
    np.testing.assert_array_equal(cols[1], [10, 20, 30])
    np.testing.assert_allclose(cols[3], [2.5, 5.5, 8.5])
    np.testing.assert_allclose(cols[4], [3.5, 6.5, 9.5])


def test_parse_numeric_block_zero_rows(parser):
    """Zero rows returns empty arrays."""
    next_idx, cols = parser._parse_numeric_block(
        [], 0, 0, int_cols=(0,), float_cols=(1,)
    )
    assert next_idx == 0
    assert len(cols[0]) == 0
    assert len(cols[1]) == 0
    assert cols[0].dtype == np.int32
    assert cols[1].dtype == np.float64


def test_parse_numeric_block_single_row(parser):
    """Single row parses correctly."""
    lines = ["5 100 3.14"]
    next_idx, cols = parser._parse_numeric_block(
        lines, 0, 1, int_cols=(0,), float_cols=(2,)
    )
    assert next_idx == 1
    assert cols[0][0] == 5
    np.testing.assert_allclose(cols[2], [3.14])


def test_parse_numeric_block_with_offset(parser):
    """start_idx offset is respected."""
    lines = ["header line", "0 10 1.5", "1 20 2.5"]
    next_idx, cols = parser._parse_numeric_block(
        lines, 1, 2, int_cols=(0,), float_cols=(2,)
    )
    assert next_idx == 3
    np.testing.assert_array_equal(cols[0], [0, 1])


def test_parse_numeric_block_fallback_on_non_numeric(parser):
    """Falls back to per-line parsing when numpy can't parse."""
    # Lines with mixed text that cause numpy to fail
    lines = ["idx 10 abc 1.5 2.5"]
    # numpy.loadtxt will fail on 'abc' → fallback kicks in
    # But fallback also can't parse 'abc' as float, so it should raise
    with pytest.raises(ValueError):
        parser._parse_numeric_block(lines, 0, 1, int_cols=(1,), float_cols=(2,))


def test_parse_numeric_block_fallback_too_few_fields(parser):
    """Fallback raises ValueError when line has too few fields."""
    # Force fallback with non-numeric content in an early column
    lines = ["a 10 1.5", "b"]  # second line too short
    with pytest.raises(ValueError, match="Expected at least"):
        parser._parse_numeric_block(lines, 0, 2, int_cols=(1,), float_cols=(2,))


def test_parse_numeric_block_int_types(parser):
    """Int columns are int32, float columns are float64."""
    lines = ["1 2 3.0", "4 5 6.0"]
    _, cols = parser._parse_numeric_block(lines, 0, 2, int_cols=(0, 1), float_cols=(2,))
    assert cols[0].dtype == np.int32
    assert cols[1].dtype == np.int32
    assert cols[2].dtype == np.float64


# ── _parse_numeric_block_wide ──


def test_parse_numeric_block_wide_basic(parser):
    """Full row parsed as float64 array."""
    lines = ["1.0 2.0 3.0", "4.0 5.0 6.0"]
    next_idx, arr = parser._parse_numeric_block_wide(lines, 0, 2)
    assert next_idx == 2
    assert arr.shape == (2, 3)
    np.testing.assert_allclose(arr[0], [1.0, 2.0, 3.0])


def test_parse_numeric_block_wide_skip_cols(parser):
    """Leading columns are skipped."""
    lines = ["idx 0 1.0 2.0 3.0", "idx 1 4.0 5.0 6.0"]
    next_idx, arr = parser._parse_numeric_block_wide(lines, 0, 2, skip_cols=2)
    assert next_idx == 2
    assert arr.shape == (2, 3)
    np.testing.assert_allclose(arr[0], [1.0, 2.0, 3.0])


def test_parse_numeric_block_wide_zero_rows(parser):
    """Zero rows returns empty array."""
    next_idx, arr = parser._parse_numeric_block_wide([], 0, 0)
    assert next_idx == 0
    assert arr.shape == (0, 0)


def test_parse_numeric_block_wide_with_offset(parser):
    """Offset into lines is respected."""
    lines = ["header", "1.0 2.0", "3.0 4.0"]
    next_idx, arr = parser._parse_numeric_block_wide(lines, 1, 2)
    assert next_idx == 3
    assert arr.shape == (2, 2)


# ── _read_non_empty_lines ──


def test_read_non_empty_lines_filters_comments(tmp_path):
    """Comment lines and blank lines are filtered out."""
    f = tmp_path / "test.dat"
    f.write_text("data1\n\n# comment\ndata2\n  \ndata3\n")
    p = _ConcreteParser(f)
    lines = p._read_non_empty_lines()
    assert lines == ["data1", "data2", "data3"]


def test_read_non_empty_lines_ascii_ignores_non_ascii(tmp_path):
    """Non-ASCII bytes are silently dropped."""
    f = tmp_path / "test.dat"
    f.write_bytes("Año Mes\n1 2\n".encode("latin-1"))
    p = _ConcreteParser(f)
    lines = p._read_non_empty_lines()
    assert len(lines) == 2
    assert "1 2" in lines[1]


# ── _parse_int / _parse_float ──


def test_parse_int_zero_padded(parser):
    assert parser._parse_int("007") == 7
    assert parser._parse_int("0") == 0
    assert parser._parse_int("000") == 0
    assert parser._parse_int("100") == 100


def test_parse_float(parser):
    assert parser._parse_float("3.14") == pytest.approx(3.14)
    assert parser._parse_float("0.0") == 0.0


# ── _next_idx ──


def test_next_idx_advances(parser):
    assert parser._next_idx(0) == 1
    assert parser._next_idx(5, lines=["a"] * 10) == 6


def test_next_idx_raises_at_end(parser):
    with pytest.raises(IndexError):
        parser._next_idx(2, lines=["a", "b", "c"])


# ── _parse_name ──


def test_parse_name(parser):
    assert parser._parse_name("'Central_1'") == "Central_1"


def test_parse_name_invalid(parser):
    with pytest.raises(ValueError):
        parser._parse_name("no quotes here")


# ── _append / get_item_by_name / get_item_by_number ──


def test_append_and_lookup(parser):
    parser._append({"name": "foo", "number": 42, "val": 1})
    parser._append({"name": "bar", "number": 7, "val": 2})
    assert parser.get_item_by_name("foo")["val"] == 1
    assert parser.get_item_by_number(7)["val"] == 2
    assert parser.get_item_by_name("missing") is None
    assert parser.get_item_by_number(999) is None
    assert parser.num_items == 2


# ── validate_file ──


def test_validate_file_missing(tmp_path):
    p = _ConcreteParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        p.validate_file()


def test_validate_file_directory(tmp_path):
    p = _ConcreteParser(tmp_path)
    with pytest.raises(ValueError, match="not a file"):
        p.validate_file()
