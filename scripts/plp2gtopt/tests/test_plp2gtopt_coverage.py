"""Tests for plp2gtopt.py — show_simulation_summary, error paths, cleanup."""

from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt.plp2gtopt import (
    convert_plp_case,
    show_simulation_summary,
    validate_plp_case,
)


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
# show_simulation_summary — exercises lines 162-275
# ---------------------------------------------------------------------------


class TestShowSimulationSummary:
    """Tests for show_simulation_summary()."""

    @staticmethod
    def _mock_terminal():
        """Create mocks for gtopt_check_json._terminal functions."""
        return patch.dict(
            "sys.modules",
            {
                "gtopt_check_json": MagicMock(),
                "gtopt_check_json._terminal": MagicMock(),
            },
        )

    def test_basic_summary(self):
        """show_simulation_summary runs without error on a basic planning."""
        planning = {
            "simulation": {
                "scenario_array": [
                    {"uid": 1, "hydrology": 0, "probability_factor": 1.0},
                ],
                "stage_array": [
                    {"uid": 1, "first_block": 0},
                ],
                "phase_array": [
                    {"uid": 1, "count_stage": 1},
                ],
                "scene_array": [],
                "block_array": [],
            },
            "options": {"sddp_options": {}},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_with_scenes(self):
        """show_simulation_summary handles scene_array."""
        planning = {
            "simulation": {
                "scenario_array": [
                    {"uid": 1, "hydrology": 0, "probability_factor": 0.5},
                    {"uid": 2, "hydrology": 1, "probability_factor": 0.5},
                ],
                "stage_array": [
                    {"uid": 1, "first_block": 0},
                ],
                "phase_array": [],
                "scene_array": [
                    {
                        "uid": 1,
                        "first_scenario": 0,
                        "count_scenario": 2,
                    },
                ],
                "block_array": [],
            },
            "options": {"sddp_options": {}},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_with_many_scenarios(self):
        """show_simulation_summary truncates to 10 scenarios."""
        scenarios = [
            {"uid": i, "hydrology": i - 1, "probability_factor": 1 / 15}
            for i in range(1, 16)
        ]
        planning = {
            "simulation": {
                "scenario_array": scenarios,
                "stage_array": [{"uid": 1, "first_block": 0}],
                "phase_array": [],
                "scene_array": [],
                "block_array": [],
            },
            "options": {"sddp_options": {}},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_with_apertures(self):
        """show_simulation_summary handles aperture_array with valid/missing."""
        planning = {
            "simulation": {
                "scenario_array": [
                    {"uid": 1, "hydrology": 0, "probability_factor": 1.0},
                ],
                "stage_array": [{"uid": 1, "first_block": 0}],
                "phase_array": [
                    {"uid": 1, "count_stage": 1, "apertures": [1, 2]},
                ],
                "aperture_array": [
                    {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
                    {"uid": 2, "source_scenario": 99, "probability_factor": 0.5},
                ],
                "scene_array": [],
                "block_array": [],
            },
            "options": {"sddp_options": {"aperture_directory": "/data/ap"}},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_with_sddp_options(self):
        """show_simulation_summary displays sddp options."""
        planning = {
            "simulation": {
                "scenario_array": [],
                "stage_array": [],
                "phase_array": [],
                "scene_array": [],
                "block_array": [],
            },
            "options": {
                "sddp_options": {
                    "max_iterations": 50,
                    "convergence_tol": 0.01,
                },
            },
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_with_many_scenes(self):
        """show_simulation_summary truncates scenes to 5."""
        scenes = [
            {"uid": i, "first_scenario": i, "count_scenario": 1} for i in range(1, 8)
        ]
        planning = {
            "simulation": {
                "scenario_array": [],
                "stage_array": [],
                "phase_array": [],
                "scene_array": scenes,
                "block_array": [],
            },
            "options": {"sddp_options": {}},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)

    def test_summary_empty_planning(self):
        """show_simulation_summary handles empty planning gracefully."""
        planning = {
            "simulation": {},
            "options": {},
        }
        with self._mock_terminal():
            show_simulation_summary(planning)


# ---------------------------------------------------------------------------
# convert_plp_case — show_simulation path (line 616)
# ---------------------------------------------------------------------------


def test_convert_show_simulation(tmp_path):
    """convert_plp_case calls show_simulation_summary when option is set."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "show_sim")
    opts["show_simulation"] = True
    opts["run_check"] = False
    # This exercises line 616
    convert_plp_case(opts)
    assert opts["output_file"].exists()


# ---------------------------------------------------------------------------
# convert_plp_case — error wrapping paths (lines 628-648)
# ---------------------------------------------------------------------------


def test_convert_file_not_found_wrapping(tmp_path):
    """convert_plp_case wraps FileNotFoundError in RuntimeError with hint."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "fnf_wrap")
    opts["run_check"] = False
    with patch(
        "plp2gtopt.plp2gtopt.PLPParser",
        side_effect=FileNotFoundError("plpbar.dat"),
    ):
        with pytest.raises(RuntimeError, match="Required file not found"):
            convert_plp_case(opts)


def test_convert_value_error_wrapping(tmp_path):
    """convert_plp_case wraps ValueError in RuntimeError with hint."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "val_wrap")
    opts["run_check"] = False
    with patch(
        "plp2gtopt.plp2gtopt.PLPParser",
        side_effect=ValueError("bad format"),
    ):
        with pytest.raises(RuntimeError, match="Invalid data format"):
            convert_plp_case(opts)


def test_convert_generic_error_wrapping(tmp_path):
    """convert_plp_case wraps generic exceptions in RuntimeError."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "gen_wrap")
    opts["run_check"] = False
    with patch(
        "plp2gtopt.plp2gtopt.PLPParser",
        side_effect=TypeError("unexpected"),
    ):
        with pytest.raises(RuntimeError, match="PLP to GTOPT conversion failed"):
            convert_plp_case(opts)


def test_convert_cleanup_on_failure(tmp_path):
    """convert_plp_case cleans up temporary directory on failure."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "cleanup")
    opts["run_check"] = False

    # Mock to fail after temp dir creation
    with patch("plp2gtopt.plp2gtopt.GTOptWriter") as mock_writer:
        mock_writer.side_effect = RuntimeError("writer crash")
        with pytest.raises(RuntimeError):
            convert_plp_case(opts)

    # Temp dir should be cleaned up
    tmp_dirs = list(tmp_path.glob(".cleanup.tmp.*"))
    assert len(tmp_dirs) == 0


# ---------------------------------------------------------------------------
# validate_plp_case — error paths
# ---------------------------------------------------------------------------


def test_validate_missing_input_dir(tmp_path):
    """validate_plp_case returns False for nonexistent directory."""
    result = validate_plp_case({"input_dir": str(tmp_path / "nonexistent")})
    assert result is False


def test_validate_success(tmp_path):
    """validate_plp_case returns True for valid input."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "validate_ok")
    result = validate_plp_case(opts)
    assert result is True


def test_validate_parser_error(tmp_path):
    """validate_plp_case returns False when parser raises."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "validate_err")
    with patch(
        "plp2gtopt.plp2gtopt.PLPParser",
        side_effect=ValueError("parse error"),
    ):
        result = validate_plp_case(opts)
    assert result is False
