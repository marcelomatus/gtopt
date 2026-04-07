# SPDX-License-Identifier: BSD-3-Clause
"""Tests for LP fingerprint computation and comparison."""

from __future__ import annotations

import json
from pathlib import Path


from gtopt_check_fingerprint._fingerprint import (
    compare_fingerprints,
    compute_from_names,
    load_fingerprint_json,
    write_fingerprint_json,
)


class TestComputeFromNames:
    """Test fingerprint computation from name lists."""

    def test_basic_template(self) -> None:
        col_names = [
            "generator_generation_0_0_0_0",
            "generator_generation_1_0_0_0",
            "bus_theta_0_0_0_0",
        ]
        row_names = [
            "bus_balance_0_0_0_0",
            "generator_capacity_0_0_0",
        ]
        fp = compute_from_names(col_names, row_names)

        assert len(fp.col_template) == 2  # bus.theta + generator.generation
        assert len(fp.row_template) == 2  # bus.balance + generator.capacity

    def test_element_count_independent(self) -> None:
        cols_5 = [f"generator_generation_{i}_0_0_0" for i in range(5)]
        cols_100 = [f"generator_generation_{i}_0_0_0" for i in range(100)]

        fp5 = compute_from_names(cols_5, [])
        fp100 = compute_from_names(cols_100, [])

        assert fp5.structural_hash == fp100.structural_hash
        assert fp5.stats.total_cols == 5
        assert fp100.stats.total_cols == 100

    def test_deterministic(self) -> None:
        names = ["bus_theta_0_0_0_0", "generator_generation_0_0_0_0"]
        fp1 = compute_from_names(names, [])
        fp2 = compute_from_names(names, [])
        assert fp1.structural_hash == fp2.structural_hash

    def test_context_types(self) -> None:
        col_names = [
            "generator_generation_0_0_0_0",  # 3 ctx fields = BlockContext
            "reservoir_volumen_0_0_0",  # 2 ctx fields = StageContext
        ]
        fp = compute_from_names(col_names, [])

        entries = {e.variable_name: e for e in fp.col_template}
        assert entries["generation"].context_type == "BlockContext"
        assert entries["volumen"].context_type == "StageContext"

    def test_stats(self) -> None:
        col_names = [
            "generator_generation_0_0_0_0",
            "generator_generation_1_0_0_0",
            "bus_theta_0_0_0_0",
        ]
        fp = compute_from_names(col_names, [])

        assert fp.stats.total_cols == 3
        assert fp.stats.cols_by_class["generator"] == 2
        assert fp.stats.cols_by_class["bus"] == 1

    def test_hash_length(self) -> None:
        fp = compute_from_names(["bus_theta_0_0_0_0"], [])
        assert len(fp.structural_hash) == 64
        assert len(fp.col_hash) == 64
        assert len(fp.row_hash) == 64


class TestCompare:
    """Test fingerprint comparison."""

    def test_identical(self) -> None:
        fp = compute_from_names(["bus_theta_0_0_0_0"], ["bus_balance_0_0_0_0"])
        result = compare_fingerprints(fp, fp)
        assert result.match

    def test_added_variable(self) -> None:
        expected = compute_from_names(["bus_theta_0_0_0_0"], [])
        actual = compute_from_names(
            ["bus_theta_0_0_0_0", "generator_generation_0_0_0_0"], []
        )
        result = compare_fingerprints(actual, expected)

        assert not result.match
        assert len(result.added_cols) == 1
        assert result.added_cols[0].class_name == "generator"

    def test_removed_variable(self) -> None:
        expected = compute_from_names(
            ["bus_theta_0_0_0_0", "generator_generation_0_0_0_0"], []
        )
        actual = compute_from_names(["bus_theta_0_0_0_0"], [])
        result = compare_fingerprints(actual, expected)

        assert not result.match
        assert len(result.removed_cols) == 1

    def test_stats_ignored(self) -> None:
        """Different element counts should still match structurally."""
        fp5 = compute_from_names(
            [f"generator_generation_{i}_0_0_0" for i in range(5)], []
        )
        fp100 = compute_from_names(
            [f"generator_generation_{i}_0_0_0" for i in range(100)], []
        )
        result = compare_fingerprints(fp5, fp100)
        assert result.match


class TestJsonRoundTrip:
    """Test JSON serialization/deserialization."""

    def test_write_and_load(self, tmp_path: Path) -> None:
        fp = compute_from_names(
            ["bus_theta_0_0_0_0", "generator_generation_0_0_0_0"],
            ["bus_balance_0_0_0_0"],
        )
        json_path = tmp_path / "fingerprint.json"
        write_fingerprint_json(fp, json_path)

        loaded = load_fingerprint_json(json_path)

        assert loaded.structural_hash == fp.structural_hash
        assert loaded.col_hash == fp.col_hash
        assert loaded.row_hash == fp.row_hash
        assert len(loaded.col_template) == len(fp.col_template)
        assert len(loaded.row_template) == len(fp.row_template)

    def test_json_structure(self, tmp_path: Path) -> None:
        fp = compute_from_names(["bus_theta_0_0_0_0"], [])
        json_path = tmp_path / "fingerprint.json"
        write_fingerprint_json(fp, json_path)

        with open(json_path) as f:
            data = json.load(f)

        assert data["version"] == 1
        assert "structural" in data
        assert "stats" in data
        assert "hash" in data["structural"]
        assert "columns" in data["structural"]
        assert "rows" in data["structural"]
