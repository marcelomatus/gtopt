"""Tests for gtopt_diagram – the network topology diagram generator.

Covers:
- ``FilterOptions`` defaults and construction
- ``TopologyBuilder`` public properties (eff_agg, eff_vthresh, auto_info)
- ``model_to_visjs`` output shape
- ``auto_voltage_threshold`` public function
- ``main()`` CLI entry-point (argparse, --help, rendering paths)
- ``--show`` flag logic and display_diagram behaviour
- Default auto-display (no --output given)
"""

import json
from pathlib import Path
from unittest import mock

import pytest

from gtopt_diagram import gtopt_diagram as gd

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

try:
    import graphviz as _graphviz  # noqa: F401

    _HAS_GRAPHVIZ = True
except ImportError:
    _HAS_GRAPHVIZ = False

_skip_no_graphviz = pytest.mark.skipif(
    not _HAS_GRAPHVIZ,
    reason="graphviz Python package not installed",
)

# ---------------------------------------------------------------------------
# Minimal planning fixtures (inline JSON, no file I/O)
# ---------------------------------------------------------------------------

_IEEE9_JSON = {
    "options": {"use_kirchhoff": True, "scale_objective": 1000},
    "system": {
        "name": "ieee9b",
        "bus_array": [{"uid": i, "name": f"B{i}", "kv": 345} for i in range(1, 10)],
        "generator_array": [
            {"uid": 1, "name": "G1", "bus": 1, "gcost": 20, "pmax": 250},
            {"uid": 2, "name": "G2", "bus": 2, "gcost": 35, "pmax": 300},
            {"uid": 3, "name": "G3", "bus": 3, "gcost": 30, "pmax": 270},
        ],
        "demand_array": [
            {"uid": 1, "name": "D5", "bus": 5, "lmax": 125},
            {"uid": 2, "name": "D7", "bus": 7, "lmax": 100},
            {"uid": 3, "name": "D9", "bus": 9, "lmax": 90},
        ],
        "line_array": [
            {"uid": 1, "name": "L14", "bus_a": 1, "bus_b": 4, "reactance": 0.0576},
            {"uid": 2, "name": "L49", "bus_a": 4, "bus_b": 9, "reactance": 0.1008},
            {"uid": 3, "name": "L45", "bus_a": 4, "bus_b": 5, "reactance": 0.0720},
        ],
    },
}

_MINI_PLANNING = {
    "system": {
        "name": "mini",
        "bus_array": [
            {"uid": 1, "name": "B1", "kv": 220},
            {"uid": 2, "name": "B2", "kv": 33},
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        "demand_array": [{"uid": 1, "name": "D1", "bus": 2, "lmax": 80}],
        "line_array": [{"uid": 1, "name": "L12", "bus_a": 1, "bus_b": 2}],
    }
}


# ---------------------------------------------------------------------------
# FilterOptions
# ---------------------------------------------------------------------------


class TestFilterOptions:
    def test_defaults(self):
        fo = gd.FilterOptions()
        assert fo.aggregate == "auto"
        assert fo.voltage_threshold == 0.0
        assert fo.no_generators is False
        assert fo.compact is False

    def test_custom(self):
        fo = gd.FilterOptions(
            aggregate="bus", voltage_threshold=220.0, no_generators=True, compact=True
        )
        assert fo.aggregate == "bus"
        assert fo.voltage_threshold == 220.0
        assert fo.no_generators is True
        assert fo.compact is True


# ---------------------------------------------------------------------------
# TopologyBuilder – basic build and public properties
# ---------------------------------------------------------------------------


class TestTopologyBuilder:
    def test_build_returns_graph_model(self):
        builder = gd.TopologyBuilder(_IEEE9_JSON)
        model = builder.build()
        assert model is not None
        assert len(model.nodes) > 0
        assert len(model.edges) > 0

    def test_public_properties_set_after_build(self):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_MINI_PLANNING, opts=fo)
        builder.build()
        assert isinstance(builder.eff_agg, str)
        assert isinstance(builder.eff_vthresh, float)
        assert builder.auto_info is None  # auto mode not used

    def test_auto_info_set_in_auto_mode(self):
        fo = gd.FilterOptions(aggregate="auto")
        builder = gd.TopologyBuilder(_IEEE9_JSON, opts=fo)
        builder.build()
        if builder.auto_info is not None:
            assert len(builder.auto_info) == 3

    def test_no_generators_flag(self):
        fo = gd.FilterOptions(aggregate="none", no_generators=True)
        builder = gd.TopologyBuilder(_IEEE9_JSON, opts=fo)
        model = builder.build()
        gen_nodes = [n for n in model.nodes if n.kind == "gen"]
        assert not gen_nodes

    def test_subsystem_electrical(self):
        builder = gd.TopologyBuilder(_IEEE9_JSON, subsystem="electrical")
        model = builder.build()
        assert len(model.nodes) > 0

    def test_eff_vthresh_zero_by_default(self):
        fo = gd.FilterOptions(aggregate="none", voltage_threshold=0.0)
        builder = gd.TopologyBuilder(_MINI_PLANNING, opts=fo)
        builder.build()
        assert builder.eff_vthresh == 0.0

    def test_explicit_voltage_threshold_preserved(self):
        fo = gd.FilterOptions(aggregate="type", voltage_threshold=100.0)
        builder = gd.TopologyBuilder(_MINI_PLANNING, opts=fo)
        builder.build()
        assert builder.eff_vthresh == 100.0


# ---------------------------------------------------------------------------
# model_to_visjs
# ---------------------------------------------------------------------------


class TestModelToVisjs:
    def test_returns_nodes_and_edges(self):
        builder = gd.TopologyBuilder(
            _IEEE9_JSON, opts=gd.FilterOptions(aggregate="none")
        )
        model = builder.build()
        result = gd.model_to_visjs(model)
        assert "nodes" in result
        assert "edges" in result
        assert isinstance(result["nodes"], list)
        assert isinstance(result["edges"], list)

    def test_nodes_have_required_vis_fields(self):
        builder = gd.TopologyBuilder(
            _MINI_PLANNING, opts=gd.FilterOptions(aggregate="none")
        )
        model = builder.build()
        visjs = gd.model_to_visjs(model)
        for node in visjs["nodes"]:
            assert "id" in node
            assert "label" in node

    def test_edges_have_from_to(self):
        builder = gd.TopologyBuilder(
            _IEEE9_JSON, opts=gd.FilterOptions(aggregate="none")
        )
        model = builder.build()
        visjs = gd.model_to_visjs(model)
        for edge in visjs["edges"]:
            assert "from" in edge
            assert "to" in edge

    def test_empty_model_returns_empty_lists(self):
        builder = gd.TopologyBuilder(
            {"system": {}}, opts=gd.FilterOptions(aggregate="none")
        )
        model = builder.build()
        result = gd.model_to_visjs(model)
        assert not result["nodes"]
        assert not result["edges"]


# ---------------------------------------------------------------------------
# auto_voltage_threshold (public function)
# ---------------------------------------------------------------------------


class TestAutoVoltageThreshold:
    def test_returns_zero_for_small_system(self):
        buses = [{"uid": i, "kv": 345} for i in range(10)]
        thresh = gd.auto_voltage_threshold(buses, [], max_buses=64)
        assert thresh == 0.0

    def test_returns_float_for_large_mixed_system(self):
        buses = [{"uid": i, "kv": 345} for i in range(50)]
        buses += [{"uid": 50 + i, "kv": 110} for i in range(50)]
        thresh = gd.auto_voltage_threshold(buses, [], max_buses=64)
        assert isinstance(thresh, float)
        assert thresh >= 0.0

    def test_single_voltage_level_returns_zero(self):
        buses = [{"uid": i, "kv": 345} for i in range(200)]
        thresh = gd.auto_voltage_threshold(buses, [], max_buses=64)
        assert thresh == 0.0


# ---------------------------------------------------------------------------
# CLI entry-point (main)
# ---------------------------------------------------------------------------


class TestMain:
    def test_help_exits_cleanly(self):
        with pytest.raises(SystemExit) as exc:
            gd.main(["--help"])
        assert exc.value.code == 0

    def test_missing_file_exits_nonzero(self, tmp_path):
        rc = gd.main([str(tmp_path / "nonexistent.json")])
        assert rc != 0

    def test_render_mermaid_to_stdout(self, tmp_path, capsys):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc = gd.main([str(json_path), "--format", "mermaid", "--aggregate", "none"])
        assert rc == 0
        out = capsys.readouterr().out
        assert "graph" in out or "flowchart" in out

    @_skip_no_graphviz
    def test_render_graphviz_missing_dot_binary(self, tmp_path):
        """render_graphviz raises SystemExit with an install hint when dot is missing."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))

        # Build a minimal GraphModel so we can call render_graphviz() directly.
        planning = json.loads(json_path.read_text())
        tb = gd.TopologyBuilder(planning, gd.FilterOptions())
        model = tb.build()

        # Patch graphviz.Graph.pipe to simulate a missing `dot` executable.
        with mock.patch("graphviz.Graph.pipe", side_effect=FileNotFoundError("dot")):
            with pytest.raises(SystemExit) as exc_info:
                gd.render_graphviz(model, fmt="svg")
        assert (
            "graphviz" in str(exc_info.value).lower()
            and "install" in str(exc_info.value).lower()
        )

    @_skip_no_graphviz
    def test_render_dot_to_stdout(self, tmp_path, capsys):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc = gd.main([str(json_path), "--format", "dot", "--aggregate", "none"])
        assert rc == 0
        out = capsys.readouterr().out
        assert "digraph" in out or "graph" in out

    @_skip_no_graphviz
    def test_render_dot_to_file(self, tmp_path):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        out_path = tmp_path / "out.dot"
        rc = gd.main(
            [
                str(json_path),
                "--format",
                "dot",
                "--output",
                str(out_path),
                "--aggregate",
                "none",
            ]
        )
        assert rc == 0
        assert out_path.exists()
        assert "digraph" in out_path.read_text() or "graph" in out_path.read_text()

    def test_no_generators_flag_runs_cleanly(self, tmp_path):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc = gd.main(
            [
                str(json_path),
                "--format",
                "mermaid",
                "--no-generators",
                "--aggregate",
                "none",
            ]
        )
        assert rc == 0

    @_skip_no_graphviz
    def test_subsystem_electrical(self, tmp_path):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc = gd.main([str(json_path), "--format", "dot", "--subsystem", "electrical"])
        assert rc == 0

    def test_planning_diagram_mermaid_to_stdout(self, tmp_path, capsys):
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc = gd.main(
            [str(json_path), "--diagram-type", "planning", "--format", "mermaid"]
        )
        assert rc == 0
        out = capsys.readouterr().out
        assert len(out) > 0
        # Relationship labels must not contain parentheses — GitHub's Mermaid
        # classDiagram parser rejects them (conflicts with method call syntax).
        for line in out.splitlines():
            if " : " in line and "--" in line:
                assert "(" not in line, (
                    f"Relationship label contains parentheses (invalid Mermaid): {line!r}"
                )


# ---------------------------------------------------------------------------
# --show flag logic
# ---------------------------------------------------------------------------


class TestShowOption:
    """Verify --show flag logic without actually opening a viewer."""

    def test_show_calls_display_diagram(self, tmp_path):
        """mermaid without --show must NOT invoke display_diagram (text goes to file)."""
        called_with = []

        original = gd.display_diagram

        def fake_display(path, fmt):
            called_with.append((path, fmt))

        gd.display_diagram = fake_display
        try:
            json_path = tmp_path / "mini.json"
            json_path.write_text(json.dumps(_MINI_PLANNING))
            out_path = tmp_path / "out.md"
            rc = gd.main(
                [
                    str(json_path),
                    "--format",
                    "mermaid",
                    "--output",
                    str(out_path),
                    "--aggregate",
                    "none",
                ]
            )
            # mermaid without --show does not call display_diagram
            assert rc == 0
            assert not called_with
        finally:
            gd.display_diagram = original

    def test_show_flag_mermaid_calls_show_mermaid(self, tmp_path):
        """--show with mermaid format must call _show_mermaid (opens browser)."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))

        with mock.patch("gtopt_diagram.cli._show_mermaid") as mock_show:
            rc = gd.main(
                [
                    str(json_path),
                    "--format",
                    "mermaid",
                    "--aggregate",
                    "none",
                    "--show",
                ]
            )
        assert rc == 0
        mock_show.assert_called_once()
        mermaid_text = mock_show.call_args[0][0]
        assert "flowchart" in mermaid_text

    def test_show_flag_mermaid_with_output_writes_file_and_shows(self, tmp_path):
        """--show with --output writes the file AND opens a browser."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        out_path = tmp_path / "out.md"

        with mock.patch("gtopt_diagram.cli._show_mermaid") as mock_show:
            rc = gd.main(
                [
                    str(json_path),
                    "--format",
                    "mermaid",
                    "--output",
                    str(out_path),
                    "--aggregate",
                    "none",
                    "--show",
                ]
            )
        assert rc == 0
        assert out_path.exists()
        assert "flowchart" in out_path.read_text()
        mock_show.assert_called_once()

    def test_mermaid_to_html_wraps_content(self):
        """_mermaid_to_html must produce HTML that embeds the mermaid source."""
        mmd = "```mermaid\nflowchart LR\n  A --> B\n```"
        html = gd._mermaid_to_html(mmd, title="Test Title")
        assert "<!DOCTYPE html>" in html
        assert "Test Title" in html
        assert "flowchart LR" in html
        assert "mermaid.initialize" in html
        # backtick fences must be stripped
        assert "```" not in html

    def test_mermaid_to_html_strips_language_tagged_closing_fence(self):
        """Closing fence with a language tag (e.g. '```mermaid') must also be stripped."""
        # Some editors produce a closing fence that echoes the opening language tag.
        mmd = "```mermaid\nflowchart TD\n  X --> Y\n```mermaid"
        html = gd._mermaid_to_html(mmd, title="T")
        assert "flowchart TD" in html
        assert "```" not in html

    def test_display_diagram_uses_webbrowser_for_svg(self, tmp_path):
        """display_diagram must call webbrowser.open for SVG files."""
        svg_file = tmp_path / "test.svg"
        svg_file.write_text("<svg/>")

        with mock.patch("gtopt_diagram._renderers.webbrowser.open") as mock_open:
            gd.display_diagram(str(svg_file), "svg")

        mock_open.assert_called_once()
        called_url: str = mock_open.call_args[0][0]
        assert "test.svg" in called_url


# ---------------------------------------------------------------------------
# Default show: auto-display when no --output is given
# ---------------------------------------------------------------------------


class TestDefaultShow:
    """Verify that display_diagram is called automatically when no --output given."""

    def _run_with_fake_display(self, json_path, extra_args=None):
        called_with = []
        original = gd.display_diagram

        def fake_display(path, fmt):
            called_with.append((path, fmt))

        gd.display_diagram = fake_display
        try:
            argv = [str(json_path), "--format", "mermaid", "--aggregate", "none"]
            if extra_args:
                argv += extra_args
            rc = gd.main(argv)
        finally:
            gd.display_diagram = original
        return rc, called_with

    def test_mermaid_to_stdout_no_show(self, tmp_path):
        """mermaid + no --output → stdout, never shows (nothing to open)."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        rc, called = self._run_with_fake_display(json_path)
        assert rc == 0
        assert not called  # mermaid to stdout must never trigger display

    def test_mermaid_with_output_file_no_show(self, tmp_path):
        """mermaid written to file must NOT auto-show (text format)."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))
        out_path = tmp_path / "out.md"
        rc, called = self._run_with_fake_display(json_path, ["--output", str(out_path)])
        assert rc == 0
        assert not called  # mermaid is text-only, never shown

    def test_explicit_show_flag_still_works(self, tmp_path):
        """--show with mermaid (no --output) must call _show_mermaid, not display_diagram."""
        json_path = tmp_path / "mini.json"
        json_path.write_text(json.dumps(_MINI_PLANNING))

        with mock.patch("gtopt_diagram.cli._show_mermaid") as mock_show:
            rc = gd.main(
                [
                    str(json_path),
                    "--format",
                    "mermaid",
                    "--aggregate",
                    "none",
                    "--show",
                ]
            )

        assert rc == 0
        mock_show.assert_called_once()  # browser opened via _show_mermaid
