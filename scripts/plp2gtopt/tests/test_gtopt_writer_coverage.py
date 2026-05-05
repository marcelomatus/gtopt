"""Tests for gtopt_writer.py — uncovered lines and edge cases."""

import json
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.plp_parser import PLPParser


_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"


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


# ---------------------------------------------------------------------------
# process_options — empty options (line 74)
# ---------------------------------------------------------------------------


class TestProcessOptions:
    """Tests for process_options edge cases."""

    def test_process_options_empty(self):
        """process_options handles None/empty options gracefully."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options({})
        assert "method" in writer.planning["options"]

    def test_process_options_cut_sharing(self, tmp_path):
        """process_options sets cut_sharing_mode in sddp_options."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["cut_sharing_mode"] = "shared"
        writer.process_options(opts)
        sddp = writer.planning["options"].get("sddp_options", {})
        assert sddp.get("cut_sharing_mode") == "shared"

    def test_process_options_backward_solver_threads_default(self, tmp_path):
        """process_options always emits backward_solver_options.threads=1."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options(_make_opts(tmp_path))
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["backward_solver_options"] == {"threads": 1}

    def test_process_options_backward_solver_threads_override(self, tmp_path):
        """`backward_solver_threads` option overrides the default."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["backward_solver_threads"] = 4
        writer.process_options(opts)
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["backward_solver_options"] == {"threads": 4}


# ---------------------------------------------------------------------------
# _load_variable_scales_file (lines 1119-1156)
# ---------------------------------------------------------------------------


class TestLoadVariableScalesFile:
    """Tests for _load_variable_scales_file."""

    def test_valid_file(self, tmp_path):
        """Loads a valid JSON array of scale entries."""
        data = [
            {
                "class_name": "Reservoir",
                "variable": "energy",
                "uid": 1,
                "scale": 0.001,
            },
        ]
        path = tmp_path / "scales.json"
        path.write_text(json.dumps(data))
        result = GTOptWriter._load_variable_scales_file(path)
        assert len(result) == 1
        assert result[0]["scale"] == 0.001

    def test_not_a_list(self, tmp_path):
        """Returns empty list when JSON is not an array."""
        path = tmp_path / "scales.json"
        path.write_text('{"not": "a list"}')
        result = GTOptWriter._load_variable_scales_file(path)
        assert not result

    def test_invalid_entry_skipped(self, tmp_path):
        """Skips entries missing required keys."""
        data = [
            {"class_name": "Reservoir"},  # missing variable, uid, scale
            {
                "class_name": "Battery",
                "variable": "energy",
                "uid": 10,
                "scale": 0.01,
            },
        ]
        path = tmp_path / "scales.json"
        path.write_text(json.dumps(data))
        result = GTOptWriter._load_variable_scales_file(path)
        assert len(result) == 1
        assert result[0]["class_name"] == "Battery"

    def test_file_not_found(self, tmp_path):
        """Returns empty list for missing file."""
        path = tmp_path / "nonexistent.json"
        result = GTOptWriter._load_variable_scales_file(path)
        assert not result

    def test_invalid_json(self, tmp_path):
        """Returns empty list for malformed JSON."""
        path = tmp_path / "bad.json"
        path.write_text("{broken json")
        result = GTOptWriter._load_variable_scales_file(path)
        assert not result


# ---------------------------------------------------------------------------
# process_variable_scales (lines 1158-1318)
# ---------------------------------------------------------------------------


class TestProcessVariableScales:
    """Tests for process_variable_scales."""

    def test_no_options(self):
        """process_variable_scales does nothing with empty options."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {"reservoir_array": [], "battery_array": []},
            "simulation": {},
        }
        writer.process_variable_scales({})
        assert "variable_scales" not in writer.planning["options"]

    def test_auto_reservoir_energy_scale(self, tmp_path):
        """Auto reservoir energy scaling uses FEscala from planos parser."""
        parser = MagicMock()
        mock_planos = MagicMock()
        mock_planos.reservoir_fescala = {"RSV1": 3}  # scale = 10^(3-6) = 0.001
        mock_central_parser = MagicMock()
        mock_central_parser.centrals = []
        parser.parsed_data = {
            "planos_parser": mock_planos,
            "central_parser": mock_central_parser,
        }

        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [{"name": "RSV1", "uid": 1}],
                "battery_array": [],
            },
            "simulation": {},
        }
        writer.process_variable_scales({"auto_reservoir_energy_scale": True})

        scales = writer.planning["options"]["variable_scales"]
        rsv_energy = [s for s in scales if s["variable"] == "energy" and s["uid"] == 1]
        assert len(rsv_energy) == 1
        assert rsv_energy[0]["scale"] == pytest.approx(0.001)

    def test_explicit_reservoir_energy_scale(self):
        """Explicit --reservoir-energy-scale overrides auto."""
        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": None,
            "central_parser": None,
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [{"name": "RSV1", "uid": 1}],
                "battery_array": [],
            },
            "simulation": {},
        }
        writer.process_variable_scales(
            {
                "reservoir_energy_scale": {"RSV1": 42.0},
                "auto_reservoir_energy_scale": True,
            }
        )
        scales = writer.planning["options"]["variable_scales"]
        rsv_energy = [s for s in scales if s["variable"] == "energy" and s["uid"] == 1]
        assert rsv_energy[0]["scale"] == pytest.approx(42.0)

    def test_auto_battery_energy_scale(self):
        """Auto battery energy scaling sets 0.01."""
        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": None,
            "central_parser": None,
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [],
                "battery_array": [{"name": "BAT1", "uid": 10}],
            },
            "simulation": {},
        }
        writer.process_variable_scales({"auto_battery_energy_scale": True})
        scales = writer.planning["options"]["variable_scales"]
        bat_energy = [s for s in scales if s["variable"] == "energy" and s["uid"] == 10]
        assert len(bat_energy) == 1
        assert bat_energy[0]["scale"] == pytest.approx(0.01)
        # Also has flow scale
        bat_flow = [s for s in scales if s["variable"] == "flow" and s["uid"] == 10]
        assert len(bat_flow) == 1

    def test_explicit_battery_energy_scale(self):
        """Explicit --battery-energy-scale overrides auto."""
        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": None,
            "central_parser": None,
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [],
                "battery_array": [{"name": "BAT1", "uid": 10}],
            },
            "simulation": {},
        }
        writer.process_variable_scales(
            {
                "battery_energy_scale": {"BAT1": 0.5},
                "auto_battery_energy_scale": True,
            }
        )
        scales = writer.planning["options"]["variable_scales"]
        bat_energy = [s for s in scales if s["variable"] == "energy" and s["uid"] == 10]
        assert bat_energy[0]["scale"] == pytest.approx(0.5)

    def test_file_scales_merged(self, tmp_path):
        """File-based scales are merged with lowest priority."""
        file_data = [
            {
                "class_name": "Reservoir",
                "variable": "energy",
                "uid": 99,
                "scale": 0.1,
            },
        ]
        file_path = tmp_path / "scales.json"
        file_path.write_text(json.dumps(file_data))

        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": None,
            "central_parser": None,
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [],
                "battery_array": [],
            },
            "simulation": {},
        }
        writer.process_variable_scales(
            {
                "variable_scales_file": str(file_path),
            }
        )
        scales = writer.planning["options"]["variable_scales"]
        assert len(scales) == 1
        assert scales[0]["uid"] == 99
        assert scales[0]["scale"] == pytest.approx(0.1)

    def test_file_scales_overridden_by_auto(self, tmp_path):
        """File-based scales are skipped when auto/explicit provides same key."""
        file_data = [
            {
                "class_name": "Reservoir",
                "variable": "energy",
                "uid": 1,
                "scale": 999.0,
            },
        ]
        file_path = tmp_path / "scales.json"
        file_path.write_text(json.dumps(file_data))

        parser = MagicMock()
        mock_planos = MagicMock()
        mock_planos.reservoir_fescala = {"RSV1": 3}
        parser.parsed_data = {
            "planos_parser": mock_planos,
            "central_parser": MagicMock(centrals=[]),
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [{"name": "RSV1", "uid": 1}],
                "battery_array": [],
            },
            "simulation": {},
        }
        writer.process_variable_scales(
            {
                "auto_reservoir_energy_scale": True,
                "variable_scales_file": str(file_path),
            }
        )
        scales = writer.planning["options"]["variable_scales"]
        # The auto scale (0.001) should take priority over file scale (999.0)
        rsv_energy = [s for s in scales if s["variable"] == "energy" and s["uid"] == 1]
        assert len(rsv_energy) == 1
        assert rsv_energy[0]["scale"] == pytest.approx(0.001)

    def test_scale_1_not_emitted(self):
        """Scale of 1.0 is not emitted (no-op)."""
        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": None,
            "central_parser": MagicMock(
                centrals=[
                    {"name": "RSV1", "type": "embalse", "energy_scale": 1.0},
                ]
            ),
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [{"name": "RSV1", "uid": 1}],
                "battery_array": [],
            },
            "simulation": {},
        }
        writer.process_variable_scales({"auto_reservoir_energy_scale": True})
        # scale=1.0 is filtered out
        assert "variable_scales" not in writer.planning["options"]


# ---------------------------------------------------------------------------
# _build_stage_to_phase_map (lines 1096-1116)
# ---------------------------------------------------------------------------


class TestBuildStageToPhaseMap:
    """Tests for _build_stage_to_phase_map."""

    def test_empty_stage_array(self):
        """Returns None when stage_array is empty."""
        parser = MagicMock()
        writer = GTOptWriter(parser)
        writer.planning = {"stage_array": []}
        result = writer._build_stage_to_phase_map()
        assert result is None

    def test_missing_stage_array(self):
        """Returns None when stage_array is absent."""
        parser = MagicMock()
        writer = GTOptWriter(parser)
        writer.planning = {}
        result = writer._build_stage_to_phase_map()
        assert result is None

    def test_valid_stage_array(self):
        """Returns correct mapping from stage UID to phase UID."""
        parser = MagicMock()
        writer = GTOptWriter(parser)
        writer.planning = {
            "stage_array": [
                {"uid": 1, "phase_uid": 1},
                {"uid": 2, "phase_uid": 1},
                {"uid": 3, "phase_uid": 2},
            ],
        }
        result = writer._build_stage_to_phase_map()
        assert result == {1: 1, 2: 1, 3: 2}


# ---------------------------------------------------------------------------
# process_boundary_cuts — edge cases
# ---------------------------------------------------------------------------


class TestProcessBoundaryCuts:
    """Tests for process_boundary_cuts edge cases."""

    def test_no_planos_parser(self, tmp_path):
        """process_boundary_cuts does nothing when planos_parser is None."""
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": None}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {},
            "simulation": {},
        }
        writer.process_boundary_cuts(_make_opts(tmp_path))
        assert "sddp_options" not in writer.planning["options"]

    def test_no_boundary_cuts_flag(self, tmp_path):
        """process_boundary_cuts skips when no_boundary_cuts is True."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100}]
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {},
            "simulation": {},
        }
        opts = _make_opts(tmp_path)
        opts["no_boundary_cuts"] = True
        writer.process_boundary_cuts(opts)
        sddp = writer.planning["options"].get("sddp_options", {})
        assert "boundary_cuts_file" not in sddp


# ---------------------------------------------------------------------------
# write() — full write path
# ---------------------------------------------------------------------------


def test_write_creates_json(tmp_path):
    """GTOptWriter.write() creates the output JSON file."""
    parser = PLPParser({"input_dir": _PLPMin1Bus})
    parser.parse_all()
    writer = GTOptWriter(parser)
    opts = _make_opts(tmp_path, "write_test")
    writer.write(opts)
    assert opts["output_file"].exists()
    data = json.loads(opts["output_file"].read_text())
    assert "options" in data
    assert "system" in data


# ---------------------------------------------------------------------------
# _normalize_method
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("input_val", "expected"),
    [
        ("sddp", "sddp"),
        ("mono", "monolithic"),
        ("monolithic", "monolithic"),
        ("cascade", "cascade"),
        ("other", "sddp"),
    ],
)
def test_normalize_method(input_val, expected):
    """_normalize_method maps aliases correctly."""
    assert GTOptWriter._normalize_method(input_val) == expected


# ---------------------------------------------------------------------------
# process_options — model_options edge cases (lines 156-159)
# ---------------------------------------------------------------------------


class TestProcessOptionsModelOpts:
    """Tests for model_options sub-fields in process_options."""

    def test_reserve_fail_cost(self, tmp_path):
        """process_options sets reserve_fail_cost when provided."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "rfc")
        opts["model_options"] = {
            "use_single_bus": False,
            "use_kirchhoff": True,
            "demand_fail_cost": 1000,
            "scale_objective": 10_000_000,
            "scale_theta": 0.0001,
            "reserve_fail_cost": 500.0,
            "use_line_losses": True,
        }
        writer.process_options(opts)
        mo = writer.planning["options"]["model_options"]
        assert mo["reserve_fail_cost"] == 500.0
        assert mo["use_line_losses"] is True


# ---------------------------------------------------------------------------
# process_stage_blocks — stages_phase spec (lines 220-237)
# ---------------------------------------------------------------------------


class TestProcessStageBlocksPhase:
    """Tests for process_stage_blocks with stages_phase spec."""

    def test_stages_phase_string(self, tmp_path):
        """stages_phase string spec creates correct phase_array."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "sp")
        # Get number of stages first
        writer.process_options(opts)
        num_stages = len(parser.parsed_data["stage_parser"].items)
        # Group all stages into one phase using range spec
        opts["stages_phase"] = f"1:{num_stages}"
        writer.process_stage_blocks(opts)
        phases = writer.planning["simulation"]["phase_array"]
        assert len(phases) == 1
        assert phases[0]["first_stage"] == 0
        assert phases[0]["count_stage"] == num_stages

    def test_stages_phase_list(self, tmp_path):
        """stages_phase list spec creates correct phase_array."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "sp_list")
        writer.process_options(opts)
        # Pass pre-parsed list of groups
        opts["stages_phase"] = [[1, 2], [3]]
        writer.process_stage_blocks(opts)
        phases = writer.planning["simulation"]["phase_array"]
        assert len(phases) == 2
        assert phases[0]["first_stage"] == 0
        assert phases[0]["count_stage"] == 2
        assert phases[1]["first_stage"] == 2
        assert phases[1]["count_stage"] == 1

    def test_monolithic_solver(self, tmp_path):
        """monolithic solver type creates a single phase."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "mono")
        opts["method"] = "monolithic"
        writer.process_options(opts)
        writer.process_stage_blocks(opts)
        phases = writer.planning["simulation"]["phase_array"]
        assert len(phases) == 1


# ---------------------------------------------------------------------------
# process_water_rights (lines 634-696)
# ---------------------------------------------------------------------------


class TestProcessWaterRights:
    """Tests for process_water_rights."""

    def test_disabled_by_default(self, tmp_path):
        """process_water_rights does nothing when expand_water_rights is False."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "wr_off")
        writer.to_json(opts)
        # No water rights entities in a basic case without the flag
        sys = writer.planning["system"]
        assert "flow_right_array" not in sys
        assert "volume_right_array" not in sys

    def test_enabled_no_laja_no_maule(self, tmp_path):
        """process_water_rights with flag but no laja/maule parsers."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "wr_on")
        opts["expand_water_rights"] = True
        # plp_min_1bus has no laja/maule data, so this should be a no-op
        writer.to_json(opts)
        # Should still complete without error


# ---------------------------------------------------------------------------
# to_json — version string paths (lines 1361-1375)
# ---------------------------------------------------------------------------


class TestToJsonVersion:
    """Tests for to_json version string construction."""

    def test_version_with_sys_version(self, tmp_path):
        """to_json includes sys_version in version string."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "ver")
        opts["sys_version"] = "v2.0"
        opts["input_dir"] = _PLPMin1Bus
        result = writer.to_json(opts)
        version = result["system"]["version"]
        assert "v2.0" in version
        assert "plp_min_1bus" in version

    def test_version_without_sys_version(self, tmp_path):
        """to_json omits sys_version when empty."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "ver_no")
        opts["input_dir"] = _PLPMin1Bus
        result = writer.to_json(opts)
        version = result["system"]["version"]
        assert "plp2gtopt" in version

    def test_version_no_input_dir(self, tmp_path):
        """to_json handles missing input_dir in version."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "ver_noid")
        result = writer.to_json(opts)
        assert "plp2gtopt" in result["system"]["version"]


# ---------------------------------------------------------------------------
# process_flow_turbines — edge cases (lines 702-742)
# ---------------------------------------------------------------------------


class TestProcessFlowTurbines:
    """Tests for process_flow_turbines edge cases."""

    def test_no_hydro_names(self, tmp_path):
        """process_flow_turbines does nothing without hydro names."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "ft_noh")
        writer.planning = {
            "options": {},
            "system": {},
            "simulation": {},
        }
        opts["_pasada_hydro_names"] = set()
        writer.process_flow_turbines(opts)
        assert "flow_array" not in writer.planning["system"]

    def test_no_central_parser(self, tmp_path):
        """process_flow_turbines does nothing without central parser."""
        parser = MagicMock()
        parser.parsed_data = {"central_parser": None}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {},
            "simulation": {},
        }
        writer.process_flow_turbines({"_pasada_hydro_names": {"CENT1"}})
        assert "flow_array" not in writer.planning["system"]


# ---------------------------------------------------------------------------
# _load_alias_file
# ---------------------------------------------------------------------------


class TestLoadAliasFile:
    """Tests for GTOptWriter._load_alias_file."""

    def test_none_returns_none(self):
        assert GTOptWriter._load_alias_file(None) is None

    def test_valid_alias(self, tmp_path):
        path = tmp_path / "alias.json"
        path.write_text(json.dumps({"CANUTILLAR": "CHAPO", "OLD": "NEW"}))
        result = GTOptWriter._load_alias_file(path)
        assert result == {"CANUTILLAR": "CHAPO", "OLD": "NEW"}

    def test_missing_file_raises(self, tmp_path):
        path = tmp_path / "nope.json"
        with pytest.raises(RuntimeError, match="Cannot read alias file"):
            GTOptWriter._load_alias_file(path)

    def test_malformed_json_raises(self, tmp_path):
        path = tmp_path / "bad.json"
        path.write_text("{broken json")
        with pytest.raises(RuntimeError, match="Cannot read alias file"):
            GTOptWriter._load_alias_file(path)

    def test_non_dict_raises(self, tmp_path):
        path = tmp_path / "list.json"
        path.write_text(json.dumps(["a", "b"]))
        with pytest.raises(RuntimeError, match="flat JSON object"):
            GTOptWriter._load_alias_file(path)

    def test_non_string_values_raise(self, tmp_path):
        path = tmp_path / "numbers.json"
        path.write_text(json.dumps({"A": 1, "B": "ok"}))
        with pytest.raises(RuntimeError, match="flat JSON object"):
            GTOptWriter._load_alias_file(path)
