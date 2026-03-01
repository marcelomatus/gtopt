"""Unit tests for ManemWriter class."""

import json
import tempfile
from pathlib import Path

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


def _make_central_parser(tmp_path, name, number=1):
    """Create a minimal CentralParser with one central entry."""

    parser = CentralParser.__new__(CentralParser)
    parser.file_path = tmp_path / "plpcnfce.dat"
    parser._data = [{"name": name, "number": number, "type": "embalse"}]
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
