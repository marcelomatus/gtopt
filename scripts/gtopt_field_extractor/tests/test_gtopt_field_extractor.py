# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for gtopt_field_extractor."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import pytest

from gtopt_field_extractor.gtopt_field_extractor import (
    ALL_ELEMENTS,
    FieldInfo,
    StructInfo,
    _cpp_to_json_type,
    _extract_units,
    _find_repo_root,
    _is_required,
    _md_field_row,
    main,
    parse_header,
    render_html,
    render_markdown,
)

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

SAMPLE_HEADER = """\
#pragma once

/**
 * @brief A test bus element
 */
struct TestBus
{
  Uid uid {0}; ///< Unique identifier
  Name name {""}; ///< Bus name
  OptReal voltage {std::nullopt}; ///< Voltage magnitude [kV]
  STBRealFieldSched load {0.0}; ///< Active power load [MW]
};

/**
 * @brief A test generator
 * @details Represents a thermal generating unit.
 */
struct TestGenerator
{
  Uid uid {0}; ///< Generator identifier
  Name name {""}; ///< Generator name
  SingleId bus_a {0}; ///< Connected bus
  OptReal pmax {std::nullopt}; ///< Maximum power output [MW]
  OptReal pmin {std::nullopt}; ///< Minimum power output [MW]
  OptBool active {std::nullopt}; ///< Whether generator is active
};
"""

EMPTY_HEADER = """\
#pragma once
// This header has no struct definitions.
"""

FORWARD_DECL_HEADER = """\
#pragma once
struct ForwardDeclared;
struct AnotherForward;
"""

NESTED_BRACES_HEADER = """\
#pragma once
/// @brief Struct with nested braces in defaults
struct Nested
{
  Name name {"default"}; ///< The name
  OptReal value {std::nullopt}; ///< Some value [m/s]
};
"""


@pytest.fixture()
def sample_header_file(tmp_path: Path) -> Path:
    """Write the sample header to a temp file and return its path."""
    p = tmp_path / "test_bus.hpp"
    p.write_text(SAMPLE_HEADER, encoding="utf-8")
    return p


@pytest.fixture()
def empty_header_file(tmp_path: Path) -> Path:
    p = tmp_path / "empty.hpp"
    p.write_text(EMPTY_HEADER, encoding="utf-8")
    return p


@pytest.fixture()
def forward_decl_file(tmp_path: Path) -> Path:
    p = tmp_path / "forward.hpp"
    p.write_text(FORWARD_DECL_HEADER, encoding="utf-8")
    return p


@pytest.fixture()
def nested_header_file(tmp_path: Path) -> Path:
    p = tmp_path / "nested.hpp"
    p.write_text(NESTED_BRACES_HEADER, encoding="utf-8")
    return p


@pytest.fixture()
def sample_structs() -> dict[str, StructInfo]:
    """Build a small structs_by_name dict for renderer tests."""
    bus = StructInfo(
        struct_name="Bus",
        file_path="bus.hpp",
        brief="A bus node",
        details="",
        fields=[
            FieldInfo("uid", "Uid", "integer", "", True, "Unique id"),
            FieldInfo("name", "Name", "string", "", True, "Bus name"),
            FieldInfo("voltage", "OptReal", "number", "kV", False, "Voltage"),
        ],
    )
    gen = StructInfo(
        struct_name="Generator",
        file_path="generator.hpp",
        brief="A generator",
        details="Thermal unit details.",
        fields=[
            FieldInfo("uid", "Uid", "integer", "", True, "Generator id"),
            FieldInfo("pmax", "OptReal", "number", "MW", False, "Max power"),
        ],
    )
    return {"Bus": bus, "Generator": gen}


# ---------------------------------------------------------------------------
# Tests: FieldInfo and StructInfo dataclasses
# ---------------------------------------------------------------------------


class TestDataclasses:
    def test_field_info_construction(self):
        f = FieldInfo(
            name="uid",
            cpp_type="Uid",
            json_type="integer",
            units="",
            required=True,
            description="Unique identifier",
        )
        assert f.name == "uid"
        assert f.cpp_type == "Uid"
        assert f.json_type == "integer"
        assert f.units == ""
        assert f.required is True
        assert f.description == "Unique identifier"

    def test_struct_info_construction(self):
        s = StructInfo(
            struct_name="Bus",
            file_path="bus.hpp",
            brief="A bus",
            details="Details here",
        )
        assert s.struct_name == "Bus"
        assert s.file_path == "bus.hpp"
        assert s.brief == "A bus"
        assert s.details == "Details here"
        assert not s.fields

    def test_struct_info_with_fields(self):
        f = FieldInfo("x", "Real", "number", "MW", False, "power")
        s = StructInfo("Gen", "gen.hpp", "brief", "", fields=[f])
        assert len(s.fields) == 1
        assert s.fields[0].name == "x"


# ---------------------------------------------------------------------------
# Tests: _extract_units
# ---------------------------------------------------------------------------


class TestExtractUnits:
    def test_with_units(self):
        desc, units = _extract_units("Voltage magnitude [kV]")
        assert units == "kV"
        assert desc == "Voltage magnitude"

    def test_without_units(self):
        desc, units = _extract_units("Unique identifier")
        assert units == ""
        assert desc == "Unique identifier"

    def test_empty_string(self):
        desc, units = _extract_units("")
        assert units == ""
        assert desc == ""

    def test_units_with_slash(self):
        desc, units = _extract_units("Flow rate [m3/s]")
        assert units == "m3/s"
        assert desc == "Flow rate"

    def test_units_at_end_with_trailing(self):
        desc, units = _extract_units("Some field, [MW]")
        assert units == "MW"
        # Trailing comma/space before [MW] gets stripped
        assert desc == "Some field"

    def test_multiple_brackets(self):
        # Only first bracket match is used
        _, units = _extract_units("Field [MW] extra [kV]")
        assert units == "MW"


# ---------------------------------------------------------------------------
# Tests: _is_required
# ---------------------------------------------------------------------------


class TestIsRequired:
    def test_uid_required(self):
        assert _is_required("Uid", "uid") is True

    def test_name_required(self):
        assert _is_required("Name", "name") is True

    def test_bus_a_required(self):
        assert _is_required("SingleId", "bus_a") is True

    def test_bus_b_required(self):
        assert _is_required("SingleId", "bus_b") is True

    def test_junction_required(self):
        assert _is_required("SingleId", "junction") is True

    def test_waterway_required(self):
        assert _is_required("SingleId", "waterway") is True

    def test_generator_required(self):
        assert _is_required("SingleId", "generator") is True

    def test_demand_required(self):
        assert _is_required("SingleId", "demand") is True

    def test_battery_required(self):
        assert _is_required("SingleId", "battery") is True

    def test_reserve_zones_required(self):
        assert _is_required("SingleId", "reserve_zones") is True

    def test_optional_type_not_required(self):
        assert _is_required("OptReal", "pmax") is False

    def test_optional_bool_not_required(self):
        assert _is_required("OptBool", "active") is False

    def test_non_optional_scalar_not_required(self):
        # Non-optional scalars with defaults are not required
        assert _is_required("Real", "cost") is False

    def test_fieldsched_not_required(self):
        assert _is_required("STBRealFieldSched", "load") is False

    def test_unknown_field_not_required(self):
        assert _is_required("SomeType", "some_field") is False


# ---------------------------------------------------------------------------
# Tests: _cpp_to_json_type
# ---------------------------------------------------------------------------


class TestCppToJsonType:
    def test_uid(self):
        assert _cpp_to_json_type("Uid") == "integer"

    def test_name(self):
        assert _cpp_to_json_type("Name") == "string"

    def test_real(self):
        assert _cpp_to_json_type("Real") == "number"

    def test_bool(self):
        assert _cpp_to_json_type("Bool") == "boolean"

    def test_opt_real(self):
        assert _cpp_to_json_type("OptReal") == "number"

    def test_opt_active(self):
        assert _cpp_to_json_type("OptActive") == "integer|boolean"

    def test_fieldsched(self):
        assert _cpp_to_json_type("STBRealFieldSched") == "number|array|string"

    def test_opt_fieldsched(self):
        assert _cpp_to_json_type("OptSTBRealFieldSched") == "number|array|string"

    def test_single_id(self):
        assert _cpp_to_json_type("SingleId") == "integer|string"

    def test_unknown_type_returns_itself(self):
        assert _cpp_to_json_type("FooBar") == "FooBar"

    def test_whitespace_stripped(self):
        assert _cpp_to_json_type("  Real  ") == "number"


# ---------------------------------------------------------------------------
# Tests: _find_repo_root
# ---------------------------------------------------------------------------


class TestFindRepoRoot:
    def test_finds_repo_root(self, tmp_path: Path):
        # Create a fake repo structure
        (tmp_path / "include" / "gtopt").mkdir(parents=True)
        subdir = tmp_path / "scripts" / "something"
        subdir.mkdir(parents=True)
        result = _find_repo_root(subdir)
        assert result == tmp_path

    def test_returns_start_when_not_found(self, tmp_path: Path):
        subdir = tmp_path / "deep" / "path"
        subdir.mkdir(parents=True)
        result = _find_repo_root(subdir)
        # Returns the resolved start directory when not found
        assert result == subdir.resolve()

    def test_finds_from_root_itself(self, tmp_path: Path):
        (tmp_path / "include" / "gtopt").mkdir(parents=True)
        result = _find_repo_root(tmp_path)
        assert result == tmp_path


# ---------------------------------------------------------------------------
# Tests: parse_header
# ---------------------------------------------------------------------------


class TestParseHeader:
    def test_parses_two_structs(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        assert len(structs) == 2
        names = [s.struct_name for s in structs]
        assert "TestBus" in names
        assert "TestGenerator" in names

    def test_testbus_fields(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        bus = next(s for s in structs if s.struct_name == "TestBus")
        assert len(bus.fields) == 4
        field_names = [f.name for f in bus.fields]
        assert field_names == ["uid", "name", "voltage", "load"]

    def test_testbus_brief(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        bus = next(s for s in structs if s.struct_name == "TestBus")
        assert bus.brief == "A test bus element"

    def test_testgenerator_details(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        gen = next(s for s in structs if s.struct_name == "TestGenerator")
        assert "thermal generating unit" in gen.details.lower()

    def test_testgenerator_fields(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        gen = next(s for s in structs if s.struct_name == "TestGenerator")
        assert len(gen.fields) == 6
        uid_field = gen.fields[0]
        assert uid_field.name == "uid"
        assert uid_field.required is True

    def test_voltage_field_units(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        bus = next(s for s in structs if s.struct_name == "TestBus")
        voltage = next(f for f in bus.fields if f.name == "voltage")
        assert voltage.units == "kV"
        assert voltage.required is False
        assert voltage.json_type == "number"

    def test_load_field_type(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        bus = next(s for s in structs if s.struct_name == "TestBus")
        load = next(f for f in bus.fields if f.name == "load")
        assert load.json_type == "number|array|string"
        assert load.units == "MW"

    def test_bus_a_required(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        gen = next(s for s in structs if s.struct_name == "TestGenerator")
        bus_a = next(f for f in gen.fields if f.name == "bus_a")
        assert bus_a.required is True

    def test_active_optional(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        gen = next(s for s in structs if s.struct_name == "TestGenerator")
        active = next(f for f in gen.fields if f.name == "active")
        assert active.required is False

    def test_empty_header(self, empty_header_file: Path):
        structs = parse_header(empty_header_file)
        assert not structs

    def test_forward_declarations_skipped(self, forward_decl_file: Path):
        structs = parse_header(forward_decl_file)
        assert not structs

    def test_file_path_is_basename(self, sample_header_file: Path):
        structs = parse_header(sample_header_file)
        for s in structs:
            assert s.file_path == "test_bus.hpp"

    def test_nested_braces(self, nested_header_file: Path):
        structs = parse_header(nested_header_file)
        assert len(structs) == 1
        s = structs[0]
        assert s.struct_name == "Nested"
        assert len(s.fields) == 2
        value = next(f for f in s.fields if f.name == "value")
        assert value.units == "m/s"


# ---------------------------------------------------------------------------
# Tests: _md_field_row
# ---------------------------------------------------------------------------


class TestMdFieldRow:
    def test_required_field(self):
        f = FieldInfo("uid", "Uid", "integer", "", True, "Unique id")
        row = _md_field_row(f)
        assert "| `uid` |" in row
        assert "| `Uid` |" in row
        assert "| **Yes** |" in row
        # No units produces an em-dash character
        assert "\u2014" in row

    def test_optional_field_with_units(self):
        f = FieldInfo("voltage", "OptReal", "number", "kV", False, "Voltage mag")
        row = _md_field_row(f)
        assert "| No |" in row
        assert "`kV`" in row

    def test_pipe_in_description_escaped(self):
        f = FieldInfo("x", "Real", "number", "", False, "A | B")
        row = _md_field_row(f)
        assert "A \\| B" in row


# ---------------------------------------------------------------------------
# Tests: render_markdown
# ---------------------------------------------------------------------------


class TestRenderMarkdown:
    def test_contains_title(self, sample_structs):
        md = render_markdown(sample_structs, ["Bus", "Generator"])
        assert "# gtopt JSON Field Reference" in md

    def test_contains_bus_section(self, sample_structs):
        md = render_markdown(sample_structs, ["Bus"])
        assert "## Bus" in md
        assert "A bus node" in md

    def test_contains_generator_section(self, sample_structs):
        md = render_markdown(sample_structs, ["Generator"])
        assert "## Generator" in md
        assert "A generator" in md
        assert "Thermal unit details." in md

    def test_table_header(self, sample_structs):
        md = render_markdown(sample_structs, ["Bus"])
        assert "| Field | C++ Type | JSON Type | Units | Required | Description |" in md

    def test_missing_element_skipped(self, sample_structs):
        md = render_markdown(sample_structs, ["NonExistent"])
        assert "## NonExistent" not in md

    def test_field_rows_present(self, sample_structs):
        md = render_markdown(sample_structs, ["Bus"])
        assert "`uid`" in md
        assert "`name`" in md
        assert "`voltage`" in md

    def test_empty_elements_list(self, sample_structs):
        md = render_markdown(sample_structs, [])
        assert "# gtopt JSON Field Reference" in md
        assert "## Bus" not in md

    def test_anchor_tag(self, sample_structs):
        md = render_markdown(sample_structs, ["Bus"])
        assert "{#bus}" in md


# ---------------------------------------------------------------------------
# Tests: render_html
# ---------------------------------------------------------------------------


class TestRenderHtml:
    def test_contains_html_structure(self, sample_structs):
        html = render_html(sample_structs, ["Bus"])
        assert "<!DOCTYPE html>" in html
        assert "</html>" in html
        assert "<title>gtopt JSON Field Reference</title>" in html

    def test_contains_bus_section(self, sample_structs):
        html = render_html(sample_structs, ["Bus"])
        assert '<h2 id="bus">Bus</h2>' in html
        assert "<em>A bus node</em>" in html

    def test_contains_generator_details(self, sample_structs):
        html = render_html(sample_structs, ["Generator"])
        assert "Thermal unit details." in html

    def test_toc_links(self, sample_structs):
        html = render_html(sample_structs, ["Bus", "Generator"])
        assert '<a href="#bus">Bus</a>' in html
        assert '<a href="#generator">Generator</a>' in html

    def test_required_field_markup(self, sample_structs):
        html = render_html(sample_structs, ["Bus"])
        assert '<span class="req">Yes</span>' in html

    def test_units_in_code_tag(self, sample_structs):
        html = render_html(sample_structs, ["Bus"])
        assert "<code>kV</code>" in html

    def test_missing_element_skipped(self, sample_structs):
        html = render_html(sample_structs, ["NonExistent"])
        assert "NonExistent" not in html.split("</nav>")[1]

    def test_table_rows_present(self, sample_structs):
        html = render_html(sample_structs, ["Generator"])
        assert "<code>uid</code>" in html
        assert "<code>pmax</code>" in html


# ---------------------------------------------------------------------------
# Tests: main() CLI
# ---------------------------------------------------------------------------


class TestMain:
    def test_main_with_tmp_header_dir_md(self, tmp_path: Path, capsys):
        """main() with a custom header dir outputs markdown to stdout."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        ret = main([str(hdr), "--format", "md", "--elements", "TestBus"])
        assert ret == 0
        out = capsys.readouterr().out
        assert "# gtopt JSON Field Reference" in out
        assert "TestBus" in out

    def test_main_with_tmp_header_dir_html(self, tmp_path: Path, capsys):
        """main() with --format html produces HTML output."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        ret = main([str(hdr), "--format", "html", "--elements", "TestBus"])
        assert ret == 0
        out = capsys.readouterr().out
        assert "<!DOCTYPE html>" in out
        assert "TestBus" in out

    def test_main_output_to_file(self, tmp_path: Path):
        """main() with --output writes to a file."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        outfile = tmp_path / "output.md"
        ret = main([str(hdr), "--output", str(outfile)])
        assert ret == 0
        assert outfile.exists()
        content = outfile.read_text(encoding="utf-8")
        assert "# gtopt JSON Field Reference" in content

    def test_main_output_html_to_file(self, tmp_path: Path):
        """main() with --format html --output writes HTML to a file."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        outfile = tmp_path / "output.html"
        ret = main([str(hdr), "--format", "html", "--output", str(outfile)])
        assert ret == 0
        content = outfile.read_text(encoding="utf-8")
        assert "<!DOCTYPE html>" in content

    def test_main_elements_filter(self, tmp_path: Path, capsys):
        """main() with --elements filters output to specific structs."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        # Request only TestBus (not in ALL_ELEMENTS but parsed)
        ret = main([str(hdr), "--elements", "TestBus"])
        assert ret == 0
        out = capsys.readouterr().out
        assert "TestBus" in out
        assert "TestGenerator" not in out

    def test_main_nonexistent_dir(self, tmp_path: Path, capsys):
        """main() returns 1 for a nonexistent header directory."""
        ret = main([str(tmp_path / "no_such_dir")])
        assert ret == 1
        err = capsys.readouterr().err
        assert "not found" in err

    def test_main_empty_header_dir(self, tmp_path: Path, capsys):
        """main() with an empty dir produces a header-only markdown."""
        hdr = tmp_path / "empty_headers"
        hdr.mkdir()
        ret = main([str(hdr)])
        assert ret == 0
        out = capsys.readouterr().out
        assert "# gtopt JSON Field Reference" in out

    def test_main_auto_detect_header_dir(self, capsys):
        """main() without header_dir auto-detects include/gtopt from repo."""
        # This test uses the real repo -- only run if the include dir exists.
        repo_root = Path("/home/marce/git/gtopt")
        include_dir = repo_root / "include" / "gtopt"
        if not include_dir.is_dir():
            pytest.skip("Real repo include/gtopt not available")
        # Patch __file__ to point inside the repo so _find_repo_root works
        with patch(
            "gtopt_field_extractor.gtopt_field_extractor.__file__",
            str(
                repo_root
                / "scripts"
                / "gtopt_field_extractor"
                / "gtopt_field_extractor.py"
            ),
        ):
            ret = main([])
            assert ret == 0
            out = capsys.readouterr().out
            assert "# gtopt JSON Field Reference" in out
            # Should have parsed some real structs
            assert "Bus" in out

    def test_main_real_headers_html(self, capsys):
        """Integration: main() with real headers produces valid HTML."""
        repo_root = Path("/home/marce/git/gtopt")
        include_dir = repo_root / "include" / "gtopt"
        if not include_dir.is_dir():
            pytest.skip("Real repo include/gtopt not available")
        ret = main([str(include_dir), "--format", "html"])
        assert ret == 0
        out = capsys.readouterr().out
        assert "<!DOCTYPE html>" in out
        assert "</html>" in out

    def test_main_written_file_message(self, tmp_path: Path, capsys):
        """main() prints 'Written to ...' on stderr when --output is used."""
        hdr = tmp_path / "headers"
        hdr.mkdir()
        (hdr / "sample.hpp").write_text(SAMPLE_HEADER, encoding="utf-8")
        outfile = tmp_path / "out.md"
        main([str(hdr), "--output", str(outfile)])
        err = capsys.readouterr().err
        assert "Written to" in err


# ---------------------------------------------------------------------------
# Tests: edge cases in parse_header
# ---------------------------------------------------------------------------


class TestParseHeaderEdgeCases:
    def test_struct_without_doxygen(self, tmp_path: Path):
        """A struct without any /** @brief */ still gets parsed."""
        hdr = tmp_path / "no_doc.hpp"
        hdr.write_text(
            "#pragma once\nstruct Plain\n{\n  Uid uid {0}; ///< The uid\n};\n",
            encoding="utf-8",
        )
        structs = parse_header(hdr)
        assert len(structs) == 1
        assert structs[0].struct_name == "Plain"
        assert structs[0].brief == ""

    def test_field_without_comment_skipped(self, tmp_path: Path):
        """Fields without ///< comments are not extracted."""
        hdr = tmp_path / "no_comment.hpp"
        hdr.write_text(
            "#pragma once\n"
            "struct Foo\n"
            "{\n"
            "  Uid uid {0}; ///< The uid\n"
            "  Real value {1.0};\n"  # no ///< comment
            "};\n",
            encoding="utf-8",
        )
        structs = parse_header(hdr)
        assert len(structs) == 1
        assert len(structs[0].fields) == 1
        assert structs[0].fields[0].name == "uid"

    def test_multiple_structs_in_one_file(self, tmp_path: Path):
        hdr = tmp_path / "multi.hpp"
        hdr.write_text(
            "#pragma once\n"
            "struct Alpha\n{\n"
            '  Name name {""}; ///< Alpha name\n'
            "};\n\n"
            "struct Beta\n{\n"
            '  Name name {""}; ///< Beta name\n'
            "};\n",
            encoding="utf-8",
        )
        structs = parse_header(hdr)
        assert len(structs) == 2
        names = {s.struct_name for s in structs}
        assert names == {"Alpha", "Beta"}

    def test_single_line_brief_comment(self, tmp_path: Path):
        hdr = tmp_path / "single_brief.hpp"
        hdr.write_text(
            "#pragma once\n"
            "/// @brief Single line brief\n"
            "struct SingleBrief\n"
            "{\n"
            "  Uid uid {0}; ///< Id\n"
            "};\n",
            encoding="utf-8",
        )
        structs = parse_header(hdr)
        assert len(structs) == 1
        assert structs[0].brief == "Single line brief"

    def test_no_hpp_files_in_dir(self, tmp_path: Path, capsys):
        """main() with a dir containing only .txt files produces no struct output."""
        hdr = tmp_path / "txt_only"
        hdr.mkdir()
        (hdr / "readme.txt").write_text("not a header", encoding="utf-8")
        ret = main([str(hdr)])
        assert ret == 0


# ---------------------------------------------------------------------------
# Tests: ALL_ELEMENTS constant
# ---------------------------------------------------------------------------


class TestAllElements:
    def test_all_elements_not_empty(self):
        assert len(ALL_ELEMENTS) > 0

    def test_all_elements_contains_key_types(self):
        for name in ["Bus", "Generator", "Demand", "Line", "Battery", "Options"]:
            assert name in ALL_ELEMENTS

    def test_all_elements_starts_with_options(self):
        assert ALL_ELEMENTS[0] == "Options"
