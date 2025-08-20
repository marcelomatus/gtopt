"""Tests for plp_parser.py module."""

from unittest.mock import patch, MagicMock

import pytest

from plp2gtopt.plp_parser import PLPParser


@pytest.fixture
def sample_input_dir(tmp_path):
    """Create sample input directory with empty PLP files."""
    files = [
        "plpblo.dat",
        "plpeta.dat",
        "plpbar.dat",
        "plpcnfli.dat",
        "plpcnfce.dat",
        "plpdem.dat",
        "plpcosce.dat",
        "plpmance.dat",
        "plpmanli.dat",
        "plpaflce.dat",
        "plpextrac.dat",
        "plpmanem.dat",
    ]
    for f in files:
        (tmp_path / f).touch()
    return tmp_path


def test_plp_parser_init_valid_dir(sample_input_dir):
    """Test PLPParser initialization with valid input directory."""
    parser = PLPParser({"input_dir": sample_input_dir})
    assert parser.input_path == sample_input_dir
    assert not parser.parsed_data


def test_plp_parser_init_invalid_dir(tmp_path):
    """Test PLPParser initialization with invalid input directory."""
    with pytest.raises(FileNotFoundError):
        PLPParser({"input_dir": tmp_path / "nonexistent"})


def test_parse_all_success(sample_input_dir):
    """Test parse_all with all required files present."""
    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch(
        "plp2gtopt.plp_parser.CentralParser"
    ) as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch(
        "plp2gtopt.plp_parser.CostParser"
    ) as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch(
        "plp2gtopt.plp_parser.ManliParser"
    ) as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch(
        "plp2gtopt.plp_parser.ExtracParser"
    ) as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem:

        # Setup mock parsers
        mock_parser = MagicMock()
        mock_parser.parse.return_value = None
        mock_block.return_value = mock_parser
        mock_stage.return_value = mock_parser
        mock_bus.return_value = mock_parser
        mock_line.return_value = mock_parser
        mock_central.return_value = mock_parser
        mock_demand.return_value = mock_parser
        mock_cost.return_value = mock_parser
        mock_mance.return_value = mock_parser
        mock_manli.return_value = mock_parser
        mock_aflce.return_value = mock_parser
        mock_extrac.return_value = mock_parser
        mock_manem.return_value = mock_parser

        parser = PLPParser({"input_dir": sample_input_dir})
        parser.parse_all()

        assert len(parser.parsed_data) == 12
        for name in [
            "block_parser",
            "stage_parser",
            "bus_parser",
            "line_parser",
            "central_parser",
            "demand_parser",
            "cost_parser",
            "mance_parser",
            "manli_parser",
            "aflce_parser",
            "extrac_parser",
            "manem_parser",
        ]:
            assert name in parser.parsed_data


def test_parse_all_missing_file(sample_input_dir):
    """Test parse_all when a required file is missing."""
    (sample_input_dir / "plpblo.dat").unlink()  # Delete block file

    parser = PLPParser({"input_dir": sample_input_dir})
    with pytest.raises(FileNotFoundError):
        parser.parse_all()
