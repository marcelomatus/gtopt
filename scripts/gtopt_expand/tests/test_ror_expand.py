# -*- coding: utf-8 -*-

"""Tests for the RoR equivalence expansion (CSV parsing + CLI)."""

import json

import pytest

from gtopt_expand.cli import main as cli_main
from gtopt_expand.ror_expand import (
    DEFAULT_ROR_CSV,
    RorSpec,
    expand_ror_from_file,
    parse_ror_equivalence_file,
    parse_ror_selection,
)


# ---------------------------------------------------------------------------
# CSV fixtures
# ---------------------------------------------------------------------------
_MINIMAL_CSV = """\
name,vmax_hm3,production_factor,pmax_mw,enabled
CentralA,1.5,2.0,100,true
CentralB,0.8,3.1,,true
CentralC,2.0,1.0,,false
"""


def _write_csv(tmp_path, content=_MINIMAL_CSV, name="ror.csv"):
    path = tmp_path / name
    path.write_text(content, encoding="utf-8")
    return path


def _write_planning(tmp_path, generators):
    """Write a minimal planning JSON with the given generator names."""
    planning = {
        "system": {
            "generator_array": [
                {"uid": i + 1, "name": g} for i, g in enumerate(generators)
            ],
        },
    }
    path = tmp_path / "gtopt.json"
    path.write_text(json.dumps(planning), encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file
# ---------------------------------------------------------------------------
class TestParseRorEquivalenceFile:
    def test_basic(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        spec = parse_ror_equivalence_file(csv_path)

        assert len(spec) == 2  # CentralC is disabled
        assert spec["CentralA"] == RorSpec(
            vmax_hm3=1.5, production_factor=2.0, pmax_mw=100.0
        )
        assert spec["CentralB"] == RorSpec(
            vmax_hm3=0.8, production_factor=3.1, pmax_mw=None
        )

    def test_disabled_skipped(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        spec = parse_ror_equivalence_file(csv_path)
        assert "CentralC" not in spec

    def test_missing_file(self, tmp_path):
        with pytest.raises(FileNotFoundError):
            parse_ror_equivalence_file(tmp_path / "missing.csv")

    def test_missing_column(self, tmp_path):
        csv_path = _write_csv(tmp_path, "name,vmax_hm3\nA,1.0\n", name="bad.csv")
        with pytest.raises(ValueError, match="missing required column"):
            parse_ror_equivalence_file(csv_path)

    def test_duplicate_name(self, tmp_path):
        content = "name,vmax_hm3,production_factor\nA,1.0,1.0\nA,2.0,2.0\n"
        csv_path = _write_csv(tmp_path, content, name="dup.csv")
        with pytest.raises(ValueError, match="duplicate"):
            parse_ror_equivalence_file(csv_path)

    def test_non_positive_vmax(self, tmp_path):
        content = "name,vmax_hm3,production_factor\nA,-1.0,1.0\n"
        csv_path = _write_csv(tmp_path, content, name="neg.csv")
        with pytest.raises(ValueError, match="must be > 0"):
            parse_ror_equivalence_file(csv_path)

    def test_bundled_csv_exists(self):
        assert DEFAULT_ROR_CSV.is_file()


# ---------------------------------------------------------------------------
# parse_ror_selection
# ---------------------------------------------------------------------------
class TestParseRorSelection:
    def test_none(self):
        assert parse_ror_selection(None) is None

    def test_none_string(self):
        assert parse_ror_selection("none") is None

    def test_all(self):
        result = parse_ror_selection("all")
        assert result == frozenset()

    def test_explicit_list(self):
        result = parse_ror_selection("A, B, C")
        assert result == frozenset({"A", "B", "C"})

    def test_empty_raises(self):
        with pytest.raises(ValueError, match="empty selection"):
            parse_ror_selection(",")


# ---------------------------------------------------------------------------
# expand_ror_from_file
# ---------------------------------------------------------------------------
class TestExpandRorFromFile:
    def test_basic(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA", "CentralB", "OtherGen"])

        result = expand_ror_from_file(csv_path, planning_path, selection="all")
        names = {p["name"] for p in result["promoted"]}
        assert names == {"CentralA", "CentralB"}

    def test_selection_filter(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA", "CentralB"])

        result = expand_ror_from_file(csv_path, planning_path, selection="CentralA")
        assert len(result["promoted"]) == 1
        assert result["promoted"][0]["name"] == "CentralA"

    def test_disabled(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA"])

        result = expand_ror_from_file(csv_path, planning_path, selection="none")
        assert not result["promoted"]

    def test_unknown_selection_raises(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA"])

        with pytest.raises(ValueError, match="not in whitelist"):
            expand_ror_from_file(csv_path, planning_path, selection="UNKNOWN")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
class TestRorCli:
    def test_cli_ror(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA", "CentralB"])
        out_dir = tmp_path / "output"

        rc = cli_main(
            [
                "ror",
                "--input",
                str(csv_path),
                "--planning",
                str(planning_path),
                "--output",
                str(out_dir),
            ]
        )
        assert rc == 0

        output_path = out_dir / "ror_promoted.json"
        assert output_path.exists()

        with open(output_path, encoding="utf-8") as fh:
            result = json.load(fh)
        assert len(result["promoted"]) == 2

    def test_cli_ror_with_selection(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        planning_path = _write_planning(tmp_path, ["CentralA", "CentralB"])
        out_dir = tmp_path / "output2"

        rc = cli_main(
            [
                "ror",
                "--input",
                str(csv_path),
                "--planning",
                str(planning_path),
                "--output",
                str(out_dir),
                "--selection",
                "CentralA",
            ]
        )
        assert rc == 0

        with open(out_dir / "ror_promoted.json", encoding="utf-8") as fh:
            result = json.load(fh)
        assert len(result["promoted"]) == 1

    def test_cli_ror_missing_planning(self, tmp_path):
        csv_path = _write_csv(tmp_path)
        rc = cli_main(
            [
                "ror",
                "--input",
                str(csv_path),
                "--planning",
                str(tmp_path / "missing.json"),
                "--output",
                str(tmp_path / "out"),
            ]
        )
        assert rc == 2
