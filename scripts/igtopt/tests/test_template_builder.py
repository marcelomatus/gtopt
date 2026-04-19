# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for igtopt.template_builder."""

from __future__ import annotations

import pathlib

import pytest

from igtopt import template_builder as tb


# ---------------------------------------------------------------------------
# _find_repo_root
# ---------------------------------------------------------------------------


class TestFindRepoRoot:
    """Tests for _find_repo_root path-walking logic."""

    def test_find_repo_root_from_scripts_dir(self):
        """Starting from scripts/, finds the repo root containing include/gtopt."""
        scripts_dir = pathlib.Path(__file__).parent.parent.parent
        result = tb._find_repo_root(scripts_dir)
        assert (result / "include" / "gtopt").is_dir()

    def test_find_repo_root_from_igtopt_dir(self):
        """Starting from scripts/igtopt/, finds the repo root."""
        igtopt_dir = pathlib.Path(__file__).parent.parent
        result = tb._find_repo_root(igtopt_dir)
        assert (result / "include" / "gtopt").is_dir()

    def test_find_repo_root_fallback(self, tmp_path):
        """From a fresh tmp_path (no include/gtopt), returns that path resolved."""
        result = tb._find_repo_root(tmp_path)
        assert result == tmp_path.resolve()

    def test_find_repo_root_from_deeply_nested(self):
        """Starting from scripts/igtopt/tests/, finds the repo root."""
        tests_dir = pathlib.Path(__file__).parent
        result = tb._find_repo_root(tests_dir)
        assert (result / "include" / "gtopt").is_dir()


# ---------------------------------------------------------------------------
# parse_json_header_fields
# ---------------------------------------------------------------------------


class TestParseJsonHeaderFields:
    """Tests for parse_json_header_fields() header parsing."""

    def test_parse_json_header_fields_basic(self, tmp_path):
        """Header with two json_member entries extracts both names."""
        header = tmp_path / "test.hpp"
        header.write_text(
            'json_member<"field1", int> f1;\njson_member<"field2", std::string> f2;\n'
        )
        result = tb.parse_json_header_fields(header)
        assert result == ["field1", "field2"]

    def test_parse_json_header_fields_deduplication(self, tmp_path):
        """Duplicate field names are deduplicated, preserving first occurrence."""
        header = tmp_path / "test.hpp"
        header.write_text(
            'json_member<"uid", int> uid1;\n'
            'json_member<"uid", int> uid2;\n'
            'json_member<"name", std::string> n;\n'
        )
        result = tb.parse_json_header_fields(header)
        assert result == ["uid", "name"]

    def test_parse_json_header_fields_empty(self, tmp_path):
        """Header with no matching patterns returns empty list."""
        header = tmp_path / "test.hpp"
        header.write_text("struct Foo { int x; };\n")
        result = tb.parse_json_header_fields(header)
        assert not result


# ---------------------------------------------------------------------------
# parse_system_arrays
# ---------------------------------------------------------------------------


class TestParseSystemArrays:
    """Tests for parse_system_arrays() hpp parsing."""

    def test_parse_system_arrays_basic(self, tmp_path):
        """Synthetic hpp with two json_array_null entries extracts both names."""
        hpp = tmp_path / "json_system.hpp"
        hpp.write_text(
            'json_array_null<"bus_array", BusArray> bus_array;\n'
            'json_array_null<"generator_array", GenArray> gen_array;\n'
        )
        result = tb.parse_system_arrays(hpp)
        assert result == ["bus_array", "generator_array"]

    def test_parse_system_arrays_empty(self, tmp_path):
        """No json_array_null entries returns empty list."""
        hpp = tmp_path / "json_system.hpp"
        hpp.write_text("struct SystemJson { int x; };\n")
        result = tb.parse_system_arrays(hpp)
        assert not result


# ---------------------------------------------------------------------------
# parse_simulation_arrays
# ---------------------------------------------------------------------------


class TestParseSimulationArrays:
    """Tests for parse_simulation_arrays() hpp parsing."""

    def test_parse_simulation_arrays_basic(self, tmp_path):
        """Synthetic hpp extracts simulation array names."""
        hpp = tmp_path / "json_simulation.hpp"
        hpp.write_text(
            'json_array_null<"block_array", BlockArray> ba;\n'
            'json_array_null<"stage_array", StageArray> sa;\n'
        )
        result = tb.parse_simulation_arrays(hpp)
        assert result == ["block_array", "stage_array"]

    def test_parse_simulation_arrays_empty(self, tmp_path):
        """No matches returns empty list."""
        hpp = tmp_path / "json_simulation.hpp"
        hpp.write_text("// no array fields here\n")
        result = tb.parse_simulation_arrays(hpp)
        assert not result


# ---------------------------------------------------------------------------
# _build_workbook and _list_sheets – fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def repo_root():
    """Find the real repo root (has include/gtopt/)."""
    return tb._find_repo_root(pathlib.Path(__file__).parent)


@pytest.fixture(scope="module")
def header_dir(repo_root):
    """Return the include/gtopt header directory, skip if not found."""
    pytest.importorskip("openpyxl")
    hdir = repo_root / "include" / "gtopt"
    if not hdir.is_dir():
        pytest.skip("C++ header directory not found")
    return hdir


@pytest.fixture(scope="module")
def generated_workbook(tmp_path_factory, header_dir):
    """Generate the template workbook once and share across tests in the module."""
    openpyxl = pytest.importorskip("openpyxl")
    out = tmp_path_factory.mktemp("tmpl") / "test_template.xlsx"
    tb._build_workbook(out, header_dir)
    return openpyxl.load_workbook(out)


# ---------------------------------------------------------------------------
# _build_workbook
# ---------------------------------------------------------------------------


class TestBuildWorkbook:
    """Tests for _build_workbook() workbook generation."""

    def test_build_workbook_creates_file(self, tmp_path, header_dir):
        """_build_workbook writes a .xlsx file to the given path."""
        output = tmp_path / "template.xlsx"
        tb._build_workbook(output, header_dir)
        assert output.exists()
        assert output.stat().st_size > 0

    def test_build_workbook_has_introduction_sheet(self, generated_workbook):
        """Generated workbook contains the .introduction sheet."""
        assert ".introduction" in generated_workbook.sheetnames

    def test_build_workbook_has_options_sheet(self, generated_workbook):
        """Generated workbook contains the options sheet."""
        assert "options" in generated_workbook.sheetnames

    def test_build_workbook_has_simulation_sheets(self, generated_workbook):
        """Generated workbook contains at least the core simulation sheets."""
        required = {
            "block_array",
            "stage_array",
            "scenario_array",
            "phase_array",
            "scene_array",
        }
        sheets = set(generated_workbook.sheetnames)
        assert required.issubset(sheets), f"Missing sheets: {required - sheets}"

    def test_build_workbook_has_system_sheets(self, generated_workbook):
        """Generated workbook contains at least the core system sheets."""
        required = {"bus_array", "generator_array", "demand_array"}
        sheets = set(generated_workbook.sheetnames)
        assert required.issubset(sheets), f"Missing sheets: {required - sheets}"


# ---------------------------------------------------------------------------
# _list_sheets
# ---------------------------------------------------------------------------


class TestListSheets:
    """Tests for _list_sheets() stdout output."""

    def test_list_sheets_output_to_stdout(self, header_dir, capsys):
        """_list_sheets prints system sheet names to stdout."""
        tb._list_sheets(header_dir)
        captured = capsys.readouterr()
        assert "bus_array" in captured.out
        assert "generator_array" in captured.out

    def test_list_sheets_includes_simulation_sheets(self, header_dir, capsys):
        """_list_sheets stdout includes core simulation sheet names."""
        tb._list_sheets(header_dir)
        captured = capsys.readouterr()
        assert "block_array" in captured.out
        assert "stage_array" in captured.out


# ---------------------------------------------------------------------------
# --make-template CLI integration
# ---------------------------------------------------------------------------


class TestMakeTemplateCli:
    """Integration tests for the --make-template CLI path in igtopt.main()."""

    def test_make_template_cli_generates_file(self, tmp_path, header_dir):
        """--make-template with -j writes an xlsx file."""
        out_path = tmp_path / "t.xlsx"
        from igtopt.igtopt import main as igtopt_main

        igtopt_main(["--make-template", "-j", str(out_path)])
        assert out_path.exists()
        assert out_path.stat().st_size > 0

    def test_make_template_cli_list_sheets(self, header_dir, capsys):
        """--make-template --list-sheets prints sheet names without writing a file."""
        from igtopt.igtopt import main as igtopt_main

        igtopt_main(["--make-template", "--list-sheets"])
        captured = capsys.readouterr()
        assert "bus_array" in captured.out
        assert "block_array" in captured.out

    def test_make_template_cli_missing_header_dir(self, tmp_path):
        """--make-template with a nonexistent --header-dir exits with code 1."""
        from igtopt.igtopt import main as igtopt_main

        with pytest.raises(SystemExit) as exc_info:
            igtopt_main(
                [
                    "--make-template",
                    "--header-dir",
                    str(tmp_path / "nonexistent"),
                ]
            )
        assert exc_info.value.code == 1


# ---------------------------------------------------------------------------
# reservoir_seepage_array sheet content
# ---------------------------------------------------------------------------


class TestSddpOptionKeysSync:
    """Verify SDDP_OPTION_KEYS matches the C++ json_data_contract<SddpOptions>."""

    def test_sddp_keys_match_cpp_header(self, repo_root):
        """All fields in json_options.hpp SddpOptions appear in SDDP_OPTION_KEYS."""
        import re

        header = repo_root / "include" / "gtopt" / "json" / "json_sddp_options.hpp"
        if not header.exists():
            pytest.skip("json_sddp_options.hpp not found")

        text = header.read_text()
        # Extract the SddpOptions contract block
        sddp_match = re.search(r"json_data_contract<SddpOptions>.*?>;", text, re.DOTALL)
        assert sddp_match, "Could not find json_data_contract<SddpOptions>"

        block = sddp_match.group()
        # Find all json_*<"field_name", ...> entries
        cpp_fields = set(re.findall(r'json_\w+<"(\w+)"', block))
        assert cpp_fields, "No fields found in SddpOptions contract"

        missing = cpp_fields - tb.SDDP_OPTION_KEYS
        assert not missing, (
            f"C++ SddpOptions fields missing from SDDP_OPTION_KEYS: {missing}"
        )


class TestReservoirSeepageArraySheet:
    """Tests that the generated template has correct reservoir_seepage_array columns."""

    def test_reservoir_seepage_array_sheet_exists(self, generated_workbook):
        """Generated workbook contains a reservoir_seepage_array sheet."""
        assert "reservoir_seepage_array" in generated_workbook.sheetnames

    def test_reservoir_seepage_array_has_segments_column(self, generated_workbook):
        """reservoir_seepage_array sheet header row includes 'segments'."""
        ws = generated_workbook["reservoir_seepage_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "segments" in headers

    def test_reservoir_seepage_array_has_slope_column(self, generated_workbook):
        """reservoir_seepage_array sheet header row includes 'slope'."""
        ws = generated_workbook["reservoir_seepage_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "slope" in headers

    def test_reservoir_seepage_array_has_constant_column(self, generated_workbook):
        """reservoir_seepage_array sheet header row includes 'constant'."""
        ws = generated_workbook["reservoir_seepage_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "constant" in headers

    def test_reservoir_seepage_array_core_columns(self, generated_workbook):
        """reservoir_seepage_array sheet header contains all required columns."""
        ws = generated_workbook["reservoir_seepage_array"]
        headers = {cell.value for cell in next(ws.iter_rows(max_row=1))}
        required = {"uid", "name", "waterway", "reservoir"}
        assert required.issubset(headers), f"Missing columns: {required - headers}"


# ---------------------------------------------------------------------------
# simulation_mode in SDDP_OPTION_KEYS
# ---------------------------------------------------------------------------


class TestSimulationModeKey:
    """Verify simulation_mode is present in SDDP_OPTION_KEYS."""

    def test_simulation_mode_in_sddp_keys(self):
        """simulation_mode must be in SDDP_OPTION_KEYS."""
        assert "simulation_mode" in tb.SDDP_OPTION_KEYS

    def test_apertures_in_sddp_keys(self):
        """apertures must be in SDDP_OPTION_KEYS."""
        assert "apertures" in tb.SDDP_OPTION_KEYS


# ---------------------------------------------------------------------------
# Cascade option keys
# ---------------------------------------------------------------------------


class TestCascadeOptionKeys:
    """Verify CASCADE_OPTION_KEYS is exported and cascade_options is importable."""

    def test_cascade_option_keys_exists(self):
        """CASCADE_OPTION_KEYS is a frozenset (may be empty for hierarchical config)."""
        assert isinstance(tb.CASCADE_OPTION_KEYS, frozenset)

    def test_cascade_options_sync_with_cpp(self, repo_root):
        """CascadeOptions fields in json_options.hpp are well-formed."""
        import re

        header = repo_root / "include" / "gtopt" / "json" / "json_cascade_options.hpp"
        if not header.exists():
            pytest.skip("json_cascade_options.hpp not found")

        text = header.read_text()

        # Verify CascadeOptions contract exists
        match = re.search(r"json_data_contract<CascadeOptions>.*?>;", text, re.DOTALL)
        assert match, "Could not find json_data_contract<CascadeOptions>"

        block = match.group()
        cpp_fields = set(re.findall(r'json_\w+<"(\w+)"', block))
        expected = {"model_options", "sddp_options", "level_array"}
        assert expected == cpp_fields, (
            f"CascadeOptions fields mismatch: expected {expected}, got {cpp_fields}"
        )

    def test_cascade_level_fields_sync_with_cpp(self, repo_root):
        """CascadeLevel fields in json_options.hpp match expected structure."""
        import re

        header = repo_root / "include" / "gtopt" / "json" / "json_cascade_options.hpp"
        if not header.exists():
            pytest.skip("json_cascade_options.hpp not found")

        text = header.read_text()

        match = re.search(r"json_data_contract<CascadeLevel>.*?>;", text, re.DOTALL)
        assert match, "Could not find json_data_contract<CascadeLevel>"

        block = match.group()
        cpp_fields = set(re.findall(r'json_\w+<"(\w+)"', block))
        expected = {
            "uid",
            "name",
            "active",
            "model_options",
            "sddp_options",
            "transition",
        }
        assert expected == cpp_fields, (
            f"CascadeLevel fields mismatch: expected {expected}, got {cpp_fields}"
        )

    def test_cascade_transition_fields_sync_with_cpp(self, repo_root):
        """CascadeTransition fields in json_options.hpp match expected structure."""
        import re

        header = repo_root / "include" / "gtopt" / "json" / "json_cascade_options.hpp"
        if not header.exists():
            pytest.skip("json_cascade_options.hpp not found")

        text = header.read_text()

        match = re.search(
            r"json_data_contract<CascadeTransition>.*?>;", text, re.DOTALL
        )
        assert match, "Could not find json_data_contract<CascadeTransition>"

        block = match.group()
        cpp_fields = set(re.findall(r'json_\w+<"(\w+)"', block))
        expected = {
            "inherit_optimality_cuts",
            "inherit_targets",
            "target_rtol",
            "target_min_atol",
            "target_penalty",
            "optimality_dual_threshold",
        }
        assert expected == cpp_fields, (
            f"CascadeTransition fields mismatch: expected {expected}, got {cpp_fields}"
        )

    def test_cascade_level_solver_fields_sync_with_cpp(self, repo_root):
        """CascadeLevelMethod fields in json_options.hpp match expected structure."""
        import re

        header = repo_root / "include" / "gtopt" / "json" / "json_cascade_options.hpp"
        if not header.exists():
            pytest.skip("json_cascade_options.hpp not found")

        text = header.read_text()

        match = re.search(
            r"json_data_contract<CascadeLevelMethod>.*?>;", text, re.DOTALL
        )
        assert match, "Could not find json_data_contract<CascadeLevelMethod>"

        block = match.group()
        cpp_fields = set(re.findall(r'json_\w+<"(\w+)"', block))
        expected = {"max_iterations", "min_iterations", "apertures", "convergence_tol"}
        assert expected == cpp_fields, (
            f"CascadeLevelMethod fields mismatch: expected {expected}, got {cpp_fields}"
        )


# ---------------------------------------------------------------------------
# _nest_sub_options — cascade and simulation_mode
# ---------------------------------------------------------------------------


class TestNestSubOptionsCascade:
    """Tests for _nest_sub_options handling of cascade_options and simulation_mode."""

    def test_simulation_mode_nested_into_sddp(self):
        """simulation_mode is placed inside sddp_options by _nest_sub_options."""
        from igtopt.igtopt import _nest_sub_options

        flat = {"method": "sddp", "simulation_mode": True}
        result = _nest_sub_options(flat)
        assert "simulation_mode" not in result
        assert result["sddp_options"]["simulation_mode"] is True

    def test_cascade_options_passthrough(self):
        """cascade_options dict is passed through as-is."""
        from igtopt.igtopt import _nest_sub_options

        cascade = {
            "levels": [
                {
                    "name": "copper_plate",
                    "model_options": {"use_single_bus": True},
                    "sddp_options": {"max_iterations": 20},
                    "transition": {"inherit_optimality_cuts": True},
                }
            ]
        }
        flat = {"method": "sddp", "cascade_options": cascade}
        result = _nest_sub_options(flat)
        assert result["cascade_options"] == cascade
        assert result["cascade_options"]["levels"][0]["name"] == "copper_plate"

    def test_cascade_options_with_sddp_keys(self):
        """cascade_options coexists with flat SDDP keys."""
        from igtopt.igtopt import _nest_sub_options

        cascade = {
            "levels": [
                {
                    "name": "level_0",
                    "model_options": {"use_single_bus": True},
                }
            ]
        }
        flat = {
            "method": "sddp",
            "max_iterations": 100,
            "simulation_mode": False,
            "cascade_options": cascade,
        }
        result = _nest_sub_options(flat)
        assert result["cascade_options"] == cascade
        assert result["sddp_options"]["max_iterations"] == 100
        assert result["sddp_options"]["simulation_mode"] is False

    def test_apertures_nested_into_sddp(self):
        """apertures list is placed inside sddp_options by _nest_sub_options."""
        from igtopt.igtopt import _nest_sub_options

        flat = {"method": "sddp", "apertures": [1, 2, 3]}
        result = _nest_sub_options(flat)
        assert "apertures" not in result
        assert result["sddp_options"]["apertures"] == [1, 2, 3]


# ---------------------------------------------------------------------------
# _nest_sub_options — solver_options
# ---------------------------------------------------------------------------


class TestNestSubOptionsSolver:
    """Tests for _nest_sub_options handling of solver_options."""

    def test_solver_prefix_time_limit(self):
        """solver_time_limit is placed inside solver_options as time_limit."""
        from igtopt.igtopt import _nest_sub_options

        flat = {"solver_time_limit": 300.0}
        result = _nest_sub_options(flat)
        assert "solver_time_limit" not in result
        assert result["solver_options"]["time_limit"] == 300.0

    def test_solver_prefix_algorithm(self):
        """solver_algorithm is placed inside solver_options as algorithm."""
        from igtopt.igtopt import _nest_sub_options

        flat = {"solver_algorithm": 2}
        result = _nest_sub_options(flat)
        assert "solver_algorithm" not in result
        assert result["solver_options"]["algorithm"] == 2

    def test_solver_prefix_multiple_keys(self):
        """Multiple solver_ keys are all nested correctly."""
        from igtopt.igtopt import _nest_sub_options

        flat = {
            "solver_algorithm": 3,
            "solver_threads": 4,
            "solver_presolve": True,
            "solver_time_limit": 600.0,
            "solver_optimal_eps": 1e-8,
            "solver_feasible_eps": 1e-7,
            "solver_barrier_eps": 1e-6,
            "solver_log_level": 1,
            "solver_reuse_basis": False,
        }
        result = _nest_sub_options(flat)
        so = result["solver_options"]
        assert so["algorithm"] == 3
        assert so["threads"] == 4
        assert so["presolve"] is True
        assert so["time_limit"] == 600.0
        assert so["optimal_eps"] == 1e-8
        assert so["feasible_eps"] == 1e-7
        assert so["barrier_eps"] == 1e-6
        assert so["log_level"] == 1
        assert so["reuse_basis"] is False

    def test_solver_options_passthrough(self):
        """Pre-nested solver_options dict is passed through."""
        from igtopt.igtopt import _nest_sub_options

        nested = {"algorithm": 1, "time_limit": 120.0}
        flat = {"solver_options": nested}
        result = _nest_sub_options(flat)
        assert result["solver_options"]["algorithm"] == 1
        assert result["solver_options"]["time_limit"] == 120.0

    def test_solver_options_merged_with_prefix(self):
        """Pre-nested and prefixed solver options are merged."""
        from igtopt.igtopt import _nest_sub_options

        flat = {
            "solver_options": {"algorithm": 1},
            "solver_time_limit": 300.0,
        }
        result = _nest_sub_options(flat)
        assert result["solver_options"]["algorithm"] == 1
        assert result["solver_options"]["time_limit"] == 300.0

    def test_solver_coexists_with_sddp_and_monolithic(self):
        """solver_options coexists with sddp_options and monolithic_options."""
        from igtopt.igtopt import _nest_sub_options

        flat = {
            "solver_time_limit": 300.0,
            "monolithic_solve_mode": "relaxed",
        }
        result = _nest_sub_options(flat)
        assert result["solver_options"]["time_limit"] == 300.0
        assert result["monolithic_options"]["solve_mode"] == "relaxed"

    def test_unknown_solver_prefix_stays_toplevel(self):
        """Keys with solver_ prefix but not in SOLVER_OPTION_KEYS stay top-level."""
        from igtopt.igtopt import _nest_sub_options

        flat = {"method": "sddp", "solver_time_limit": 60.0}
        result = _nest_sub_options(flat)
        # method is a top-level option (not in SOLVER_OPTION_KEYS)
        assert result["method"] == "sddp"
        assert result["solver_options"]["time_limit"] == 60.0
