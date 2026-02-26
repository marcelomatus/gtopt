"""Tests for gtopt_writer.py module."""

import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt.central_parser import CentralParser
from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.plp_parser import PLPParser

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"
_PLPMinBess = _CASES_DIR / "plp_min_bess"


def _make_opts(tmp_path: Path, case_name: str = "test") -> dict:
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "0",
        "discount_rate": 0.0,
        "last_stage": -1,
        "compression": "gzip",
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

    def test_to_json_simulation_has_stages_and_blocks(self, tmp_path):
        """simulation block must contain stage_array and block_array."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        sim = result["simulation"]
        assert "stage_array" in sim
        assert "block_array" in sim
        assert len(sim["stage_array"]) > 0
        assert len(sim["block_array"]) > 0

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
        """options block includes expected keys."""

        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        result = writer.to_json(_make_opts(tmp_path))
        opts = result["options"]
        assert "input_directory" in opts
        assert "output_directory" in opts
        assert "demand_fail_cost" in opts


class TestGTOptWriterProcessMethods:
    """Unit tests for individual process_* methods."""

    def test_process_options_default_discount(self):
        """process_options sets annual_discount_rate=0 when not provided."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out"})
        assert writer.planning["options"]["annual_discount_rate"] == 0.0

    def test_process_options_with_discount(self):
        """process_options passes discount_rate through."""
        writer = GTOptWriter(MagicMock())
        writer.process_options({"output_dir": "out", "discount_rate": 0.08})
        assert writer.planning["options"]["annual_discount_rate"] == pytest.approx(0.08)

    def test_process_scenarios_single_hydrology(self):
        """Single hydrology → probability_factor = 1.0."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "2", "probability_factors": None})
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 1
        assert scenarios[0]["probability_factor"] == pytest.approx(1.0)
        assert scenarios[0]["hydrology"] == 2

    def test_process_scenarios_two_hydrologies_equal(self):
        """Two hydrologies with no explicit weights → 0.5 each."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios({"hydrologies": "1,3", "probability_factors": None})
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 2
        for s in scenarios:
            assert s["probability_factor"] == pytest.approx(0.5)

    def test_process_scenarios_explicit_weights(self):
        """Explicit probability_factors are parsed as floats."""
        writer = GTOptWriter(MagicMock())
        writer.process_scenarios(
            {"hydrologies": "0,1,2", "probability_factors": "0.2,0.5,0.3"}
        )
        scenarios = writer.planning["simulation"]["scenario_array"]
        assert len(scenarios) == 3
        assert scenarios[0]["probability_factor"] == pytest.approx(0.2)
        assert scenarios[2]["probability_factor"] == pytest.approx(0.3)

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
