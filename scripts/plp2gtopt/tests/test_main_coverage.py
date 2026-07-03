"""Tests for main.py — CLI argument parsing, modes, and edge cases."""

import argparse
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt.main import (
    _parse_name_value_pairs,
    _SECTION_DEFAULTS,
    build_options,
    main,
    make_parser,
    signal_handler,
)


_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"


# ---------------------------------------------------------------------------
# _parse_name_value_pairs
# ---------------------------------------------------------------------------


class TestParseNameValuePairs:
    """Tests for the _parse_name_value_pairs helper."""

    def test_basic_pair(self):
        result = _parse_name_value_pairs("RAPEL:500")
        assert result == {"RAPEL": 500.0}

    def test_multiple_pairs(self):
        result = _parse_name_value_pairs("RAPEL:500,COLBUN:15000")
        assert result == {"RAPEL": 500.0, "COLBUN": 15000.0}

    def test_empty_string(self):
        result = _parse_name_value_pairs("")
        assert not result

    def test_whitespace_tokens(self):
        result = _parse_name_value_pairs(" A : 1.5 , B : 2.5 ")
        assert result == {"A": 1.5, "B": 2.5}

    def test_trailing_comma(self):
        result = _parse_name_value_pairs("A:1,")
        assert result == {"A": 1.0}

    def test_missing_colon_raises(self):
        with pytest.raises(ValueError, match="Invalid name:value pair"):
            _parse_name_value_pairs("NOCOLON")

    def test_invalid_number_raises(self):
        with pytest.raises(ValueError, match="Invalid numeric value"):
            _parse_name_value_pairs("NAME:abc")


# ---------------------------------------------------------------------------
# signal_handler
# ---------------------------------------------------------------------------


def test_signal_handler_exits():
    """signal_handler should call sys.exit(0)."""
    with pytest.raises(SystemExit) as exc_info:
        signal_handler(2, None)
    assert exc_info.value.code == 0


# ---------------------------------------------------------------------------
# _conf_defaults / _init_config
# ---------------------------------------------------------------------------


def test_conf_defaults_missing_section():
    """_conf_defaults returns empty dict when section is missing."""
    from plp2gtopt.main import _conf_defaults  # noqa: PLC0415

    with patch("plp2gtopt.main.load_config") as mock_cfg:
        mock_cfg.return_value = MagicMock()
        mock_cfg.return_value.has_section.return_value = False
        result = _conf_defaults()
    assert not result


def test_conf_defaults_with_section():
    """_conf_defaults returns items from the section."""
    from plp2gtopt.main import _conf_defaults  # noqa: PLC0415

    with patch("plp2gtopt.main.load_config") as mock_cfg:
        mock_inst = MagicMock()
        mock_inst.has_section.return_value = True
        mock_inst.items.return_value = [("key1", "val1")]
        mock_cfg.return_value = mock_inst
        result = _conf_defaults()
    assert result == {"key1": "val1"}


def test_init_config(tmp_path, capsys):
    """_init_config writes defaults and prints a message."""
    from plp2gtopt.main import _init_config  # noqa: PLC0415

    with patch("plp2gtopt.main.save_section") as mock_save, patch(
        "plp2gtopt.main.DEFAULT_CONFIG_PATH", tmp_path / ".gtopt.conf"
    ):
        _init_config()
    mock_save.assert_called_once()
    out = capsys.readouterr().out
    assert "Initialized" in out


# ---------------------------------------------------------------------------
# make_parser / build_options
# ---------------------------------------------------------------------------


class TestMakeParser:
    """Tests for make_parser and the argument parser it builds."""

    def test_parser_creation(self):
        with patch("plp2gtopt.main.load_config") as mock_cfg:
            mock_cfg.return_value = MagicMock()
            mock_cfg.return_value.has_section.return_value = False
            parser = make_parser()
        assert isinstance(parser, argparse.ArgumentParser)

    def test_parser_has_key_actions(self):
        with patch("plp2gtopt.main.load_config") as mock_cfg:
            mock_cfg.return_value = MagicMock()
            mock_cfg.return_value.has_section.return_value = False
            parser = make_parser()
        # Check that key arguments exist
        dests = {a.dest for a in parser._actions}  # noqa: SLF001
        assert "show_info" in dests
        assert "validate" in dests
        assert "init_config" in dests


# ---------------------------------------------------------------------------
# build_options
# ---------------------------------------------------------------------------


class TestBuildOptions:
    """Tests for build_options with various argument combinations."""

    @staticmethod
    def _make_args(**overrides):
        """Create a namespace with default argument values."""
        defaults = {
            "input_dir": Path("input"),
            "positional_input": None,
            "output_dir": None,
            "output_file": None,
            "name": None,
            "last_stage": -1,
            "last_time": -1,
            "compression": "snappy",
            "compression_level": 1,
            "output_format": "parquet",
            "input_format": None,
            "hydrologies": "all",
            "first_scenario": False,
            "show_simulation": False,
            "probability_factors": None,
            "discount_rate": 0.0,
            "management_factor": 0.0,
            "zip_output": False,
            "excel_output": False,
            "excel_file": None,
            "sys_version": "",
            "method": "cascade",
            "stages_phase": None,
            "num_apertures": None,
            "aperture_directory": None,
            "demand_fail_cost": 1000.0,
            "state_fail_cost": 1000.0,
            "scale_objective": 1000.0,
            "scale_theta": None,
            "use_single_bus": False,
            "use_kirchhoff": True,
            "kirchhoff_mode": "cycle_basis",
            "reserve_fail_cost": None,
            "use_line_losses": None,
            "line_losses_mode": "piecewise_direct",
            "plp_legacy": False,
            "cut_sharing_mode": None,
            "boundary_cuts_mode": None,
            "boundary_max_iterations": None,
            "no_boundary_cuts": False,
            "alias_file": None,
            "stationary_tol": None,
            "stationary_window": None,
            "reservoir_scale_mode": "auto",
            "reservoir_energy_scale": None,
            "auto_reservoir_energy_scale": False,
            "battery_energy_scale": None,
            "auto_battery_energy_scale": False,
            "variable_scales_file": None,
            "soft_emin_cost": None,
            "soft_storage_bounds": True,
            "vert_cost_cap": 500.0,
            "drop_spillway_waterway": False,
            "vrebemb_as_sink": False,
            "embed_reservoir_constraints": False,
            "expand_water_rights": False,
            "expand_lng": True,
            "expand_ror": True,
            "ror_as_reservoirs": None,
            "ror_as_reservoirs_file": None,
            "run_check": True,
            "auto_detect_tech": True,
            "tech_overrides": None,
            "pasada_mode": "flow-turbine",
            "log_file": None,
            "log_level": "INFO",
        }
        defaults.update(overrides)
        return argparse.Namespace(**defaults)

    def test_basic_options(self):
        args = self._make_args(input_dir=_PLPMin1Bus)
        opts = build_options(args)
        assert opts["input_dir"] == _PLPMin1Bus
        assert opts["hydrologies"] == "all"
        assert opts["pasada_hydro"] is True

    def test_first_scenario(self):
        args = self._make_args(first_scenario=True)
        opts = build_options(args)
        assert opts["hydrologies"] == "first"

    def test_output_dir_inferred_from_plp_prefix(self):
        args = self._make_args(
            positional_input=Path("plp_case_2y"),
            input_dir=None,
        )
        opts = build_options(args)
        assert opts["output_dir"] == Path("gtopt_case_2y")

    def test_optional_solver_options(self):
        # ``hot_start_cuts`` retired in 2026-05 — internal hot-start
        # cuts now travel via the typed Parquet path on the gtopt side.
        args = self._make_args(
            cut_sharing_mode="none",
            boundary_cuts_mode="forward",
            boundary_max_iterations=100,
            no_boundary_cuts=True,
            stationary_tol=0.01,
            stationary_window=5,
            reserve_fail_cost=500.0,
            use_line_losses=True,
        )
        opts = build_options(args)
        # Explicit --cut-sharing-mode wins over the multicut default.
        assert opts["cut_sharing_mode"] == "none"
        assert opts["boundary_cuts_mode"] == "forward"
        assert opts["boundary_max_iterations"] == 100
        assert opts["no_boundary_cuts"] is True
        assert opts["stationary_tol"] == 0.01
        assert opts["stationary_window"] == 5
        # §11.10: legacy CLI name reserve_fail_cost maps to canonical
        # gtopt key reserve_shortage_cost.
        assert opts["model_options"]["reserve_shortage_cost"] == 500.0
        assert opts["model_options"]["use_line_losses"] is True

    def test_reservoir_energy_scale_parsing(self):
        args = self._make_args(reservoir_energy_scale="RAPEL:500,COLBUN:15000")
        opts = build_options(args)
        assert opts["reservoir_energy_scale"] == {"RAPEL": 500.0, "COLBUN": 15000.0}

    def test_battery_energy_scale_parsing(self):
        args = self._make_args(battery_energy_scale="BESS1:100")
        opts = build_options(args)
        assert opts["battery_energy_scale"] == {"BESS1": 100.0}

    def test_tech_overrides(self):
        with patch("plp2gtopt.tech_detect.load_overrides", return_value={"A": "solar"}):
            args = self._make_args(tech_overrides="A:solar")
            opts = build_options(args)
        assert opts["tech_overrides"] == {"A": "solar"}

    def test_variable_scales_file(self):
        args = self._make_args(variable_scales_file="scales.json")
        opts = build_options(args)
        assert opts["variable_scales_file"] == "scales.json"

    def test_alias_file_argument(self):
        args = self._make_args(alias_file=Path("alias.json"))
        opts = build_options(args)
        assert opts["alias_file"] == Path("alias.json")

    def test_alias_file_default_absent(self):
        """alias_file key is absent from opts when CLI flag not provided."""
        args = self._make_args()
        opts = build_options(args)
        assert "alias_file" not in opts

    def test_pasada_mode_hydro(self):
        args = self._make_args(pasada_mode="hydro")
        opts = build_options(args)
        assert opts["pasada_mode"] == "hydro"
        assert opts["pasada_hydro"] is True

    def test_pasada_mode_profile(self):
        args = self._make_args(pasada_mode="profile")
        opts = build_options(args)
        assert opts["pasada_mode"] == "profile"
        assert opts["pasada_hydro"] is False

    def test_pasada_mode_none_defaults_to_flow_turbine(self):
        args = self._make_args(pasada_mode=None)
        opts = build_options(args)
        assert opts["pasada_mode"] == "flow-turbine"

    def test_line_losses_mode_explicit_override(self):
        # Default is now piecewise_direct; an explicit mode must still win.
        args = self._make_args(line_losses_mode="bidirectional")
        opts = build_options(args)
        assert opts["model_options"]["line_losses_mode"] == "bidirectional"

    def test_line_losses_mode_defaults_to_piecewise_direct(self):
        # plp2gtopt defaults to the PLP-faithful piecewise_direct model.
        args = self._make_args()
        opts = build_options(args)
        assert opts["model_options"]["line_losses_mode"] == "piecewise_direct"

    def test_sddp_method_fast_path_defaults(self):
        # method=sddp gets the benchmarked iterative fast-path defaults:
        # lp_reduction + dual aperture warm-start over all-apertures-per-phase
        # chunks (-1) + dual simplex w/ advanced basis on fwd/bwd passes.
        args = self._make_args(method="sddp")
        opts = build_options(args)
        assert opts["model_options"]["lp_reduction"] is True
        assert opts["sddp_options"]["aperture_solve_mode"] == "warm"
        assert opts["sddp_options"]["aperture_chunk_size"] == 0
        assert opts["sddp_options"]["forward_solver_options"] == {
            "algorithm": "dual",
            "advanced_basis": True,
        }
        assert opts["sddp_options"]["backward_solver_options"] == {
            "algorithm": "dual",
        }

    def test_cascade_method_fast_path_defaults(self):
        # method=cascade now also gets the iterative fast-path defaults: they
        # land on the TOP-LEVEL sddp_options / model_options, which become the
        # cascade base options inherited by every level (m_base_opts_).
        args = self._make_args(method="cascade")
        opts = build_options(args)
        assert opts["model_options"]["lp_reduction"] is True
        assert opts["sddp_options"]["aperture_solve_mode"] == "warm"
        assert opts["sddp_options"]["aperture_chunk_size"] == 0
        assert opts["sddp_options"]["forward_solver_options"]["algorithm"] == "dual"
        assert opts["sddp_options"]["backward_solver_options"]["algorithm"] == "dual"

    def test_monolithic_method_no_fast_path_defaults(self):
        # monolithic (non-iterative) gets none of the iterative fast-path knobs.
        args = self._make_args(method="monolithic")
        opts = build_options(args)
        assert "lp_reduction" not in opts["model_options"]
        assert "sddp_options" not in opts
        # monolithic must NOT get the multicut cut-sharing default.
        assert "cut_sharing_mode" not in opts

    def test_sddp_fast_path_invariant(self):
        # --solver-invariant selects the HYBRID config (off==compress
        # reproducibility, far cheaper than all-barrier):
        #   * forward  — barrier + primal crossover (deterministic vertex
        #     duals) + presolve ON (cold barrier benefits from presolve);
        #   * backward — warm dual + presolve OFF (fast; lp_reduction does the
        #     build-time structural reduction in place of CPLEX presolve).
        args = self._make_args(method="sddp", solver_invariant=True)
        opts = build_options(args)
        assert opts["sddp_options"]["forward_solver_options"] == {
            "algorithm": "barrier",
            "crossover": "primal",
            "presolve": True,
        }
        assert opts["sddp_options"]["backward_solver_options"] == {
            "algorithm": "dual",
            "advanced_basis": True,
            "presolve": False,
        }
        # lp_reduction stays on (substitutes for CPLEX presolve on the backward)
        assert opts["model_options"]["lp_reduction"] is True
        # invariant couples with compress (memory savings WITH reproducibility)
        assert opts["sddp_options"]["low_memory_mode"] == "compress"
        # never leaks the plp2gtopt-meta flag into the gtopt JSON options
        assert "solver_invariant" not in opts["sddp_options"]

    def test_sddp_fast_path_default_not_invariant(self):
        # Without --solver-invariant the default stays dual + warm-start, and
        # memory mode defaults to OFF (the reference oracle: off never diverges;
        # dual+warm is correct there).  compress is opt-in via --solver-invariant.
        args = self._make_args(method="sddp")
        opts = build_options(args)
        assert opts["sddp_options"]["forward_solver_options"]["algorithm"] == "dual"
        assert opts["sddp_options"]["low_memory_mode"] == "off"

    def test_sddp_method_defaults_cut_sharing_multicut(self):
        # PLP-faithful sharing: sddp defaults cut_sharing_mode to multicut.
        args = self._make_args(method="sddp")
        opts = build_options(args)
        assert opts["cut_sharing_mode"] == "multicut"

    def test_cascade_method_defaults_cut_sharing_multicut(self):
        # cascade inherits the same multicut default on the top-level opts.
        args = self._make_args(method="cascade")
        opts = build_options(args)
        assert opts["cut_sharing_mode"] == "multicut"

    def test_cut_sharing_default_overridable(self):
        # An explicit --cut-sharing-mode wins over the multicut default.
        args = self._make_args(method="sddp", cut_sharing_mode="none")
        opts = build_options(args)
        assert opts["cut_sharing_mode"] == "none"

    def test_sddp_fast_path_defaults_overridable(self):
        # An explicit --aperture-chunk-size wins over the warm default of -1.
        args = self._make_args(method="sddp", aperture_chunk_size=4)
        opts = build_options(args)
        assert opts["sddp_options"]["aperture_chunk_size"] == 4
        assert opts["sddp_options"]["aperture_solve_mode"] == "warm"

    def test_plp_legacy_bundles_method_and_losses(self):
        # Empty argv → neither --method nor --line-losses-mode is explicit,
        # so --plp-legacy fills method + line_losses_mode + use_line_losses.
        with patch.object(sys, "argv", ["plp2gtopt", "--plp-legacy"]):
            args = self._make_args(plp_legacy=True)
            opts = build_options(args)
        assert opts["method"] == "sddp"
        assert opts["model_options"]["line_losses_mode"] == "piecewise_direct"
        assert opts["model_options"]["use_line_losses"] is True

    def test_plp_legacy_respects_explicit_use_line_losses(self):
        # User passed --use-line-losses → bundle must not touch the value,
        # but still bundles method + line_losses_mode.
        with patch.object(sys, "argv", ["plp2gtopt", "--plp-legacy", "-L"]):
            args = self._make_args(plp_legacy=True, use_line_losses=True)
            opts = build_options(args)
        assert opts["model_options"]["use_line_losses"] is True
        assert opts["method"] == "sddp"
        assert opts["model_options"]["line_losses_mode"] == "piecewise_direct"

    def test_plp_legacy_respects_explicit_method(self):
        # User passes --method=monolithic explicitly → legacy bundle
        # must NOT override it, but still sets line_losses_mode.
        with patch.object(
            sys, "argv", ["plp2gtopt", "--plp-legacy", "--method=monolithic"]
        ):
            args = self._make_args(plp_legacy=True, method="monolithic")
            opts = build_options(args)
        assert opts["method"] == "monolithic"
        assert opts["model_options"]["line_losses_mode"] == "piecewise_direct"

    def test_plp_legacy_respects_explicit_losses_mode(self):
        with patch.object(
            sys,
            "argv",
            ["plp2gtopt", "--plp-legacy", "--line-losses-mode", "piecewise"],
        ):
            args = self._make_args(plp_legacy=True, line_losses_mode="piecewise")
            opts = build_options(args)
        assert opts["method"] == "sddp"  # still bundled
        assert opts["model_options"]["line_losses_mode"] == "piecewise"

    def test_plp_legacy_forces_pasada_mode_flow_turbine(self):
        """--plp-legacy overrides the default `auto` pasada_mode."""
        with patch.object(sys, "argv", ["plp2gtopt", "--plp-legacy"]):
            args = self._make_args(plp_legacy=True, pasada_mode="auto")
            opts = build_options(args)
        assert opts["pasada_mode"] == "flow-turbine"

    def test_plp_legacy_respects_explicit_pasada_mode(self):
        """An explicit --pasada-mode wins over the legacy bundle."""
        with patch.object(
            sys, "argv", ["plp2gtopt", "--plp-legacy", "--pasada-mode", "hydro"]
        ):
            args = self._make_args(plp_legacy=True, pasada_mode="hydro")
            opts = build_options(args)
        assert opts["pasada_mode"] == "hydro"

    def test_plp_legacy_off_leaves_defaults(self):
        with patch.object(sys, "argv", ["plp2gtopt"]):
            args = self._make_args(plp_legacy=False)
            opts = build_options(args)
        assert opts["method"] == "cascade"
        # piecewise_direct is the plp2gtopt default (not part of the legacy bundle)
        assert opts["model_options"]["line_losses_mode"] == "piecewise_direct"


# ---------------------------------------------------------------------------
# main() — CLI mode branches
# ---------------------------------------------------------------------------


class TestMainCLI:
    """Tests for main() covering various CLI mode branches."""

    def test_init_config_mode(self):
        """--init-config calls _init_config and returns."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main._init_config"
        ) as mock_init:
            ns = argparse.Namespace(init_config=True)
            mock_parser.return_value.parse_args.return_value = ns
            main(["--init-config"])
        mock_init.assert_called_once()

    def test_show_info_mode(self, tmp_path):
        """--info calls display_plp_info."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.display_plp_info"
        ) as mock_info:
            ns = argparse.Namespace(
                init_config=False,
                show_info=True,
                input_dir=_PLPMin1Bus,
                positional_input=None,
                log_level="INFO",
                last_stage=-1,
                hydrologies="all",
            )
            mock_parser.return_value.parse_args.return_value = ns
            main(["--info", "-i", str(_PLPMin1Bus)])
        mock_info.assert_called_once()

    def test_show_info_error_handling(self):
        """--info handles RuntimeError gracefully."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.display_plp_info", side_effect=RuntimeError("bad")
        ):
            ns = argparse.Namespace(
                init_config=False,
                show_info=True,
                input_dir=Path("/nonexistent"),
                positional_input=None,
                log_level="INFO",
                last_stage=-1,
                hydrologies="all",
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(["--info", "-i", "/nonexistent"])
            assert exc_info.value.code == 1

    def test_tech_list_mode(self, capsys):
        """--tech-list prints technology types and returns."""
        with patch("plp2gtopt.main.make_parser") as mock_parser:
            ns = argparse.Namespace(
                init_config=False,
                show_info=False,
                tech_list=True,
                input_dir=Path("input"),
                positional_input=None,
                log_level="INFO",
            )
            mock_parser.return_value.parse_args.return_value = ns
            main(["--tech-list"])
        out = capsys.readouterr().out
        assert "Known generator technology types:" in out

    def test_validate_mode_success(self):
        """--validate calls validate_plp_case and exits 0 on success."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.validate_plp_case", return_value=True
        ) as mock_val, patch("plp2gtopt.main.build_options", return_value={}):
            ns = TestBuildOptions._make_args(
                validate=True,
                show_info=False,
                tech_list=False,
                init_config=False,
                variable_scales_template=False,
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(["--validate", "-i", str(_PLPMin1Bus)])
            assert exc_info.value.code == 0
        mock_val.assert_called_once()

    def test_validate_mode_failure(self):
        """--validate exits 1 on validation failure."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.validate_plp_case", return_value=False
        ), patch("plp2gtopt.main.build_options", return_value={}):
            ns = TestBuildOptions._make_args(
                validate=True,
                show_info=False,
                tech_list=False,
                init_config=False,
                variable_scales_template=False,
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(["--validate", "-i", str(_PLPMin1Bus)])
            assert exc_info.value.code == 1

    def test_variable_scales_template_mode(self):
        """--variable-scales-template calls print_variable_scales_template."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.print_variable_scales_template", return_value=0
        ) as mock_tmpl, patch("plp2gtopt.main.build_options", return_value={}):
            ns = TestBuildOptions._make_args(
                validate=False,
                show_info=False,
                tech_list=False,
                init_config=False,
                variable_scales_template=True,
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(["--variable-scales-template"])
            assert exc_info.value.code == 0
        mock_tmpl.assert_called_once()

    def test_convert_error_handling_no_args(self, capsys):
        """main shows usage hint when convert fails and no args were given."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.convert_plp_case",
            side_effect=RuntimeError("fail"),
        ), patch("plp2gtopt.main.build_options", return_value={}), patch.object(
            sys, "argv", ["plp2gtopt"]
        ):
            ns = TestBuildOptions._make_args(
                validate=False,
                show_info=False,
                tech_list=False,
                init_config=False,
                variable_scales_template=False,
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(None)
            assert exc_info.value.code == 1
        err = capsys.readouterr().err
        assert "error:" in err
        assert "Usage:" in err

    def test_convert_error_handling_with_args(self, capsys):
        """main prints error without usage when args were given."""
        with patch("plp2gtopt.main.make_parser") as mock_parser, patch(
            "plp2gtopt.main.convert_plp_case",
            side_effect=FileNotFoundError("missing"),
        ), patch("plp2gtopt.main.build_options", return_value={}), patch.object(
            sys, "argv", ["plp2gtopt", "-i", "somedir"]
        ):
            ns = TestBuildOptions._make_args(
                validate=False,
                show_info=False,
                tech_list=False,
                init_config=False,
                variable_scales_template=False,
            )
            mock_parser.return_value.parse_args.return_value = ns
            with pytest.raises(SystemExit) as exc_info:
                main(["-i", "somedir"])
            assert exc_info.value.code == 1
        err = capsys.readouterr().err
        assert "error:" in err


# ---------------------------------------------------------------------------
# Version fallback
# ---------------------------------------------------------------------------


def test_version_fallback():
    """__version__ is 'dev' when package is not installed."""
    # The version is set at module level during import
    from plp2gtopt.main import __version__  # noqa: PLC0415

    assert isinstance(__version__, str)
    assert len(__version__) > 0


# ---------------------------------------------------------------------------
# _SECTION_DEFAULTS
# ---------------------------------------------------------------------------


def test_section_defaults_keys():
    """_SECTION_DEFAULTS contains expected configuration keys."""
    assert "compression" in _SECTION_DEFAULTS
    assert "method" in _SECTION_DEFAULTS
    assert "reservoir_scale_mode" in _SECTION_DEFAULTS
