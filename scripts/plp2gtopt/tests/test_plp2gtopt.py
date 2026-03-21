"""Tests for plp2gtopt.py — convert_plp_case(), GTOptWriter, and main()."""

import argparse
import json
import sys
import zipfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt.block_parser import BlockParser
from plp2gtopt.bus_parser import BusParser
from plp2gtopt.central_parser import CentralParser
from plp2gtopt.demand_parser import DemandParser
from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.line_parser import LineParser
from plp2gtopt.main import _resolve_input_dir, build_options, main, make_parser
from plp2gtopt.plp2gtopt import (
    convert_plp_case,
    create_zip_output,
    generate_variable_scales_template,
    print_variable_scales_template,
    run_post_check,
    validate_plp_case,
)
from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.stage_parser import StageParser

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"


def _make_opts(input_dir: Path, tmp_path: Path, case_name: str = "test") -> dict:
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": input_dir,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
        "last_stage": -1,
        "last_time": -1,
        "compression": "zstd",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
    }


# ---------------------------------------------------------------------------
# convert_plp_case() — happy path


def test_convert_plp_case_success(tmp_path):
    """convert_plp_case completes without error on the plp_min_1bus fixture."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)
    convert_plp_case(opts)
    assert opts["output_file"].exists()
    data = json.loads(opts["output_file"].read_text())
    assert "options" in data
    assert "system" in data
    assert "simulation" in data


def test_convert_plp_case_with_discount_rate(tmp_path):
    """convert_plp_case applies discount_rate to the options block."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)
    opts["discount_rate"] = 0.10
    convert_plp_case(opts)
    data = json.loads(opts["output_file"].read_text())
    assert data["options"]["annual_discount_rate"] == pytest.approx(0.10)


def test_convert_plp_case_failure_raises_runtime_error(tmp_path):
    """convert_plp_case wraps parser errors as RuntimeError."""
    opts = _make_opts(tmp_path / "nonexistent", tmp_path)
    # The non-existent input dir triggers FileNotFoundError inside PLPParser
    opts["input_dir"] = tmp_path / "nonexistent"
    (tmp_path / "nonexistent").mkdir()
    # Missing all .dat files → parse_all raises FileNotFoundError
    with pytest.raises(RuntimeError, match="PLP to GTOPT conversion failed"):
        convert_plp_case(opts)


def test_convert_plp_case_missing_input_dir_raises_runtime_error(tmp_path):
    """convert_plp_case raises RuntimeError when input_dir does not exist."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)
    opts["input_dir"] = tmp_path / "does_not_exist"
    with pytest.raises(RuntimeError, match="Input directory does not exist"):
        convert_plp_case(opts)


def test_convert_plp_case_zip_output(tmp_path):
    """convert_plp_case with zip_output=True creates a ZIP archive."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)
    opts["zip_output"] = True
    convert_plp_case(opts)

    zip_path = opts["output_file"].with_suffix(".zip")
    assert zip_path.exists(), "ZIP archive not created"

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

    # JSON at archive root
    case_name = opts["output_file"].stem
    assert f"{case_name}.json" in names

    # Data files must be under the input_directory prefix (output_dir name)
    input_dir_name = opts["output_dir"].name
    data_files = [n for n in names if n != f"{case_name}.json"]
    assert len(data_files) > 0, "ZIP archive contains no data files"
    for name in data_files:
        assert name.startswith(f"{input_dir_name}/"), (
            f"Data file not under input_dir prefix: {name}"
        )


def test_convert_plp_case_zip_preserves_subdirs(tmp_path):
    """ZIP archive mirrors the output_dir subdirectory structure."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)
    opts["zip_output"] = True
    convert_plp_case(opts)

    zip_path = opts["output_file"].with_suffix(".zip")
    input_dir_name = opts["output_dir"].name

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

    # Demand/lmax.parquet must be under the input_dir prefix
    assert f"{input_dir_name}/Demand/lmax.parquet" in names


def test_create_zip_output_unit(tmp_path):
    """create_zip_output produces a valid ZIP with correct archive layout."""
    # Create a fake output structure
    output_dir = tmp_path / "mycase"
    output_dir.mkdir()
    (output_dir / "Demand").mkdir()
    (output_dir / "Demand" / "lmax.parquet").write_bytes(b"dummy")
    (output_dir / "Generator").mkdir()
    (output_dir / "Generator" / "pmax.parquet").write_bytes(b"dummy2")

    output_file = tmp_path / "mycase.json"
    output_file.write_text('{"options":{}}')

    zip_path = tmp_path / "mycase.zip"
    create_zip_output(output_file, output_dir, zip_path)

    assert zip_path.exists()
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())

    assert "mycase.json" in names
    assert "mycase/Demand/lmax.parquet" in names
    assert "mycase/Generator/pmax.parquet" in names


# ---------------------------------------------------------------------------
# GTOptWriter — unit tests with mocked parser
# ---------------------------------------------------------------------------


def _make_mock_parser():
    """Build a minimal mock PLPParser with the structure GTOptWriter expects."""

    # --- block_parser ---
    block_p = MagicMock(spec=BlockParser)
    block_p.items = [{"number": 1, "stage": 1, "duration": 1.0, "hydrology": 0}]
    block_p.blocks = block_p.items

    # --- stage_parser ---
    stage_p = MagicMock(spec=StageParser)
    stage_p.items = [{"number": 1, "duration": 1}]
    stage_p.stages = stage_p.items

    # --- bus_parser ---
    bus_p = MagicMock(spec=BusParser)
    bus_p.buses = [{"name": "Bus1", "number": 1}]
    bus_p.get_bus_by_name = MagicMock(
        side_effect=lambda name: {"name": name, "number": 1}
    )

    # --- central_parser ---
    central_p = MagicMock(spec=CentralParser)
    central_p.centrals = []

    # --- line_parser ---
    line_p = MagicMock(spec=LineParser)
    line_p.lines = []

    # --- demand_parser ---
    demand_p = MagicMock(spec=DemandParser)
    demand_p.get_all = MagicMock(return_value=[])

    parser = MagicMock()
    parser.parsed_data = {
        "block_parser": block_p,
        "stage_parser": stage_p,
        "bus_parser": bus_p,
        "central_parser": central_p,
        "line_parser": line_p,
        "demand_parser": demand_p,
        "cost_parser": MagicMock(),
        "mance_parser": MagicMock(),
        "aflce_parser": MagicMock(),
        "extrac_parser": MagicMock(),
        "manem_parser": MagicMock(),
        "manli_parser": MagicMock(),
    }
    return parser


def test_gtopt_writer_to_json_returns_planning(tmp_path):
    """GTOptWriter.to_json() returns a dict with options/system/simulation keys."""
    mock_parser = _make_mock_parser()

    with patch("plp2gtopt.gtopt_writer.BlockWriter") as mock_bw, patch(
        "plp2gtopt.gtopt_writer.StageWriter"
    ) as mock_sw, patch("plp2gtopt.gtopt_writer.BusWriter") as mock_busw, patch(
        "plp2gtopt.gtopt_writer.CentralWriter"
    ) as mock_cw, patch("plp2gtopt.gtopt_writer.LineWriter") as mock_lw, patch(
        "plp2gtopt.gtopt_writer.DemandWriter"
    ) as mock_dw, patch(
        "plp2gtopt.gtopt_writer.GeneratorProfileWriter"
    ) as mock_gpw, patch("plp2gtopt.gtopt_writer.AflceWriter") as mock_aw, patch(
        "plp2gtopt.gtopt_writer.JunctionWriter"
    ) as mock_jw, patch("plp2gtopt.gtopt_writer.BatteryWriter") as mock_battery:
        # All writers return empty arrays
        for m in [mock_bw, mock_sw, mock_busw, mock_cw, mock_lw, mock_dw]:
            m.return_value.to_json_array.return_value = []
        mock_gpw.return_value.to_json_array.return_value = []
        mock_aw.return_value.to_parquet.return_value = None
        mock_jw.return_value.to_json_array.return_value = []
        mock_battery.return_value.process.return_value = {
            "battery_array": [],
            "converter_array": [],
            "generator_array": [],
            "demand_array": [],
        }

        opts = {
            "output_dir": str(tmp_path),
            "output_file": str(tmp_path / "out.json"),
            "hydrologies": "1",
            "discount_rate": 0.0,
        }

        writer = GTOptWriter(mock_parser)
        result = writer.to_json(opts)

    assert "options" in result
    assert "system" in result
    assert "simulation" in result
    assert result["system"]["name"] == "plp2gtopt"


def test_gtopt_writer_process_options_discount():
    """process_options sets annual_discount_rate from options."""
    mock_parser = MagicMock()
    writer = GTOptWriter(mock_parser)
    writer.process_options({"discount_rate": 0.05, "output_dir": "out"})
    assert writer.planning["options"]["annual_discount_rate"] == pytest.approx(0.05)


def test_gtopt_writer_process_scenarios_equal_weights():
    """process_scenarios assigns equal weights when probability_factors is None."""
    mock_parser = MagicMock()
    writer = GTOptWriter(mock_parser)
    writer.process_scenarios({"hydrologies": "0,1", "probability_factors": None})
    scenarios = writer.planning["simulation"]["scenario_array"]
    assert len(scenarios) == 2
    assert scenarios[0]["probability_factor"] == pytest.approx(0.5)
    assert scenarios[1]["probability_factor"] == pytest.approx(0.5)


def test_gtopt_writer_process_scenarios_explicit_weights():
    """process_scenarios uses explicit probability_factors."""
    mock_parser = MagicMock()
    writer = GTOptWriter(mock_parser)
    writer.process_scenarios({"hydrologies": "0,1", "probability_factors": "0.3,0.7"})
    scenarios = writer.planning["simulation"]["scenario_array"]
    assert scenarios[0]["probability_factor"] == pytest.approx(0.3)
    assert scenarios[1]["probability_factor"] == pytest.approx(0.7)


def test_gtopt_writer_write_creates_file(tmp_path):
    """GTOptWriter.write() produces a valid JSON file."""
    opts = _make_opts(_PLPMin1Bus, tmp_path)

    parser = PLPParser({"input_dir": _PLPMin1Bus})
    parser.parse_all()
    writer = GTOptWriter(parser)
    writer.write(opts)
    assert opts["output_file"].exists()
    data = json.loads(opts["output_file"].read_text())
    assert isinstance(data, dict)
    assert "options" in data


# ---------------------------------------------------------------------------
# main.py — CLI argument parsing
# ---------------------------------------------------------------------------


def test_main_default_args(tmp_path):
    """main() converts plp_min_1bus when called with default paths pointing to fixture."""

    out_file = tmp_path / "gtopt_case.json"
    test_argv = [
        "plp2gtopt",
        "-i",
        str(_PLPMin1Bus),
        "-o",
        str(tmp_path),
        "-f",
        str(out_file),
    ]
    with patch.object(sys, "argv", test_argv):
        main()
    assert out_file.exists()


def test_main_with_discount_rate(tmp_path):
    """main() passes discount_rate through to convert_plp_case."""

    out_file = tmp_path / "gtopt_case.json"
    test_argv = [
        "plp2gtopt",
        "-i",
        str(_PLPMin1Bus),
        "-o",
        str(tmp_path),
        "-f",
        str(out_file),
        "-d",
        "0.10",
    ]
    with patch.object(sys, "argv", test_argv):
        main()
    data = json.loads(out_file.read_text())
    assert data["options"]["annual_discount_rate"] == pytest.approx(0.10)


def test_main_version(capsys):
    """main() --version prints version string."""

    with pytest.raises(SystemExit) as exc_info:
        with patch.object(sys, "argv", ["plp2gtopt", "--version"]):
            main()
    assert exc_info.value.code == 0
    captured = capsys.readouterr()
    assert "plp2gtopt" in captured.out


def test_main_log_level_debug(tmp_path):
    """main() accepts -l DEBUG without error."""

    out_file = tmp_path / "gtopt_case.json"
    test_argv = [
        "plp2gtopt",
        "-i",
        str(_PLPMin1Bus),
        "-o",
        str(tmp_path),
        "-f",
        str(out_file),
        "-l",
        "DEBUG",
    ]
    with patch.object(sys, "argv", test_argv):
        main()
    assert out_file.exists()


# ---------------------------------------------------------------------------
# make_parser() / build_options() — unit tests (no I/O, no real parser)
# ---------------------------------------------------------------------------


def test_make_parser_returns_argument_parser():
    """make_parser() returns an ArgumentParser with prog='plp2gtopt'."""
    p = make_parser()
    assert isinstance(p, argparse.ArgumentParser)
    assert p.prog == "plp2gtopt"


def test_build_options_defaults():
    """build_options() with no CLI args produces the expected default dict."""
    args = make_parser().parse_args([])
    opts = build_options(args)

    assert opts["input_dir"] == Path("input")
    assert opts["output_dir"] == Path("output")
    assert opts["output_file"] == Path("output/output.json")
    assert opts["last_stage"] == -1
    assert opts["last_time"] == -1
    assert opts["compression"] == "zstd"
    assert opts["hydrologies"] == "all"
    assert opts["probability_factors"] is None
    assert opts["discount_rate"] == pytest.approx(0.0)
    assert opts["management_factor"] == pytest.approx(0.0)


def test_build_options_custom_input_output_dirs():
    """build_options() maps -i / -o / -f to the correct dict keys."""
    args = make_parser().parse_args(
        ["-i", "myinput", "-o", "myoutput", "-f", "myoutput/case.json"]
    )
    opts = build_options(args)

    assert opts["input_dir"] == Path("myinput")
    assert opts["output_dir"] == Path("myoutput")
    assert opts["output_file"] == Path("myoutput/case.json")


def test_build_options_output_file_derived_from_output_dir():
    """build_options() derives output_file from output_dir when -f is omitted."""
    args = make_parser().parse_args(["-o", "gtopt_case_2y"])
    opts = build_options(args)

    assert opts["output_dir"] == Path("gtopt_case_2y")
    assert opts["output_file"] == Path("gtopt_case_2y/gtopt_case_2y.json")


def test_build_options_discount_rate():
    """build_options() maps -d to discount_rate."""
    args = make_parser().parse_args(["-d", "0.10"])
    opts = build_options(args)
    assert opts["discount_rate"] == pytest.approx(0.10)


def test_build_options_management_factor():
    """build_options() maps -m to management_factor."""
    args = make_parser().parse_args(["-m", "0.05"])
    opts = build_options(args)
    assert opts["management_factor"] == pytest.approx(0.05)


def test_build_options_last_stage():
    """build_options() maps -s to last_stage."""
    args = make_parser().parse_args(["-s", "5"])
    opts = build_options(args)
    assert opts["last_stage"] == 5


def test_build_options_last_time():
    """build_options() maps -t to last_time."""
    args = make_parser().parse_args(["-t", "8760.0"])
    opts = build_options(args)
    assert opts["last_time"] == pytest.approx(8760.0)


def test_build_options_compression():
    """build_options() maps -c to compression."""
    args = make_parser().parse_args(["-c", "snappy"])
    opts = build_options(args)
    assert opts["compression"] == "snappy"


def test_build_options_hydrologies():
    """build_options() maps -y to the hydrologies string."""
    args = make_parser().parse_args(["-y", "0,1,2"])
    opts = build_options(args)
    assert opts["hydrologies"] == "0,1,2"


def test_build_options_probability_factors():
    """build_options() maps -p to the probability_factors string."""
    args = make_parser().parse_args(["-y", "0,1", "-p", "0.3,0.7"])
    opts = build_options(args)
    assert opts["probability_factors"] == "0.3,0.7"


def test_build_options_probability_factors_default_none():
    """build_options() leaves probability_factors as None when -p is omitted."""
    args = make_parser().parse_args(["-y", "0,1"])
    opts = build_options(args)
    assert opts["probability_factors"] is None


def test_make_parser_version_exits(capsys):
    """make_parser() produces a parser whose --version flag exits cleanly."""
    with pytest.raises(SystemExit) as exc_info:
        make_parser().parse_args(["--version"])
    assert exc_info.value.code == 0
    assert "plp2gtopt" in capsys.readouterr().out


def test_build_options_zip_default_false():
    """build_options() defaults zip_output to False when -z is omitted."""
    args = make_parser().parse_args([])
    opts = build_options(args)
    assert opts["zip_output"] is False


def test_build_options_zip_flag():
    """build_options() sets zip_output=True when -z is passed."""
    args = make_parser().parse_args(["-z"])
    opts = build_options(args)
    assert opts["zip_output"] is True


def test_main_zip_creates_archive(tmp_path):
    """main() with -z creates a ZIP archive next to the JSON file."""
    out_dir = tmp_path / "mycase"
    out_file = out_dir / "mycase.json"

    test_argv = [
        "plp2gtopt",
        "-i",
        str(_PLPMin1Bus),
        "-o",
        str(out_dir),
        "-f",
        str(out_file),
        "-z",
    ]
    with patch.object(sys, "argv", test_argv):
        main()

    assert out_file.exists()
    zip_path = out_file.with_suffix(".zip")
    assert zip_path.exists(), "ZIP archive not created by main() -z"

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

    assert "mycase.json" in names
    # Data files are stored under the input_directory prefix
    input_dir_name = out_dir.name
    data_files = [n for n in names if not n.endswith(".json")]
    assert all(n.startswith(f"{input_dir_name}/") for n in data_files)


def test_build_options_num_apertures():
    """build_options() maps --num-apertures to num_apertures as a string."""
    args = make_parser().parse_args(["--num-apertures", "5"])
    opts = build_options(args)
    assert opts["num_apertures"] == "5"


def test_build_options_num_apertures_short():
    """-a is the short form for --num-apertures."""
    args = make_parser().parse_args(["-a", "5"])
    opts = build_options(args)
    assert opts["num_apertures"] == "5"


def test_build_options_num_apertures_range():
    """build_options() maps --num-apertures range to a string."""
    args = make_parser().parse_args(["--num-apertures", "1-5"])
    opts = build_options(args)
    assert opts["num_apertures"] == "1-5"


def test_build_options_num_apertures_all_keyword():
    """build_options() maps --num-apertures all to the string 'all'."""
    args = make_parser().parse_args(["--num-apertures", "all"])
    opts = build_options(args)
    assert opts["num_apertures"] == "all"


def test_build_options_num_apertures_all():
    """build_options() maps --num-apertures -1 to '-1' (legacy all)."""
    args = make_parser().parse_args(["--num-apertures", "-1"])
    opts = build_options(args)
    assert opts["num_apertures"] == "-1"


def test_build_options_num_apertures_default():
    """build_options() defaults num_apertures to 'all' when not specified."""
    args = make_parser().parse_args([])
    opts = build_options(args)
    assert opts["num_apertures"] == "all"


def test_build_options_short_flags_solver():
    """-S is the short form for --solver."""
    args = make_parser().parse_args(["-S", "mono"])
    opts = build_options(args)
    assert opts["solver_type"] == "mono"


def test_build_options_short_flags_stages_phase():
    """-g is the short form for --stages-phase."""
    args = make_parser().parse_args(["-g", "1:4,5,..."])
    opts = build_options(args)
    assert opts["stages_phase"] == "1:4,5,..."


def test_build_options_short_flags_aperture_directory():
    """-A is the short form for --aperture-directory."""
    args = make_parser().parse_args(["-A", "/tmp/apes"])
    opts = build_options(args)
    assert opts["aperture_directory"] == "/tmp/apes"


# ---------------------------------------------------------------------------
# Positional input_dir argument
# ---------------------------------------------------------------------------


def test_positional_input_dir():
    """Positional argument works as input_dir."""

    args = make_parser().parse_args(["my_case"])
    resolved = _resolve_input_dir(args)
    assert resolved == Path("my_case")


def test_positional_input_dir_same_as_flag():
    """'plp2gtopt case' is equivalent to 'plp2gtopt -i case'."""

    args_pos = make_parser().parse_args(["my_case"])
    args_flag = make_parser().parse_args(["-i", "my_case"])
    assert _resolve_input_dir(args_pos) == _resolve_input_dir(args_flag)


def test_flag_overrides_positional():
    """-i flag takes priority over positional argument."""

    args = make_parser().parse_args(["pos_case", "-i", "flag_case"])
    assert _resolve_input_dir(args) == Path("flag_case")


def test_default_input_dir():
    """Default input dir is 'input' when nothing is specified."""

    args = make_parser().parse_args([])
    assert _resolve_input_dir(args) == Path("input")


# ---------------------------------------------------------------------------
# --info error handling
# ---------------------------------------------------------------------------


def test_info_nonexistent_dir(tmp_path: Path):
    """--info with a non-existent directory should exit gracefully."""
    nonexistent = str(tmp_path / "does_not_exist")
    with pytest.raises(SystemExit) as exc_info:
        with patch("sys.argv", ["plp2gtopt", "--info", nonexistent]):
            main()
    assert exc_info.value.code == 1


def test_info_empty_dir(tmp_path: Path):
    """--info with an empty directory should exit gracefully."""
    empty_dir = tmp_path / "empty_case"
    empty_dir.mkdir()
    with pytest.raises(SystemExit) as exc_info:
        with patch("sys.argv", ["plp2gtopt", "--info", str(empty_dir)]):
            main()
    assert exc_info.value.code == 1


# ---------------------------------------------------------------------------
# PLP/gtopt indicator tests
# ---------------------------------------------------------------------------


class TestPlpIndicators:
    """Test _plp_indicators and _gtopt_indicators functions."""

    def test_plp_indicators_min_1bus(self, tmp_path: Path):
        """Test PLP indicator extraction from plp_min_1bus case."""
        from plp2gtopt.plp2gtopt import _plp_indicators  # noqa: PLC0415

        opts = _make_opts(_PLPMin1Bus, tmp_path, "ind_1bus")
        parser = PLPParser(opts)
        parser.parse_all()

        ind = _plp_indicators(parser)

        # plp_min_1bus has Thermal1 with pmax=100 and Falla1 (excluded by type)
        assert ind["total_gen_capacity_mw"] == pytest.approx(100.0)

        # plpdem.dat has 1 demand of 80 MW in 1 block (first == last)
        assert ind["first_block_demand_mw"] == pytest.approx(80.0)
        assert ind["last_block_demand_mw"] == pytest.approx(80.0)

        # 1 block × 1 hour × 80 MW = 80 MWh
        assert ind["total_energy_mwh"] == pytest.approx(80.0)

    def test_gtopt_indicators_min_1bus(self, tmp_path: Path):
        """Test gtopt indicator extraction from converted plp_min_1bus case."""
        from plp2gtopt.plp2gtopt import _gtopt_indicators  # noqa: PLC0415

        opts = _make_opts(_PLPMin1Bus, tmp_path, "gind_1bus")
        parser = PLPParser(opts)
        parser.parse_all()
        writer = GTOptWriter(parser)
        planning = writer.to_json(opts)

        ind = _gtopt_indicators(planning)

        # Should match PLP indicators (excluding falla)
        assert ind["total_gen_capacity_mw"] == pytest.approx(100.0)

    def test_plp_vs_gtopt_capacity_match(self, tmp_path: Path):
        """PLP and gtopt total generation capacity should match."""
        from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
            _gtopt_indicators,
            _plp_indicators,
        )

        opts = _make_opts(_PLPMin1Bus, tmp_path, "cmp_1bus")
        parser = PLPParser(opts)
        parser.parse_all()
        writer = GTOptWriter(parser)
        planning = writer.to_json(opts)

        plp_ind = _plp_indicators(parser)
        gtopt_ind = _gtopt_indicators(planning)

        assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
            gtopt_ind["total_gen_capacity_mw"]
        )

    def test_plp_vs_gtopt_demand_match(self, tmp_path: Path):
        """PLP and gtopt first/last block demand should match when base_dir given."""
        from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
            _gtopt_indicators,
            _plp_indicators,
        )

        opts = _make_opts(_PLPMin1Bus, tmp_path, "cmp_dem")
        parser = PLPParser(opts)
        parser.parse_all()
        writer = GTOptWriter(parser)
        planning = writer.to_json(opts)

        plp_ind = _plp_indicators(parser)

        # Without base_dir, gtopt indicators cannot resolve file references
        gtopt_ind_no_dir = _gtopt_indicators(planning)
        assert gtopt_ind_no_dir["first_block_demand_mw"] == pytest.approx(0.0)

        # With base_dir pointing at the output_dir, file references are resolved
        base_dir = str(opts["output_dir"])
        gtopt_ind = _gtopt_indicators(planning, base_dir=base_dir)

        assert plp_ind["first_block_demand_mw"] == pytest.approx(80.0)
        assert gtopt_ind["first_block_demand_mw"] == pytest.approx(
            plp_ind["first_block_demand_mw"]
        )
        assert gtopt_ind["last_block_demand_mw"] == pytest.approx(
            plp_ind["last_block_demand_mw"]
        )

    def test_gtopt_indicators_demand_with_base_dir(self, tmp_path: Path):
        """_gtopt_indicators resolves demand from Parquet when base_dir given."""
        from plp2gtopt.plp2gtopt import _gtopt_indicators  # noqa: PLC0415

        opts = _make_opts(_PLPMin1Bus, tmp_path, "gind_dem")
        parser = PLPParser(opts)
        parser.parse_all()
        writer = GTOptWriter(parser)
        planning = writer.to_json(opts)

        base_dir = str(opts["output_dir"])
        ind = _gtopt_indicators(planning, base_dir=base_dir)

        # plp_min_1bus has 80 MW demand in 1 block
        assert ind["first_block_demand_mw"] == pytest.approx(80.0)
        assert ind["last_block_demand_mw"] == pytest.approx(80.0)
        assert ind["total_energy_mwh"] == pytest.approx(80.0)

    def test_log_comparison_with_indicators(self, tmp_path: Path):
        """_log_comparison should accept and log indicator dicts."""
        from plp2gtopt.plp2gtopt import _log_comparison  # noqa: PLC0415

        plp_counts = {"buses": 1, "centrals": 2}
        gtopt_counts = {"buses": 1, "generators": 1}
        plp_ind = {
            "total_gen_capacity_mw": 100.0,
            "first_block_demand_mw": 80.0,
            "last_block_demand_mw": 80.0,
            "total_energy_mwh": 80.0,
            "first_block_affluent_avg": 0.0,
            "last_block_affluent_avg": 0.0,
        }
        gtopt_ind = {
            "total_gen_capacity_mw": 100.0,
            "first_block_demand_mw": 80.0,
            "last_block_demand_mw": 80.0,
            "total_energy_mwh": 80.0,
            "first_block_affluent_avg": 0.0,
            "last_block_affluent_avg": 0.0,
        }
        # Should not raise
        _log_comparison(plp_counts, gtopt_counts, plp_ind, gtopt_ind)

    def test_log_comparison_without_indicators(self):
        """_log_comparison should work without indicator dicts (backward compat)."""
        from plp2gtopt.plp2gtopt import _log_comparison  # noqa: PLC0415

        plp_counts = {"buses": 1}
        gtopt_counts = {"buses": 1}
        # Should not raise
        _log_comparison(plp_counts, gtopt_counts)


# ---------------------------------------------------------------------------
# validate_plp_case()
# ---------------------------------------------------------------------------


def test_validate_plp_case_success(tmp_path):
    """validate_plp_case returns True for a valid PLP case."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "validate_ok")
    assert validate_plp_case(opts) is True


def test_validate_plp_case_missing_input_dir(tmp_path):
    """validate_plp_case returns False when input_dir does not exist."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "validate_nodir")
    opts["input_dir"] = tmp_path / "nonexistent"
    assert validate_plp_case(opts) is False


def test_validate_plp_case_parse_error(tmp_path):
    """validate_plp_case returns False when parsing raises an error."""
    empty_dir = tmp_path / "empty_case"
    empty_dir.mkdir()
    opts = _make_opts(empty_dir, tmp_path, "validate_err")
    opts["input_dir"] = empty_dir
    assert validate_plp_case(opts) is False


# ---------------------------------------------------------------------------
# convert_plp_case() — error wrapping paths
# ---------------------------------------------------------------------------


def test_convert_plp_case_file_not_found_wraps(tmp_path):
    """convert_plp_case wraps FileNotFoundError with hint message."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "fnf_wrap")
    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls:
        mock_parser = MagicMock()
        mock_parser.parse_all.side_effect = FileNotFoundError("plpdem.dat")
        mock_cls.return_value = mock_parser
        with pytest.raises(RuntimeError, match="Required file not found"):
            convert_plp_case(opts)


def test_convert_plp_case_value_error_wraps(tmp_path):
    """convert_plp_case wraps ValueError with hint message."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "val_wrap")
    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls:
        mock_parser = MagicMock()
        mock_parser.parse_all.side_effect = ValueError("bad format")
        mock_cls.return_value = mock_parser
        with pytest.raises(RuntimeError, match="Invalid data format"):
            convert_plp_case(opts)


def test_convert_plp_case_generic_exception_wraps(tmp_path):
    """convert_plp_case wraps generic Exception as RuntimeError."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "gen_wrap")
    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls:
        mock_parser = MagicMock()
        mock_parser.parse_all.side_effect = TypeError("unexpected")
        mock_cls.return_value = mock_parser
        with pytest.raises(RuntimeError, match="PLP to GTOPT conversion failed"):
            convert_plp_case(opts)


def test_convert_plp_case_runtime_error_passthrough(tmp_path):
    """convert_plp_case re-raises RuntimeError without wrapping."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "rt_pass")
    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls:
        mock_parser = MagicMock()
        mock_parser.parse_all.side_effect = RuntimeError("original error")
        mock_cls.return_value = mock_parser
        with pytest.raises(RuntimeError, match="original error"):
            convert_plp_case(opts)


# ---------------------------------------------------------------------------
# run_post_check() — error handling paths
# ---------------------------------------------------------------------------


def test_run_post_check_import_error(tmp_path):
    """run_post_check returns gracefully when gtopt_check_json is not importable."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "postchk_imp")
    parser = PLPParser(opts)
    parser.parse_all()
    writer = GTOptWriter(parser)
    planning = writer.to_json(opts)

    with patch.dict("sys.modules", {"gtopt_check_json._checks": None}):
        # Should not raise; just returns
        run_post_check(planning, parser, output_dir=opts["output_dir"])


def test_run_post_check_with_findings(tmp_path):
    """run_post_check prints findings when checks return issues."""
    from gtopt_check_json._checks import Severity  # noqa: PLC0415

    opts = _make_opts(_PLPMin1Bus, tmp_path, "postchk_find")
    parser = PLPParser(opts)
    parser.parse_all()
    writer = GTOptWriter(parser)
    planning = writer.to_json(opts)

    # Create mock findings with real Severity values
    finding_crit = MagicMock()
    finding_crit.severity = Severity.CRITICAL
    finding_crit.check_id = "CHK001"
    finding_crit.message = "Critical issue"

    finding_warn = MagicMock()
    finding_warn.severity = Severity.WARNING
    finding_warn.check_id = "CHK002"
    finding_warn.message = "Warning issue"

    finding_note = MagicMock()
    finding_note.severity = Severity.NOTE
    finding_note.check_id = "CHK003"
    finding_note.message = "A note"

    with patch(
        "gtopt_check_json._checks.run_all_checks",
        return_value=[finding_crit, finding_warn, finding_note],
    ), patch(
        "gtopt_check_json._terminal.print_finding",
    ), patch(
        "gtopt_check_json._terminal.print_status",
    ), patch(
        "gtopt_check_json._terminal.print_summary",
    ) as mock_summary:
        run_post_check(planning, parser, output_dir=opts["output_dir"])
        mock_summary.assert_called_once_with(1, 1, 1)


# ---------------------------------------------------------------------------
# create_zip_output() — additional edge cases
# ---------------------------------------------------------------------------


def test_create_zip_output_empty_dir(tmp_path):
    """create_zip_output works when output_dir has no data files."""
    output_dir = tmp_path / "emptycase"
    output_dir.mkdir()

    output_file = tmp_path / "emptycase.json"
    output_file.write_text('{"options":{}}')

    zip_path = tmp_path / "emptycase.zip"
    create_zip_output(output_file, output_dir, zip_path)

    assert zip_path.exists()
    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

    # Only the JSON file should be in the archive
    assert names == ["emptycase.json"]


def test_create_zip_output_nested_subdirs(tmp_path):
    """create_zip_output preserves deeply nested subdirectory structure."""
    output_dir = tmp_path / "deep"
    output_dir.mkdir()
    (output_dir / "a" / "b").mkdir(parents=True)
    (output_dir / "a" / "b" / "data.parquet").write_bytes(b"nested")

    output_file = tmp_path / "deep.json"
    output_file.write_text("{}")

    zip_path = tmp_path / "deep.zip"
    create_zip_output(output_file, output_dir, zip_path)

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

    assert "deep/a/b/data.parquet" in names


# ---------------------------------------------------------------------------
# generate_variable_scales_template() / print_variable_scales_template()
# ---------------------------------------------------------------------------


def test_generate_variable_scales_template_missing_input_dir(tmp_path):
    """generate_variable_scales_template raises RuntimeError for missing dir."""
    opts = {"input_dir": str(tmp_path / "nonexistent")}
    with pytest.raises(RuntimeError, match="Input directory does not exist"):
        generate_variable_scales_template(opts)


def test_generate_variable_scales_template_basic(tmp_path):
    """generate_variable_scales_template returns valid JSON for a simple case."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "vscale")
    result = generate_variable_scales_template(opts)
    parsed = json.loads(result)
    assert isinstance(parsed, list)


def test_generate_variable_scales_template_with_reservoirs(tmp_path):
    """generate_variable_scales_template includes reservoir entries with scale."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "vscale_rsv")

    # Mock a parser that has reservoir data
    mock_planning = {
        "system": {
            "reservoir_array": [
                {"name": "Rsv1", "uid": 1},
                {"name": "Rsv2", "uid": 2},
            ],
            "battery_array": [
                {"name": "Bat1", "uid": 10},
            ],
        },
    }
    mock_planos = MagicMock()
    mock_planos.reservoir_fescala = {"Rsv1": 3}  # scale = 10^(3-6) = 0.001

    mock_central_parser = MagicMock()
    mock_central = {"name": "Rsv2", "type": "embalse", "energy_scale": 0.5}
    mock_central_parser.centrals = [mock_central]

    mock_parser = MagicMock()
    mock_parser.parsed_data = {
        "planos_parser": mock_planos,
        "central_parser": mock_central_parser,
    }

    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls, patch(
        "plp2gtopt.plp2gtopt.GTOptWriter"
    ) as mock_writer_cls:
        mock_cls.return_value = mock_parser
        mock_writer_inst = MagicMock()
        mock_writer_inst.to_json.return_value = mock_planning
        mock_writer_cls.return_value = mock_writer_inst

        result = generate_variable_scales_template(opts)

    parsed = json.loads(result)
    # 2 reservoirs × 2 (volume + flow) + 1 battery = 5
    assert len(parsed) == 5

    # Rsv1: fescala=3 -> scale = 10^(3-6) = 0.001
    rsv1_energy = [
        e for e in parsed if e["name"] == "Rsv1" and e["variable"] == "energy"
    ][0]
    assert rsv1_energy["class_name"] == "Reservoir"
    assert rsv1_energy["scale"] == pytest.approx(0.001)
    assert rsv1_energy["_fescala"] == 3

    # Rsv1 flow entry uses the same scale
    rsv1_flow = [e for e in parsed if e["name"] == "Rsv1" and e["variable"] == "flow"][
        0
    ]
    assert rsv1_flow["scale"] == pytest.approx(0.001)

    # Rsv2: fallback to central energy_scale = 0.5
    rsv2_energy = [
        e for e in parsed if e["name"] == "Rsv2" and e["variable"] == "energy"
    ][0]
    assert rsv2_energy["scale"] == pytest.approx(0.5)
    assert "_fescala" not in rsv2_energy

    # Bat1: default battery scale = 0.01
    bat1 = [e for e in parsed if e["name"] == "Bat1"][0]
    assert bat1["class_name"] == "Battery"
    assert bat1["variable"] == "energy"
    assert bat1["scale"] == pytest.approx(0.01)


def test_generate_variable_scales_template_no_fescala(tmp_path):
    """generate_variable_scales_template uses default scale=1.0 when no source."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "vscale_def")

    mock_planning = {
        "system": {
            "reservoir_array": [{"name": "RsvNoScale", "uid": 99}],
            "battery_array": [],
        },
    }
    mock_parser = MagicMock()
    mock_parser.parsed_data = {
        "planos_parser": None,
        "central_parser": None,
    }

    with patch("plp2gtopt.plp2gtopt.PLPParser") as mock_cls, patch(
        "plp2gtopt.plp2gtopt.GTOptWriter"
    ) as mock_writer_cls:
        mock_cls.return_value = mock_parser
        mock_writer_inst = MagicMock()
        mock_writer_inst.to_json.return_value = mock_planning
        mock_writer_cls.return_value = mock_writer_inst

        result = generate_variable_scales_template(opts)

    parsed = json.loads(result)
    # 1 reservoir × 2 (volume + flow) = 2 entries
    assert len(parsed) == 2
    vol_entry = [e for e in parsed if e["variable"] == "energy"][0]
    assert vol_entry["scale"] == pytest.approx(1.0)
    assert "_fescala" not in vol_entry
    flow_entry = [e for e in parsed if e["variable"] == "flow"][0]
    assert flow_entry["scale"] == pytest.approx(1.0)


def test_print_variable_scales_template_success(tmp_path, capsys):
    """print_variable_scales_template prints JSON and returns 0 on success."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "pvscale_ok")

    with patch(
        "plp2gtopt.plp2gtopt.generate_variable_scales_template",
        return_value='[{"class_name": "Reservoir"}]',
    ):
        ret = print_variable_scales_template(opts)

    assert ret == 0
    captured = capsys.readouterr()
    assert "Reservoir" in captured.out


def test_print_variable_scales_template_error(tmp_path, capsys):
    """print_variable_scales_template prints error to stderr and returns 1."""
    opts = {"input_dir": str(tmp_path / "nonexistent")}
    ret = print_variable_scales_template(opts)
    assert ret == 1
    captured = capsys.readouterr()
    assert "error:" in captured.err


# ---------------------------------------------------------------------------
# _log_stats() — edge case: all element counts zero
# ---------------------------------------------------------------------------


def test_log_stats_all_zero_elements():
    """_log_stats handles planning with all zero element counts (line 93)."""
    from plp2gtopt.plp2gtopt import _log_stats  # noqa: PLC0415

    planning = {
        "system": {"name": "empty", "version": "1.0"},
        "simulation": {"block_array": [], "stage_array": [], "scenario_array": []},
        "options": {},
    }
    # Should not raise; exercises the fallback when no elements > 0
    _log_stats(planning, 0.0)
