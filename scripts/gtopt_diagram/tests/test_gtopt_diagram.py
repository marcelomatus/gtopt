"""Tests for gtopt_diagram – the network topology diagram generator.

Covers:
- ``FilterOptions`` defaults and construction
- ``TopologyBuilder`` public properties (eff_agg, eff_vthresh, auto_info)
- ``model_to_visjs`` output shape
- ``auto_voltage_threshold`` public function
- ``main()`` CLI entry-point (argparse, --help, rendering paths)
"""

import json
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
# Hydro topology correctness
# ---------------------------------------------------------------------------

_HYDRO_PLANNING = {
    "system": {
        "name": "hydro_test",
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
            {"uid": 3, "name": "J3"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 2, "fmax": 500},
            {"uid": 2, "name": "W2", "junction_a": 2, "junction_b": 3, "fmax": 300},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "Res1", "junction": 1, "emax": 10000},
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T1",
                "waterway": 1,
                "generator": 1,
                "conversion_rate": 0.003,
                "capacity": 150,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G_hydro", "bus": 1, "pmax": 150}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        "flow_array": [
            {"uid": 1, "name": "F1", "junction": 2, "discharge": 100, "direction": 1}
        ],
        "filtration_array": [
            {"uid": 1, "name": "Filt1", "waterway": 2, "reservoir": 1}
        ],
    }
}


class TestHydroTopology:
    """Verify correct arc connections for hydro elements."""

    def _build(self, subsystem="hydro"):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem=subsystem, opts=fo)
        return builder.build()

    def _edge_pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_turbine_has_water_in_edge(self):
        """junction_a → turbine edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("J1_1", "T1_1") in pairs

    def test_turbine_has_water_out_edge(self):
        """turbine → junction_b edge must exist (was missing before fix)."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("T1_1", "J2_2") in pairs

    def test_waterway_direct_arc_suppressed_when_turbine_present(self):
        """Direct junction_a → junction_b arc must be suppressed for W1 (turbine)."""
        model = self._build()
        pairs = self._edge_pairs(model)
        # W1 has a turbine → direct arc must not appear
        assert ("J1_1", "J2_2") not in pairs

    def test_waterway_without_turbine_draws_direct_arc(self):
        """W2 has no turbine → direct junction_a → junction_b arc must appear."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("J2_2", "J3_3") in pairs

    def test_turbine_power_out_edge_to_generator(self):
        """turbine → generator (power out, dashed) edge must exist."""
        model = self._build(subsystem="full")
        pairs = self._edge_pairs(model)
        assert ("T1_1", "G_hydro_1") in pairs

    def test_filtration_is_a_node(self):
        """Filtration must be rendered as a node, not only as an edge."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "Filt1_1" in node_ids

    def test_filtration_has_waterway_junction_edge(self):
        """junction_a of filtration's waterway → filtration node edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("J2_2", "Filt1_1") in pairs

    def test_filtration_has_reservoir_edge(self):
        """filtration node → reservoir edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("Filt1_1", "Res1_1") in pairs

    def test_flow_edge_to_junction(self):
        """Flow node must be connected to its junction."""
        model = self._build()
        pairs = self._edge_pairs(model)
        flow_edges = {(s, d) for s, d in pairs if "F1_1" in s or "F1_1" in d}
        assert flow_edges, "Expected at least one flow edge"

    def test_reservoir_connected_to_junction(self):
        """Reservoir must be connected to its junction."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("Res1_1", "J1_1") in pairs


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

        with mock.patch.object(gd, "_show_mermaid") as mock_show:
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

        with mock.patch.object(gd, "_show_mermaid") as mock_show:
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

        with mock.patch("gtopt_diagram.gtopt_diagram.webbrowser.open") as mock_open:
            gd.display_diagram(str(svg_file), "svg")

        mock_open.assert_called_once()
        called_url: str = mock_open.call_args[0][0]
        assert "test.svg" in called_url


# ---------------------------------------------------------------------------
# Reservoir efficiency (head-dependent turbines)
# ---------------------------------------------------------------------------

_HYDRO_WITH_EFFICIENCY = {
    "system": {
        "name": "hydro_eff_test",
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 2, "fmax": 400},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "Res1", "junction": 1, "emax": 5000},
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T1",
                "waterway": 1,
                "generator": 1,
                "conversion_rate": 0.0025,
                "main_reservoir": 1,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        "reservoir_efficiency_array": [
            {
                "uid": 1,
                "name": "eff_T1",
                "turbine": 1,
                "reservoir": 1,
                "mean_efficiency": 0.0025,
                "segments": [{"volume": 0.0, "slope": 0.0, "constant": 0.0025}],
            }
        ],
    }
}

_HYDRO_WITH_MAIN_RES_ONLY = {
    "system": {
        "name": "hydro_mainres_test",
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 2, "fmax": 400},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "Res1", "junction": 1, "emax": 5000},
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T1",
                "waterway": 1,
                "generator": 1,
                "conversion_rate": 0.0025,
                "main_reservoir": 1,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        # No reservoir_efficiency_array
    }
}


class TestReservoirEfficiency:
    """Verify reservoir_efficiency_array and main_reservoir edge drawing."""

    def _build(self, planning, subsystem="hydro"):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem=subsystem, opts=fo)
        return builder.build()

    def _edge_pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_reservoir_efficiency_draws_edge(self):
        """reservoir_efficiency_array must produce a reservoir → turbine edge."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        pairs = self._edge_pairs(model)
        assert ("Res1_1", "T1_1") in pairs

    def test_reservoir_efficiency_suppresses_main_reservoir_fallback(self):
        """main_reservoir edge must not duplicate when efficiency array covers it."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        eff_edges = [e for e in model.edges if e.src == "Res1_1" and e.dst == "T1_1"]
        # Exactly one edge (from reservoir_efficiency_array, not main_reservoir)
        assert len(eff_edges) == 1

    def test_main_reservoir_fallback_draws_edge_when_no_efficiency_array(self):
        """Turbine.main_reservoir must produce a reservoir → turbine edge when
        no reservoir_efficiency_array entry exists for that turbine."""
        model = self._build(_HYDRO_WITH_MAIN_RES_ONLY)
        pairs = self._edge_pairs(model)
        assert ("Res1_1", "T1_1") in pairs

    def test_efficiency_edge_color_distinct(self):
        """The efficiency edge must use the efficiency_edge palette colour."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        eff_edges = [e for e in model.edges if e.src == "Res1_1" and e.dst == "T1_1"]
        assert eff_edges
        assert eff_edges[0].color == gd._PALETTE["efficiency_edge"]  # noqa: SLF001


# ---------------------------------------------------------------------------
# _scalar  —  number formatting (2 decimal places)
# ---------------------------------------------------------------------------


class TestScalar:
    """Verify _scalar() formats numbers to at most 2 decimal places."""

    def test_none_returns_em_dash(self):
        assert gd._scalar(None) == "\u2014"  # noqa: SLF001

    def test_int_unchanged(self):
        assert gd._scalar(100) == "100"  # noqa: SLF001
        assert gd._scalar(0) == "0"  # noqa: SLF001

    def test_whole_float_no_decimals(self):
        # Whole float values must display without a decimal point.
        assert gd._scalar(100.0) == "100"  # noqa: SLF001
        assert gd._scalar(500.0) == "500"  # noqa: SLF001

    def test_float_two_decimal_places(self):
        assert gd._scalar(100.5) == "100.50"  # noqa: SLF001
        assert gd._scalar(25.123456) == "25.12"  # noqa: SLF001
        assert gd._scalar(0.005) == "0.01"  # rounded  # noqa: SLF001

    def test_float_small_value_two_decimals(self):
        # Very small values (e.g. conversion rates) round to "0.00" with 2dp.
        assert gd._scalar(0.0025) == "0.00"  # noqa: SLF001

    def test_list_range_uses_scalar_formatting(self):
        # Range values must go through _scalar so they also obey 2dp.
        # 100.12345 → "100.12" proves the list path truncates to 2dp.
        result = gd._scalar([10.0, 100.12345])  # noqa: SLF001
        assert result == "10\u2026100.12"  # min…max, each formatted  # noqa: SLF001
        assert "100.12345" not in result  # raw precision must not appear

    def test_list_identical_values_no_range(self):
        assert gd._scalar([42.0, 42.0]) == "42"  # noqa: SLF001

    def test_list_empty_returns_em_dash(self):
        assert gd._scalar([]) == "\u2014"  # noqa: SLF001

    def test_string_quoted(self):
        assert gd._scalar("parquet") == '"parquet"'  # noqa: SLF001


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

        with mock.patch.object(gd, "_show_mermaid") as mock_show:
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


# ---------------------------------------------------------------------------
# Filtration → reservoir dependency
# ---------------------------------------------------------------------------


class TestFiltrationReservoirDependency:
    """Confirm the filtration-to-reservoir edge is present in every valid topology."""

    def test_filtration_reservoir_edge_present(self):
        """filtration_node → reservoir edge must always be drawn."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("Filt1_1", "Res1_1") in pairs, (
            "filtration → reservoir dependency edge missing"
        )

    def test_filtration_reservoir_edge_style(self):
        """The filtration→reservoir edge must use a dotted style."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "Filt1_1" and e.dst == "Res1_1"
        ]
        assert filt_res_edges, "filtration → reservoir edge not found"
        assert filt_res_edges[0].style == "dotted"

    def test_filtration_reservoir_edge_color(self):
        """The filtration→reservoir edge must use the filtration_border colour."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "Filt1_1" and e.dst == "Res1_1"
        ]
        assert filt_res_edges
        assert filt_res_edges[0].color == gd._PALETTE["filtration_border"]  # noqa: SLF001


# ---------------------------------------------------------------------------
# _elem_name — name:uid label formatting
# ---------------------------------------------------------------------------


class TestElemName:
    """Verify _elem_name() produces 'NAME:UID' formatted labels."""

    def test_name_and_uid_combined(self):
        assert gd._elem_name({"name": "ELTORO", "uid": 2}) == "ELTORO:2"  # noqa: SLF001

    def test_name_equals_uid_no_duplication(self):
        """When name and uid stringify the same, show only name."""
        assert gd._elem_name({"name": "2", "uid": 2}) == "2"  # noqa: SLF001

    def test_name_only(self):
        assert gd._elem_name({"name": "B1"}) == "B1"  # noqa: SLF001

    def test_uid_only(self):
        assert gd._elem_name({"uid": 3}) == "3"  # noqa: SLF001

    def test_empty_returns_question_mark(self):
        assert gd._elem_name({}) == "?"  # noqa: SLF001

    def test_bus_label_contains_uid(self):
        """Bus node label must include uid when name and uid differ."""
        planning = {
            "system": {
                "bus_array": [{"uid": 7, "name": "ALTO"}],
                "generator_array": [{"uid": 1, "bus": 7, "pmax": 100}],
                "demand_array": [],
                "line_array": [],
            }
        }
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, opts=fo)
        model = builder.build()
        bus_nodes = [n for n in model.nodes if n.kind == "bus"]
        assert bus_nodes, "No bus node found"
        assert "ALTO:7" in bus_nodes[0].label

    def test_generator_label_contains_uid(self):
        """Generator node label must include uid in 'name:uid' format."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_IEEE9_JSON, opts=fo)
        model = builder.build()
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert gen_nodes, "No generator nodes found"
        # G1 has uid=1 → label should contain "G1:1"
        g1 = next((n for n in gen_nodes if "G1" in n.label), None)
        assert g1 is not None, "G1 generator node not found"
        assert "G1:1" in g1.label


# ---------------------------------------------------------------------------
# Edge pruning — dangling edges with missing endpoints are removed
# ---------------------------------------------------------------------------


class TestEdgePruning:
    """Verify that edges referencing non-existent nodes are removed in build()."""

    def test_hydro_subsystem_no_dangling_generator_edges(self):
        """subsystem='hydro' must not have turbine→generator edges (no gen nodes)."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()

        node_ids = {n.node_id for n in model.nodes}
        for e in model.edges:
            assert e.src in node_ids, f"Edge src '{e.src}' references non-existent node"
            assert e.dst in node_ids, f"Edge dst '{e.dst}' references non-existent node"

    def test_full_subsystem_retains_turbine_generator_edge(self):
        """subsystem='full' must keep the turbine→generator edge (gen nodes exist)."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="full", opts=fo)
        model = builder.build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("T1_1", "G_hydro_1") in pairs

    def test_all_edges_have_valid_endpoints(self):
        """For any subsystem, every edge endpoint must exist in model.nodes."""
        for subsystem in ("full", "electrical", "hydro"):
            fo = gd.FilterOptions(aggregate="none")
            builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem=subsystem, opts=fo)
            model = builder.build()
            node_ids = {n.node_id for n in model.nodes}
            for e in model.edges:
                assert e.src in node_ids, (
                    f"[{subsystem}] Edge src '{e.src}' not in nodes"
                )
                assert e.dst in node_ids, (
                    f"[{subsystem}] Edge dst '{e.dst}' not in nodes"
                )
