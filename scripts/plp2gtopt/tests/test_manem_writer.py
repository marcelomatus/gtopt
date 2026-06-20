"""Unit tests for ManemWriter class."""

import json
import tempfile
from pathlib import Path

import pandas as pd
import pytest

from ..central_parser import CentralParser
from ..manem_parser import ManemParser
from ..manem_writer import ManemWriter
from ..stage_parser import StageParser
from .conftest import get_example_file


@pytest.fixture
def sample_manem_file():
    """Fixture providing path to sample reservoir maintenance file."""
    return get_example_file("plpmanem.dat")


@pytest.fixture
def sample_manem_writer(sample_manem_file):
    """Fixture providing initialized ManemWriter with sample data."""
    parser = ManemParser(sample_manem_file)
    parser.parse()
    return ManemWriter(parser)


def test_manem_writer_initialization(sample_manem_file):
    """Test ManemWriter initialization."""
    parser = ManemParser(sample_manem_file)
    parser.parse()
    writer = ManemWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_manems


def test_to_json_array(sample_manem_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_manems = sample_manem_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_manems, list)
    assert len(json_manems) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "stage": list,
        "emin": list,
        "emax": list,
    }

    for manem in json_manems:
        for field, field_type in required_fields.items():
            assert field in manem, f"Missing field: {field}"
            assert isinstance(manem[field], field_type), (
                f"Field {field} should be {field_type}, got {type(manem[field])}"
            )


def test_write_to_file(sample_manem_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_manem_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_manem_writer):
    """Verify JSON output matches expected structure."""
    json_manems = sample_manem_writer.to_json_array()

    # Expected structure
    required_fields = {
        "name": str,
        "stage": list,
        "emin": list,
        "emax": list,
    }

    for manem in json_manems:
        # Check all required fields exist and have correct types
        assert set(manem.keys()) == set(required_fields.keys())
        for field, field_type in required_fields.items():
            assert isinstance(manem[field], field_type), (
                f"Field {field} should be {field_type}, got {type(manem[field])}"
            )

        # Additional value checks
        assert len(manem["name"]) > 0, "Name should not be empty"
        assert len(manem["stage"]) > 0, "Should have at least one stage"
        assert len(manem["stage"]) == len(manem["emin"]), (
            "Stages and vol_min should match"
        )
        assert len(manem["stage"]) == len(manem["emax"]), (
            "Stages and vol_max should match"
        )


class MockEmptyManemParser(ManemParser):
    """Mock parser that returns empty data."""

    def __init__(self):
        """Initializes the mock parser with a dummy file path."""
        super().__init__("dummy.dat")

    def get_all(self) -> list:
        """Return an empty list of items."""
        return []


def test_write_empty_manems():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = MockEmptyManemParser()
    writer = ManemWriter(parser)

    # Test empty array conversion
    json_manems = writer.to_json_array()
    assert isinstance(json_manems, list)
    assert len(json_manems) == 0

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        # Verify file exists and is valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) == 0


def test_to_dataframe_with_empty_parser():
    """Test DataFrame creation with empty parser."""
    parser = MockEmptyManemParser()
    writer = ManemWriter(parser)

    df_emin, df_emax = writer.to_dataframe()

    assert df_emin.empty
    assert df_emax.empty


def _make_central_parser(tmp_path, name, number=1, emin=None, emax=None):
    """Create a minimal CentralParser with one central entry.

    When ``emin`` / ``emax`` are given they are stored on the central
    record so the ManemWriter can use them as the per-stage DENSE default
    for stages a reservoir's maintenance does not cover.
    """

    parser = CentralParser.__new__(CentralParser)
    parser.file_path = tmp_path / "plpcnfce.dat"
    record = {"name": name, "number": number, "type": "embalse"}
    if emin is not None:
        record["emin"] = emin
    if emax is not None:
        record["emax"] = emax
    parser._data = [record]
    parser._name_index_map = {name: 0}
    parser._number_index_map = {number: 0}
    parser.num_embalses = 1
    return parser


def _make_stage_parser(tmp_path, n_stages=2):
    """Create a minimal StageParser with n stages."""

    parser = StageParser.__new__(StageParser)
    parser.file_path = tmp_path / "plpeta.dat"
    parser._data = [
        {"number": i + 1, "duration": 7.0, "discount_factor": 1.0}
        for i in range(n_stages)
    ]
    parser._name_index_map = {}
    parser._number_index_map = {i + 1: i for i in range(n_stages)}
    return parser


def test_to_dataframe_with_parsers(tmp_path):
    """Test to_dataframe with central and stage parsers."""

    manem_f = tmp_path / "plpmanem.dat"
    manem_f.write_text(
        " 1\n'RESERVOIR1'\n   2\n   03     001  0.30  1.50\n   03     002  0.30  1.50\n"
    )
    manem_parser = ManemParser(manem_f)
    manem_parser.parse()

    central_parser = _make_central_parser(tmp_path, "RESERVOIR1")
    stage_parser = _make_stage_parser(tmp_path, 2)

    writer = ManemWriter(manem_parser, central_parser, stage_parser)
    df_emin, df_emax = writer.to_dataframe()

    assert not df_emin.empty
    assert not df_emax.empty


def test_to_parquet(tmp_path):
    """Test to_parquet writes emin and emax parquet files."""

    manem_f = tmp_path / "plpmanem.dat"
    manem_f.write_text(
        " 1\n'RESERVOIR1'\n   2\n   03     001  0.30  1.50\n   03     002  0.35  1.45\n"
    )
    manem_parser = ManemParser(manem_f)
    manem_parser.parse()

    central_parser = _make_central_parser(tmp_path, "RESERVOIR1")
    stage_parser = _make_stage_parser(tmp_path, 2)

    writer = ManemWriter(manem_parser, central_parser, stage_parser)
    out_dir = tmp_path / "manem_out"
    cols = writer.to_parquet(out_dir)

    assert (out_dir / "emin.parquet").exists()
    assert (out_dir / "emax.parquet").exists()
    assert len(cols["emin"]) > 0
    assert len(cols["emax"]) > 0


def test_to_parquet_empty(tmp_path):
    """Test to_parquet with no data returns empty cols dict."""
    parser = MockEmptyManemParser()
    writer = ManemWriter(parser)
    out_dir = tmp_path / "empty_out"
    cols = writer.to_parquet(out_dir)
    assert not cols["emin"]
    assert not cols["emax"]


def test_emin_parquet_is_per_stage_only(tmp_path):
    """Verify emin.parquet (and emax.parquet) are per-stage, never per-block.

    The C++ Reservoir reader broadcasts a stage-indexed series across all
    blocks of the stage. If the writer ever added a ``block`` column the
    reader would mis-broadcast — this guards the contract from regressions.
    """
    manem_f = tmp_path / "plpmanem.dat"
    manem_f.write_text(
        " 1\n'RESERVOIR1'\n   2\n   03     001  0.30  1.50\n   03     002  0.35  1.45\n"
    )
    manem_parser = ManemParser(manem_f)
    manem_parser.parse()

    central_parser = _make_central_parser(tmp_path, "RESERVOIR1")
    stage_parser = _make_stage_parser(tmp_path, 2)

    writer = ManemWriter(manem_parser, central_parser, stage_parser)
    out_dir = tmp_path / "manem_out_schema"
    writer.to_parquet(out_dir)

    df_emin = pd.read_parquet(out_dir / "emin.parquet")
    df_emax = pd.read_parquet(out_dir / "emax.parquet")

    assert "stage" in df_emin.columns
    assert "block" not in df_emin.columns, (
        "emin.parquet must be per-stage only — adding a 'block' column "
        "would make the C++ Reservoir reader mis-broadcast"
    )
    assert "stage" in df_emax.columns
    assert "block" not in df_emax.columns
    assert len(df_emin) == 2
    assert len(df_emax) == 2

    # Spot-check values against the fixture above:
    # stage 1: emin=0.30, emax=1.50 ; stage 2: emin=0.35, emax=1.45
    data_cols_emin = [c for c in df_emin.columns if c.startswith("uid:")]
    data_cols_emax = [c for c in df_emax.columns if c.startswith("uid:")]
    assert len(data_cols_emin) == 1
    assert len(data_cols_emax) == 1

    s1_emin = df_emin[df_emin["stage"] == 1][data_cols_emin[0]].iloc[0]
    s2_emin = df_emin[df_emin["stage"] == 2][data_cols_emin[0]].iloc[0]
    s1_emax = df_emax[df_emax["stage"] == 1][data_cols_emax[0]].iloc[0]
    s2_emax = df_emax[df_emax["stage"] == 2][data_cols_emax[0]].iloc[0]
    assert float(s1_emin) == pytest.approx(0.30)
    assert float(s2_emin) == pytest.approx(0.35)
    assert float(s1_emax) == pytest.approx(1.50)
    assert float(s2_emax) == pytest.approx(1.45)


def test_partial_manem_coverage_falls_back_to_base(tmp_path):
    """A reservoir with PARTIAL manem coverage must get the BASE emin/emax on
    the uncovered stages and the maintenance value on covered ones.

    The emitted emin/emax profile must be DENSE: every stage carries a
    value, never NaN / missing.  Without the dense default the C++
    ``Reservoir`` reader (which broadcasts a stage-indexed series across all
    blocks) would see a hole on the uncovered stages.
    """
    # Manem lists ONLY stage 2 (emin=200, emax=900); stages 1 and 3 are
    # uncovered and must inherit the reservoir's base emin=100 / emax=1000.
    manem_f = tmp_path / "plpmanem.dat"
    manem_f.write_text(" 1\n'RESERVOIR1'\n   1\n   02     001  200.0  900.0\n")
    manem_parser = ManemParser(manem_f)
    manem_parser.parse()

    central_parser = _make_central_parser(
        tmp_path, "RESERVOIR1", emin=100.0, emax=1000.0
    )
    stage_parser = _make_stage_parser(tmp_path, 3)

    writer = ManemWriter(manem_parser, central_parser, stage_parser)
    df_emin, df_emax = writer.to_dataframe()

    data_col = next(c for c in df_emin.columns if c.startswith("uid:"))
    # Dense: one row per stage, no NaN anywhere.
    assert len(df_emin) == 3
    assert len(df_emax) == 3
    assert not df_emin[data_col].isna().any()
    assert not df_emax[data_col].isna().any()

    covered_stage = int(manem_parser.get_all()[0]["stage"][0])
    for stage in (1, 2, 3):
        emin_v = float(df_emin[df_emin["stage"] == stage][data_col].iloc[0])
        emax_col = next(c for c in df_emax.columns if c.startswith("uid:"))
        emax_v = float(df_emax[df_emax["stage"] == stage][emax_col].iloc[0])
        if stage == covered_stage:
            assert emin_v == pytest.approx(200.0)
            assert emax_v == pytest.approx(900.0)
        else:
            # Uncovered stage → reservoir BASE emin / emax (dense default).
            assert emin_v == pytest.approx(100.0)
            assert emax_v == pytest.approx(1000.0)


def test_missing_base_emin_emax_no_nan(tmp_path):
    """Even when the CentralParser record lacks base emin/emax, the emitted
    profile must be NaN-free (last-resort 0.0 fill).  Guards the C++ reader
    from ever receiving a NaN bound."""
    manem_f = tmp_path / "plpmanem.dat"
    manem_f.write_text(" 1\n'RESERVOIR1'\n   1\n   02     001  200.0  900.0\n")
    manem_parser = ManemParser(manem_f)
    manem_parser.parse()

    # No emin/emax on the central record.
    central_parser = _make_central_parser(tmp_path, "RESERVOIR1")
    stage_parser = _make_stage_parser(tmp_path, 3)

    writer = ManemWriter(manem_parser, central_parser, stage_parser)
    df_emin, df_emax = writer.to_dataframe()

    data_col = next(c for c in df_emin.columns if c.startswith("uid:"))
    emax_col = next(c for c in df_emax.columns if c.startswith("uid:"))
    assert not df_emin[data_col].isna().any()
    assert not df_emax[emax_col].isna().any()
