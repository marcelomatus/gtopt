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
        opts["hydrologies"] = "1"
        opts["method"] = "mono"
        result = writer.to_json(opts)
        sim = result["simulation"]

        num_stages = len(sim["stage_array"])
        assert num_stages > 0

        # Monolithic: exactly one phase spanning all stages
        assert len(sim["phase_array"]) == 1
        assert sim["phase_array"][0]["first_stage"] == 0
        assert sim["phase_array"][0]["count_stage"] == num_stages

        # Monolithic: exactly one scene containing all scenarios
        assert len(sim["scene_array"]) == 1
        assert sim["scene_array"][0]["first_scenario"] == 0
        assert sim["scene_array"][0]["count_scenario"] == 1

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
        assert "demand_fail_cost" in opts.get("model_options", {})
        # Default method is sddp (top-level field)
        assert opts["method"] == "sddp"

    def test_to_json_options_monolithic_solver(self, tmp_path):
        """options block contains method=monolithic when requested."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["method"] = "mono"
        result = writer.to_json(opts)
        assert result["options"]["method"] == "monolithic"


class TestGTOptWriterProcessMethods:
    """Unit tests for individual process_* methods."""

    def test_process_options_default_discount(self):
        """process_options sets annual_discount_rate=0 on simulation."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["simulation"]["annual_discount_rate"] == 0.0

    def test_process_options_default_method(self):
        """process_options defaults to method='sddp' at top level."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["method"] == "sddp"

    def test_process_options_monolithic_method(self):
        """process_options normalizes 'mono' to 'monolithic' in JSON output."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "method": "mono"})
        assert writer.planning["options"]["method"] == "monolithic"

    def test_process_options_cascade_method(self):
        """process_options emits cascade_options with 3-level default config."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "method": "cascade"})
        opts = writer.planning["options"]
        assert opts["method"] == "cascade"
        assert "cascade_options" in opts
        cascade = opts["cascade_options"]
        assert "level_array" in cascade
        levels = cascade["level_array"]
        assert len(levels) == 3
        # Level 0: uninodal
        assert levels[0]["name"] == "uninodal"
        assert levels[0]["model_options"]["use_single_bus"] is True
        # Level 1: transport (no kirchhoff, no losses)
        assert levels[1]["name"] == "transport"
        assert levels[1]["model_options"]["use_single_bus"] is False
        assert levels[1]["model_options"]["use_kirchhoff"] is False
        assert levels[1]["model_options"]["use_line_losses"] is False
        # Level 2: full network
        assert levels[2]["name"] == "full_network"

    def test_process_options_cascade_iteration_split(self):
        """cascade uses full max_iterations for level 0; 1/4 for levels 1 and 2."""
        writer = GTOptWriter(MagicMock())
        writer.process_options(
            {
                "output_dir": "out",
                "method": "cascade",
                "max_iterations": 100,
            }
        )
        levels = writer.planning["options"]["cascade_options"]["level_array"]
        assert levels[0]["sddp_options"]["max_iterations"] == 100
        assert levels[1]["sddp_options"]["max_iterations"] == 25
        assert levels[2]["sddp_options"]["max_iterations"] == 25

    def test_process_options_cascade_global_budget_is_sum_of_levels(self):
        """Cascade-level max_iterations = sum of per-level budgets, not total_iter.

        Regression: previously the cascade-global budget equalled total_iter,
        so level 0 (which gets full max_iterations) alone exhausted it and
        levels 1-2 never ran.  The global budget must be >= the sum of
        per-level budgets for all levels to execute.
        """
        writer = GTOptWriter(MagicMock())
        writer.process_options(
            {
                "output_dir": "out",
                "method": "cascade",
                "max_iterations": 100,
            }
        )
        cascade = writer.planning["options"]["cascade_options"]
        levels = cascade["level_array"]
        per_level_sum = sum(level["sddp_options"]["max_iterations"] for level in levels)
        assert cascade["sddp_options"]["max_iterations"] == per_level_sum
        assert cascade["sddp_options"]["max_iterations"] == 150  # 100 + 25 + 25

    def test_process_options_cascade_small_budget_still_runs_all_levels(self):
        """With max_iterations=5 (small), cascade global budget must be 5+1+1=7.

        Regression: previously with PDMaxIte=5 from plpmat.dat, level 0
        consumed all 5 iters and levels 1-2 never ran.  The cascade global
        budget must now leave room for all three levels.
        """
        writer = GTOptWriter(MagicMock())
        writer.process_options(
            {
                "output_dir": "out",
                "method": "cascade",
                "max_iterations": 5,
            }
        )
        cascade = writer.planning["options"]["cascade_options"]
        levels = cascade["level_array"]
        assert levels[0]["sddp_options"]["max_iterations"] == 5
        assert levels[1]["sddp_options"]["max_iterations"] == 1  # max(5//4, 1)
        assert levels[2]["sddp_options"]["max_iterations"] == 1
        assert cascade["sddp_options"]["max_iterations"] == 7  # 5 + 1 + 1

    def test_process_options_cascade_no_cascade_for_sddp(self):
        """cascade_options is NOT emitted for plain sddp."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "method": "sddp"})
        assert "cascade_options" not in writer.planning["options"]

    def test_process_options_no_num_apertures_in_sddp(self):
        """num_apertures is never emitted in sddp_options (C++ has no such field)."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "num_apertures": "5"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp

    def test_process_options_no_num_apertures_all(self):
        """num_apertures is absent for 'all'/-1 and default cases."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "num_apertures": "all"})
        sddp = writer.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp

        writer2 = GTOptWriter(MagicMock())
        writer2.process_options({"output_dir": "out"})
        sddp2 = writer2.planning["options"]["sddp_options"]
        assert "num_apertures" not in sddp2

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

    def test_process_options_convergence_tol_from_plpmat(self):
        """convergence_tol takes the raw PDError value from plpmat.dat."""
        mock_parser = self._make_plpmat_parser(pd_error=0.001)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_tol"] == pytest.approx(0.001)

    def test_process_options_convergence_tol_no_unit_conversion(self):
        """PDError=1.0 is emitted verbatim (no implicit percentage→fraction)."""
        mock_parser = self._make_plpmat_parser(pd_error=1.0)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_tol"] == pytest.approx(1.0)

    def test_process_options_convergence_tol_default(self):
        """convergence_tol defaults to 0.01 when plpmat has no PDError."""
        mock_parser = self._make_plpmat_parser(pd_error=0.0)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_tol"] == pytest.approx(0.01)

    def test_process_options_convergence_tol_no_plpmat(self):
        """convergence_tol defaults to 0.01 when no plpmat.dat is present."""
        mock_parser = MagicMock()
        mock_parser.parsed_data = {}
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_tol"] == pytest.approx(0.01)

    def test_process_options_convergence_tol_explicit_overrides(self):
        """Explicit convergence_tol overrides plpmat.dat value."""
        mock_parser = self._make_plpmat_parser(pd_error=0.001)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out", "convergence_tol": 0.05})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_tol"] == pytest.approx(0.05)

    def test_process_options_stationary_tol_defaults_to_one_percent(self):
        """stationary_tol defaults to 0.01 (1 %) — independent of
        convergence_tol so the gap-still-moving heuristic isn't pinned to
        the (often much tighter) PDError-derived convergence_tol."""
        mock_parser = self._make_plpmat_parser(pd_error=0.001)
        writer = GTOptWriter(mock_parser)
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["stationary_tol"] == pytest.approx(0.01)
        # convergence_tol is still inherited from plpmat.dat (PDError = 0.001).
        assert sddp["convergence_tol"] == pytest.approx(0.001)
        assert sddp["stationary_window"] == 4

    def test_process_options_min_iterations_default_three(self):
        """min_iterations defaults to 3 when not specified — forces the
        SDDP loop to train at least 3 iterations before any convergence
        test fires (CI / gap / stationary)."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["min_iterations"] == 3

    def test_process_options_min_iterations_explicit_overrides(self):
        """Explicit min_iterations overrides the default of 3."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "min_iterations": 10})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["min_iterations"] == 10

    def test_process_options_convergence_confidence_default_p99(self):
        """convergence_confidence defaults to 0.99 (z=2.576) so the
        statistical CI test fires only when σ is truly tight."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_confidence"] == pytest.approx(0.99)

    def test_process_options_convergence_confidence_explicit_overrides(self):
        """Explicit convergence_confidence overrides the default of 0.99."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "convergence_confidence": 0.95})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["convergence_confidence"] == pytest.approx(0.95)

    def test_process_options_stationary_tol_explicit_overrides(self):
        """Explicit stationary_tol overrides the convergence_tol default."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "stationary_tol": 0.005})
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["stationary_tol"] == pytest.approx(0.005)

    def test_process_options_demand_fail_cost_default(self):
        """demand_fail_cost defaults to 0 when not specified.

        Real demands receive their curtailment cost via per-demand
        ``fcost`` (from plpcnfce.dat falla centrals).  The global
        default is 0 so that synthetic demands created by C++
        ``System::expand_batteries`` (for battery AC-side charging)
        inherit fcost=0 and are truly dispatchable — without this
        the LP would otherwise force batteries to charge at pmax.
        """
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["model_options"]["demand_fail_cost"] == 0

    def test_process_options_with_discount(self):
        """process_options passes discount_rate to simulation."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "discount_rate": 0.08})
        assert writer.planning["simulation"]["annual_discount_rate"] == pytest.approx(
            0.08
        )

    def test_process_options_strict_storage_emin_default_false(self):
        """plp2gtopt always emits ``strict_storage_emin = False``.

        gtopt's C++ default for ``model_options.strict_storage_emin`` was
        flipped to ``true`` in 3581a80e (per-stage emin floor as a HARD
        lower bound on ``reservoir_sini`` and last-block ``efin``).  PLP's
        per-stage LP, however, treats ``ve<u>`` as Free mid-stage and only
        ``vf<u>`` (future volume) carries the ``vmin`` floor.  Without an
        explicit override, plp2gtopt-generated cases would silently switch
        to strict semantics and may go infeasible at iter-0 of an SDDP
        cascade whenever an upstream Benders cut clamps ``sini`` near 0
        but the schedule still demands ``efin >= emin``.
        """
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert (
            writer.planning["options"]["model_options"]["strict_storage_emin"] is False
        )

    def test_process_options_strict_storage_emin_explicit_override(self):
        """User can opt back into strict mode by setting it in model_options."""
        writer = GTOptWriter(MagicMock())
        writer.process_options(
            {
                "output_dir": "out",
                "model_options": {"strict_storage_emin": True},
            }
        )
        assert (
            writer.planning["options"]["model_options"]["strict_storage_emin"] is True
        )

    # ---- SDDP (default) scenario/scene tests --------------------------------

    @staticmethod
    def _make_scenario_mock(num_hydrologies=10):
        """Create a mock parser with enough hydrologies for scenario tests."""
        mock = MagicMock()
        aflce = MagicMock()
        aflce.items = [{"num_hydrologies": num_hydrologies}]
        mock.parsed_data = {"idsim_parser": None, "aflce_parser": aflce}
        return mock

    def test_process_scenarios_single_hydrology(self):
        """Single hydrology → probability_factor = 1.0, uid = Fortran index."""
        writer = GTOptWriter(self._make_scenario_mock())
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
        writer = GTOptWriter(self._make_scenario_mock())
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
        writer = GTOptWriter(self._make_scenario_mock())
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
        writer = GTOptWriter(self._make_scenario_mock())
        writer.process_scenarios({"hydrologies": "1,2", "method": "monolithic"})
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
        writer = GTOptWriter(self._make_scenario_mock())
        writer.process_scenarios({"hydrologies": "1,2,3", "method": "mono"})
        scenes = writer.planning["simulation"]["scene_array"]
        assert len(scenes) == 1
        assert scenes[0]["count_scenario"] == 3

    def test_process_scenarios_invalid_index_zero(self):
        """Index 0 is not a valid 1-based hydrology index."""
        writer = GTOptWriter(self._make_scenario_mock(num_hydrologies=10))
        with pytest.raises(ValueError, match="Invalid hydrology indices"):
            writer.process_scenarios({"hydrologies": "0", "probability_factors": None})

    def test_process_scenarios_invalid_index_out_of_range(self):
        """Index beyond available hydrologies raises ValueError."""
        writer = GTOptWriter(self._make_scenario_mock(num_hydrologies=10))
        with pytest.raises(ValueError, match="Invalid hydrology indices"):
            writer.process_scenarios({"hydrologies": "50", "probability_factors": None})

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
        with patch("plp2gtopt._writer_hydro.JunctionWriter") as mock_jw:
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
        opts = {"output_dir": tmp_path, "hydrologies": "1", "method": "sddp"}
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
        opts = {"output_dir": tmp_path, "hydrologies": "1", "method": "sddp"}
        sim = SimulationWriter(parser.parsed_data, opts).build()
        assert len(sim["phase_array"]) == len(sim["stage_array"])

    def test_monolithic_one_phase(self, tmp_path):
        parser = PLPParser({"input_dir": self._CASES_DIR / "plp_min_1bus"})
        parser.parse_all()
        opts = {
            "output_dir": tmp_path,
            "hydrologies": "1",
            "method": "monolithic",
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
        """auto_reservoir_energy_scale adds reservoir energy+flow scale entries."""
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

        writer.process_variable_scales({**opts, "auto_reservoir_energy_scale": True})
        scales = writer.planning["options"].get("variable_scales", [])
        rsv_scales = [s for s in scales if s["class_name"] == "Reservoir"]
        assert len(rsv_scales) == 2  # energy + flow
        assert rsv_scales[0]["variable"] == "energy"
        assert rsv_scales[1]["variable"] == "flow"

    def test_battery_auto_scale(self, tmp_path):
        """auto_battery_energy_scale adds battery scale entries."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)

        writer.planning["system"]["battery_array"] = [{"uid": 1, "name": "bat1"}]
        writer.planning["options"] = {}

        writer.process_variable_scales({**opts, "auto_battery_energy_scale": True})
        scales = writer.planning["options"].get("variable_scales", [])
        bat_scales = [s for s in scales if s["class_name"] == "Battery"]
        assert len(bat_scales) == 2
        bat_energy = [s for s in bat_scales if s["variable"] == "energy"]
        bat_flow = [s for s in bat_scales if s["variable"] == "flow"]
        assert len(bat_energy) == 1
        assert bat_energy[0]["scale"] == 0.01
        assert len(bat_flow) == 1
        assert bat_flow[0]["scale"] == 0.01

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


# ---------------------------------------------------------------------------
# Falla → Demand fcost mapping
# ---------------------------------------------------------------------------


class TestFallaFcost:
    """Test falla central → demand fcost mapping."""

    def _make_writer_with_fallas(self, falla_centrals):
        """Create a GTOptWriter with mock central_parser containing fallas."""
        mock_parser = MagicMock(spec=PLPParser)
        mock_cp = MagicMock()
        mock_cp.centrals = falla_centrals
        mock_parser.parsed_data = {"central_parser": mock_cp}
        writer = GTOptWriter(mock_parser)
        return writer

    def test_falla_by_bus_basic(self):
        """Single falla per bus returns the falla central."""
        writer = self._make_writer_with_fallas(
            [
                {"name": "FALLA_001", "type": "falla", "bus": 1, "gcost": 406.0},
                {"name": "FALLA_002", "type": "falla", "bus": 2, "gcost": 500.0},
            ]
        )
        result = writer._falla_by_bus()
        assert result[1]["gcost"] == 406.0
        assert result[2]["gcost"] == 500.0

    def test_falla_by_bus_min_on_same_bus(self):
        """Multiple fallas on same bus: smallest gcost wins."""
        writer = self._make_writer_with_fallas(
            [
                {"name": "FALLA_001_A", "type": "falla", "bus": 1, "gcost": 500.0},
                {"name": "FALLA_001_B", "type": "falla", "bus": 1, "gcost": 300.0},
                {"name": "FALLA_001_C", "type": "falla", "bus": 1, "gcost": 800.0},
            ]
        )
        result = writer._falla_by_bus()
        assert result[1]["gcost"] == 300.0
        assert result[1]["name"] == "FALLA_001_B"

    def test_falla_by_bus_skips_bus_zero(self):
        """Falla centrals with bus <= 0 are ignored."""
        writer = self._make_writer_with_fallas(
            [
                {"name": "FALLA_NOBUS", "type": "falla", "bus": 0, "gcost": 100.0},
                {"name": "FALLA_NEG", "type": "falla", "bus": -1, "gcost": 100.0},
                {"name": "FALLA_OK", "type": "falla", "bus": 3, "gcost": 200.0},
            ]
        )
        result = writer._falla_by_bus()
        assert 3 in result
        assert 0 not in result

    def test_falla_by_bus_skips_non_falla(self):
        """Non-falla centrals are ignored."""
        writer = self._make_writer_with_fallas(
            [
                {"name": "GEN1", "type": "termica", "bus": 1, "gcost": 50.0},
                {"name": "FALLA_001", "type": "falla", "bus": 1, "gcost": 406.0},
            ]
        )
        result = writer._falla_by_bus()
        assert result[1]["gcost"] == 406.0

    def test_falla_by_bus_empty_when_no_centrals(self):
        """No central_parser → empty mapping."""
        mock_parser = MagicMock(spec=PLPParser)
        mock_parser.parsed_data = {}
        writer = GTOptWriter(mock_parser)
        assert not writer._falla_by_bus()

    def test_process_demands_sets_fcost(self, tmp_path):
        """process_demands populates fcost from falla centrals."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        writer.to_json(opts)

        demand_array = writer.planning["system"].get("demand_array", [])
        if not demand_array:
            pytest.skip("plp_min_1bus has no demands")

        # Check that at least one demand got an fcost
        demands_with_fcost = [d for d in demand_array if "fcost" in d]
        # If the case has falla centrals, demands should have fcost
        central_parser = parser.parsed_data.get("central_parser")
        if central_parser:
            fallas = [
                c
                for c in central_parser.centrals
                if c.get("type") == "falla" and c.get("bus", 0) > 0
            ]
            if fallas:
                assert len(demands_with_fcost) > 0


class TestWriteFcostParquet:
    """Test Demand/fcost generation from falla cost schedules."""

    def _make_mocks(self, fallas, cost_entries, stages):
        """Build mock parsers for falla+cost+stage data."""
        import numpy as np

        mock_cp = MagicMock()
        mock_cp.centrals = fallas

        mock_cost = MagicMock()
        mock_cost.get_cost_by_name = lambda name: next(
            (
                {
                    "cost": np.array(e["cost"], dtype=np.float64),
                    "stage": np.array(e["stage"], dtype=np.int32),
                }
                for e in cost_entries
                if e["name"] == name
            ),
            None,
        )

        mock_stage = MagicMock()
        mock_stage.items = [{"number": s} for s in stages]

        return mock_cost, mock_stage, mock_cp

    @staticmethod
    def _falla_by_bus(fallas):
        """Replicate GTOptWriter._falla_by_bus logic for test falla lists."""
        result = {}
        for c in fallas:
            if c.get("type") != "falla":
                continue
            bus = c.get("bus", 0)
            if bus <= 0:
                continue
            prev = result.get(bus)
            if prev is None or c.get("gcost", 0.0) < prev.get("gcost", 0.0):
                result[bus] = c
        return result

    def test_writes_parquet_with_schedule(self, tmp_path):
        """Falla with cost schedule produces Demand/fcost.parquet."""
        import pandas as pd
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_1", "type": "falla", "bus": 1, "gcost": 400.0},
        ]
        costs = [
            {"name": "FALLA_1", "stage": [1, 2, 3], "cost": [400.0, 450.0, 500.0]},
        ]
        cost_p, stage_p, central_p = self._make_mocks(fallas, costs, stages=[1, 2, 3])
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        filed = dw.write_fcost(demand_array, falla_by_bus, cost_p, stage_p, central_p)

        assert 1 in filed
        parquet_path = tmp_path / "Demand" / "fcost.parquet"
        assert parquet_path.exists()
        df = pd.read_parquet(parquet_path)
        assert "stage" in df.columns
        assert "uid:1" in df.columns
        assert list(df["uid:1"]) == [400.0, 450.0, 500.0]

    def test_constant_schedule_stays_scalar(self, tmp_path):
        """Falla whose schedule equals base gcost → no file, stays scalar."""
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_1", "type": "falla", "bus": 1, "gcost": 400.0},
        ]
        costs = [
            {"name": "FALLA_1", "stage": [1, 2, 3], "cost": [400.0, 400.0, 400.0]},
        ]
        cost_p, stage_p, central_p = self._make_mocks(fallas, costs, stages=[1, 2, 3])
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        filed = dw.write_fcost(demand_array, falla_by_bus, cost_p, stage_p, central_p)

        assert not filed  # No file written

    def test_multiple_fallas_min_per_stage(self, tmp_path):
        """Multiple fallas on same bus → element-wise min cost per stage."""
        import pandas as pd
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_A", "type": "falla", "bus": 1, "gcost": 500.0},
            {"name": "FALLA_B", "type": "falla", "bus": 1, "gcost": 400.0},
        ]
        costs = [
            {"name": "FALLA_A", "stage": [1, 2, 3], "cost": [300.0, 600.0, 500.0]},
            {"name": "FALLA_B", "stage": [1, 2, 3], "cost": [500.0, 400.0, 450.0]},
        ]
        cost_p, stage_p, central_p = self._make_mocks(fallas, costs, stages=[1, 2, 3])
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        filed = dw.write_fcost(demand_array, falla_by_bus, cost_p, stage_p, central_p)

        assert 1 in filed
        df = pd.read_parquet(tmp_path / "Demand" / "fcost.parquet")
        # min(300,500)=300, min(600,400)=400, min(500,450)=450
        assert list(df["uid:1"]) == [300.0, 400.0, 450.0]

    def test_falla_without_schedule_uses_constant(self, tmp_path):
        """Falla with no cost schedule contributes its constant gcost."""
        import pandas as pd
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_A", "type": "falla", "bus": 1, "gcost": 300.0},
            {"name": "FALLA_B", "type": "falla", "bus": 1, "gcost": 500.0},
        ]
        # Only FALLA_B has a schedule; FALLA_A uses constant 300.0
        costs = [
            {"name": "FALLA_B", "stage": [1, 2, 3], "cost": [200.0, 600.0, 500.0]},
        ]
        cost_p, stage_p, central_p = self._make_mocks(fallas, costs, stages=[1, 2, 3])
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        filed = dw.write_fcost(demand_array, falla_by_bus, cost_p, stage_p, central_p)

        assert 1 in filed
        df = pd.read_parquet(tmp_path / "Demand" / "fcost.parquet")
        # min(300,200)=200, min(300,600)=300, min(300,500)=300
        assert list(df["uid:1"]) == [200.0, 300.0, 300.0]

    def test_no_cost_parser_returns_empty(self, tmp_path):
        """No cost_parser → no file written."""
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_1", "type": "falla", "bus": 1, "gcost": 400.0},
        ]
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        filed = dw.write_fcost(demand_array, falla_by_bus, None, None, None)

        assert not filed

    def test_parquet_has_stage_index_only(self, tmp_path):
        """Parquet file has 'stage' column but no 'block' column (T-indexed)."""
        import pandas as pd
        from plp2gtopt.demand_writer import DemandWriter

        fallas = [
            {"name": "FALLA_1", "type": "falla", "bus": 1, "gcost": 400.0},
        ]
        costs = [
            {"name": "FALLA_1", "stage": [1, 2], "cost": [400.0, 500.0]},
        ]
        cost_p, stage_p, central_p = self._make_mocks(fallas, costs, stages=[1, 2])
        demand_array = [{"uid": 1, "name": "d1", "bus": 1}]
        falla_by_bus = self._falla_by_bus(fallas)
        opts = {"output_dir": tmp_path, "compression": "zstd"}
        dw = DemandWriter(options=opts)

        dw.write_fcost(demand_array, falla_by_bus, cost_p, stage_p, central_p)

        df = pd.read_parquet(tmp_path / "Demand" / "fcost.parquet")
        assert "stage" in df.columns
        assert "block" not in df.columns
        assert df["stage"].dtype == "int32"
