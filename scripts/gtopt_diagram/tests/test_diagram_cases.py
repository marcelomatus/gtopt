"""Tests for gtopt_diagram – reference case topology validation.

Covers:
- IEEE 9-bus (original), IEEE 9-bus (solar profile), IEEE 14-bus cases
- bat_4b (4-bus with battery) case
- Minimal hand-crafted single-element cases
- Battery direct-bus and battery-with-converter cases
- Empty system and generator-without-bus edge cases
"""

import json
from pathlib import Path

import pytest

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


def _assert_valid_mermaid(model):
    """Assert that mermaid output looks structurally valid."""
    mermaid = gd.render_mermaid(model)
    assert "flowchart" in mermaid
    # Must have at least one node definition (non-empty lines after flowchart)
    lines = [ln.strip() for ln in mermaid.splitlines() if ln.strip()]
    assert len(lines) > 3, "Mermaid output too short to contain nodes"


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
# IEEE case tests
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
# Singular element cases (minimal hand-crafted JSON)
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


# ---------------------------------------------------------------------------
# Empty system and generator-without-bus edge cases
# ---------------------------------------------------------------------------


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
