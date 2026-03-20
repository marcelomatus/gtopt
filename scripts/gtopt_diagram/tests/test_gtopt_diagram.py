"""Tests for gtopt_diagram – the network topology diagram generator.

Covers:
- ``FilterOptions`` defaults and construction
- ``TopologyBuilder`` public properties (eff_agg, eff_vthresh, auto_info)
- ``model_to_visjs`` output shape
- ``auto_voltage_threshold`` public function
- ``main()`` CLI entry-point (argparse, --help, rendering paths)
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
        assert ("junc_J1_1", "turb_T1_1") in pairs

    def test_turbine_has_water_out_edge(self):
        """turbine → junction_b edge must exist (was missing before fix)."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("turb_T1_1", "junc_J2_2") in pairs

    def test_waterway_direct_arc_suppressed_when_turbine_present(self):
        """Direct junction_a → junction_b arc must be suppressed for W1 (turbine)."""
        model = self._build()
        pairs = self._edge_pairs(model)
        # W1 has a turbine → direct arc must not appear
        assert ("junc_J1_1", "junc_J2_2") not in pairs

    def test_waterway_without_turbine_draws_direct_arc(self):
        """W2 has no turbine → direct junction_a → junction_b arc must appear."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("junc_J2_2", "junc_J3_3") in pairs

    def test_turbine_power_out_edge_to_generator(self):
        """turbine → generator (power out, dashed) edge must exist."""
        model = self._build(subsystem="full")
        pairs = self._edge_pairs(model)
        assert ("turb_T1_1", "gen_G_hydro_1") in pairs

    def test_filtration_is_a_node(self):
        """Filtration must be rendered as a node, not only as an edge."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "filt_Filt1_1" in node_ids

    def test_filtration_has_waterway_junction_edge(self):
        """junction_a of filtration's waterway → filtration node edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("junc_J2_2", "filt_Filt1_1") in pairs

    def test_filtration_has_reservoir_edge(self):
        """filtration node → reservoir edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("filt_Filt1_1", "res_Res1_1") in pairs

    def test_flow_edge_to_junction(self):
        """Flow node must be connected to its junction."""
        model = self._build()
        pairs = self._edge_pairs(model)
        flow_edges = {(s, d) for s, d in pairs if "flow_F1_1" in s or "flow_F1_1" in d}
        assert flow_edges, "Expected at least one flow edge"

    def test_reservoir_connected_to_junction(self):
        """Reservoir must be connected to its junction."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("res_Res1_1", "junc_J1_1") in pairs


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
        assert ("res_Res1_1", "turb_T1_1") in pairs

    def test_reservoir_efficiency_suppresses_main_reservoir_fallback(self):
        """main_reservoir edge must not duplicate when efficiency array covers it."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        eff_edges = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "turb_T1_1"
        ]
        # Exactly one edge (from reservoir_efficiency_array, not main_reservoir)
        assert len(eff_edges) == 1

    def test_main_reservoir_fallback_draws_edge_when_no_efficiency_array(self):
        """Turbine.main_reservoir must produce a reservoir → turbine edge when
        no reservoir_efficiency_array entry exists for that turbine."""
        model = self._build(_HYDRO_WITH_MAIN_RES_ONLY)
        pairs = self._edge_pairs(model)
        assert ("res_Res1_1", "turb_T1_1") in pairs

    def test_efficiency_edge_color_distinct(self):
        """The efficiency edge must use the efficiency_edge palette colour."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        eff_edges = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "turb_T1_1"
        ]
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
        assert ("filt_Filt1_1", "res_Res1_1") in pairs, (
            "filtration → reservoir dependency edge missing"
        )

    def test_filtration_reservoir_edge_style(self):
        """The filtration→reservoir edge must use a dotted style."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "filt_Filt1_1" and e.dst == "res_Res1_1"
        ]
        assert filt_res_edges, "filtration → reservoir edge not found"
        assert filt_res_edges[0].style == "dotted"

    def test_filtration_reservoir_edge_color(self):
        """The filtration→reservoir edge must use the filtration_border colour."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "filt_Filt1_1" and e.dst == "res_Res1_1"
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
        assert ("turb_T1_1", "gen_G_hydro_1") in pairs

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


# ---------------------------------------------------------------------------
# IEEE case file fixtures
# ---------------------------------------------------------------------------

_CASES_DIR = Path(__file__).resolve().parent.parent.parent.parent / "cases"


@pytest.fixture(scope="module")
def ieee_9b_ori_planning():
    """Load the ieee_9b_ori case JSON."""
    path = _CASES_DIR / "ieee_9b_ori" / "ieee_9b_ori.json"
    with open(path, encoding="utf-8") as f:
        return json.load(f)


@pytest.fixture(scope="module")
def ieee_9b_planning():
    """Load the ieee_9b case JSON."""
    path = _CASES_DIR / "ieee_9b" / "ieee_9b.json"
    with open(path, encoding="utf-8") as f:
        return json.load(f)


@pytest.fixture(scope="module")
def ieee_14b_planning():
    """Load the ieee_14b case JSON."""
    path = _CASES_DIR / "ieee_14b" / "ieee_14b.json"
    with open(path, encoding="utf-8") as f:
        return json.load(f)


@pytest.fixture(scope="module")
def bat_4b_planning():
    """Load the bat_4b case JSON."""
    path = _CASES_DIR / "bat_4b" / "bat_4b.json"
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Helper: common model-level validation
# ---------------------------------------------------------------------------


def _build_model(planning, subsystem="full", aggregate="none"):
    """Build a GraphModel from a planning dict."""
    fo = gd.FilterOptions(aggregate=aggregate)
    builder = gd.TopologyBuilder(planning, subsystem=subsystem, opts=fo)
    return builder.build()


def _assert_no_dangling_edges(model):
    """Assert every edge endpoint references an existing node."""
    node_ids = {n.node_id for n in model.nodes}
    for e in model.edges:
        assert e.src in node_ids, f"Dangling edge src '{e.src}' not in nodes"
        assert e.dst in node_ids, f"Dangling edge dst '{e.dst}' not in nodes"


def _assert_no_duplicate_node_ids(model):
    """Assert all node IDs are unique."""
    ids = [n.node_id for n in model.nodes]
    assert len(ids) == len(set(ids)), (
        f"Duplicate node IDs: {[x for x in ids if ids.count(x) > 1]}"
    )


def _assert_valid_mermaid(model):
    """Assert that mermaid output looks structurally valid."""
    mermaid = gd.render_mermaid(model)
    assert "flowchart" in mermaid
    # Must have at least one node definition (non-empty lines after flowchart)
    lines = [ln.strip() for ln in mermaid.splitlines() if ln.strip()]
    assert len(lines) > 3, "Mermaid output too short to contain nodes"


# ---------------------------------------------------------------------------
# Task 1: IEEE case tests
# ---------------------------------------------------------------------------


class TestIEEE9bOri:
    """Tests for the ieee_9b_ori case (9 buses, 3 thermal gens, 3 demands)."""

    def test_bus_count(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        bus_nodes = [n for n in model.nodes if n.kind == "bus"]
        assert len(bus_nodes) == 9

    def test_generator_count_and_connectivity(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert len(gen_nodes) == 3
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for gn in gen_nodes:
            edges_from_gen = [e for e in model.edges if e.src == gn.node_id]
            targets = {e.dst for e in edges_from_gen}
            assert targets & bus_ids, f"Generator {gn.node_id} not connected to any bus"

    def test_demand_count_and_connectivity(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        dem_nodes = [n for n in model.nodes if n.kind == "demand"]
        assert len(dem_nodes) == 3
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for dn in dem_nodes:
            edges_to_dem = [e for e in model.edges if e.dst == dn.node_id]
            sources = {e.src for e in edges_to_dem}
            assert sources & bus_ids, f"Demand {dn.node_id} not connected to any bus"

    def test_no_dangling_edges(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        _assert_no_duplicate_node_ids(model)

    def test_mermaid_valid(self, ieee_9b_ori_planning):
        model = _build_model(ieee_9b_ori_planning)
        _assert_valid_mermaid(model)


class TestIEEE9b:
    """Tests for the ieee_9b case (9 buses, solar profile, 24 blocks)."""

    def test_bus_count(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        bus_nodes = [n for n in model.nodes if n.kind == "bus"]
        assert len(bus_nodes) == 9

    def test_all_generators_connected(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert len(gen_nodes) >= 3
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for gn in gen_nodes:
            edges_from_gen = [e for e in model.edges if e.src == gn.node_id]
            targets = {e.dst for e in edges_from_gen}
            assert targets & bus_ids, f"Generator {gn.node_id} not connected to any bus"

    def test_all_demands_connected(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        dem_nodes = [n for n in model.nodes if n.kind == "demand"]
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for dn in dem_nodes:
            edges_to_dem = [e for e in model.edges if e.dst == dn.node_id]
            sources = {e.src for e in edges_to_dem}
            assert sources & bus_ids, f"Demand {dn.node_id} not connected to any bus"

    def test_no_dangling_edges(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        _assert_no_duplicate_node_ids(model)

    def test_mermaid_valid(self, ieee_9b_planning):
        model = _build_model(ieee_9b_planning)
        _assert_valid_mermaid(model)


class TestIEEE14b:
    """Tests for the ieee_14b case (14 buses, 5 gens, 11 demands)."""

    def test_bus_count(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        bus_nodes = [n for n in model.nodes if n.kind == "bus"]
        assert len(bus_nodes) == 14

    def test_generator_count(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert len(gen_nodes) == 5

    def test_demand_count(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        dem_nodes = [n for n in model.nodes if n.kind == "demand"]
        assert len(dem_nodes) == 11

    def test_all_generators_connected_to_bus(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for gn in gen_nodes:
            edges_from_gen = [e for e in model.edges if e.src == gn.node_id]
            targets = {e.dst for e in edges_from_gen}
            assert targets & bus_ids, f"Generator {gn.node_id} not connected to any bus"

    def test_all_demands_connected_to_bus(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        dem_nodes = [n for n in model.nodes if n.kind == "demand"]
        bus_ids = {n.node_id for n in model.nodes if n.kind == "bus"}
        for dn in dem_nodes:
            edges_to_dem = [e for e in model.edges if e.dst == dn.node_id]
            sources = {e.src for e in edges_to_dem}
            assert sources & bus_ids, f"Demand {dn.node_id} not connected to any bus"

    def test_no_dangling_edges(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        _assert_no_duplicate_node_ids(model)

    def test_mermaid_valid(self, ieee_14b_planning):
        model = _build_model(ieee_14b_planning)
        _assert_valid_mermaid(model)


class TestBat4b:
    """Tests for the bat_4b case (4 buses, 1 battery, 3 gens, 2 demands)."""

    def test_bus_count(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        bus_nodes = [n for n in model.nodes if n.kind == "bus"]
        assert len(bus_nodes) == 4

    def test_battery_node_present(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        bat_nodes = [n for n in model.nodes if n.kind == "battery"]
        assert len(bat_nodes) == 1

    def test_generator_count(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert len(gen_nodes) == 3

    def test_demand_count(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        dem_nodes = [n for n in model.nodes if n.kind == "demand"]
        assert len(dem_nodes) == 2

    def test_no_dangling_edges(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        _assert_no_duplicate_node_ids(model)

    def test_mermaid_valid(self, bat_4b_planning):
        model = _build_model(bat_4b_planning)
        _assert_valid_mermaid(model)


# ---------------------------------------------------------------------------
# Task 2: Singular element cases (minimal hand-crafted JSON)
# ---------------------------------------------------------------------------


class TestSingleBusSingleGen:
    """Single bus + single generator: 2 nodes, 1 edge."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        }
    }

    def test_node_count(self):
        model = _build_model(self._PLANNING)
        assert len(model.nodes) == 2

    def test_edge_gen_to_bus(self):
        model = _build_model(self._PLANNING)
        assert len(model.edges) == 1
        edge = model.edges[0]
        assert edge.src == "gen_G1_1"
        assert edge.dst == "bus_B1_1"

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)


class TestSingleBusSingleDemand:
    """Single bus + single demand: 2 nodes, 1 edge."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "demand_array": [{"uid": 1, "name": "D1", "bus": 1, "lmax": 50}],
        }
    }

    def test_node_count(self):
        model = _build_model(self._PLANNING)
        assert len(model.nodes) == 2

    def test_edge_bus_to_demand(self):
        model = _build_model(self._PLANNING)
        assert len(model.edges) == 1
        edge = model.edges[0]
        assert edge.src == "bus_B1_1"
        assert edge.dst == "dem_D1_1"

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)


class TestSingleBatteryWithConverter:
    """Battery + converter + generator + demand + bus: verify battery-converter edges."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 60}],
            "demand_array": [{"uid": 1, "name": "D1", "bus": 1, "lmax": 60}],
            "battery_array": [
                {"uid": 1, "name": "Bat1", "emax": 200},
            ],
            "converter_array": [
                {
                    "uid": 1,
                    "name": "Conv1",
                    "battery": 1,
                    "generator": 1,
                    "demand": 1,
                    "capacity": 60,
                },
            ],
        }
    }

    def test_battery_node_exists(self):
        model = _build_model(self._PLANNING)
        bat_nodes = [n for n in model.nodes if n.kind == "battery"]
        assert len(bat_nodes) == 1

    def test_converter_node_exists(self):
        model = _build_model(self._PLANNING)
        conv_nodes = [n for n in model.nodes if n.kind == "converter"]
        assert len(conv_nodes) == 1

    def test_battery_to_converter_edge(self):
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("bat_Bat1_1", "conv_Conv1_1") in pairs

    def test_converter_to_generator_edge(self):
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("conv_Conv1_1", "gen_G1_1") in pairs

    def test_demand_to_converter_edge(self):
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("dem_D1_1", "conv_Conv1_1") in pairs

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)


class TestBatteryDirectBus:
    """Battery with bus field but no converter — direct bus connection."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "generator_array": [],
            "battery_array": [
                {
                    "uid": 10,
                    "name": "BESS1",
                    "bus": 1,
                    "emax": 100,
                    "pmax_charge": 50,
                    "pmax_discharge": 50,
                },
            ],
        }
    }

    def test_battery_node_exists(self):
        model = _build_model(self._PLANNING)
        bat_nodes = [n for n in model.nodes if n.kind == "battery"]
        assert len(bat_nodes) == 1

    def test_battery_to_bus_edge(self):
        """Battery connects directly to bus when no converter exists."""
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("bat_BESS1_10", "bus_B1_1") in pairs

    def test_no_converter_node(self):
        model = _build_model(self._PLANNING)
        conv_nodes = [n for n in model.nodes if n.kind == "converter"]
        assert not conv_nodes

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)


class TestBatteryWithConverter:
    """Battery with converter — converter handles bus connection."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "generator_array": [
                {"uid": 20, "name": "G_dis", "bus": 1, "pmax": 50},
            ],
            "demand_array": [
                {"uid": 20, "name": "D_chg", "bus": 1, "lmax": 50},
            ],
            "battery_array": [
                {
                    "uid": 10,
                    "name": "BESS2",
                    "bus": 1,
                    "emax": 200,
                },
            ],
            "converter_array": [
                {
                    "uid": 10,
                    "name": "Conv_BESS2",
                    "battery": 10,
                    "generator": 20,
                    "demand": 20,
                },
            ],
        }
    }

    def test_no_direct_bus_edge(self):
        """Battery does NOT connect directly to bus when converter exists."""
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("bat_BESS2_10", "bus_B1_1") not in pairs

    def test_converter_edges_exist(self):
        """Converter handles the battery-generator-demand connections."""
        model = _build_model(self._PLANNING)
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("bat_BESS2_10", "conv_Conv_BESS2_10") in pairs
        assert ("conv_Conv_BESS2_10", "gen_G_dis_20") in pairs

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)


class TestHydroPassthrough:
    """Pasada-style hydro: junction -> waterway -> turbine -> generator -> bus."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "junction_array": [
                {"uid": 1, "name": "J1"},
                {"uid": 2, "name": "J2"},
            ],
            "waterway_array": [
                {"uid": 1, "junction_a": 1, "junction_b": 2, "name": "W1", "fmax": 100},
            ],
            "turbine_array": [
                {"uid": 1, "name": "T1", "waterway": 1, "generator": 1},
            ],
            "generator_array": [
                {"uid": 1, "name": "G1", "bus": 1, "pmax": 50, "type": "pasada"},
            ],
        }
    }

    def test_full_turbine_to_generator(self):
        """subsystem='full': turbine -> generator edge exists."""
        model = _build_model(self._PLANNING, subsystem="full")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("turb_T1_1", "gen_G1_1") in pairs

    def test_full_generator_to_bus(self):
        """subsystem='full': generator -> bus edge exists."""
        model = _build_model(self._PLANNING, subsystem="full")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gen_G1_1", "bus_B1_1") in pairs

    def test_full_junction_to_turbine(self):
        """subsystem='full': junction_a -> turbine (water-in) edge exists."""
        model = _build_model(self._PLANNING, subsystem="full")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("junc_J1_1", "turb_T1_1") in pairs

    def test_full_turbine_to_junction_b(self):
        """subsystem='full': turbine -> junction_b (water-out) edge exists."""
        model = _build_model(self._PLANNING, subsystem="full")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("turb_T1_1", "junc_J2_2") in pairs

    def test_full_no_dangling(self):
        model = _build_model(self._PLANNING, subsystem="full")
        _assert_no_dangling_edges(model)

    def test_hydro_auto_creates_generator_and_bus(self):
        """subsystem='hydro': turbine auto-creates generator and bus nodes."""
        model = _build_model(self._PLANNING, subsystem="hydro")
        node_ids = {n.node_id for n in model.nodes}
        # Generator and bus auto-created in hydro subsystem
        assert "gen_G1_1" in node_ids
        assert "bus_B1_1" in node_ids

    def test_hydro_turbine_to_generator(self):
        """subsystem='hydro': turbine -> auto-created generator edge exists."""
        model = _build_model(self._PLANNING, subsystem="hydro")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("turb_T1_1", "gen_G1_1") in pairs

    def test_hydro_generator_to_bus(self):
        """subsystem='hydro': auto-created generator -> auto-created bus edge exists."""
        model = _build_model(self._PLANNING, subsystem="hydro")
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gen_G1_1", "bus_B1_1") in pairs

    def test_hydro_no_dangling(self):
        model = _build_model(self._PLANNING, subsystem="hydro")
        _assert_no_dangling_edges(model)


class TestPasadaFullChain:
    """Verify complete pasada topology: flow -> junction -> turbine -> gen -> bus.

    Uses two pasada centrals on different buses to verify all connections
    independently. Each pasada has the isolated hydro topology:
    flow -> junction_a -> [turbine via waterway] -> junction_b(ocean) + gen -> bus.
    """

    _PLANNING = {
        "system": {
            "name": "pasada_chain_test",
            "bus_array": [
                {"uid": 1, "name": "BusA", "voltage": 220},
                {"uid": 2, "name": "BusB", "voltage": 110},
            ],
            "generator_array": [
                {"uid": 10, "name": "Pasada1", "bus": 1, "pmax": 30, "type": "pasada"},
                {"uid": 20, "name": "Pasada2", "bus": 2, "pmax": 60, "type": "pasada"},
            ],
            "junction_array": [
                {"uid": 10, "name": "J_P1"},
                {"uid": 11, "name": "J_P1_ocean"},
                {"uid": 20, "name": "J_P2"},
                {"uid": 21, "name": "J_P2_ocean"},
            ],
            "waterway_array": [
                {
                    "uid": 10,
                    "name": "W_P1",
                    "junction_a": 10,
                    "junction_b": 11,
                    "fmax": 50,
                },
                {
                    "uid": 20,
                    "name": "W_P2",
                    "junction_a": 20,
                    "junction_b": 21,
                    "fmax": 80,
                },
            ],
            "turbine_array": [
                {
                    "uid": 10,
                    "name": "Pasada1",
                    "waterway": 10,
                    "generator": 10,
                    "conversion_rate": 1.0,
                },
                {
                    "uid": 20,
                    "name": "Pasada2",
                    "waterway": 20,
                    "generator": 20,
                    "conversion_rate": 1.0,
                },
            ],
            "flow_array": [
                {"uid": 10, "name": "Flow_P1", "junction": 10, "discharge": 25},
                {"uid": 20, "name": "Flow_P2", "junction": 20, "discharge": 50},
            ],
        }
    }

    def _pairs(self, subsystem):
        model = _build_model(self._PLANNING, subsystem=subsystem)
        return {(e.src, e.dst) for e in model.edges}, model

    def test_full_flow_to_junction(self):
        """Flow nodes connect to their junctions."""
        pairs, _ = self._pairs("full")
        assert ("flow_Flow_P1_10", "junc_J_P1_10") in pairs
        assert ("flow_Flow_P2_20", "junc_J_P2_20") in pairs

    def test_full_junction_to_turbine(self):
        """Junction_a -> turbine (water-in)."""
        pairs, _ = self._pairs("full")
        assert ("junc_J_P1_10", "turb_Pasada1_10") in pairs
        assert ("junc_J_P2_20", "turb_Pasada2_20") in pairs

    def test_full_turbine_to_ocean(self):
        """Turbine -> junction_b (water-out to ocean)."""
        pairs, _ = self._pairs("full")
        assert ("turb_Pasada1_10", "junc_J_P1_ocean_11") in pairs
        assert ("turb_Pasada2_20", "junc_J_P2_ocean_21") in pairs

    def test_full_turbine_to_generator(self):
        """Turbine -> generator (power out)."""
        pairs, _ = self._pairs("full")
        assert ("turb_Pasada1_10", "gen_Pasada1_10") in pairs
        assert ("turb_Pasada2_20", "gen_Pasada2_20") in pairs

    def test_full_generator_to_bus(self):
        """Generator -> bus (electrical connection)."""
        pairs, _ = self._pairs("full")
        assert ("gen_Pasada1_10", "bus_BusA_1") in pairs
        assert ("gen_Pasada2_20", "bus_BusB_2") in pairs

    def test_full_complete_chain(self):
        """Complete chain: flow -> junc -> turb -> gen -> bus for each pasada."""
        pairs, _ = self._pairs("full")
        # Pasada1 chain
        assert ("flow_Flow_P1_10", "junc_J_P1_10") in pairs
        assert ("junc_J_P1_10", "turb_Pasada1_10") in pairs
        assert ("turb_Pasada1_10", "gen_Pasada1_10") in pairs
        assert ("gen_Pasada1_10", "bus_BusA_1") in pairs
        # Pasada2 chain
        assert ("flow_Flow_P2_20", "junc_J_P2_20") in pairs
        assert ("junc_J_P2_20", "turb_Pasada2_20") in pairs
        assert ("turb_Pasada2_20", "gen_Pasada2_20") in pairs
        assert ("gen_Pasada2_20", "bus_BusB_2") in pairs

    def test_full_no_dangling(self):
        _, model = self._pairs("full")
        _assert_no_dangling_edges(model)

    def test_full_no_duplicate_ids(self):
        _, model = self._pairs("full")
        ids = [n.node_id for n in model.nodes]
        assert len(ids) == len(set(ids)), (
            f"Duplicate IDs: {[x for x in ids if ids.count(x) > 1]}"
        )

    def test_hydro_complete_chain(self):
        """In hydro mode, auto-created generators still connect to bus."""
        pairs, model = self._pairs("hydro")
        # Generators and buses auto-created
        node_ids = {n.node_id for n in model.nodes}
        assert "gen_Pasada1_10" in node_ids
        assert "gen_Pasada2_20" in node_ids
        assert "bus_BusA_1" in node_ids
        assert "bus_BusB_2" in node_ids
        # Full chain works
        assert ("turb_Pasada1_10", "gen_Pasada1_10") in pairs
        assert ("gen_Pasada1_10", "bus_BusA_1") in pairs
        assert ("turb_Pasada2_20", "gen_Pasada2_20") in pairs
        assert ("gen_Pasada2_20", "bus_BusB_2") in pairs

    def test_hydro_no_dangling(self):
        _, model = self._pairs("hydro")
        _assert_no_dangling_edges(model)

    def test_hydro_gen_kind_is_hydro(self):
        """Auto-created generators in hydro mode have gen_hydro kind."""
        _, model = self._pairs("hydro")
        gen_nodes = [n for n in model.nodes if n.node_id.startswith("gen_")]
        assert all(n.kind == "gen_hydro" for n in gen_nodes)

    def test_hydro_bus_voltage_preserved(self):
        """Auto-created buses in hydro mode preserve voltage in label."""
        _, model = self._pairs("hydro")
        bus_a = next(n for n in model.nodes if n.node_id == "bus_BusA_1")
        assert "220" in bus_a.label


class TestEmptySystem:
    """Empty system: builds without error, 0 nodes."""

    def test_empty_system_dict(self):
        model = _build_model({"system": {}})
        assert len(model.nodes) == 0
        assert len(model.edges) == 0

    def test_empty_arrays(self):
        model = _build_model(
            {
                "system": {
                    "bus_array": [],
                    "generator_array": [],
                    "demand_array": [],
                    "line_array": [],
                }
            }
        )
        assert len(model.nodes) == 0
        assert len(model.edges) == 0


class TestGeneratorWithoutBus:
    """Generator without a bus field: must not crash, gen node exists but no edge to bus."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "generator_array": [{"uid": 1, "name": "G1", "pmax": 100}],
        }
    }

    def test_no_crash(self):
        model = _build_model(self._PLANNING)
        assert model is not None

    def test_generator_node_present(self):
        model = _build_model(self._PLANNING)
        gen_nodes = [
            n for n in model.nodes if n.kind in ("gen", "gen_hydro", "gen_solar")
        ]
        assert len(gen_nodes) == 1

    def test_no_edge_from_generator(self):
        """Generator without bus should have no edge connecting to a bus."""
        model = _build_model(self._PLANNING)
        gen_edges = [e for e in model.edges if e.src == "gen_G1_1"]
        assert len(gen_edges) == 0


# ---------------------------------------------------------------------------
# Task 3: Node ID uniqueness for turbine/generator name collision
# ---------------------------------------------------------------------------


class TestNodeIDUniqueness:
    """Turbine and generator sharing the same name+uid must get different IDs."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "B1"}],
            "junction_array": [
                {"uid": 1, "name": "J1"},
                {"uid": 2, "name": "J2"},
            ],
            "waterway_array": [
                {"uid": 1, "junction_a": 1, "junction_b": 2, "name": "W1", "fmax": 100},
            ],
            "turbine_array": [
                {"uid": 1, "name": "T1", "waterway": 1, "generator": 1},
            ],
            "generator_array": [
                {"uid": 1, "name": "T1", "bus": 1, "pmax": 50, "type": "hydro"},
            ],
        }
    }

    def test_different_prefixes(self):
        """turb_T1_1 and gen_T1_1 must be distinct IDs."""
        model = _build_model(self._PLANNING, subsystem="full")
        node_ids = [n.node_id for n in model.nodes]
        turb_id = "turb_T1_1"
        gen_id = "gen_T1_1"
        assert turb_id in node_ids
        assert gen_id in node_ids
        assert turb_id != gen_id

    def test_no_id_collision(self):
        """All node IDs must be unique even with shared name+uid."""
        model = _build_model(self._PLANNING, subsystem="full")
        _assert_no_duplicate_node_ids(model)


# ---------------------------------------------------------------------------
# Task 4: Colon safety in labels
# ---------------------------------------------------------------------------


class TestLineWidthByVoltage:
    """Electrical line edges have width proportional to bus voltage."""

    _PLANNING = {
        "system": {
            "bus_array": [
                {"uid": 1, "name": "HV500", "voltage": 500},
                {"uid": 2, "name": "HV220", "voltage": 220},
                {"uid": 3, "name": "MV66", "voltage": 66},
                {"uid": 4, "name": "LV33", "voltage": 33},
            ],
            "generator_array": [],
            "line_array": [
                {"uid": 1, "name": "L_500", "bus_a": 1, "bus_b": 2},
                {"uid": 2, "name": "L_220", "bus_a": 2, "bus_b": 3},
                {"uid": 3, "name": "L_66", "bus_a": 3, "bus_b": 4},
            ],
        }
    }

    def test_line_edges_exist(self):
        model = _build_model(self._PLANNING)
        line_edges = [e for e in model.edges if not e.directed]
        assert len(line_edges) == 3

    def test_high_voltage_wider_than_low(self):
        """500 kV line is wider than 66 kV line."""
        model = _build_model(self._PLANNING)
        weights = sorted(e.weight for e in model.edges if not e.directed)
        assert weights[-1] > weights[0]

    def test_widths_are_distinct(self):
        """Each voltage level produces a different width."""
        model = _build_model(self._PLANNING)
        widths = sorted({e.weight for e in model.edges if not e.directed})
        assert len(widths) == 3, f"Expected 3 distinct widths, got {widths}"

    def test_500kv_line_weight_is_5(self):
        """500 kV line should have weight 5.0."""
        model = _build_model(self._PLANNING)
        edges = [e for e in model.edges if not e.directed]
        # The 500-220 line has max(500, 220) = 500 kV
        hv_edge = next(e for e in edges if e.weight == max(e.weight for e in edges))
        assert hv_edge.weight == 5.0

    def test_33kv_line_weight_is_2(self):
        """33 kV line should have weight 2.0."""
        model = _build_model(self._PLANNING)
        edges = [e for e in model.edges if not e.directed]
        lv_edge = next(e for e in edges if e.weight == min(e.weight for e in edges))
        assert lv_edge.weight == 2.5  # max(66, 33) = 66 kV -> weight 2.5

    def test_line_colors_are_not_blue(self):
        """No electrical line should use a blue color."""
        model = _build_model(self._PLANNING)
        for e in model.edges:
            if not e.directed and e.color:
                # Blue hues have hex starting with #0, #1, #2 followed by high B
                assert not e.color.startswith("#0"), f"Blue line color: {e.color}"

    def test_vis_js_width_uses_weight_directly(self):
        """vis.js edge width = clamped weight, not the old /100 formula."""
        from gtopt_diagram.gtopt_diagram import model_to_visjs

        model = _build_model(self._PLANNING)
        visjs = model_to_visjs(model)
        vis_widths = {ve["width"] for ve in visjs["edges"]}
        # All widths should be >= 2.0 (not ~1.0 from old formula)
        assert all(w >= 2.0 for w in vis_widths), f"Widths too small: {vis_widths}"


class TestColonSafetyInLabels:
    """Elements with colons in names must not crash Mermaid or DOT rendering."""

    _PLANNING = {
        "system": {
            "bus_array": [{"uid": 1, "name": "Bus:A"}],
            "generator_array": [
                {"uid": 1, "name": "Gen:1", "bus": 1, "pmax": 100},
            ],
            "demand_array": [
                {"uid": 1, "name": "Dem:X", "bus": 1, "lmax": 50},
            ],
        }
    }

    def test_mermaid_no_crash(self):
        """Mermaid output must be generated without error for colon names."""
        model = _build_model(self._PLANNING)
        mermaid = gd.render_mermaid(model)
        assert "flowchart" in mermaid

    def test_mermaid_contains_nodes(self):
        """All three element types must appear in the Mermaid output."""
        model = _build_model(self._PLANNING)
        mermaid_text = gd.render_mermaid(model)
        # Node IDs with colons are replaced by _make_id (colons appear in labels only)
        assert len(model.nodes) == 3
        assert "Gen:1" in mermaid_text or "Gen" in mermaid_text

    @_skip_no_graphviz
    def test_dot_no_crash(self):
        """DOT/Graphviz output must be generated without error for colon names."""
        model = _build_model(self._PLANNING)
        dot_src = gd.render_graphviz(model, fmt="dot")
        assert "graph" in dot_src.lower() or "digraph" in dot_src.lower()

    def test_no_dangling_edges(self):
        model = _build_model(self._PLANNING)
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self):
        model = _build_model(self._PLANNING)
        _assert_no_duplicate_node_ids(model)


# ---------------------------------------------------------------------------
# Reserve zone and reserve provision rendering
# ---------------------------------------------------------------------------

_RESERVE_PLANNING = {
    "system": {
        "name": "reserve_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G1", "bus": 1, "pmax": 100},
            {"uid": 2, "name": "G2", "bus": 1, "pmax": 200},
        ],
        "demand_array": [],
        "line_array": [],
        # A dummy junction keeps subsystem="full" (otherwise auto-switches to
        # "electrical" when no hydro elements exist, which skips reserve zones).
        "junction_array": [{"uid": 1, "name": "J_dummy"}],
        "reserve_zone_array": [
            {"uid": 1, "name": "RZ_Norte"},
            {"uid": 2, "name": "RZ_Sur"},
        ],
        "reserve_provision_array": [
            {"uid": 1, "generator": 1, "reserve_zones": "RZ_Norte:RZ_Sur"},
            {"uid": 2, "generator": 2, "reserve_zones": "RZ_Norte"},
        ],
    }
}


class TestReserveZoneRendering:
    """Verify reserve zone nodes and reserve provision edges."""

    def _build(self):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_RESERVE_PLANNING, subsystem="full", opts=fo)
        return builder.build()

    def test_reserve_zone_nodes_exist(self):
        """Each reserve_zone_array entry produces a node with kind='reserve_zone'."""
        model = self._build()
        rz_nodes = [n for n in model.nodes if n.kind == "reserve_zone"]
        assert len(rz_nodes) == 2

    def test_reserve_zone_node_ids(self):
        """Reserve zone node IDs use the rzone_ prefix."""
        model = self._build()
        rz_ids = {n.node_id for n in model.nodes if n.kind == "reserve_zone"}
        assert "rzone_RZ_Norte_1" in rz_ids
        assert "rzone_RZ_Sur_2" in rz_ids

    def test_reserve_zone_label_contains_name(self):
        """Reserve zone labels include the zone name."""
        model = self._build()
        rz_nodes = [n for n in model.nodes if n.kind == "reserve_zone"]
        labels = {n.label for n in rz_nodes}
        assert any("RZ_Norte" in lbl for lbl in labels)
        assert any("RZ_Sur" in lbl for lbl in labels)

    def test_reserve_provision_edges_gen1_to_both_zones(self):
        """Generator 1 has reserve provision to zones 1 and 2 (colon-separated)."""
        model = self._build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gen_G1_1", "rzone_RZ_Norte_1") in pairs
        assert ("gen_G1_1", "rzone_RZ_Sur_2") in pairs

    def test_reserve_provision_edges_gen2_to_zone1(self):
        """Generator 2 has reserve provision to zone 1 only."""
        model = self._build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gen_G2_2", "rzone_RZ_Norte_1") in pairs

    def test_reserve_provision_edge_style(self):
        """Reserve provision edges use dotted style."""
        model = self._build()
        rp_edges = [e for e in model.edges if e.label == "reserve"]
        assert rp_edges
        for e in rp_edges:
            assert e.style == "dotted"

    def test_no_dangling_edges(self):
        model = self._build()
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self):
        model = self._build()
        _assert_no_duplicate_node_ids(model)


_RESERVE_LIST_PLANNING = {
    "system": {
        "name": "reserve_list_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G1", "bus": 1, "pmax": 100},
        ],
        "demand_array": [],
        "line_array": [],
        "junction_array": [{"uid": 1, "name": "J_dummy"}],
        "reserve_zone_array": [
            {"uid": 1, "name": "RZ1"},
        ],
        "reserve_provision_array": [
            {"uid": 1, "generator": 1, "reserve_zones": ["RZ1"]},
        ],
    }
}


class TestReserveZoneListFormat:
    """Verify reserve_zones as list (not just colon-separated string)."""

    def test_list_format_creates_edge(self):
        """reserve_zones given as a list [1] produces an edge."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_RESERVE_LIST_PLANNING, subsystem="full", opts=fo)
        model = builder.build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gen_G1_1", "rzone_RZ1_1") in pairs


# ---------------------------------------------------------------------------
# Generator profile and demand profile rendering
# ---------------------------------------------------------------------------

_PROFILE_PLANNING = {
    "system": {
        "name": "profile_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G1", "bus": 1, "pmax": 100},
        ],
        "demand_array": [
            {"uid": 1, "name": "D1", "bus": 1, "lmax": 80},
        ],
        "line_array": [],
        "generator_profile_array": [
            {"uid": 1, "name": "GP1", "generator": 1, "profile": "solar_profile.csv"},
        ],
        "demand_profile_array": [
            {"uid": 1, "name": "DP1", "demand": 1, "profile": "load_curve.csv"},
        ],
    }
}


class TestGeneratorProfileRendering:
    """Verify generator profile nodes and edges."""

    def _build(self):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_PROFILE_PLANNING, subsystem="electrical", opts=fo)
        return builder.build()

    def test_gen_profile_node_exists(self):
        """Generator profile produces a node with kind='gen_profile'."""
        model = self._build()
        gp_nodes = [n for n in model.nodes if n.kind == "gen_profile"]
        assert len(gp_nodes) == 1

    def test_gen_profile_node_id(self):
        """Generator profile node ID uses the gprof_ prefix."""
        model = self._build()
        gp_nodes = [n for n in model.nodes if n.kind == "gen_profile"]
        assert gp_nodes[0].node_id == "gprof_GP1_1"

    def test_gen_profile_edge_to_generator(self):
        """Generator profile node connects to its generator via dotted edge."""
        model = self._build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("gprof_GP1_1", "gen_G1_1") in pairs

    def test_gen_profile_edge_style_dotted(self):
        """Profile-to-generator edge uses dotted style."""
        model = self._build()
        profile_edges = [
            e for e in model.edges if e.src == "gprof_GP1_1" and e.dst == "gen_G1_1"
        ]
        assert profile_edges
        assert profile_edges[0].style == "dotted"

    def test_gen_profile_edge_label(self):
        """Profile-to-generator edge has label 'profile'."""
        model = self._build()
        profile_edges = [
            e for e in model.edges if e.src == "gprof_GP1_1" and e.dst == "gen_G1_1"
        ]
        assert profile_edges[0].label == "profile"

    def test_no_dangling_edges(self):
        model = self._build()
        _assert_no_dangling_edges(model)


class TestDemandProfileRendering:
    """Verify demand profile nodes and edges."""

    def _build(self):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_PROFILE_PLANNING, subsystem="electrical", opts=fo)
        return builder.build()

    def test_dem_profile_node_exists(self):
        """Demand profile produces a node with kind='dem_profile'."""
        model = self._build()
        dp_nodes = [n for n in model.nodes if n.kind == "dem_profile"]
        assert len(dp_nodes) == 1

    def test_dem_profile_node_id(self):
        """Demand profile node ID uses the dprof_ prefix."""
        model = self._build()
        dp_nodes = [n for n in model.nodes if n.kind == "dem_profile"]
        assert dp_nodes[0].node_id == "dprof_DP1_1"

    def test_dem_profile_edge_to_demand(self):
        """Demand profile node connects to its demand via dotted edge."""
        model = self._build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("dprof_DP1_1", "dem_D1_1") in pairs

    def test_dem_profile_edge_style_dotted(self):
        """Profile-to-demand edge uses dotted style."""
        model = self._build()
        profile_edges = [
            e for e in model.edges if e.src == "dprof_DP1_1" and e.dst == "dem_D1_1"
        ]
        assert profile_edges
        assert profile_edges[0].style == "dotted"

    def test_dem_profile_edge_label(self):
        """Profile-to-demand edge has label 'profile'."""
        model = self._build()
        profile_edges = [
            e for e in model.edges if e.src == "dprof_DP1_1" and e.dst == "dem_D1_1"
        ]
        assert profile_edges[0].label == "profile"

    def test_no_dangling_edges(self):
        model = self._build()
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self):
        model = self._build()
        _assert_no_duplicate_node_ids(model)


class TestProfileCompactMode:
    """Verify profile labels in compact mode are shorter."""

    def test_compact_gen_profile_label(self):
        """In compact mode, gen profile label is just the name."""
        fo = gd.FilterOptions(aggregate="none", compact=True)
        builder = gd.TopologyBuilder(_PROFILE_PLANNING, subsystem="electrical", opts=fo)
        model = builder.build()
        gp_nodes = [n for n in model.nodes if n.kind == "gen_profile"]
        assert gp_nodes
        # Compact label should be just the name, not prefixed with [GenProfile]
        assert "[GenProfile]" not in gp_nodes[0].label

    def test_compact_dem_profile_label(self):
        """In compact mode, demand profile label is just the name."""
        fo = gd.FilterOptions(aggregate="none", compact=True)
        builder = gd.TopologyBuilder(_PROFILE_PLANNING, subsystem="electrical", opts=fo)
        model = builder.build()
        dp_nodes = [n for n in model.nodes if n.kind == "dem_profile"]
        assert dp_nodes
        assert "[DemProfile]" not in dp_nodes[0].label
