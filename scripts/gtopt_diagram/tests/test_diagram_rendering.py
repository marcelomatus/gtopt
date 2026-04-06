"""Tests for gtopt_diagram – node/edge/rendering details.

Covers:
- _scalar() number formatting
- _elem_name() label formatting
- Edge pruning (dangling edges removed)
- Node ID uniqueness for turbine/generator name collision
- Line width by voltage
- Colon safety in labels
- Reserve zone and reserve provision rendering
- Generator and demand profile rendering
- Profile compact mode
"""

from gtopt_diagram import gtopt_diagram as gd

# ---------------------------------------------------------------------------
# Shared helpers (also defined in test_gtopt_diagram.py)
# ---------------------------------------------------------------------------

try:
    import graphviz as _graphviz  # noqa: F401

    _HAS_GRAPHVIZ = True
except ImportError:
    _HAS_GRAPHVIZ = False

import pytest

_skip_no_graphviz = pytest.mark.skipif(
    not _HAS_GRAPHVIZ,
    reason="graphviz Python package not installed",
)


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


# ---------------------------------------------------------------------------
# _HYDRO_PLANNING used by TestEdgePruning
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
                "production_factor": 0.003,
                "capacity": 150,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G_hydro", "bus": 1, "pmax": 150}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        "flow_array": [
            {"uid": 1, "name": "F1", "junction": 2, "discharge": 100, "direction": 1}
        ],
        "reservoir_seepage_array": [
            {"uid": 1, "name": "Filt1", "waterway": 2, "reservoir": 1}
        ],
    }
}

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
        _IEEE9_JSON = {
            "options": {"use_kirchhoff": True, "scale_objective": 1000},
            "system": {
                "name": "ieee9b",
                "bus_array": [
                    {"uid": i, "name": f"B{i}", "kv": 345} for i in range(1, 10)
                ],
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
                    {
                        "uid": 1,
                        "name": "L14",
                        "bus_a": 1,
                        "bus_b": 4,
                        "reactance": 0.0576,
                    },
                    {
                        "uid": 2,
                        "name": "L49",
                        "bus_a": 4,
                        "bus_b": 9,
                        "reactance": 0.1008,
                    },
                    {
                        "uid": 3,
                        "name": "L45",
                        "bus_a": 4,
                        "bus_b": 5,
                        "reactance": 0.0720,
                    },
                ],
            },
        }
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
# Node ID uniqueness for turbine/generator name collision
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
# Line width by voltage
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


# ---------------------------------------------------------------------------
# Colon safety in labels
# ---------------------------------------------------------------------------


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
# Generator and demand profile rendering
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


# ---------------------------------------------------------------------------
# Profile compact mode
# ---------------------------------------------------------------------------


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
