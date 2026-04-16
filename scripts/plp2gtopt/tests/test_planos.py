"""Tests for the PLP boundary-cuts (planos) parser and writer."""

import csv
from pathlib import Path

import pytest

from plp2gtopt.planos_parser import PlanosParser, find_planos_files
from plp2gtopt.planos_writer import write_boundary_cuts_csv, write_hot_start_cuts_csv


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
        assert cut["scene"] == 1  # PLP ISimul=1 → scene UID=1
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
        assert cut["scene"] == 2  # PLP ISimul=2 → scene UID=2
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


class TestPlanosParserCSVFormat:
    """Tests for PlanosParser with CSV-format plpplem1.dat."""

    def test_csv_reservoir_names(self, tmp_path):
        """CSV-format plpplem1.dat should be parsed correctly."""
        p1 = tmp_path / "plpplem1.dat"
        p2 = tmp_path / "plpplem2.dat"

        # CSV format with extra columns (Tipo, Barra, N/A, VolMin, VolMax, ...)
        p1.write_text(
            "1,LMAULE                  ,A,  0,  0,      0.00,1424293.60\n"
            "2,COLBUN                  ,A,  0,  0,      0.00, 518000.00\n"
        )
        p2.write_text("5\n1  5  1  -1000.0  0.25  0.75\n")

        parser = PlanosParser(p1, p2)
        parser.parse()

        assert parser.reservoir_names == ["LMAULE", "COLBUN"]
        assert len(parser.cuts) == 1

    def test_csv_format_with_trailing_spaces(self, tmp_path):
        """CSV names with trailing spaces should be stripped."""
        p1 = tmp_path / "plpplem1.dat"
        p2 = tmp_path / "plpplem2.dat"

        p1.write_text(
            "1, Rapel   , A, 0, 0, 0.0, 100.0\n2, 'Colbun' , A, 0, 0, 0.0, 200.0\n"
        )
        p2.write_text("3\n1  3  1  -500.0  0.10  0.20\n")

        parser = PlanosParser(p1, p2)
        parser.parse()

        assert parser.reservoir_names == ["Rapel", "Colbun"]


class TestPlanosParserAllCuts:
    """Tests for PlanosParser.all_cuts (all stages, not just boundary)."""

    def test_all_cuts_includes_non_boundary(self, planos_files):
        """all_cuts should include cuts from ALL stages."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        # 3 boundary (stage 5) + 1 non-boundary (stage 3) = 4 total
        assert len(parser.all_cuts) == 4
        assert len(parser.cuts) == 3

    def test_all_cuts_have_stage_field(self, planos_files):
        """Each cut in all_cuts should have a 'stage' field."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        for cut in parser.all_cuts:
            assert "stage" in cut

    def test_non_boundary_cut_stage(self, planos_files):
        """The non-boundary cut should have stage=3."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        non_boundary = [c for c in parser.all_cuts if c["stage"] != 5]
        assert len(non_boundary) == 1
        assert non_boundary[0]["stage"] == 3
        assert non_boundary[0]["coefficients"]["Rapel"] == pytest.approx(0.99)


class TestFindPlanosFiles:
    """Tests for find_planos_files() auto-discovery."""

    def test_plpplaem_naming(self, tmp_path):
        """Original naming (plpplaem*) should be found."""
        (tmp_path / "plpplaem1.dat").write_text("1\n1 R1\n")
        (tmp_path / "plpplaem2.dat").write_text("1\n1 1 1 -100.0 0.5\n")

        result = find_planos_files(tmp_path)
        assert result is not None
        assert result[0].name == "plpplaem1.dat"
        assert result[1].name == "plpplaem2.dat"

    def test_plpplem_naming(self, tmp_path):
        """Abbreviated naming (plpplem*) should be found."""
        (tmp_path / "plpplem1.dat").write_text("1\n1 R1\n")
        (tmp_path / "plpplem2.dat").write_text("1\n1 1 1 -100.0 0.5\n")

        result = find_planos_files(tmp_path)
        assert result is not None
        assert result[0].name == "plpplem1.dat"
        assert result[1].name == "plpplem2.dat"

    def test_plpplaem_preferred_over_plpplem(self, tmp_path):
        """If both naming conventions exist, plpplaem* is preferred."""
        (tmp_path / "plpplaem1.dat").write_text("1\n1 R1\n")
        (tmp_path / "plpplaem2.dat").write_text("1\n")
        (tmp_path / "plpplem1.dat").write_text("1\n1 R1\n")
        (tmp_path / "plpplem2.dat").write_text("1\n")

        result = find_planos_files(tmp_path)
        assert result is not None
        assert result[0].name == "plpplaem1.dat"

    def test_missing_files(self, tmp_path):
        """Should return None when no planos files exist."""
        result = find_planos_files(tmp_path)
        assert result is None

    def test_partial_files(self, tmp_path):
        """Should return None when only one of the pair exists."""
        (tmp_path / "plpplem1.dat").write_text("1\n1 R1\n")
        # plpplem2.dat is missing

        result = find_planos_files(tmp_path)
        assert result is None


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
        assert row["scene"] == "1"  # PLP ISimul=1 → scene UID=1
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


class TestHotStartCutsWriter:
    """Tests for write_hot_start_cuts_csv."""

    def test_write_hot_start_csv(self, planos_files, tmp_path):
        """Hot-start CSV should have a phase column."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        # Filter non-boundary cuts
        non_boundary = [
            c for c in parser.all_cuts if c["stage"] != parser.boundary_stage
        ]

        csv_path = write_hot_start_cuts_csv(
            non_boundary,
            parser.reservoir_names,
            tmp_path / "hot_start.csv",
            stage_to_phase={3: 2, 5: 4},
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
            "phase",
            "rhs",
            "Rapel",
            "Colbun",
        ]
        assert len(rows) == 1
        # Stage 3 maps to phase 2
        assert rows[0][3] == "2"

    def test_hot_start_default_stage_to_phase(self, tmp_path):
        """Without stage_to_phase, phase = stage (identity)."""
        cuts = [
            {
                "name": "hs_1",
                "iteration": 1,
                "stage": 7,
                "scene": 1,
                "rhs": -100.0,
                "coefficients": {"R1": 0.5},
            }
        ]
        csv_path = write_hot_start_cuts_csv(cuts, ["R1"], tmp_path / "hs.csv")

        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            row = next(reader)

        assert row["phase"] == "7"  # identity: stage 7 → phase 7


class TestNameAlias:
    """Tests for the ``name_alias`` header-rename option."""

    def test_boundary_cuts_alias_renames_header(self, planos_files, tmp_path):
        """Alias map renames columns; coefficient lookup uses original keys."""
        plaem1, plaem2 = planos_files
        parser = PlanosParser(plaem1, plaem2)
        parser.parse()

        csv_path = write_boundary_cuts_csv(
            parser.cuts,
            parser.reservoir_names,
            tmp_path / "bc_alias.csv",
            name_alias={"Rapel": "RAPEL_GT", "Unknown": "ignored"},
        )

        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            header = next(reader)
            first = next(reader)

        assert header == [
            "name",
            "iteration",
            "scene",
            "rhs",
            "RAPEL_GT",
            "Colbun",
        ]
        # Renamed column still carries the original Rapel coefficient.
        assert float(first[4]) == pytest.approx(0.25)
        assert float(first[5]) == pytest.approx(0.75)

    def test_boundary_cuts_alias_none_is_identity(self, tmp_path):
        """Passing ``name_alias=None`` leaves headers unchanged."""
        csv_path = write_boundary_cuts_csv(
            [], ["R1", "R2"], tmp_path / "nb.csv", name_alias=None
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            header = next(csv.reader(f))
        assert header[-2:] == ["R1", "R2"]

    def test_hot_start_alias_renames_header(self, tmp_path):
        """Alias applies to hot-start-cut headers too."""
        cuts = [
            {
                "name": "hs_1",
                "iteration": 1,
                "stage": 2,
                "scene": 1,
                "rhs": -10.0,
                "coefficients": {"CANUTILLAR": 0.5, "R2": 0.25},
            }
        ]
        csv_path = write_hot_start_cuts_csv(
            cuts,
            ["CANUTILLAR", "R2"],
            tmp_path / "hs_alias.csv",
            name_alias={"CANUTILLAR": "CHAPO"},
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            header = next(reader)
            first = next(reader)

        assert header == [
            "name",
            "iteration",
            "scene",
            "phase",
            "rhs",
            "CHAPO",
            "R2",
        ]
        assert float(first[5]) == pytest.approx(0.5)
        assert float(first[6]) == pytest.approx(0.25)
