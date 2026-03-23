"""Tests for gtopt_writer.py module."""

import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt.central_parser import CentralParser
from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.simulation_writer import SimulationWriter

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"
_PLPMinBess = _CASES_DIR / "plp_min_bess"


def _make_opts(tmp_path: Path, case_name: str = "test") -> dict:
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
        "discount_rate": 0.0,
        "last_stage": -1,
        "compression": "zstd",
    }


class TestGTOptWriterWithRealParser:
    """Integration-style tests using the real PLPParser + GTOptWriter pipeline."""

    def test_to_json_structure(self, tmp_path):
        """GTOptWriter.to_json() produces the three top-level keys."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        assert set(result.keys()) >= {"options", "system", "simulation"}

    def test_to_json_simulation_has_required_arrays(self, tmp_path):
        """simulation block must contain stage_array, block_array, phase_array, scene_array."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        sim = result["simulation"]
        assert "stage_array" in sim
        assert "block_array" in sim
        assert len(sim["stage_array"]) > 0
        assert len(sim["block_array"]) > 0
        # phase_array: one phase per stage (sddp default)
        assert "phase_array" in sim
        assert len(sim["phase_array"]) == len(sim["stage_array"])
        for i, phase in enumerate(sim["phase_array"]):
            assert phase["first_stage"] == i
            assert phase["count_stage"] == 1
        # scene_array: one scene per scenario (sddp default)
        assert "scene_array" in sim
        assert len(sim["scene_array"]) == len(sim["scenario_array"])

    def test_to_json_simulation_monolithic_single_phase_scene(self, tmp_path):
        """Monolithic solver: single phase covering all stages, single scene with all scenarios."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["hydrologies"] = "1,2,3"
        opts["solver_type"] = "mono"
        result = writer.to_json(opts)
        sim = result["simulation"]

        num_stages = len(sim["stage_array"])
        assert num_stages > 0

        # Monolithic: exactly one phase spanning all stages
        assert len(sim["phase_array"]) == 1
        assert sim["phase_array"][0]["first_stage"] == 0
        assert sim["phase_array"][0]["count_stage"] == num_stages

        # Monolithic: exactly one scene containing all 3 scenarios
        assert len(sim["scene_array"]) == 1
        assert sim["scene_array"][0]["first_scenario"] == 0
        assert sim["scene_array"][0]["count_scenario"] == 3

    def test_to_json_system_has_generators(self, tmp_path):
        """system block must contain a non-empty generator_array."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        assert "generator_array" in result["system"]
        assert len(result["system"]["generator_array"]) > 0

    def test_write_creates_valid_json_file(self, tmp_path):
        """write() creates a JSON file that can be re-parsed."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.write(opts)
        out_file = Path(opts["output_file"])
        assert out_file.exists()
        data = json.loads(out_file.read_text(encoding="utf-8"))
        assert isinstance(data, dict)

    def test_to_json_options_block(self, tmp_path):
        """options block includes expected keys including sddp_options."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        opts = result["options"]
        assert "input_directory" in opts
        assert "output_directory" in opts
        assert "demand_fail_cost" in opts
        # Default solver type is sddp (top-level field)
        assert opts["solver_type"] == "sddp"

    def test_to_json_options_monolithic_solver(self, tmp_path):
        """options block contains solver_type=monolithic when requested."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["solver_type"] = "mono"
        result = writer.to_json(opts)
        assert result["options"]["solver_type"] == "monolithic"


class TestGTOptWriterProcessMethods:
    """Unit tests for individual process_* methods."""

    def test_process_options_default_discount(self):
        """process_options sets annual_discount_rate=0 when not provided."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["annual_discount_rate"] == 0.0

    def test_process_options_default_solver_type(self):
        """process_options defaults to solver_type='sddp' at top level."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["solver_type"] == "sddp"

    def test_process_options_monolithic_solver_type(self):
        """process_options normalizes 'mono' to 'monolithic' in JSON output."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "solver_type": "mono"})
        assert writer.planning["options"]["solver_type"] == "monolithic"

    def test_process_options_num_apertures(self):
        """process_options writes num_apertures inside sddp_options."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "num_apertures": "5"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["num_apertures"] == 5

    def test_process_options_num_apertures_all(self):
        """process_options with 'all'/-1 does not set num_apertures (auto-detect)."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "num_apertures": "all"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp

        writer2 = GTOptWriter(MagicMock())
        writer2.process_options({"output_dir": "out", "num_apertures": "-1"})
        sddp2 = writer2.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp2

    def test_process_options_no_apertures_by_default(self):
        """num_apertures is absent when num_apertures not supplied."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp

    @staticmethod
    def _make_plpmat_parser(max_iterations=0, pd_error=0.0, default_cuts=0):
        """Build a mock parser with a plpmat_parser in parsed_data."""
        from types import SimpleNamespace

        mock_parser = MagicMock()
        mock_parser.parsed_data = {
            "plpmat_parser": SimpleNamespace(
                max_iterations=max_iterations,
                pd_error=pd_error,
                default_cuts=default_cuts,
            )
        }
        return mock_parser

    def test_process_options_max_iterations_from_plpmat(self):
        """max_iterations defaults to PDMaxIte from plpmat.dat when > 1."""
        mock_parser = self._make_plpmat_parser(max_iterations=150)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["max_iterations"] == 150

    def test_process_options_max_iterations_plpmat_one_ignored(self):
        """PDMaxIte=1 means monolithic in PLP, should not set max_iterations."""
        mock_parser = self._make_plpmat_parser(max_iterations=1)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "max_iterations" not in sddp

    def test_process_options_max_iterations_explicit_overrides_plpmat(self):
        """Explicit max_iterations overrides plpmat.dat value."""
        mock_parser = self._make_plpmat_parser(max_iterations=150)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out", "max_iterations": 300})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["max_iterations"] == 300

    def test_process_options_max_iterations_no_plpmat(self):
        """Without plpmat.dat, max_iterations is absent when not specified."""
        mock_parser = MagicMock()
        mock_parser.parsed_data = {}
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "max_iterations" not in sddp

    def test_process_options_demand_fail_cost_default(self):
        """demand_fail_cost defaults to 1000 when not specified."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["demand_fail_cost"] == 1000

    def test_process_options_with_discount(self):
        """process_options passes discount_rate through."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "discount_rate": 0.08})
        assert writer.planning["options"]["annual_discount_rate"] == pytest.approx(0.08)

    # ---- SDDP (default) scenario/scene tests --------------------------------

    def test_process_scenarios_single_hydrology(self):
        """Single hydrology → probability_factor = 1.0, uid = Fortran index."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "2", "probability_factors": None})
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 1
        assert scenarios[0]["probability_factor"] == pytest.approx(1.0)
        assert scenarios[0]["hydrology"] == 1
        # UID = Fortran 1-based hydrology index (PLP convention)
        assert scenarios[0]["uid"] == 2

        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 1
        assert scenes[0]["uid"] == 1
        assert scenes[0]["first_scenario"] == 0
        assert scenes[0]["count_scenario"] == 1

    def test_process_scenarios_two_hydrologies_equal(self):
        """Two hydrologies with no explicit weights → 0.5 each, unique UIDs, 2 scenes."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "1,3", "probability_factors": None})
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 2
        for s in scenarios:
            assert s["probability_factor"] == pytest.approx(0.5)
        # UIDs = Fortran hydrology indices
        assert scenarios[0]["uid"] == 1
        assert scenarios[1]["uid"] == 3

        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 2
        assert scenes[0]["first_scenario"] == 0
        assert scenes[1]["first_scenario"] == 1
        assert scenes[0]["count_scenario"] == 1
        assert scenes[1]["count_scenario"] == 1

    def test_process_scenarios_explicit_weights(self):
        """Explicit probability_factors are parsed as floats, 3 unique UIDs, 3 scenes."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios(
            {"hydrologies": "1,2,3", "probability_factors": "0.2,0.5,0.3"}
        )
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 3
        assert scenarios[0]["probability_factor"] == pytest.approx(0.2)
        assert scenarios[2]["probability_factor"] == pytest.approx(0.3)
        # Each scenario has a distinct UID
        uids = [s["uid"] for s in scenarios]
        assert len(uids) == len(set(uids)), "scenario UIDs must be unique"
        assert uids == sorted(uids), "scenario UIDs must be sorted"

        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 3
        for i, scene in enumerate(scenes):
            assert scene["first_scenario"] == i
            assert scene["count_scenario"] == 1

    # ---- Monolithic scenario/scene tests ------------------------------------

    def test_process_scenarios_monolithic_two_hydrologies(self):
        """Monolithic solver: 2 scenarios → 1 scene containing both."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "1,2", "solver_type": "monolithic"})
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 2
        assert scenarios[0]["probability_factor"] == pytest.approx(0.5)
        assert scenarios[1]["probability_factor"] == pytest.approx(0.5)

        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 1
        assert scenes[0]["uid"] == 1
        assert scenes[0]["first_scenario"] == 0
        assert scenes[0]["count_scenario"] == 2

    def test_process_scenarios_mono_alias(self):
        """'mono' is accepted as an alias for 'monolithic'."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "1,2,3", "solver_type": "mono"})
        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 1
        assert scenes[0]["count_scenario"] == 3

    def test_process_buses_empty(self):
        """process_buses handles missing bus_parser gracefully."""
        mock_parser = MagicMock()
        mock_parser.parsed_data = {"bus_parser": []}
        writer = GTOptWriter(mock_parser)
        writer.process_buses()  # should not raise
        assert "bus_array" not in writer.planning["system"]

    def test_process_demands_empty(self):
        """process_demands handles missing demand_parser gracefully."""
        mock_parser = MagicMock()
        mock_parser.parsed_data = {}
        writer = GTOptWriter(mock_parser)
        writer.process_demands({})  # should not raise

    def test_process_junctions_empty(self):
        """process_junctions is a no-op when JunctionWriter returns []."""
        mock_parser = MagicMock()
        mock_parser.parsed_data = {}
        writer = GTOptWriter(mock_parser)
        with patch("plp2gtopt.gtopt_writer.JunctionWriter") as mock_jw:
            mock_jw.return_value.to_json_array.return_value = []
            writer.process_junctions({})  # should not raise

    def test_process_battery_no_storage(self):
        """process_battery is a no-op when no battery data present."""
        mock_central = MagicMock(spec=CentralParser)
        mock_central.centrals = []  # no batteries

        mock_parser = MagicMock()
        mock_parser.parsed_data = {
            "battery_parser": None,
            "central_parser": mock_central,
        }
        writer = GTOptWriter(mock_parser)
        writer.process_battery({})  # should not raise, and not add any arrays
        assert "battery_array" not in writer.planning["system"]


class TestSimulationWriter:
    """Unit tests for the standalone SimulationWriter."""

    _CASES_DIR = Path(__file__).parent.parent.parent / "cases"

    def test_build_returns_required_keys(self, tmp_path):
        parser = PLPParser({"input_dir": self._CASES_DIR / "plp_min_1bus"})
        parser.parse_all()
        opts = {"output_dir": tmp_path, "hydrologies": "1", "solver_type": "sddp"}
        sim = SimulationWriter(parser.parsed_data, opts).build()
        for key in (
            "block_array",
            "stage_array",
            "phase_array",
            "scenario_array",
            "scene_array",
        ):
            assert key in sim

    def test_sddp_one_phase_per_stage(self, tmp_path):
        parser = PLPParser({"input_dir": self._CASES_DIR / "plp_min_1bus"})
        parser.parse_all()
        opts = {"output_dir": tmp_path, "hydrologies": "1", "solver_type": "sddp"}
        sim = SimulationWriter(parser.parsed_data, opts).build()
        assert len(sim["phase_array"]) == len(sim["stage_array"])

    def test_monolithic_one_phase(self, tmp_path):
        parser = PLPParser({"input_dir": self._CASES_DIR / "plp_min_1bus"})
        parser.parse_all()
        opts = {
            "output_dir": tmp_path,
            "hydrologies": "1",
            "solver_type": "monolithic",
        }
        sim = SimulationWriter(parser.parsed_data, opts).build()
        assert len(sim["phase_array"]) == 1
        assert sim["phase_array"][0]["count_stage"] == len(sim["stage_array"])

    def test_stages_phase_spec(self, tmp_path):
        parser = PLPParser({"input_dir": self._CASES_DIR / "plp_min_1bus"})
        parser.parse_all()
        # plp_min_1bus has 1 stage
        opts = {"output_dir": tmp_path, "hydrologies": "1", "stages_phase": "1"}
        sim = SimulationWriter(parser.parsed_data, opts).build()
        assert len(sim["phase_array"]) == 1
        assert sim["phase_array"][0]["first_stage"] == 0


class TestLoadVariableScalesFile:
    """Tests for GTOptWriter._load_variable_scales_file."""

    def test_valid_file(self, tmp_path):
        f = tmp_path / "scales.json"
        data = [
            {"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": 0.01}
        ]
        f.write_text(json.dumps(data))
        result = GTOptWriter._load_variable_scales_file(f)
        assert len(result) == 1
        assert result[0]["scale"] == 0.01

    def test_empty_array(self, tmp_path):
        f = tmp_path / "scales.json"
        f.write_text("[]")
        result = GTOptWriter._load_variable_scales_file(f)
        assert not result

    def test_not_an_array(self, tmp_path):
        f = tmp_path / "scales.json"
        f.write_text('{"key": "value"}')
        result = GTOptWriter._load_variable_scales_file(f)
        assert not result

    def test_invalid_entry_skipped(self, tmp_path):
        f = tmp_path / "scales.json"
        data = [
            {"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": 0.01},
            {"bad": "entry"},
            "not a dict",
        ]
        f.write_text(json.dumps(data))
        result = GTOptWriter._load_variable_scales_file(f)
        assert len(result) == 1

    def test_missing_file(self, tmp_path):
        result = GTOptWriter._load_variable_scales_file(tmp_path / "missing.json")
        assert not result

    def test_corrupt_json(self, tmp_path):
        f = tmp_path / "scales.json"
        f.write_text("{ not json")
        result = GTOptWriter._load_variable_scales_file(f)
        assert not result


class TestBuildStageToPhaseMap:
    """Tests for GTOptWriter._build_stage_to_phase_map."""

    def test_returns_none_when_empty(self, tmp_path):
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.planning["stage_array"] = []
        result = writer._build_stage_to_phase_map()
        assert result is None

    def test_returns_mapping(self, tmp_path):
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.planning["stage_array"] = [
            {"uid": 1, "phase_uid": 1},
            {"uid": 2, "phase_uid": 1},
            {"uid": 3, "phase_uid": 2},
        ]
        result = writer._build_stage_to_phase_map()
        assert result == {1: 1, 2: 1, 3: 2}


class TestProcessVariableScales:
    """Tests for GTOptWriter.process_variable_scales."""

    def test_reservoir_auto_scale(self, tmp_path):
        """auto_rsv_energy_scale adds reservoir energy+flow scale entries."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)

        # Inject a reservoir to test scaling
        writer.planning["system"]["reservoir_array"] = [{"uid": 1, "name": "rsv1"}]
        writer.planning["options"] = {}

        # Mock central_parser with energy_scale
        cp = MagicMock()
        cp.centrals = [{"name": "rsv1", "type": "embalse", "energy_scale": 0.001}]
        writer.parser.parsed_data["central_parser"] = cp

        writer.process_variable_scales({**opts, "auto_rsv_energy_scale": True})
        scales = writer.planning["options"].get("variable_scales", [])
        rsv_scales = [s for s in scales if s["class_name"] == "Reservoir"]
        assert len(rsv_scales) == 2  # energy + flow
        assert rsv_scales[0]["variable"] == "energy"
        assert rsv_scales[1]["variable"] == "flow"

    def test_battery_auto_scale(self, tmp_path):
        """auto_bat_energy_scale adds battery scale entries."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)

        writer.planning["system"]["battery_array"] = [{"uid": 1, "name": "bat1"}]
        writer.planning["options"] = {}

        writer.process_variable_scales({**opts, "auto_bat_energy_scale": True})
        scales = writer.planning["options"].get("variable_scales", [])
        bat_scales = [s for s in scales if s["class_name"] == "Battery"]
        assert len(bat_scales) == 1
        assert bat_scales[0]["scale"] == 0.01

    def test_file_scales_merged(self, tmp_path):
        """File-based scales are loaded and merged."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)
        writer.planning["options"] = {}

        scales_file = tmp_path / "scales.json"
        scales_file.write_text(
            json.dumps(
                [
                    {
                        "class_name": "Generator",
                        "variable": "power",
                        "uid": 1,
                        "scale": 0.5,
                    }
                ]
            )
        )
        writer.process_variable_scales(
            {**opts, "variable_scales_file": str(scales_file)}
        )
        scales = writer.planning["options"].get("variable_scales", [])
        assert any(s["class_name"] == "Generator" for s in scales)

    def test_no_scales_when_disabled(self, tmp_path):
        """No variable_scales when nothing is enabled."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)
        writer.planning["options"] = {}

        writer.process_variable_scales(opts)
        assert "variable_scales" not in writer.planning.get("options", {})
