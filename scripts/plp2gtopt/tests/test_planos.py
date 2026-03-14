"""Tests for the PLP boundary-cuts (planos) parser and writer."""

import csv
from pathlib import Path

import pytest

from plp2gtopt.planos_parser import PlanosParser
from plp2gtopt.planos_writer import write_boundary_cuts_csv


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture()
def planos_files(tmp_path: Path):
    """Create minimal plpplaem1.dat and plpplaem2.dat files."""
    plaem1 = tmp_path / "plpplaem1.dat"
    plaem2 = tmp_path / "plpplaem2.dat"

    # plpplaem1.dat — 2 reservoirs
    plaem1.write_text(
        "2\n"  # number of reservoirs
        "1 'Rapel'\n"
        "2 'Colbun'\n"
    )

    # plpplaem2.dat — 3 cuts, boundary stage = 5
    # Fields: IPDNumIte  IEtapa  ISimul  LDPhiPrv  GradX(1)  GradX(2)
    plaem2.write_text(
        "5\n"  # boundary stage
        "1  5  1  -1000.5  0.25  0.75\n"
        "1  5  2  -2000.3  0.50  0.60\n"
        "2  5  1  -1500.0  0.35  0.80\n"
        # This cut is for stage 3 (not boundary stage 5) — should be skipped
        "2  3  1  -9999.0  0.99  0.99\n"
    )

    return plaem1, plaem2


# ---------------------------------------------------------------------------
# Parser tests
# ---------------------------------------------------------------------------


class TestPlanosParser:
    """Tests for PlanosParser."""

    def test_parse_reservoir_names(self, planos_files):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        assert parser.reservoir_names == ["Rapel", "Colbun"]

    def test_parse_boundary_stage(self, planos_files):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        assert parser.boundary_stage == 5

    def test_parse_cuts_count(self, planos_files):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        # 3 cuts for stage 5, 1 cut for stage 3 (skipped)
        assert len(parser.cuts) == 3

    def test_parse_cut_fields(self, planos_files):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        cut = parser.cuts[0]
        assert cut["name"] == "bc_1_1"
        assert cut["iteration"] == 1
        assert cut["scene"] == 0  # PLP ISimul=1 → 0-based scene=0
        # PLP stores negative intercept: rhs = -LDPhiPrv = -(-1000.5) = 1000.5
        assert cut["rhs"] == pytest.approx(1000.5)
        assert cut["coefficients"]["Rapel"] == pytest.approx(0.25)
        assert cut["coefficients"]["Colbun"] == pytest.approx(0.75)

    def test_parse_second_cut(self, planos_files):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        cut = parser.cuts[1]
        assert cut["name"] == "bc_1_2"
        assert cut["iteration"] == 1
        assert cut["scene"] == 1  # PLP ISimul=2 → 0-based scene=1
        assert cut["rhs"] == pytest.approx(2000.3)

    def test_parse_iteration_preserved(self, planos_files):
        """The iteration (IPDNumIte) field should be preserved per cut."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        # Cut 0 and 1 are from iteration 1, cut 2 from iteration 2
        assert parser.cuts[0]["iteration"] == 1
        assert parser.cuts[1]["iteration"] == 1
        assert parser.cuts[2]["iteration"] == 2

    def test_stage_filter(self, planos_files):
        """Cuts not matching the boundary stage should be excluded."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        # Only cuts for stage 5 are kept
        for cut in parser.cuts:
            assert "_3_" not in cut["name"]  # stage-3 cut not present

    def test_empty_files(self, tmp_path):
        """Empty files should produce no cuts without raising errors."""
        p1 = tmp_path / "plaem1.dat"
        p2 = tmp_path / "plaem2.dat"
        p1.write_text("")
        p2.write_text("")

        parser = PlanosParser(p1, p2)
        parser.parse()

        assert not parser.reservoir_names
        assert not parser.cuts

    def test_zero_coefficient_excluded(self, tmp_path):
        """Zero coefficients should not appear in the coefficients dict."""
        p1 = tmp_path / "plaem1.dat"
        p2 = tmp_path / "plaem2.dat"
        p1.write_text("2\n1 'R1'\n2 'R2'\n")
        p2.write_text("1\n1  1  1  -100.0  0.5  0.0\n")

        parser = PlanosParser(p1, p2)
        parser.parse()

        assert len(parser.cuts) == 1
        assert "R1" in parser.cuts[0]["coefficients"]
        assert "R2" not in parser.cuts[0]["coefficients"]


# ---------------------------------------------------------------------------
# Writer tests
# ---------------------------------------------------------------------------


class TestPlanosWriter:
    """Tests for write_boundary_cuts_csv."""

    def test_write_csv(self, planos_files, tmp_path):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        csv_path = write_boundary_cuts_csv(
            parser.cuts, parser.reservoir_names, tmp_path / "boundary_cuts.csv"
        )
        assert csv_path.exists()

        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            header = next(reader)
            rows = list(reader)

        assert header == [
            "name",
            "iteration",
            "scene",
            "rhs",
            "Rapel",
            "Colbun",
        ]
        assert len(rows) == 3

    def test_csv_values(self, planos_files, tmp_path):
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        csv_path = write_boundary_cuts_csv(
            parser.cuts, parser.reservoir_names, tmp_path / "bc.csv"
        )

        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            row = next(reader)

        assert row["name"] == "bc_1_1"
        assert row["iteration"] == "1"
        assert row["scene"] == "0"  # PLP ISimul=1 → 0-based scene=0
        assert float(row["rhs"]) == pytest.approx(1000.5)
        assert float(row["Rapel"]) == pytest.approx(0.25)
        assert float(row["Colbun"]) == pytest.approx(0.75)

    def test_empty_cuts(self, tmp_path):
        """Writing zero cuts should produce a header-only CSV."""
        csv_path = write_boundary_cuts_csv([], ["R1", "R2"], tmp_path / "empty.csv")
        assert csv_path.exists()

        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            header = next(reader)
            rows = list(reader)

        assert header == ["name", "iteration", "scene", "rhs", "R1", "R2"]
        assert not rows
