"""Tests for gtopt_writer.py — uncovered lines and edge cases."""

import csv
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
        "compression": "snappy",
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

    def test_process_options_forward_sampling(self, tmp_path):
        """process_options pipes forward_sampling_mode into sddp_options."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["forward_sampling_mode"] = "resampled"
        writer.process_options(opts)
        sddp = writer.planning["options"].get("sddp_options", {})
        assert sddp.get("forward_sampling_mode") == "resampled"

    def test_process_options_forward_sampling_unset(self, tmp_path):
        """Unset forward_sampling_mode emits nothing (gtopt default wins)."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options(_make_opts(tmp_path))
        sddp = writer.planning["options"].get("sddp_options", {})
        assert "forward_sampling_mode" not in sddp

    def test_process_options_integer_cuts(self, tmp_path):
        """process_options pipes integer_cuts_mode into sddp_options."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["integer_cuts_mode"] = "strengthened"
        writer.process_options(opts)
        sddp = writer.planning["options"].get("sddp_options", {})
        assert sddp.get("integer_cuts_mode") == "strengthened"

    def test_process_options_integer_cuts_unset(self, tmp_path):
        """Unset integer_cuts_mode emits nothing (gtopt default wins)."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options(_make_opts(tmp_path))
        sddp = writer.planning["options"].get("sddp_options", {})
        assert "integer_cuts_mode" not in sddp

    def test_process_options_backward_solver_threads_default(self, tmp_path):
        """Default (sddp method) gets the iterative fast-path dual backward solver.

        The threads field is left to the C++ default (no `threads` key); the
        iterative fast-path adds `algorithm: dual` (see process_options).
        """
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options(_make_opts(tmp_path))
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["backward_solver_options"] == {"algorithm": "dual"}

    def test_process_options_emits_multicut_default(self, tmp_path):
        """Writer-direct callers (no CLI) also get cut_sharing_mode=multicut.

        The iterative fast-path defaults multicut in process_options itself,
        not only in main.build_options, so a raw-opts caller emits it too.
        An explicit cut_sharing_mode still wins (see test above).
        """
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        writer.process_options(_make_opts(tmp_path))
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp.get("cut_sharing_mode") == "multicut"

    def test_process_options_backward_solver_threads_override(self, tmp_path):
        """`backward_solver_threads` sets threads; fast-path still adds dual."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path)
        opts["backward_solver_threads"] = 4
        writer.process_options(opts)
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["backward_solver_options"] == {"threads": 4, "algorithm": "dual"}


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


# ``_build_stage_to_phase_map`` was retired in 2026-05 alongside the
# hot-start CSV writer; the coverage tests it backed are dropped here.


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

    @staticmethod
    def _make_planos_writer(tmp_path):
        """Writer with a 2-plane planos fixture (NVarPhi = 2)."""
        mock_planos = MagicMock()
        mock_planos.cuts = [
            {"iteration": 1, "scene": 1, "rhs": 100.0, "coefficients": {"R1": -4.0}},
            {"iteration": 1, "scene": 2, "rhs": 200.0, "coefficients": {"R1": -8.0}},
        ]
        mock_planos.reservoir_names = ["R1"]
        mock_planos.reservoir_fescala = {}
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {"reservoir_array": []},
            "simulation": {"scenario_array": [{"uid": 1}, {"uid": 2}]},
        }
        writer._build_cut_water_values = lambda: {}
        opts = _make_opts(tmp_path)
        return writer, opts

    @staticmethod
    def _read_rhs(csv_path):
        with open(csv_path, encoding="utf-8") as fh:
            return [float(row["rhs"]) for row in csv.DictReader(fh)]

    def test_default_mode_is_phi_expectation_with_raw_csv(self, tmp_path):
        """Without --boundary-cuts-mode the writer defaults to
        phi_expectation and emits the CSV RAW (no 1/NVarPhi
        pre-division) — gtopt's p_s/NVarPhi column pricing carries the
        probability composition instead."""
        writer, opts = self._make_planos_writer(tmp_path)
        writer.process_boundary_cuts(opts)
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["boundary_cuts_mode"] == "phi_expectation"
        assert sddp["boundary_cuts_file"] == "boundary_cuts.csv"
        rhs = self._read_rhs(Path(opts["output_dir"]) / "boundary_cuts.csv")
        assert rhs == [100.0, 200.0]  # raw — NOT divided by NVarPhi=2

    def test_explicit_combined_keeps_nvarphi_predivision(self, tmp_path):
        """--boundary-cuts-mode combined keeps the legacy 1/NVarPhi
        pre-division — existing combined-mode cases stay numerically
        unchanged."""
        writer, opts = self._make_planos_writer(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        writer.process_boundary_cuts(opts)
        sddp = writer.planning["options"]["sddp_options"]
        assert sddp["boundary_cuts_mode"] == "combined"
        rhs = self._read_rhs(Path(opts["output_dir"]) / "boundary_cuts.csv")
        assert rhs == [50.0, 100.0]  # divided by NVarPhi = max(scene) = 2

    def test_phi_expectation_governs_terminal(self, tmp_path):
        """cuts_govern_terminal applies under phi_expectation exactly as
        under combined — the loaded FCF cuts are the sole terminal-value
        mechanism (PLP CFUE behaviour)."""
        writer, opts = self._make_planos_writer(tmp_path)
        writer.planning["system"]["reservoir_array"] = [
            {"name": "R1", "efin": 10.0, "efin_cost": 5.0},
        ]
        writer._build_cut_water_values = lambda: {"R1": 4.0}
        writer.process_boundary_cuts(opts)
        res = writer.planning["system"]["reservoir_array"][0]
        assert "efin" not in res and "efin_cost" not in res

    def test_cuts_govern_terminal_strips_efin(self, tmp_path):
        """--cuts-govern-terminal drops efin/efin_cost from cut-covered
        reservoirs (combined mode) so the loaded cuts govern the
        terminal value (PLP CFUE behaviour), leaving non-cut
        reservoirs untouched."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100, "scene": 1}]
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [
                    {"name": "ELTORO", "efin": 3777.0, "efin_cost": 411400.0},
                    {"name": "NOCUT", "efin": 50.0, "efin_cost": 100.0},
                ]
            },
            "simulation": {"scenario_array": [{"uid": 1}]},
        }
        # Cut-covered set = ELTORO only.
        writer._build_cut_water_values = lambda: {"ELTORO": 374000.0}
        opts = _make_opts(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        opts["cuts_govern_terminal"] = True
        writer.process_boundary_cuts(opts)
        res = {r["name"]: r for r in writer.planning["system"]["reservoir_array"]}
        assert "efin" not in res["ELTORO"] and "efin_cost" not in res["ELTORO"]
        # Non-cut reservoir keeps its hard efin bound.
        assert res["NOCUT"]["efin"] == 50.0

    def test_cuts_govern_terminal_on_by_default(self, tmp_path):
        """plp2gtopt replicates PLP: cut-covered reservoirs are
        cuts-governed by DEFAULT (efin/efin_cost stripped) even without
        the flag, in combined mode."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100, "scene": 1}]
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [
                    {"name": "ELTORO", "efin": 3777.0, "efin_cost": 411400.0},
                ]
            },
            "simulation": {"scenario_array": [{"uid": 1}]},
        }
        writer._build_cut_water_values = lambda: {"ELTORO": 374000.0}
        opts = _make_opts(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        # No cuts_govern_terminal key -> defaults to True.
        writer.process_boundary_cuts(opts)
        res = writer.planning["system"]["reservoir_array"][0]
        assert "efin" not in res and "efin_cost" not in res

    def test_cuts_govern_terminal_cfue_false_keeps_hard_efin(self, tmp_path):
        """A CFUE=F reservoir in the cut set keeps its efin (hard
        vol_end>=EmbVFin bound, PLP volfinem.f) and only sheds the
        soft-slack price — CFUE=T reservoirs drop both."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100, "scene": 1}]
        central = MagicMock()
        central.centrals = [
            {"name": "ELTORO", "cfue": True},
            {"name": "PEHUENCHE", "cfue": False},
        ]
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos, "central_parser": central}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [
                    {"name": "ELTORO", "efin": 3777.0, "efin_cost": 411400.0},
                    {"name": "PEHUENCHE", "efin": 133.0, "efin_cost": 1584.0},
                ]
            },
            "simulation": {"scenario_array": [{"uid": 1}]},
        }
        writer._build_cut_water_values = lambda: {
            "ELTORO": 374000.0,
            "PEHUENCHE": 1440.0,
        }
        opts = _make_opts(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        writer.process_boundary_cuts(opts)
        res = {r["name"]: r for r in writer.planning["system"]["reservoir_array"]}
        # CFUE=T -> cuts govern (both gone).
        assert "efin" not in res["ELTORO"] and "efin_cost" not in res["ELTORO"]
        # CFUE=F -> hard vol_end>=EmbVFin (efin kept, price dropped).
        assert res["PEHUENCHE"]["efin"] == 133.0
        assert "efin_cost" not in res["PEHUENCHE"]

    def test_cuts_govern_terminal_maintenance_override_drops_efin(self, tmp_path):
        """A CFUE=F reservoir with a plpmanem.dat entry covering the LAST
        stage loses its efin — PLP's ``LeeManEmb`` (leemanem.f) overwrites
        ``EmbVFin`` at the last stage, so the maintenance emin governs the
        terminal.  A CFUE=F reservoir without maintenance keeps its vfin."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100, "scene": 1}]
        central = MagicMock()
        central.centrals = [
            {"name": "PEHUENCHE", "cfue": False},
            {"name": "PANGUE", "cfue": False},
        ]
        # PANGUE's maintenance profile covers the last stage (52);
        # PEHUENCHE has no plpmanem entry at all.
        manem = MagicMock()
        manem.get_manem_by_name = lambda name: (
            {"name": "PANGUE", "stage": [1, 2, 52]} if name == "PANGUE" else None
        )
        stages = MagicMock()
        stages.num_stages = 52
        parser = MagicMock()
        parser.parsed_data = {
            "planos_parser": mock_planos,
            "central_parser": central,
            "manem_parser": manem,
            "stage_parser": stages,
        }
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [
                    {"name": "PEHUENCHE", "efin": 121.6, "efin_cost": 1584.0},
                    {"name": "PANGUE", "efin": 67.9, "efin_cost": 536.8},
                ]
            },
            "simulation": {"scenario_array": [{"uid": 1}]},
        }
        # CFUE=F reservoirs carry no cut coefficients (leeplaem only reads
        # them for CFUE=T), so they are absent from the cut water values.
        writer._build_cut_water_values = lambda: {}
        opts = _make_opts(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        writer.process_boundary_cuts(opts)
        res = {r["name"]: r for r in writer.planning["system"]["reservoir_array"]}
        # No maintenance → hard vol_end>=EmbVFin survives.
        assert res["PEHUENCHE"]["efin"] == 121.6
        # Last-stage maintenance override → efin dropped (maintenance emin
        # governs, matching PLP LeeManEmb).
        assert "efin" not in res["PANGUE"]
        assert "efin_cost" not in res["PANGUE"]

    def test_cuts_govern_terminal_disabled_keeps_efin(self, tmp_path):
        """--no-cuts-govern-terminal (False) restores the legacy soft
        slack: cut-covered reservoirs keep efin/efin_cost."""
        mock_planos = MagicMock()
        mock_planos.cuts = [{"stage": 1, "rhs": 100, "scene": 1}]
        parser = MagicMock()
        parser.parsed_data = {"planos_parser": mock_planos}
        writer = GTOptWriter(parser)
        writer.planning = {
            "options": {},
            "system": {
                "reservoir_array": [
                    {"name": "ELTORO", "efin": 3777.0, "efin_cost": 411400.0},
                ]
            },
            "simulation": {"scenario_array": [{"uid": 1}]},
        }
        writer._build_cut_water_values = lambda: {"ELTORO": 374000.0}
        opts = _make_opts(tmp_path)
        opts["boundary_cuts_mode"] = "combined"
        opts["cuts_govern_terminal"] = False
        writer.process_boundary_cuts(opts)
        res = writer.planning["system"]["reservoir_array"][0]
        assert res["efin"] == 3777.0 and res["efin_cost"] == 411400.0


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
        """process_options accepts legacy reserve_fail_cost (renamed to
        reserve_shortage_cost in §11.10)."""
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
        assert mo["reserve_shortage_cost"] == 500.0
        assert mo["use_line_losses"] is True


class TestScaleObjectiveMethodAware:
    """``scale_objective`` is method-aware (commit ab041592f).

    The C++ default is 1.0 for sddp/cascade (it is the dominant LP
    basis-condition contributor once Benders cuts accumulate) and 1000
    for monolithic.  plp2gtopt's CLI/conf default is the monolithic
    1000, so the writer must emit 1.0 for sddp/cascade while honoring a
    deliberate non-1000 override verbatim for any method.
    """

    @pytest.mark.parametrize("method", ["sddp", "cascade", "cascade-reduced"])
    def test_sddp_cascade_default_becomes_one(self, tmp_path, method):
        """The monolithic 1000 default is rewritten to 1.0 for sddp/cascade."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, f"so_{method.replace('-', '_')}")
        opts["method"] = method
        # The conf/CLI default arrives in model_options as 1000.
        opts["model_options"] = {"scale_objective": 1000.0}
        writer.process_options(opts)
        assert writer.planning["options"]["model_options"]["scale_objective"] == 1.0

    def test_cascade_propagates_into_cascade_options(self, tmp_path):
        """The 1.0 value also lands in the cascade-level model_options base."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "so_casc_prop")
        opts["method"] = "cascade"
        opts["model_options"] = {"scale_objective": 1000.0}
        writer.process_options(opts)
        casc = writer.planning["options"]["cascade_options"]
        assert casc["model_options"]["scale_objective"] == 1.0

    def test_monolithic_keeps_1000(self, tmp_path):
        """Monolithic retains the 1000 value (matches its C++ default)."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "so_mono")
        opts["method"] = "monolithic"
        opts["model_options"] = {"scale_objective": 1000.0}
        writer.process_options(opts)
        mo = writer.planning["options"]["model_options"]
        assert mo["scale_objective"] == 1000.0

    def test_explicit_override_forwarded_for_cascade(self, tmp_path):
        """A deliberate non-1000 value is honored verbatim, even for cascade."""
        parser = PLPParser({"input_dir": _PLPMin1Bus})
        parser.parse_all()
        writer = GTOptWriter(parser)
        opts = _make_opts(tmp_path, "so_override")
        opts["method"] = "cascade"
        opts["model_options"] = {"scale_objective": 500.0}
        writer.process_options(opts)
        mo = writer.planning["options"]["model_options"]
        assert mo["scale_objective"] == 500.0


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
