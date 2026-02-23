"""Unit tests for ManliWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..manli_writer import ManliWriter
from ..manli_parser import ManliParser
from .conftest import get_example_file


@pytest.fixture
def sample_manli_file():
    """Fixture providing path to sample line maintenance file."""
    return get_example_file("plpmanli.dat")


@pytest.fixture
def sample_manli_writer(sample_manli_file):
    """Fixture providing initialized ManliWriter with sample data."""
    parser = ManliParser(sample_manli_file)
    parser.parse()
    return ManliWriter(parser)


def test_manli_writer_initialization(sample_manli_file):
    """Test ManliWriter initialization."""
    parser = ManliParser(sample_manli_file)
    parser.parse()
    writer = ManliWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_manlis


def test_to_json_array(sample_manli_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_manlis = sample_manli_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_manlis, list)
    assert len(json_manlis) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "block": list,
        "tmax_ab": list,
        "tmax_ba": list,
        "active": list,
    }

    for manli in json_manlis:
        for field, field_type in required_fields.items():
            assert field in manli, f"Missing field: {field}"
            assert isinstance(manli[field], field_type), (
                f"Field {field} should be {field_type}, got {type(manli[field])}"
            )


def test_write_to_file(sample_manli_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_manli_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_manli_writer):
    """Verify JSON output matches expected structure."""
    json_manlis = sample_manli_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "block": list,
        "tmax_ab": list,
        "tmax_ba": list,
        "active": list,
    }

    for manli in json_manlis:
        # Check all required fields exist and have correct types
        assert set(manli.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(manli[field], field_type), (
                f"Field {field} should be {field_type}, got {type(manli[field])}"
            )

        # Additional value checks
        assert len(manli["name"]) > 0, "Name should not be empty"
        assert len(manli["block"]) > 0, "Should have at least one block"
        assert len(manli["block"]) == len(manli["tmax_ab"]), (
            "Blocks and tmax_ab should match"
        )
        assert len(manli["block"]) == len(manli["tmax_ba"]), (
            "Blocks and tmax_ba should match"
        )
        assert len(manli["block"]) == len(manli["active"]), (
            "Blocks and active should match"
        )


def test_write_empty_manlis():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = ManliParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    writer = ManliWriter(parser)

    # Test empty array conversion
    json_manlis = writer.to_json_array()
    assert isinstance(json_manlis, list)
    assert len(json_manlis) == 0

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


def _make_line_parser(tmp_path, name, number=1):
    """Create a minimal LineParser with one line entry."""
    from ..line_parser import LineParser

    parser = LineParser.__new__(LineParser)
    parser.file_path = tmp_path / "plpcnfli.dat"
    parser._data = [{"name": name, "number": number}]
    parser._name_index_map = {name: 0}
    parser._number_index_map = {number: 0}
    return parser


def _make_block_parser(tmp_path, n_blocks=2):
    """Create a minimal BlockParser with n blocks (all stage 1)."""
    from ..block_parser import BlockParser

    parser = BlockParser.__new__(BlockParser)
    parser.file_path = tmp_path / "plpblo.dat"
    parser._data = [
        {
            "number": i + 1,
            "stage": 1,
            "duration": 7.0,
            "accumulated_time": (i + 1) * 7.0,
        }
        for i in range(n_blocks)
    ]
    parser._name_index_map = {}
    parser._number_index_map = {i + 1: i for i in range(n_blocks)}
    parser.stage_number_map = {i + 1: 1 for i in range(n_blocks)}
    return parser


def test_to_dataframe_with_parsers(tmp_path):
    """Test to_dataframe with line and block parsers."""
    manli_f = tmp_path / "plpmanli.dat"
    manli_f.write_text(
        " 1\n"
        "'LINEA1'\n"
        "   2\n"
        "   001               0.0        0.0         F\n"
        "   002             100.0      100.0         T\n"
    )
    manli_parser = ManliParser(manli_f)
    manli_parser.parse()

    line_parser = _make_line_parser(tmp_path, "LINEA1")
    block_parser = _make_block_parser(tmp_path, 2)

    writer = ManliWriter(manli_parser, line_parser, block_parser)
    df_tmax_ab, df_tmax_ba, df_active = writer.to_dataframe()

    assert not df_tmax_ab.empty
    assert not df_tmax_ba.empty


def test_block_to_stage_df_empty(tmp_path):
    """Test block_to_stage_df with empty DataFrame returns it unchanged."""
    import pandas as pd

    writer = ManliWriter()
    result = writer.block_to_stage_df(pd.DataFrame())
    assert result.empty


def test_block_to_stage_df_with_stage(tmp_path):
    """Test block_to_stage_df with a DataFrame that has a stage column."""
    import pandas as pd

    writer = ManliWriter()
    df = pd.DataFrame({"stage": [1, 1, 2], "uid:1": [0, 0, 1]})
    result = writer.block_to_stage_df(df)
    # Duplicate stage=1 removed, keeps first occurrence
    assert len(result) == 2
    assert list(result["stage"]) == [1, 2]


def test_to_parquet(tmp_path):
    """Test to_parquet writes tmax_ab, tmax_ba, active parquet files."""
    manli_f = tmp_path / "plpmanli.dat"
    manli_f.write_text(
        " 1\n"
        "'LINEA1'\n"
        "   2\n"
        "   001               0.0        0.0         F\n"
        "   002             100.0      100.0         T\n"
    )
    manli_parser = ManliParser(manli_f)
    manli_parser.parse()

    line_parser = _make_line_parser(tmp_path, "LINEA1")
    block_parser = _make_block_parser(tmp_path, 2)

    writer = ManliWriter(manli_parser, line_parser, block_parser)
    out_dir = tmp_path / "manli_out"
    cols = writer.to_parquet(out_dir)

    assert (out_dir / "tmax_ab.parquet").exists()
    assert (out_dir / "tmax_ba.parquet").exists()
    assert (out_dir / "active.parquet").exists()
    assert isinstance(cols, dict)
