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
# filtration_array sheet content
# ---------------------------------------------------------------------------


class TestFiltrationArraySheet:
    """Tests that the generated template has correct filtration_array columns."""

    def test_filtration_array_sheet_exists(self, generated_workbook):
        """Generated workbook contains a filtration_array sheet."""
        assert "filtration_array" in generated_workbook.sheetnames

    def test_filtration_array_has_segments_column(self, generated_workbook):
        """filtration_array sheet header row includes 'segments'."""
        ws = generated_workbook["filtration_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "segments" in headers

    def test_filtration_array_has_slope_column(self, generated_workbook):
        """filtration_array sheet header row includes 'slope'."""
        ws = generated_workbook["filtration_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "slope" in headers

    def test_filtration_array_has_constant_column(self, generated_workbook):
        """filtration_array sheet header row includes 'constant'."""
        ws = generated_workbook["filtration_array"]
        headers = [cell.value for cell in next(ws.iter_rows(max_row=1))]
        assert "constant" in headers

    def test_filtration_array_core_columns(self, generated_workbook):
        """filtration_array sheet header contains all required columns."""
        ws = generated_workbook["filtration_array"]
        headers = {cell.value for cell in next(ws.iter_rows(max_row=1))}
        required = {"uid", "name", "waterway", "reservoir"}
        assert required.issubset(headers), f"Missing columns: {required - headers}"
