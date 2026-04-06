"""Tests for gtopt_diagram – hydro topology rendering.

Covers:
- Hydro arc connections (turbines, waterways, reservoirs, junctions)
- Reservoir production factor and head-dependent turbines
- ReservoirSeepage → reservoir dependency
- Pasada hydro passthrough topology
- Full pasada chain
- Turbine with flow reference (instead of waterway)
- Embedded reservoir features (seepage, discharge_limit, production_factor)
- Reservoir discharge limit hydro detection
- VolumeRight and FlowRight water rights
"""

from gtopt_diagram import gtopt_diagram as gd

# ---------------------------------------------------------------------------
# Shared helpers (also defined in test_gtopt_diagram.py)
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

    def test_seepage_is_a_node(self):
        """ReservoirSeepage must be rendered as a node, not only as an edge."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "filt_Filt1_1" in node_ids

    def test_seepage_has_waterway_junction_edge(self):
        """junction_a of seepage's waterway → seepage node edge must exist."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("junc_J2_2", "filt_Filt1_1") in pairs

    def test_seepage_has_reservoir_edge(self):
        """seepage node → reservoir edge must exist."""
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
                "production_factor": 0.0025,
                "main_reservoir": 1,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        "reservoir_production_factor_array": [
            {
                "uid": 1,
                "name": "eff_T1",
                "turbine": 1,
                "reservoir": 1,
                "mean_production_factor": 0.0025,
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
                "production_factor": 0.0025,
                "main_reservoir": 1,
            }
        ],
        "generator_array": [{"uid": 1, "name": "G1", "bus": 1, "pmax": 100}],
        "bus_array": [{"uid": 1, "name": "B1"}],
        # No reservoir_production_factor_array
    }
}


class TestReservoirProductionFactor:
    """Verify reservoir_production_factor_array and main_reservoir edge drawing."""

    def _build(self, planning, subsystem="hydro"):
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem=subsystem, opts=fo)
        return builder.build()

    def _edge_pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_reservoir_production_factor_draws_edge(self):
        """reservoir_production_factor_array must produce a reservoir → turbine edge."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        pairs = self._edge_pairs(model)
        assert ("res_Res1_1", "turb_T1_1") in pairs

    def test_reservoir_production_factor_suppresses_main_reservoir_fallback(self):
        """main_reservoir edge must not duplicate when efficiency array covers it."""
        model = self._build(_HYDRO_WITH_EFFICIENCY)
        eff_edges = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "turb_T1_1"
        ]
        # Exactly one edge (from reservoir_production_factor_array, not main_reservoir)
        assert len(eff_edges) == 1

    def test_main_reservoir_fallback_draws_edge_when_no_efficiency_array(self):
        """Turbine.main_reservoir must produce a reservoir → turbine edge when
        no reservoir_production_factor_array entry exists for that turbine."""
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
# ReservoirSeepage → reservoir dependency
# ---------------------------------------------------------------------------


class TestReservoirSeepageReservoirDependency:
    """Confirm the seepage-to-reservoir edge is present in every valid topology."""

    def test_seepage_reservoir_edge_present(self):
        """seepage_node → reservoir edge must always be drawn."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        pairs = {(e.src, e.dst) for e in model.edges}
        assert ("filt_Filt1_1", "res_Res1_1") in pairs, (
            "seepage → reservoir dependency edge missing"
        )

    def test_seepage_reservoir_edge_style(self):
        """The seepage→reservoir edge must use a dotted style."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "filt_Filt1_1" and e.dst == "res_Res1_1"
        ]
        assert filt_res_edges, "seepage → reservoir edge not found"
        assert filt_res_edges[0].style == "dotted"

    def test_seepage_reservoir_edge_color(self):
        """The seepage→reservoir edge must use the seepage_border colour."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_HYDRO_PLANNING, subsystem="hydro", opts=fo)
        model = builder.build()
        filt_res_edges = [
            e for e in model.edges if e.src == "filt_Filt1_1" and e.dst == "res_Res1_1"
        ]
        assert filt_res_edges
        assert filt_res_edges[0].color == gd._PALETTE["seepage_border"]  # noqa: SLF001


# ---------------------------------------------------------------------------
# Hydro passthrough (pasada-style)
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# Full pasada chain
# ---------------------------------------------------------------------------


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
                    "production_factor": 1.0,
                },
                {
                    "uid": 20,
                    "name": "Pasada2",
                    "waterway": 20,
                    "generator": 20,
                    "production_factor": 1.0,
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


# ---------------------------------------------------------------------------
# Turbine with flow (alternative to waterway)
# ---------------------------------------------------------------------------

_TURBINE_FLOW_PLANNING = {
    "system": {
        "name": "turbine_flow_test",
        "bus_array": [{"uid": 1, "name": "B1"}],
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
        ],
        "flow_array": [
            {"uid": 1, "name": "F1", "junction": 1, "discharge": 50},
        ],
        "generator_array": [
            {"uid": 1, "name": "G1", "bus": 1, "pmax": 30, "type": "pasada"},
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T_flow",
                "flow": 1,
                "generator": 1,
                "production_factor": 1.0,
            },
        ],
    },
}


class TestTurbineFlowMode:
    """Verify turbine with ``flow`` instead of ``waterway``."""

    def _build(self, subsystem="hydro"):
        return _build_model(_TURBINE_FLOW_PLANNING, subsystem=subsystem)

    def _pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_flow_to_turbine_edge(self):
        """flow → turbine edge must exist when turbine uses flow ref."""
        model = self._build()
        pairs = self._pairs(model)
        assert ("flow_F1_1", "turb_T_flow_1") in pairs

    def test_turbine_to_generator_edge(self):
        """turbine → generator edge must exist."""
        model = self._build(subsystem="full")
        pairs = self._pairs(model)
        assert ("turb_T_flow_1", "gen_G1_1") in pairs

    def test_no_waterway_edges_when_flow_used(self):
        """No junction→turbine waterway edges when flow is used."""
        model = self._build()
        pairs = self._pairs(model)
        # Junction-to-turbine edges come from waterway path only
        junc_turb = {
            (s, d) for s, d in pairs if s.startswith("junc_") and d.startswith("turb_")
        }
        assert not junc_turb

    def test_flow_turbine_node_exists(self):
        """Turbine node must exist."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "turb_T_flow_1" in node_ids

    def test_no_dangling_edges(self):
        model = self._build(subsystem="full")
        _assert_no_dangling_edges(model)


# ---------------------------------------------------------------------------
# Embedded reservoir features (seepage, discharge_limit, production_factor)
# ---------------------------------------------------------------------------

_EMBEDDED_RESERVOIR_PLANNING = {
    "system": {
        "name": "embedded_reservoir_test",
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
            {"uid": 3, "name": "J3"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 2, "fmax": 500},
            {"uid": 2, "name": "W2", "junction_a": 1, "junction_b": 3, "fmax": 200},
        ],
        "reservoir_array": [
            {
                "uid": 1,
                "name": "Res1",
                "junction": 1,
                "emax": 8000,
                "soft_emin": 500,
                "soft_emin_cost": 100,
            },
        ],
        "bus_array": [{"uid": 1, "name": "B1"}],
        "generator_array": [
            {"uid": 1, "name": "G_hydro", "bus": 1, "pmax": 100},
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "T1",
                "waterway": 1,
                "generator": 1,
                "production_factor": 0.003,
                "main_reservoir": 1,
            },
        ],
        # These arrays simulate the output of expand_reservoir_constraints():
        # originally embedded in the reservoir, now flattened to system-level.
        "reservoir_seepage_array": [
            {
                "uid": 100,
                "name": "Res1_seep_W2",
                "waterway": 2,
                "reservoir": 1,
                "slope": 0.001,
                "constant": 0.5,
            },
        ],
        "reservoir_discharge_limit_array": [
            {
                "uid": 200,
                "name": "Res1_dlim_W1",
                "waterway": 1,
                "reservoir": 1,
                "segments": [
                    {"volume": 0.0, "slope": 0.0, "intercept": 300},
                    {"volume": 4000.0, "slope": 0.05, "intercept": 100},
                ],
            },
        ],
        "reservoir_production_factor_array": [
            {
                "uid": 300,
                "name": "Res1_pf_T1",
                "turbine": 1,
                "reservoir": 1,
                "mean_production_factor": 0.003,
                "segments": [
                    {"volume": 0.0, "slope": 0.0, "constant": 0.002},
                    {"volume": 4000.0, "slope": 0.0001, "constant": 0.003},
                ],
            },
        ],
    },
}


class TestEmbeddedReservoirFeatures:
    """Verify diagram rendering of expanded embedded reservoir constraints.

    The C++ ``expand_reservoir_constraints()`` flattens inline reservoir
    definitions (seepage, discharge_limit, production_factor) into system-level
    arrays.  The diagram must render the flattened arrays correctly.
    """

    def _build(self, subsystem="hydro"):
        return _build_model(_EMBEDDED_RESERVOIR_PLANNING, subsystem=subsystem)

    def _pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def _node_ids(self, model):
        return {n.node_id for n in model.nodes}

    def test_reservoir_node_exists(self):
        model = self._build()
        assert "res_Res1_1" in self._node_ids(model)

    def test_seepage_node_from_embedded(self):
        """Seepage flattened from embedded definition must appear as a node."""
        model = self._build()
        assert "filt_Res1_seep_W2_100" in self._node_ids(model)

    def test_seepage_to_reservoir_edge(self):
        """Seepage → reservoir edge for embedded seepage."""
        model = self._build()
        pairs = self._pairs(model)
        assert ("filt_Res1_seep_W2_100", "res_Res1_1") in pairs

    def test_seepage_waterway_junction_edge(self):
        """Seepage must connect to the junction_a of its waterway."""
        model = self._build()
        pairs = self._pairs(model)
        # W2's junction_a is J1 (uid=1)
        assert ("junc_J1_1", "filt_Res1_seep_W2_100") in pairs

    def test_production_factor_reservoir_to_turbine_edge(self):
        """Embedded production_factor must produce a reservoir → turbine edge."""
        model = self._build()
        pairs = self._pairs(model)
        assert ("res_Res1_1", "turb_T1_1") in pairs

    def test_production_factor_suppresses_main_reservoir(self):
        """production_factor edge must not duplicate main_reservoir fallback."""
        model = self._build()
        res_turb = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "turb_T1_1"
        ]
        assert len(res_turb) == 1

    def test_discharge_limit_counted_as_hydro_element(self):
        """reservoir_discharge_limit_array must trigger hydro subsystem detection."""
        # Build with full subsystem — should detect hydro
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(
            _EMBEDDED_RESERVOIR_PLANNING, subsystem="full", opts=fo
        )
        model = builder.build()
        # Hydro nodes must exist (reservoir, turbine, junctions)
        node_kinds = {n.kind for n in model.nodes}
        assert "turbine" in node_kinds

    def test_no_dangling_edges(self):
        model = self._build(subsystem="full")
        _assert_no_dangling_edges(model)

    def test_no_duplicate_node_ids(self):
        model = self._build()
        _assert_no_duplicate_node_ids(model)


# ---------------------------------------------------------------------------
# Reservoir discharge limit hydro detection
# ---------------------------------------------------------------------------


class TestReservoirDischargeLimitHydroDetection:
    """Verify that reservoir_discharge_limit_array alone triggers hydro mode."""

    _PLANNING_DL_ONLY = {
        "system": {
            "name": "dl_only_test",
            "bus_array": [{"uid": 1, "name": "B1"}],
            "junction_array": [{"uid": 1, "name": "J1"}],
            "waterway_array": [
                {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 1, "fmax": 100},
            ],
            "reservoir_array": [
                {"uid": 1, "name": "R1", "junction": 1, "emax": 1000},
            ],
            "reservoir_discharge_limit_array": [
                {
                    "uid": 1,
                    "name": "DL1",
                    "waterway": 1,
                    "reservoir": 1,
                    "segments": [{"volume": 0, "slope": 0, "intercept": 50}],
                },
            ],
        },
    }

    def test_hydro_subsystem_detected(self):
        """reservoir_discharge_limit_array must trigger hydro subsystem."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(self._PLANNING_DL_ONLY, subsystem="full", opts=fo)
        model = builder.build()
        # Reservoir and junction nodes must be present (hydro was detected)
        node_kinds = {n.kind for n in model.nodes}
        assert any("reservoir" in k for k in node_kinds)

    def test_element_count_includes_discharge_limits(self):
        """_count_elements must include reservoir_discharge_limit_array."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(self._PLANNING_DL_ONLY, subsystem="full", opts=fo)
        count = builder._count_elements()  # noqa: SLF001
        # At least: 1 bus + 1 junction + 1 waterway + 1 reservoir + 1 dl = 5
        assert count >= 5


# ---------------------------------------------------------------------------
# VolumeRight and FlowRight — water rights diagram elements
# ---------------------------------------------------------------------------

_RIGHTS_PLANNING = {
    "system": {
        "name": "rights_test",
        "junction_array": [
            {"uid": 1, "name": "J1"},
            {"uid": 2, "name": "J2"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "W1", "junction_a": 1, "junction_b": 2},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "Res1", "junction": 1, "emax": 5000},
        ],
        "volume_right_array": [
            {
                "uid": 1,
                "name": "VR1",
                "purpose": "irrigation",
                "reservoir": 1,
                "emax": 200,
            },
            {
                "uid": 2,
                "name": "VR2",
                "purpose": "generation",
                "reservoir": 1,
                "right_reservoir": 1,
                "direction": -1,
                "emax": 100,
            },
        ],
        "flow_right_array": [
            {
                "uid": 1,
                "name": "FR1",
                "purpose": "environmental",
                "junction": 2,
                "discharge": 15.0,
            },
        ],
    }
}


class TestVolumeRightDiagram:
    """Verify VolumeRight nodes and edges are rendered correctly."""

    def _build(self, planning=None, subsystem="hydro", compact=False):
        fo = gd.FilterOptions(aggregate="none", compact=compact)
        builder = gd.TopologyBuilder(
            planning or _RIGHTS_PLANNING, subsystem=subsystem, opts=fo
        )
        return builder.build()

    def _edge_pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_volume_right_nodes_created(self):
        """volume_right_array entries must produce nodes of kind 'volume_right'."""
        model = self._build()
        vr_nodes = [n for n in model.nodes if n.kind == "volume_right"]
        assert len(vr_nodes) == 2

    def test_volume_right_node_ids(self):
        """Volume right nodes must use the 'vright_' prefix."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "vright_VR1_1" in node_ids
        assert "vright_VR2_2" in node_ids

    def test_volume_right_reservoir_edge(self):
        """VolumeRight must have a directed edge from its source reservoir."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("res_Res1_1", "vright_VR1_1") in pairs

    def test_volume_right_right_reservoir_edge(self):
        """VolumeRight with right_reservoir must have a directed edge to that right."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("vright_VR2_2", "vright_VR1_1") in pairs

    def test_volume_right_label_contains_purpose(self):
        """Non-compact label must include the purpose field."""
        model = self._build()
        vr1 = next(n for n in model.nodes if n.node_id == "vright_VR1_1")
        assert "irrigation" in vr1.label

    def test_volume_right_label_contains_emax(self):
        """Non-compact label must include emax in hm³."""
        model = self._build()
        vr1 = next(n for n in model.nodes if n.node_id == "vright_VR1_1")
        assert "hm³" in vr1.label or "hm\u00b3" in vr1.label

    def test_volume_right_compact_label_is_name_only(self):
        """In compact mode, VolumeRight label must be just the name."""
        model = self._build(compact=True)
        vr1 = next(n for n in model.nodes if n.node_id == "vright_VR1_1")
        assert "[VolRight]" not in vr1.label

    def test_volume_right_edge_uses_right_edge_color(self):
        """Reservoir → VolumeRight edge must use the right_edge palette color."""
        model = self._build()
        edges = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "vright_VR1_1"
        ]
        assert edges
        assert edges[0].color == gd._PALETTE["right_edge"]  # noqa: SLF001

    def test_volume_right_edge_is_dotted(self):
        """Reservoir → VolumeRight edge must use dotted style."""
        model = self._build()
        edges = [
            e for e in model.edges if e.src == "res_Res1_1" and e.dst == "vright_VR1_1"
        ]
        assert edges
        assert edges[0].style == "dotted"

    def test_volume_right_in_full_subsystem(self):
        """VolumeRight nodes must appear in full subsystem."""
        model = self._build(subsystem="full")
        vr_nodes = [n for n in model.nodes if n.kind == "volume_right"]
        assert len(vr_nodes) == 2

    def test_volume_right_triggers_has_hydro(self):
        """A system with only volume_right_array must activate the hydro path."""
        planning = {
            "system": {
                "name": "vright_only",
                "volume_right_array": [
                    {"uid": 1, "name": "VR1", "purpose": "irrigation"}
                ],
            }
        }
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem="full", opts=fo)
        builder.build()
        # subsystem must remain "full" or "hydro", not forced to "electrical"
        assert builder.subsystem in ("full", "hydro")

    def test_volume_right_no_reservoir_no_edge(self):
        """VolumeRight without a reservoir reference must still produce a node."""
        planning = {
            "system": {
                "name": "vright_no_res",
                "volume_right_array": [
                    {"uid": 1, "name": "VR_nores", "purpose": "irrigation"}
                ],
            }
        }
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem="hydro", opts=fo)
        model = builder.build()
        vr_nodes = [n for n in model.nodes if n.kind == "volume_right"]
        assert len(vr_nodes) == 1
        # No edges since there's no reservoir
        vr_edges = [e for e in model.edges if "vright_VR_nores_1" in (e.src, e.dst)]
        assert not vr_edges


class TestFlowRightDiagram:
    """Verify FlowRight nodes and edges are rendered correctly."""

    def _build(self, planning=None, subsystem="hydro", compact=False):
        fo = gd.FilterOptions(aggregate="none", compact=compact)
        builder = gd.TopologyBuilder(
            planning or _RIGHTS_PLANNING, subsystem=subsystem, opts=fo
        )
        return builder.build()

    def _edge_pairs(self, model):
        return {(e.src, e.dst) for e in model.edges}

    def test_flow_right_node_created(self):
        """flow_right_array entries must produce nodes of kind 'flow_right'."""
        model = self._build()
        fr_nodes = [n for n in model.nodes if n.kind == "flow_right"]
        assert len(fr_nodes) == 1

    def test_flow_right_node_id(self):
        """Flow right nodes must use the 'fright_' prefix."""
        model = self._build()
        node_ids = {n.node_id for n in model.nodes}
        assert "fright_FR1_1" in node_ids

    def test_flow_right_junction_edge(self):
        """FlowRight must have a directed edge from its reference junction."""
        model = self._build()
        pairs = self._edge_pairs(model)
        assert ("junc_J2_2", "fright_FR1_1") in pairs

    def test_flow_right_label_contains_purpose(self):
        """Non-compact label must include the purpose field."""
        model = self._build()
        fr1 = next(n for n in model.nodes if n.node_id == "fright_FR1_1")
        assert "environmental" in fr1.label

    def test_flow_right_label_contains_discharge(self):
        """Non-compact label must include discharge in m³/s."""
        model = self._build()
        fr1 = next(n for n in model.nodes if n.node_id == "fright_FR1_1")
        assert "m³" in fr1.label or "m\u00b3" in fr1.label

    def test_flow_right_compact_label_is_name_only(self):
        """In compact mode, FlowRight label must be just the name."""
        model = self._build(compact=True)
        fr1 = next(n for n in model.nodes if n.node_id == "fright_FR1_1")
        assert "[FlowRight]" not in fr1.label

    def test_flow_right_edge_uses_right_edge_color(self):
        """Junction → FlowRight edge must use the right_edge palette color."""
        model = self._build()
        edges = [
            e for e in model.edges if e.src == "junc_J2_2" and e.dst == "fright_FR1_1"
        ]
        assert edges
        assert edges[0].color == gd._PALETTE["right_edge"]  # noqa: SLF001

    def test_flow_right_edge_is_dotted(self):
        """Junction → FlowRight edge must use dotted style."""
        model = self._build()
        edges = [
            e for e in model.edges if e.src == "junc_J2_2" and e.dst == "fright_FR1_1"
        ]
        assert edges
        assert edges[0].style == "dotted"

    def test_flow_right_in_full_subsystem(self):
        """FlowRight nodes must appear in full subsystem."""
        model = self._build(subsystem="full")
        fr_nodes = [n for n in model.nodes if n.kind == "flow_right"]
        assert len(fr_nodes) == 1

    def test_flow_right_triggers_has_hydro(self):
        """A system with only flow_right_array must activate the hydro path."""
        planning = {
            "system": {
                "name": "fright_only",
                "flow_right_array": [
                    {
                        "uid": 1,
                        "name": "FR1",
                        "purpose": "environmental",
                        "discharge": 5,
                    }
                ],
            }
        }
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem="full", opts=fo)
        builder.build()
        assert builder.subsystem in ("full", "hydro")

    def test_flow_right_no_junction_no_edge(self):
        """FlowRight without a junction reference must still produce a node."""
        planning = {
            "system": {
                "name": "fright_no_junc",
                "flow_right_array": [
                    {"uid": 1, "name": "FR_nojunc", "purpose": "environmental"}
                ],
            }
        }
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(planning, subsystem="hydro", opts=fo)
        model = builder.build()
        fr_nodes = [n for n in model.nodes if n.kind == "flow_right"]
        assert len(fr_nodes) == 1
        fr_edges = [e for e in model.edges if "fright_FR_nojunc_1" in (e.src, e.dst)]
        assert not fr_edges

    def test_count_elements_includes_rights(self):
        """_count_elements must count volume_right_array and flow_right_array entries."""
        fo = gd.FilterOptions(aggregate="none")
        builder = gd.TopologyBuilder(_RIGHTS_PLANNING, opts=fo)
        count = builder._count_elements()  # noqa: SLF001
        # System has: 2 junctions + 1 waterway + 1 reservoir + 2 volume rights +
        # 1 flow right = 7
        assert count >= 7
