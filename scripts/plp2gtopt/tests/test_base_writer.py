# SPDX-License-Identifier: BSD-3-Clause
"""Tests for BaseWriter._create_dataframe optimized path."""

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.base_writer import BaseWriter, _probe_parquet_codec


class _FakeParser:
    """Minimal parser stub for unit tests."""

    def __init__(self, items):
        self._items = {item["name"]: item for item in items if "name" in item}
        self._all = items

    def get_all(self):
        return self._all

    def get_item_by_name(self, name):
        return self._items.get(name)

    @property
    def items(self):
        return self._all


class _ConcreteWriter(BaseWriter):
    """Concrete writer for testing base class methods."""

    def __init__(self, parser=None, options=None):
        if parser is None:
            parser = _FakeParser([])
        super().__init__(parser, options)


# ── _create_dataframe: identity value_oper ──


def test_create_dataframe_identity_numpy_values():
    """Numpy array values pass through without copy when identity."""
    units = _FakeParser([{"name": "g1", "number": 1}])
    items = [
        {"name": "g1", "value": np.array([1.0, 2.0, 3.0]), "block": np.array([1, 2, 3])}
    ]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert not df.empty
    assert "uid:1" in df.columns


def test_create_dataframe_identity_list_values():
    """List values are converted to numpy arrays."""
    units = _FakeParser([{"name": "g1", "number": 1}])
    items = [{"name": "g1", "value": [10.0, 20.0], "block": [1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert len(df) == 2
    np.testing.assert_allclose(df["uid:1"].values, [10.0, 20.0])


# ── _create_dataframe: custom value_oper ──


def test_create_dataframe_custom_value_oper():
    """Custom value_oper is applied to each value."""
    units = _FakeParser([{"name": "g1", "number": 1}])
    items = [{"name": "g1", "value": [[1, 2], [3, 4]], "block": [1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
        value_oper=lambda v: v[0],
    )
    np.testing.assert_allclose(df["uid:1"].values, [1.0, 3.0])


# ── _create_dataframe: fill_values skipping ──


def test_create_dataframe_skips_constant_fill():
    """Items whose values all match the fill value are skipped."""
    units = _FakeParser([{"name": "g1", "number": 1, "pmax": 100.0}])
    items = [{"name": "g1", "value": [100.0, 100.0], "block": [1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
        fill_field="pmax",
    )
    assert df.empty


def test_create_dataframe_keeps_non_constant():
    """Items with non-constant values are kept even with fill_field."""
    units = _FakeParser([{"name": "g1", "number": 1, "pmax": 100.0}])
    items = [{"name": "g1", "value": [100.0, 50.0], "block": [1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
        fill_field="pmax",
    )
    assert not df.empty


# ── _create_dataframe: duplicate indices ──


def test_create_dataframe_duplicate_indices_keep_last():
    """Duplicate index entries keep the last value."""
    units = _FakeParser([{"name": "g1", "number": 1}])
    items = [{"name": "g1", "value": [1.0, 2.0, 3.0], "block": [1, 1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    # block=1 should have value 2.0 (last), block=2 should have 3.0
    assert df.loc[1, "uid:1"] == pytest.approx(2.0)
    assert df.loc[2, "uid:1"] == pytest.approx(3.0)


# ── _create_dataframe: skip_types ──


def test_create_dataframe_skip_types():
    """Items with skipped types are excluded."""
    units = _FakeParser(
        [
            {"name": "g1", "number": 1, "type": "falla"},
            {"name": "g2", "number": 2, "type": "thermal"},
        ]
    )
    items = [
        {"name": "g1", "value": [1.0], "block": [1]},
        {"name": "g2", "value": [2.0], "block": [1]},
    ]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
        skip_types=("falla",),
    )
    assert "uid:2" in df.columns
    assert "uid:1" not in df.columns


# ── _create_dataframe: empty items ──


def test_create_dataframe_empty_items():
    writer = _ConcreteWriter()
    df = writer._create_dataframe(
        [],
        unit_parser=None,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert df.empty


def test_create_dataframe_no_matching_units():
    """All items filtered by unit_parser returns empty."""
    units = _FakeParser([])
    items = [{"name": "unknown", "value": [1.0], "block": [1]}]
    writer = _ConcreteWriter()
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert df.empty


# ── _create_dataframe: with index_parser ──


def test_create_dataframe_with_index_parser():
    """Index parser adds an index column to the DataFrame."""
    units = _FakeParser([{"name": "g1", "number": 1}])
    blocks = _FakeParser([{"number": 1}, {"number": 2}])
    items = [{"name": "g1", "value": [10.0, 20.0], "block": [1, 2]}]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=blocks,
        value_field="value",
        index_field="block",
    )
    assert "block" in df.columns


# ── _create_dataframe: multiple items with aligned indices ──


def test_create_dataframe_multiple_items_aligned():
    """Multiple items with same index produce multi-column DataFrame."""
    units = _FakeParser(
        [
            {"name": "g1", "number": 1},
            {"name": "g2", "number": 2},
        ]
    )
    items = [
        {"name": "g1", "value": [1.0, 2.0], "block": [1, 2]},
        {"name": "g2", "value": [3.0, 4.0], "block": [1, 2]},
    ]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert "uid:1" in df.columns
    assert "uid:2" in df.columns
    assert len(df) == 2


def test_create_dataframe_multiple_items_misaligned():
    """Items with different indices are aligned via concat."""
    units = _FakeParser(
        [
            {"name": "g1", "number": 1},
            {"name": "g2", "number": 2},
        ]
    )
    items = [
        {"name": "g1", "value": [1.0, 2.0], "block": [1, 2]},
        {"name": "g2", "value": [3.0, 4.0], "block": [2, 3]},
    ]
    writer = _ConcreteWriter(options={"use_uid_label": True})
    df = writer._create_dataframe(
        items,
        unit_parser=units,
        index_parser=None,
        value_field="value",
        index_field="block",
    )
    assert len(df) == 3  # blocks 1, 2, 3
    assert pd.isna(df.loc[1, "uid:2"])  # g2 has no block 1
    assert pd.isna(df.loc[3, "uid:1"])  # g1 has no block 3


# ── _probe_parquet_codec ──


def test_probe_parquet_codec_empty():
    assert _probe_parquet_codec("") == ""
    assert _probe_parquet_codec("none") == "none"
    assert _probe_parquet_codec("uncompressed") == "uncompressed"


def test_probe_parquet_codec_gzip():
    assert _probe_parquet_codec("gzip") == "gzip"


# ── get_compression / pcol_name ──


def test_get_compression_default():
    writer = _ConcreteWriter(options={})
    result = writer.get_compression()
    assert result is not None  # default should be zstd or gzip


def test_get_compression_uncompressed():
    writer = _ConcreteWriter(options={"compression": "none"})
    assert writer.get_compression() is None


def test_pcol_name_uid_label():
    writer = _ConcreteWriter(options={"use_uid_label": True})
    assert writer.pcol_name("gen1", 42) == "uid:42"


def test_pcol_name_name_label():
    writer = _ConcreteWriter(options={"use_uid_label": False})
    assert writer.pcol_name("gen1", 42) == "gen1:42"


def test_pcol_name_string_number():
    writer = _ConcreteWriter(options={"use_uid_label": True})
    assert writer.pcol_name("gen1", "custom") == "custom"
