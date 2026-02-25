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
from plp2gtopt.main import build_options, main, make_parser
from plp2gtopt.plp2gtopt import convert_plp_case, create_zip_output
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
        "hydrologies": "0",
        "last_stage": -1,
        "last_time": -1,
        "compression": "gzip",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
    }


# ---------------------------------------------------------------------------
# convert_plp_case() — happy path
# ---------------------------------------------------------------------------


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
        assert name.startswith(
            f"{input_dir_name}/"
        ), f"Data file not under input_dir prefix: {name}"


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
    ) as mock_jw, patch("plp2gtopt.gtopt_writer.BessWriter") as mock_bess:
        # All writers return empty arrays
        for m in [mock_bw, mock_sw, mock_busw, mock_cw, mock_lw, mock_dw]:
            m.return_value.to_json_array.return_value = []
        mock_gpw.return_value.to_json_array.return_value = []
        mock_aw.return_value.to_parquet.return_value = None
        mock_jw.return_value.to_json_array.return_value = []
        mock_bess.return_value.process.return_value = {
            "battery_array": [],
            "converter_array": [],
            "generator_array": [],
            "demand_array": [],
        }

        opts = {
            "output_dir": str(tmp_path),
            "output_file": str(tmp_path / "out.json"),
            "hydrologies": "0",
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
    assert opts["output_file"] == Path("output.json")
    assert opts["last_stage"] == -1
    assert opts["last_time"] == -1
    assert opts["compression"] == "gzip"
    assert opts["hydrologies"] == "0"
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
    assert opts["output_file"] == Path("gtopt_case_2y.json")


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
        "-i", str(_PLPMin1Bus),
        "-o", str(out_dir),
        "-f", str(out_file),
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
