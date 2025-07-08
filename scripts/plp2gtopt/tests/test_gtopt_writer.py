"""Tests for gtopt_writer.py module."""

import json
from pathlib import Path
from unittest.mock import MagicMock, patch
import pytest

from plp2gtopt.gtopt_writer import GTOptWriter


@pytest.fixture
def mock_parser():
    """Create a mock PLPParser with sample data."""
    parser = MagicMock()
    # Create mutable lists that can be modified by _process_stage_blocks
    stage_data = [{"uid": 1, "name": "Stage 1"}, {"uid": 2, "name": "Stage 2"}]
    block_data = [{"uid": 1, "stage": 1}, {"uid": 2, "stage": 1}, {"uid": 3, "stage": 2}]
    parser.parsed_data = {
        "block_array": MagicMock(to_json_array=lambda: block_data),
        "stage_array": MagicMock(to_json_array=lambda: stage_data),
        "bus_array": MagicMock(to_json_array=lambda: [{"id": "Bus1"}]),
        "line_array": MagicMock(to_json_array=lambda: [{"id": "Line1"}]),
        "central_array": MagicMock(to_json_array=lambda: [{"id": "Gen1"}]),
        "demand_array": MagicMock(to_json_array=lambda: [{"id": "Load1"}]),
        "cost_array": MagicMock(to_json_array=lambda: [{"id": "Cost1"}])
    }
    parser.input_path = Path("/input")
    return parser


def test_gtopt_writer_init(mock_parser):
    """Test GTOptWriter initialization."""
    writer = GTOptWriter(mock_parser)
    assert writer.parser == mock_parser
    assert writer.output_path is None


def test_process_stage_blocks(mock_parser):
    """Test _process_stage_blocks updates stage blocks correctly."""
    writer = GTOptWriter(mock_parser)
    # Test protected method access is ok for testing
    writer._process_stage_blocks()  # pylint: disable=protected-access
    stages = mock_parser.parsed_data["stage_array"].to_json_array()
    assert stages[0]["first_block"] == 0
    assert stages[0]["count_block"] == 2
    assert stages[1]["first_block"] == 2
    assert stages[1]["count_block"] == 1


def test_to_json(mock_parser, tmp_path):
    """Test to_json produces correct output structure."""
    with patch.dict('sys.modules',
                   {'plp2gtopt.block_writer': MagicMock(),
                    'plp2gtopt.stage_writer': MagicMock(),
                    'plp2gtopt.bus_writer': MagicMock(),
                    'plp2gtopt.line_writer': MagicMock(),
                    'plp2gtopt.central_writer': MagicMock(),
                    'plp2gtopt.demand_writer': MagicMock(),
                    'plp2gtopt.cost_writer': MagicMock()}):
        writer = GTOptWriter(mock_parser)
        writer.output_path = tmp_path

        result = writer.to_json()
        assert "options" in result
        assert "simulation" in result
        assert "system" in result
        assert len(result["simulation"]["stage_array"]) == 2
        assert len(result["system"]["bus_array"]) == 1


def test_write_json_file(mock_parser, tmp_path):
    """Test write creates valid JSON output file."""
    with patch.dict('sys.modules',
                   {'plp2gtopt.block_writer': MagicMock(),
                    'plp2gtopt.stage_writer': MagicMock(),
                    'plp2gtopt.bus_writer': MagicMock(),
                    'plp2gtopt.line_writer': MagicMock(),
                    'plp2gtopt.central_writer': MagicMock(),
                    'plp2gtopt.demand_writer': MagicMock(),
                    'plp2gtopt.cost_writer': MagicMock()}):
        output_file = tmp_path / "output.json"
        writer = GTOptWriter(mock_parser)
        writer.write(tmp_path)

        assert output_file.exists()
        with open(output_file, encoding='utf-8') as f:
            data = json.load(f)
            assert "options" in data
            assert "simulation" in data


def test_write_empty_data(tmp_path):
    """Test write with empty parser data."""
    empty_parser = MagicMock()
    empty_parser.parsed_data = {}
    empty_parser.input_path = Path("/input")

    writer = GTOptWriter(empty_parser)
    writer.write(tmp_path)
    output_file = tmp_path / "plp2gtopt.json"
    assert output_file.exists()
