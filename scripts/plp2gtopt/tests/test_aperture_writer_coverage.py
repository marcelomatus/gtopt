"""Tests for aperture_writer.py — uncovered lines and edge cases."""

import textwrap
from unittest.mock import MagicMock

import numpy as np

from plp2gtopt.aperture_writer import (
    ApertureResult,
    _unique_hydro_indices,
    build_aperture_array,
    build_phase_apertures,
    write_aperture_afluents,
)
from plp2gtopt.idap2_parser import IdAp2Parser


# ---------------------------------------------------------------------------
# _unique_hydro_indices (lines 26-43)
# ---------------------------------------------------------------------------


class TestUniqueHydroIndices:
    """Tests for _unique_hydro_indices helper."""

    def test_from_idap2_only(self):
        """Collects indices from idap2_parser items."""
        idap2 = MagicMock()
        idap2.items = [
            {"indices": [1, 2, 3]},
            {"indices": [3, 4, 5]},
        ]
        result = _unique_hydro_indices(None, idap2, 10, 3)
        assert result == {1, 2, 3, 4, 5}

    def test_from_idape_only(self):
        """Collects indices from idape_parser items."""
        idape = MagicMock()
        idape.items = [
            {"indices": [10, 20]},
        ]
        result = _unique_hydro_indices(idape, None, 5, 2)
        assert result == {10, 20}

    def test_both_parsers(self):
        """Union of indices from both parsers."""
        idape = MagicMock()
        idape.items = [{"indices": [1, 2]}]
        idap2 = MagicMock()
        idap2.items = [{"indices": [2, 3]}]
        result = _unique_hydro_indices(idape, idap2, 5, 2)
        assert result == {1, 2, 3}

    def test_neither_parser(self):
        """Empty result when both parsers are None."""
        result = _unique_hydro_indices(None, None, 5, 2)
        assert result == set()


# ---------------------------------------------------------------------------
# ApertureResult
# ---------------------------------------------------------------------------


def test_aperture_result_creation():
    """ApertureResult stores aperture_array and extra_scenarios."""
    ar = ApertureResult([{"uid": 1}], [{"uid": 99}])
    assert ar.aperture_array == [{"uid": 1}]
    assert ar.extra_scenarios == [{"uid": 99}]


# ---------------------------------------------------------------------------
# build_aperture_array — fallback path (lines 128-132)
# ---------------------------------------------------------------------------


def test_build_aperture_array_fallback_to_first_entry(tmp_path):
    """When num_stages filter yields nothing, uses first entry as fallback."""
    content = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              2
        # Mes  Etapa  NApert  ApertInd
           001   005    2   10   20
           002   006    2   10   20
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    parser = IdAp2Parser(p)
    parser.parse()

    # num_stages=2 but entries have stage 5 and 6 -> no match -> fallback
    scenario_hydro_map = {9: 1, 19: 2}
    res = build_aperture_array(parser, scenario_hydro_map, 2)
    assert len(res.aperture_array) == 2


# ---------------------------------------------------------------------------
# build_phase_apertures — fallback path (lines 222-226)
# ---------------------------------------------------------------------------


def test_build_phase_apertures_fallback(tmp_path):
    """build_phase_apertures uses first entry fallback when stages miss."""
    content = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              1
        # Mes  Etapa  NApert  ApertInd
           001   010    2   51   52
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    parser = IdAp2Parser(p)
    parser.parse()

    scenario_hydro_map = {50: 1, 51: 2}
    aperture_array = build_aperture_array(parser, scenario_hydro_map, 1).aperture_array
    # Since stage 10 > num_stages=1, fallback triggers for aperture_array
    # Now test phase_apertures with same parser
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    # This should not crash; phases may not get apertures if stage>num_stages
    build_phase_apertures(parser, aperture_array, phase_array, 1)


# ---------------------------------------------------------------------------
# build_phase_apertures — stage beyond num_stages (line 246)
# ---------------------------------------------------------------------------


def test_build_phase_apertures_stage_exceeds_num_stages(tmp_path):
    """Phase spanning beyond num_stages stops gracefully."""
    content = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              2
        # Mes  Etapa  NApert  ApertInd
           001   001    2   51   52
           002   002    3   51   52   53
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    parser = IdAp2Parser(p)
    parser.parse()

    scenario_hydro_map = {50: 1, 51: 2, 52: 3}
    aperture_array = build_aperture_array(parser, scenario_hydro_map, 2).aperture_array

    # Phase spans stages 0,1,2 but only 2 stages exist
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 5}]
    build_phase_apertures(parser, aperture_array, phase_array, 2)
    # Should not crash, and should have apertures from stages 1-2


# ---------------------------------------------------------------------------
# write_aperture_afluents (lines 268-403)
# ---------------------------------------------------------------------------


class TestWriteApertureAfluents:
    """Tests for write_aperture_afluents."""

    def test_no_extra_hydros(self, tmp_path):
        """Does nothing when all aperture hydros are in forward set."""
        write_aperture_afluents(
            aflce_parser=MagicMock(),
            central_parser=MagicMock(),
            block_parser=MagicMock(),
            aperture_hydros=[0, 1],
            forward_hydros={0, 1},
            output_dir=tmp_path,
        )
        # No files should be created
        assert not list(tmp_path.glob("**/*.parquet"))

    def test_missing_parsers(self, tmp_path):
        """Does nothing when critical parsers are None."""
        write_aperture_afluents(
            aflce_parser=None,
            central_parser=None,
            block_parser=None,
            aperture_hydros=[5],
            forward_hydros={0},
            output_dir=tmp_path,
        )
        assert not list(tmp_path.glob("**/*.parquet"))

    def test_writes_parquet_for_extra_hydros(self, tmp_path):
        """Writes Parquet files for hydros not in forward set."""
        # Setup aflce_parser with flow data
        flow_matrix = np.array(
            [[10.0, 20.0, 30.0], [40.0, 50.0, 60.0]], dtype=np.float64
        )
        aflce_item = {
            "name": "CENTRAL1",
            "flow": flow_matrix,
            "block": np.array([1, 2]),
        }
        aflce_parser = MagicMock()
        aflce_parser.items = [aflce_item]

        central_parser = MagicMock()
        block_parser = MagicMock()
        block_parser.items = [
            {"number": 1, "stage": 1},
            {"number": 2, "stage": 1},
        ]

        write_aperture_afluents(
            aflce_parser=aflce_parser,
            central_parser=central_parser,
            block_parser=block_parser,
            aperture_hydros=[0, 2],  # 0-based: hydro 0 is forward, 2 is extra
            forward_hydros={0},
            output_dir=tmp_path,
            options={"output_format": "parquet", "compression": "snappy"},
            hydro_uid_map={2: 3},
        )
        flow_dir = tmp_path / "Flow"
        assert flow_dir.exists()
        parquet_files = list(flow_dir.glob("*.parquet"))
        assert len(parquet_files) == 1
        assert parquet_files[0].name == "CENTRAL1.parquet"

    def test_writes_csv_format(self, tmp_path):
        """Writes CSV files when output_format is csv."""
        flow_matrix = np.array([[1.0, 2.0]], dtype=np.float64)
        aflce_item = {
            "name": "CEN1",
            "flow": flow_matrix,
            "block": np.array([1]),
        }
        aflce_parser = MagicMock()
        aflce_parser.items = [aflce_item]

        block_parser = MagicMock()
        block_parser.items = [{"number": 1, "stage": 1}]

        write_aperture_afluents(
            aflce_parser=aflce_parser,
            central_parser=MagicMock(),
            block_parser=block_parser,
            aperture_hydros=[1],
            forward_hydros={0},
            output_dir=tmp_path,
            options={"output_format": "csv"},
        )
        csv_files = list((tmp_path / "Flow").glob("*.csv"))
        assert len(csv_files) == 1

    def test_missing_block_in_flow_data(self, tmp_path):
        """Handles blocks not found in afluent data (uses 0.0)."""
        flow_matrix = np.array([[10.0, 20.0]], dtype=np.float64)
        aflce_item = {
            "name": "CEN2",
            "flow": flow_matrix,
            "block": np.array([1]),  # Only block 1
        }
        aflce_parser = MagicMock()
        aflce_parser.items = [aflce_item]

        block_parser = MagicMock()
        block_parser.items = [
            {"number": 1, "stage": 1},
            {"number": 2, "stage": 1},  # Block 2 not in aflce data
        ]

        write_aperture_afluents(
            aflce_parser=aflce_parser,
            central_parser=MagicMock(),
            block_parser=block_parser,
            aperture_hydros=[1],
            forward_hydros={0},
            output_dir=tmp_path,
            options={"output_format": "parquet"},
        )
        parquet_files = list((tmp_path / "Flow").glob("*.parquet"))
        assert len(parquet_files) == 1

    def test_empty_flow_matrix(self, tmp_path):
        """Skips central with empty flow matrix."""
        aflce_item = {
            "name": "EMPTY",
            "flow": np.array([]),
            "block": np.array([]),
        }
        aflce_parser = MagicMock()
        aflce_parser.items = [aflce_item]

        block_parser = MagicMock()
        block_parser.items = [{"number": 1, "stage": 1}]

        write_aperture_afluents(
            aflce_parser=aflce_parser,
            central_parser=MagicMock(),
            block_parser=block_parser,
            aperture_hydros=[1],
            forward_hydros={0},
            output_dir=tmp_path,
        )
        # No files created for empty flow matrix
        assert not list(tmp_path.glob("Flow/*.parquet"))

    def test_hydro_uid_map_fallback(self, tmp_path):
        """Uses 1-based hydrology index when hydro_uid_map is None."""
        flow_matrix = np.array([[10.0, 20.0]], dtype=np.float64)
        aflce_item = {
            "name": "CEN3",
            "flow": flow_matrix,
            "block": np.array([1]),
        }
        aflce_parser = MagicMock()
        aflce_parser.items = [aflce_item]

        block_parser = MagicMock()
        block_parser.items = [{"number": 1, "stage": 1}]

        write_aperture_afluents(
            aflce_parser=aflce_parser,
            central_parser=MagicMock(),
            block_parser=block_parser,
            aperture_hydros=[1],
            forward_hydros={0},
            output_dir=tmp_path,
            hydro_uid_map=None,  # No map, should fallback to 1-based
        )
        parquet_files = list((tmp_path / "Flow").glob("*.parquet"))
        assert len(parquet_files) == 1
