"""Tests for the PLP boundary-cuts (planos) parser and writer."""

import csv
from pathlib import Path

import pytest

from plp2gtopt.planos_parser import PlanosParser, find_planos_files
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

    # plpplaem2.dat — 3 cuts, boundary stage = 5.
    # Fields: IPDNumIte  IEtapa  ISimul  LDPhiPrv  GradX(1)  GradX(2)
    # LDPhiPrv values are POSITIVE (real PLP output), matching
    # `plp-espercnd.f:54` (`LDPhiPrv = PromedioZ` — averaged LP obj
    # across apertures, always positive for cost-minimization SDDP).
    plaem2.write_text(
        "5\n"  # boundary stage
        "1  5  1  1000.5  0.25  0.75\n"
        "1  5  2  2000.3  0.50  0.60\n"
        "2  5  1  1500.0  0.35  0.80\n"
        # This cut is for stage 3 (not boundary stage 5) — should be skipped
        "2  3  1  9999.0  0.99  0.99\n"
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

        # No leading ``name`` column — retired in 2026-05 (PLP never
        # emitted one).
        assert header == [
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

        # No ``name`` column — retired in 2026-05.
        assert "name" not in row
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

        assert header == ["iteration", "scene", "rhs", "R1", "R2"]
        assert not rows


# ``TestHotStartCutsWriter`` was retired in 2026-05 along with
# ``write_hot_start_cuts_csv`` (hot-start cuts now travel via the
# typed Parquet path on the gtopt side).


class TestProbabilityScaling:
    """Tests for the ``num_scenarios`` (NVarPhi) scaling at write time.

    PLP and gtopt put the scenario probability in different places: PLP's
    α-column carries ``1/NVarPhi`` as its LP objective coefficient, while
    gtopt's per-scene α-column carries ``1.0``.  The writer must therefore
    divide both ``rhs`` and every gradient coefficient by ``NVarPhi`` so
    each per-scene gtopt LP loads only its own share of the expected
    future cost.  See ``planos_writer`` module docstring.
    """

    def test_default_unscaled(self, tmp_path):
        """Without ``num_scenarios`` the writer is a verbatim pass-through."""
        cuts = [
            {
                "name": "bc_1_1",
                "iteration": 1,
                "scene": 1,
                "rhs": 1.0,
                "coefficients": {"R1": 0.5, "R2": 0.25},
            }
        ]
        csv_path = write_boundary_cuts_csv(cuts, ["R1", "R2"], tmp_path / "x.csv")
        with open(csv_path, newline="", encoding="utf-8") as f:
            row = next(csv.DictReader(f))
        assert float(row["rhs"]) == pytest.approx(1.0)
        assert float(row["R1"]) == pytest.approx(0.5)
        assert float(row["R2"]) == pytest.approx(0.25)

    def test_num_scenarios_four_divides_rhs_and_grads(self, tmp_path):
        """``num_scenarios=4`` divides ``rhs`` and every gradient by 4."""
        cuts = [
            {
                "name": "bc_1_1",
                "iteration": 1,
                "scene": 1,
                "rhs": 1.0,
                "coefficients": {"R1": 0.5, "R2": 0.25},
            }
        ]
        csv_path = write_boundary_cuts_csv(
            cuts, ["R1", "R2"], tmp_path / "scaled.csv", num_scenarios=4
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            row = next(csv.DictReader(f))
        # Round-trip: rhs = 1.0 / 4 = 0.25
        assert float(row["rhs"]) == pytest.approx(0.25)
        # Gradients are scaled by the same factor.
        assert float(row["R1"]) == pytest.approx(0.125)
        assert float(row["R2"]) == pytest.approx(0.0625)

    def test_num_scenarios_one_no_op(self, tmp_path):
        """``num_scenarios=1`` is a no-op (single-scene case)."""
        cuts = [
            {
                "name": "bc_1_1",
                "iteration": 1,
                "scene": 1,
                "rhs": 100.0,
                "coefficients": {"R1": 2.0},
            }
        ]
        csv_path = write_boundary_cuts_csv(
            cuts, ["R1"], tmp_path / "n1.csv", num_scenarios=1
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            row = next(csv.DictReader(f))
        assert float(row["rhs"]) == pytest.approx(100.0)
        assert float(row["R1"]) == pytest.approx(2.0)

    def test_num_scenarios_zero_no_op(self, tmp_path):
        """``num_scenarios=0`` is a no-op (degenerate guard)."""
        cuts = [
            {
                "name": "bc_1_1",
                "iteration": 1,
                "scene": 1,
                "rhs": 7.0,
                "coefficients": {"R1": 3.0},
            }
        ]
        csv_path = write_boundary_cuts_csv(
            cuts, ["R1"], tmp_path / "n0.csv", num_scenarios=0
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            row = next(csv.DictReader(f))
        assert float(row["rhs"]) == pytest.approx(7.0)
        assert float(row["R1"]) == pytest.approx(3.0)

    def test_num_scenarios_preserves_other_columns(self, tmp_path):
        """Scaling does not affect ``iteration`` / ``scene``.

        The legacy leading ``name`` column was retired in 2026-05; the
        writer silently ignores any ``name`` key present on the input
        cut dicts (we leave it on the fixture below to also pin the
        backward-compatibility behaviour).
        """
        cuts = [
            {
                "name": "bc_2_3",  # ignored on output (no name column)
                "iteration": 2,
                "scene": 3,
                "rhs": 16.0,
                "coefficients": {"R1": 4.0},
            }
        ]
        csv_path = write_boundary_cuts_csv(
            cuts, ["R1"], tmp_path / "id.csv", num_scenarios=4
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            row = next(csv.DictReader(f))
        # No ``name`` field in the CSV row (column was dropped).
        assert "name" not in row
        assert row["iteration"] == "2"
        assert row["scene"] == "3"
        # Scaled column
        assert float(row["rhs"]) == pytest.approx(4.0)
        assert float(row["R1"]) == pytest.approx(1.0)


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
            "iteration",
            "scene",
            "rhs",
            "RAPEL_GT",
            "Colbun",
        ]
        # Renamed column still carries the original Rapel coefficient.
        # Indices shifted by 1 after the legacy ``name`` column was
        # dropped in 2026-05.
        assert float(first[3]) == pytest.approx(0.25)
        assert float(first[4]) == pytest.approx(0.75)

    def test_boundary_cuts_alias_none_is_identity(self, tmp_path):
        """Passing ``name_alias=None`` leaves headers unchanged."""
        csv_path = write_boundary_cuts_csv(
            [], ["R1", "R2"], tmp_path / "nb.csv", name_alias=None
        )
        with open(csv_path, newline="", encoding="utf-8") as f:
            header = next(csv.reader(f))
        assert header[-2:] == ["R1", "R2"]


class TestLowerBoundWaterValueByReservoir:
    """Tests for PlanosParser.lower_bound_water_value_by_reservoir.

    The lower bound is ``-max(signed coeff)`` positive-floored (see
    :func:`gtopt_shared.water_values.cut_lower_bound`), with the same
    ``1/num_scenarios`` and ``10^(FEscala-6)`` scaling as the average
    helper.
    """

    def test_basic_lower_bound_is_neg_max_signed(self, tmp_path):
        """GradX = [-100, -200, -300] → lower bound = -max = 100."""
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("1\n1 'LMAULE'\n")
        p2.write_text(
            "5\n"
            "1  5  1  1000.0  -100.0\n"
            "1  5  2  1000.0  -200.0\n"
            "1  5  3  1000.0  -300.0\n"
        )
        parser = PlanosParser(p1, p2)
        parser.parse()

        out = parser.lower_bound_water_value_by_reservoir()
        # -max([-100, -200, -300]) = -(-100) = 100.0
        assert out["LMAULE"] == pytest.approx(100.0)

    def test_num_scenarios_divides_before_reduction(self, tmp_path):
        """``num_scenarios=N`` divides each coeff by N before ``-max``."""
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("1\n1 'R1'\n")
        p2.write_text("5\n1  5  1  1000.0  -100.0\n1  5  2  1000.0  -200.0\n")
        parser = PlanosParser(p1, p2)
        parser.parse()

        # coeffs /4 = [-25, -50]; -max = 25.0
        out = parser.lower_bound_water_value_by_reservoir(num_scenarios=4)
        assert out["R1"] == pytest.approx(25.0)

    def test_fescala_scaling_applied(self, tmp_path):
        """FEscala scales each coeff by ``10^(FEscala-6)`` before reduction."""
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        # CSV format so FEscala (field index 9) is parsed.  FEscala=8 →
        # 10^(8-6) = 100.
        p1.write_text(
            "#Numero,Nombre,Tipo,Barra,NA,VolMin,VolMax,VMinN,VMaxN,FEscala\n"
            "1,R1,A,0,0,0,0,0,0,8\n"
        )
        p2.write_text("5\n1  5  1  1000.0  -100.0\n")
        parser = PlanosParser(p1, p2)
        parser.parse()
        assert parser.reservoir_fescala.get("R1") == 8

        # coeff -100 * 100 = -10000; -max = 10000.0
        out = parser.lower_bound_water_value_by_reservoir()
        assert out["R1"] == pytest.approx(10000.0)
        # apply_fescala=False disables the multiplier.
        out_no = parser.lower_bound_water_value_by_reservoir(apply_fescala=False)
        assert out_no["R1"] == pytest.approx(100.0)

    def test_positive_only_coeff_falls_back_to_cut_floor(self, tmp_path):
        """A cut pricing water as a liability (only positive coeff) → floor.

        ``cut_lower_bound`` computes ``-max([+50]) = -50 <= 0``; with no
        negative coefficient to fall back to, it returns the ``_CUT_FLOOR``
        sentinel (1.0) so the reservoir is never priced at zero.
        """
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("1\n1 'R1'\n")
        p2.write_text("5\n1  5  1  1000.0  50.0\n")
        parser = PlanosParser(p1, p2)
        parser.parse()

        out = parser.lower_bound_water_value_by_reservoir()
        assert out["R1"] == pytest.approx(1.0)

    def test_mixed_sign_floors_to_smallest_positive_water_value(self, tmp_path):
        """Mixed signs: ``-max`` non-positive ⇒ use ``-max_neg_coeff``.

        Coeffs ``[+30, -80]`` ⇒ ``-max = -30 <= 0`` ⇒ fall back to the
        smallest positive water value ``-max(neg) = -(-80) = 80``.
        """
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("1\n1 'R1'\n")
        p2.write_text("5\n1  5  1  1000.0  30.0\n1  5  2  1000.0  -80.0\n")
        parser = PlanosParser(p1, p2)
        parser.parse()

        out = parser.lower_bound_water_value_by_reservoir()
        assert out["R1"] == pytest.approx(80.0)

    def test_empty_cuts_empty_result(self, tmp_path):
        """No cuts ⇒ empty dict (None / empty handling)."""
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("")
        p2.write_text("")
        parser = PlanosParser(p1, p2)
        parser.parse()
        assert not parser.lower_bound_water_value_by_reservoir()

    def test_zero_coefficients_omitted(self, tmp_path):
        """All-zero cuts for a reservoir ⇒ no lower bound emitted."""
        p1 = tmp_path / "plpplaem1.dat"
        p2 = tmp_path / "plpplaem2.dat"
        p1.write_text("2\n1 'R1'\n2 'R2'\n")
        # R1 has a real gradient; R2 is always zero.
        p2.write_text("5\n1  5  1  1000.0  -100.0  0.0\n1  5  2  1000.0  -200.0  0.0\n")
        parser = PlanosParser(p1, p2)
        parser.parse()

        out = parser.lower_bound_water_value_by_reservoir()
        assert out == {"R1": pytest.approx(100.0)}
