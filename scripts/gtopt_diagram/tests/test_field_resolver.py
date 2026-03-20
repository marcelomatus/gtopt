# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_diagram._field_resolver -- FieldSchedResolver.

Covers:
- resolve() with scalar field (not a file ref) returns fallback
- resolve() with missing uid returns fallback
- resolve() with Parquet file (temp Parquet via pyarrow)
- resolve() with CSV file (temp CSV)
- resolve() with cross-class reference (e.g. "Flow@discharge")
- _extract_row() filtering by scenario/stage/block
- Caching: same file read only once
"""

from __future__ import annotations

from unittest import mock

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

from gtopt_diagram._field_resolver import FieldSchedResolver


# ---------------------------------------------------------------------------
# resolve() basics
# ---------------------------------------------------------------------------


class TestResolveBasics:
    """Verify resolve() behaviour for non-file-reference scenarios."""

    def test_missing_uid_returns_fallback(self, tmp_path):
        """resolve() with uid=None returns the fallback value."""
        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=None, fallback="N/A") == "N/A"

    def test_missing_uid_returns_default_none(self, tmp_path):
        """resolve() with uid=None and no explicit fallback returns None."""
        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=None) is None

    def test_no_file_returns_fallback(self, tmp_path):
        """When no Parquet or CSV file exists, resolve() returns fallback."""
        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=1, fallback=-1) == -1

    def test_uid_not_in_file_returns_fallback(self, tmp_path):
        """resolve() returns fallback when uid column is absent from the file."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:99": [42.0]})
        table = pa.Table.from_pandas(df)
        pq.write_table(table, gen_dir / "pmax.parquet")

        resolver = FieldSchedResolver(tmp_path)
        result = resolver.resolve("Generator", "pmax", uid=1, fallback="miss")
        assert result == "miss"


# ---------------------------------------------------------------------------
# resolve() with Parquet files
# ---------------------------------------------------------------------------


class TestResolveParquet:
    """Verify resolve() reads Parquet files and returns correct values."""

    def test_parquet_basic_read(self, tmp_path):
        """resolve() reads a simple Parquet file and returns the value for uid."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:1": [250.0],
                "uid:2": [300.0],
            }
        )
        table = pa.Table.from_pandas(df)
        pq.write_table(table, gen_dir / "pmax.parquet")

        resolver = FieldSchedResolver(tmp_path, scenario=1, stage=1, block=1)
        assert resolver.resolve("Generator", "pmax", uid=1) == 250.0
        assert resolver.resolve("Generator", "pmax", uid=2) == 300.0

    def test_parquet_with_input_dir(self, tmp_path):
        """resolve() respects the input_dir parameter."""
        input_dir = tmp_path / "input"
        gen_dir = input_dir / "Generator"
        gen_dir.mkdir(parents=True)
        df = pd.DataFrame({"uid:5": [77.0]})
        table = pa.Table.from_pandas(df)
        pq.write_table(table, gen_dir / "pmax.parquet")

        resolver = FieldSchedResolver(tmp_path, input_dir="input")
        assert resolver.resolve("Generator", "pmax", uid=5) == 77.0

    def test_parquet_string_uid_coerced(self, tmp_path):
        """resolve() coerces string uid to int for lookup."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame({"uid:3": [99.5]})
        table = pa.Table.from_pandas(df)
        pq.write_table(table, gen_dir / "pmax.parquet")

        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid="3") == 99.5


# ---------------------------------------------------------------------------
# resolve() with CSV files
# ---------------------------------------------------------------------------


class TestResolveCSV:
    """Verify resolve() reads CSV files when Parquet is absent."""

    def test_csv_basic_read(self, tmp_path):
        """resolve() falls back to CSV when no Parquet exists."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:1": [150.0],
                "uid:2": [200.0],
            }
        )
        df.to_csv(gen_dir / "pmax.csv", index=False)

        resolver = FieldSchedResolver(tmp_path, scenario=1, stage=1, block=1)
        assert resolver.resolve("Generator", "pmax", uid=1) == 150.0
        assert resolver.resolve("Generator", "pmax", uid=2) == 200.0

    def test_parquet_preferred_over_csv(self, tmp_path):
        """When both Parquet and CSV exist, Parquet is read."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()

        # Parquet file with value 500
        df_pq = pd.DataFrame({"uid:1": [500.0]})
        pq.write_table(pa.Table.from_pandas(df_pq), gen_dir / "pmax.parquet")

        # CSV file with value 999
        df_csv = pd.DataFrame({"uid:1": [999.0]})
        df_csv.to_csv(gen_dir / "pmax.csv", index=False)

        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=1) == 500.0


# ---------------------------------------------------------------------------
# Cross-class references
# ---------------------------------------------------------------------------


class TestCrossClassReference:
    """Verify resolve() handles cross-class references (e.g. 'Flow@discharge')."""

    def test_cross_class_ref_splits_correctly(self, tmp_path):
        """'Flow@discharge' reads from Flow/discharge.parquet, not Generator/."""
        flow_dir = tmp_path / "Flow"
        flow_dir.mkdir()
        df = pd.DataFrame({"uid:1": [42.0]})
        pq.write_table(pa.Table.from_pandas(df), flow_dir / "discharge.parquet")

        resolver = FieldSchedResolver(tmp_path)
        result = resolver.resolve("Generator", "Flow@discharge", uid=1)
        assert result == 42.0

    def test_cross_class_ref_missing_returns_fallback(self, tmp_path):
        """Cross-class ref to nonexistent file returns fallback."""
        resolver = FieldSchedResolver(tmp_path)
        result = resolver.resolve("Generator", "Flow@discharge", uid=1, fallback=-1)
        assert result == -1


# ---------------------------------------------------------------------------
# _extract_row() filtering
# ---------------------------------------------------------------------------


class TestExtractRow:
    """Verify _extract_row() filters by scenario/stage/block correctly."""

    def _make_resolver(self, tmp_path, scenario=1, stage=1, block=1):
        return FieldSchedResolver(
            tmp_path, scenario=scenario, stage=stage, block=block
        )

    def test_filters_by_scenario(self, tmp_path):
        """Rows with matching scenario are selected."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [1, 2],
                "uid:1": [10.0, 20.0],
            }
        )
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path, scenario=2)
        assert resolver.resolve("Generator", "pmax", uid=1) == 20.0

    def test_filters_by_stage(self, tmp_path):
        """Rows with matching stage are selected."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "stage": [1, 2],
                "uid:1": [100.0, 200.0],
            }
        )
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path, stage=2)
        assert resolver.resolve("Generator", "pmax", uid=1) == 200.0

    def test_filters_by_block(self, tmp_path):
        """Rows with matching block are selected."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "block": [1, 2, 3],
                "uid:1": [5.0, 10.0, 15.0],
            }
        )
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path, block=3)
        assert resolver.resolve("Generator", "pmax", uid=1) == 15.0

    def test_filters_by_all_three(self, tmp_path):
        """Rows matching scenario+stage+block are selected."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [1, 1, 2, 2],
                "stage": [1, 2, 1, 2],
                "block": [1, 1, 1, 1],
                "uid:1": [10.0, 20.0, 30.0, 40.0],
            }
        )
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path, scenario=2, stage=2, block=1)
        assert resolver.resolve("Generator", "pmax", uid=1) == 40.0

    def test_falls_back_to_first_scenario(self, tmp_path):
        """When requested scenario is missing, falls back to first available."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [5, 5],
                "uid:1": [99.0, 99.0],
            }
        )
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path, scenario=999)
        assert resolver.resolve("Generator", "pmax", uid=1) == 99.0

    def test_no_filter_columns(self, tmp_path):
        """DataFrame without scenario/stage/block columns still works."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame({"uid:1": [33.0], "uid:2": [44.0]})
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = self._make_resolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=1) == 33.0
        assert resolver.resolve("Generator", "pmax", uid=2) == 44.0


# ---------------------------------------------------------------------------
# Caching
# ---------------------------------------------------------------------------


class TestCaching:
    """Verify that the same file is read only once (cached)."""

    def test_second_resolve_uses_cache(self, tmp_path):
        """Calling resolve() twice for same class/field should read file once."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()
        df = pd.DataFrame({"uid:1": [100.0], "uid:2": [200.0]})
        pq.write_table(pa.Table.from_pandas(df), gen_dir / "pmax.parquet")

        resolver = FieldSchedResolver(tmp_path)
        # First call populates the cache
        assert resolver.resolve("Generator", "pmax", uid=1) == 100.0

        # Patch _load_file to verify it is NOT called again
        with mock.patch.object(resolver, "_load_file", wraps=resolver._load_file) as m:
            result = resolver.resolve("Generator", "pmax", uid=2)
            assert result == 200.0
            m.assert_not_called()

    def test_different_fields_not_shared(self, tmp_path):
        """Different field names use separate cache entries."""
        gen_dir = tmp_path / "Generator"
        gen_dir.mkdir()

        df_pmax = pd.DataFrame({"uid:1": [100.0]})
        pq.write_table(pa.Table.from_pandas(df_pmax), gen_dir / "pmax.parquet")

        df_pmin = pd.DataFrame({"uid:1": [10.0]})
        pq.write_table(pa.Table.from_pandas(df_pmin), gen_dir / "pmin.parquet")

        resolver = FieldSchedResolver(tmp_path)
        assert resolver.resolve("Generator", "pmax", uid=1) == 100.0
        assert resolver.resolve("Generator", "pmin", uid=1) == 10.0
        assert len(resolver._cache) == 2

    def test_cache_key_uses_class_and_field(self, tmp_path):
        """Cache key is (class_name, field_name), not the resolved path."""
        resolver = FieldSchedResolver(tmp_path)
        # Access two different classes -- both miss, but cache has 2 entries
        resolver.resolve("Generator", "pmax", uid=1)
        resolver.resolve("Demand", "pmax", uid=1)
        assert ("Generator", "pmax") in resolver._cache
        assert ("Demand", "pmax") in resolver._cache


# ---------------------------------------------------------------------------
# _root() path resolution
# ---------------------------------------------------------------------------


class TestRootPath:
    """Verify _root() resolves input_dir correctly."""

    def test_dot_input_dir_uses_base(self, tmp_path):
        resolver = FieldSchedResolver(tmp_path, input_dir=".")
        assert resolver._root() == tmp_path

    def test_empty_input_dir_uses_base(self, tmp_path):
        resolver = FieldSchedResolver(tmp_path, input_dir="")
        assert resolver._root() == tmp_path

    def test_custom_input_dir(self, tmp_path):
        resolver = FieldSchedResolver(tmp_path, input_dir="data")
        assert resolver._root() == tmp_path / "data"
