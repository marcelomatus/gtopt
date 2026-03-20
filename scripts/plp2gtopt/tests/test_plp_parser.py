"""Tests for plp_parser.py module."""

import shutil
from pathlib import Path
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
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
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

        assert len(parser.parsed_data) == 13  # 12 parsers + _options
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


def test_parse_all_with_battery(tmp_path):
    """parse_all uses the BatteryParser when plpcenbat.dat exists (no plpess.dat)."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    # Remove plpess.dat so battery path is taken, then create plpcenbat.dat
    (tmp_path / "plpess.dat").unlink(missing_ok=True)
    (tmp_path / "plpcenbat.dat").write_text("")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.BatteryParser") as mock_battery:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_battery,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "battery_parser" in parser.parsed_data
        mock_battery.assert_called_once()


def test_parse_all_with_ess_only(tmp_path):
    """parse_all uses EssParser when only plpess.dat exists (no plpcenbat.dat)."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    (tmp_path / "plpess.dat").write_text("# Numero de ESS\n 0\n")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.EssParser") as mock_ess:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_ess,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "ess_parser" in parser.parsed_data
        assert "battery_parser" not in parser.parsed_data
        mock_ess.assert_called_once()


def test_parse_all_ess_takes_priority_when_both_present(tmp_path):
    """parse_all uses ESS (not battery) when both plpess.dat and plpcenbat.dat exist."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    (tmp_path / "plpess.dat").write_text("# Numero de ESS\n 0\n")
    (tmp_path / "plpcenbat.dat").write_text("")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.EssParser") as mock_ess, patch(
        "plp2gtopt.plp_parser.BatteryParser"
    ) as mock_battery:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_ess,
            mock_battery,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        # ESS takes priority – battery_parser must NOT be created
        assert "ess_parser" in parser.parsed_data
        assert "battery_parser" not in parser.parsed_data
        mock_ess.assert_called_once()
        mock_battery.assert_not_called()


def test_parse_all_with_maness(tmp_path):
    """parse_all parses plpmaness.dat when plpess.dat exists."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    (tmp_path / "plpess.dat").write_text("# Numero de ESS\n 0\n")
    (tmp_path / "plpmaness.dat").write_text("# Numero de ESS con mantenimiento\n 0\n")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.EssParser") as mock_ess, patch(
        "plp2gtopt.plp_parser.ManessParser"
    ) as mock_maness:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_ess,
            mock_maness,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "ess_parser" in parser.parsed_data
        assert "maness_parser" in parser.parsed_data
        mock_maness.assert_called_once()


def test_parse_all_with_cenre(tmp_path):
    """parse_all uses CenreParser when plpcenre.dat exists."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    # Write a minimal plpcenre.dat (0 entries)
    (tmp_path / "plpcenre.dat").write_text("# Archivo de Rendimiento de Embalses\n 0\n")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.CenreParser") as mock_cenre:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_cenre,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "cenre_parser" in parser.parsed_data
        mock_cenre.assert_called_once()


def test_parse_all_with_cenfi(tmp_path):
    """parse_all uses CenfiParser when plpcenfi.dat exists."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    # Write a minimal plpcenfi.dat (0 entries)
    (tmp_path / "plpcenfi.dat").write_text("# Archivo de centrales filtracion\n 0\n")

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.CenfiParser") as mock_cenfi:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_cenfi,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "cenfi_parser" in parser.parsed_data
        mock_cenfi.assert_called_once()


def test_parse_all_without_cenre_cenfi(sample_input_dir):
    """parse_all does not create cenre/cenfi parsers when files are absent."""
    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": sample_input_dir})
        parser.parse_all()

        assert "cenre_parser" not in parser.parsed_data
        assert "cenfi_parser" not in parser.parsed_data
        assert "filemb_parser" not in parser.parsed_data


def test_parse_all_with_filemb(tmp_path):
    """parse_all uses FilembParser when plpfilemb.dat exists."""
    cases_dir = Path(__file__).parent.parent.parent / "cases"
    base_dir = cases_dir / "plp_min_1bus"
    for f in base_dir.iterdir():
        if f.suffix == ".dat":
            shutil.copy(f, tmp_path / f.name)
    # Write a minimal plpfilemb.dat (0 entries)
    (tmp_path / "plpfilemb.dat").write_text(
        "# Archivo de Filtraciones de Embalses\n# Numero\n 0\n"
    )

    with patch("plp2gtopt.plp_parser.BlockParser") as mock_block, patch(
        "plp2gtopt.plp_parser.StageParser"
    ) as mock_stage, patch("plp2gtopt.plp_parser.BusParser") as mock_bus, patch(
        "plp2gtopt.plp_parser.LineParser"
    ) as mock_line, patch("plp2gtopt.plp_parser.CentralParser") as mock_central, patch(
        "plp2gtopt.plp_parser.DemandParser"
    ) as mock_demand, patch("plp2gtopt.plp_parser.CostParser") as mock_cost, patch(
        "plp2gtopt.plp_parser.ManceParser"
    ) as mock_mance, patch("plp2gtopt.plp_parser.ManliParser") as mock_manli, patch(
        "plp2gtopt.plp_parser.AflceParser"
    ) as mock_aflce, patch("plp2gtopt.plp_parser.ExtracParser") as mock_extrac, patch(
        "plp2gtopt.plp_parser.ManemParser"
    ) as mock_manem, patch("plp2gtopt.plp_parser.FilembParser") as mock_filemb:
        mock_p = MagicMock()
        mock_p.parse.return_value = None
        for m in [
            mock_block,
            mock_stage,
            mock_bus,
            mock_line,
            mock_central,
            mock_demand,
            mock_cost,
            mock_mance,
            mock_manli,
            mock_aflce,
            mock_extrac,
            mock_manem,
            mock_filemb,
        ]:
            m.return_value = mock_p

        parser = PLPParser({"input_dir": tmp_path})
        parser.parse_all()

        assert "filemb_parser" in parser.parsed_data
        mock_filemb.assert_called_once()
