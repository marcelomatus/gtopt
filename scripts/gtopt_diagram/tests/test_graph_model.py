"""Tests for gtopt_diagram._graph_model -- graph model data classes.

Covers:
- ``FilterOptions`` default values and custom construction.
- ``Node`` creation and field access.
- ``Edge`` creation, defaults, and field access.
- ``GraphModel`` add_node, add_edge, and default title.
- Threshold constants.
"""

from gtopt_diagram._graph_model import (
    AUTO_BUS_THRESHOLD,
    AUTO_MAX_HV_BUSES,
    AUTO_NONE_THRESHOLD,
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)


# ---------------------------------------------------------------------------
# FilterOptions
# ---------------------------------------------------------------------------


class TestFilterOptions:
    """Verify FilterOptions defaults and custom construction."""

    def test_defaults(self):
        fo = FilterOptions()
        assert fo.aggregate == "auto"
        assert fo.no_generators is False
        assert fo.top_gens == 0
        assert fo.filter_types == []
        assert fo.focus_buses == []
        assert fo.focus_hops == 2
        assert fo.max_nodes == 0
        assert fo.hide_isolated is False
        assert fo.compact is False
        assert fo.voltage_threshold == 0.0

    def test_custom_values(self):
        fo = FilterOptions(
            aggregate="bus",
            no_generators=True,
            top_gens=5,
            filter_types=["solar", "wind"],
            focus_buses=["B1", "B2"],
            focus_hops=3,
            max_nodes=100,
            hide_isolated=True,
            compact=True,
            voltage_threshold=220.0,
        )
        assert fo.aggregate == "bus"
        assert fo.no_generators is True
        assert fo.top_gens == 5
        assert fo.filter_types == ["solar", "wind"]
        assert fo.focus_buses == ["B1", "B2"]
        assert fo.focus_hops == 3
        assert fo.max_nodes == 100
        assert fo.hide_isolated is True
        assert fo.compact is True
        assert fo.voltage_threshold == 220.0

    def test_filter_types_independent_per_instance(self):
        """Mutable default (list) must not be shared between instances."""
        fo1 = FilterOptions()
        fo2 = FilterOptions()
        fo1.filter_types.append("hydro")
        assert fo2.filter_types == []

    def test_focus_buses_independent_per_instance(self):
        """Mutable default (list) must not be shared between instances."""
        fo1 = FilterOptions()
        fo2 = FilterOptions()
        fo1.focus_buses.append("B1")
        assert fo2.focus_buses == []


# ---------------------------------------------------------------------------
# Node
# ---------------------------------------------------------------------------


class TestNode:
    """Verify Node creation and field access."""

    def test_basic_creation(self):
        n = Node(node_id="bus_1", label="B1", kind="bus")
        assert n.node_id == "bus_1"
        assert n.label == "B1"
        assert n.kind == "bus"
        assert n.tooltip == ""
        assert n.cluster == ""

    def test_full_creation(self):
        n = Node(
            node_id="gen_5",
            label="Solar Farm\n100 MW",
            kind="gen_solar",
            tooltip="Generator uid=5 pmax=100",
            cluster="electrical",
        )
        assert n.node_id == "gen_5"
        assert n.label == "Solar Farm\n100 MW"
        assert n.kind == "gen_solar"
        assert n.tooltip == "Generator uid=5 pmax=100"
        assert n.cluster == "electrical"

    def test_equality(self):
        n1 = Node(node_id="bus_1", label="B1", kind="bus")
        n2 = Node(node_id="bus_1", label="B1", kind="bus")
        assert n1 == n2

    def test_inequality(self):
        n1 = Node(node_id="bus_1", label="B1", kind="bus")
        n2 = Node(node_id="bus_2", label="B2", kind="bus")
        assert n1 != n2


# ---------------------------------------------------------------------------
# Edge
# ---------------------------------------------------------------------------


class TestEdge:
    """Verify Edge creation, defaults, and field access."""

    def test_minimal_creation(self):
        e = Edge(src="bus_1", dst="bus_2")
        assert e.src == "bus_1"
        assert e.dst == "bus_2"
        assert e.label == ""
        assert e.style == "solid"
        assert e.color == ""
        assert e.directed is True
        assert e.weight == 1.0

    def test_full_creation(self):
        e = Edge(
            src="gen_1",
            dst="bus_1",
            label="100 MW",
            style="dashed",
            color="#FF0000",
            directed=False,
            weight=42.5,
        )
        assert e.src == "gen_1"
        assert e.dst == "bus_1"
        assert e.label == "100 MW"
        assert e.style == "dashed"
        assert e.color == "#FF0000"
        assert e.directed is False
        assert e.weight == 42.5

    def test_equality(self):
        e1 = Edge(src="a", dst="b", label="x")
        e2 = Edge(src="a", dst="b", label="x")
        assert e1 == e2


# ---------------------------------------------------------------------------
# GraphModel
# ---------------------------------------------------------------------------


class TestGraphModel:
    """Verify GraphModel add_node, add_edge, and defaults."""

    def test_default_title(self):
        gm = GraphModel()
        assert gm.title == "gtopt Network"
        assert gm.nodes == []
        assert gm.edges == []

    def test_custom_title(self):
        gm = GraphModel(title="My Network")
        assert gm.title == "My Network"

    def test_add_node(self):
        gm = GraphModel()
        n = Node(node_id="bus_1", label="B1", kind="bus")
        gm.add_node(n)
        assert len(gm.nodes) == 1
        assert gm.nodes[0] is n

    def test_add_edge(self):
        gm = GraphModel()
        e = Edge(src="bus_1", dst="bus_2")
        gm.add_edge(e)
        assert len(gm.edges) == 1
        assert gm.edges[0] is e

    def test_add_multiple_nodes_and_edges(self):
        gm = GraphModel()
        n1 = Node(node_id="bus_1", label="B1", kind="bus")
        n2 = Node(node_id="bus_2", label="B2", kind="bus")
        e = Edge(src="bus_1", dst="bus_2")
        gm.add_node(n1)
        gm.add_node(n2)
        gm.add_edge(e)
        assert len(gm.nodes) == 2
        assert len(gm.edges) == 1

    def test_nodes_list_independent_per_instance(self):
        """Mutable default (list) must not be shared between instances."""
        gm1 = GraphModel()
        gm2 = GraphModel()
        gm1.add_node(Node(node_id="bus_1", label="B1", kind="bus"))
        assert len(gm2.nodes) == 0

    def test_edges_list_independent_per_instance(self):
        """Mutable default (list) must not be shared between instances."""
        gm1 = GraphModel()
        gm2 = GraphModel()
        gm1.add_edge(Edge(src="a", dst="b"))
        assert len(gm2.edges) == 0


# ---------------------------------------------------------------------------
# Threshold constants
# ---------------------------------------------------------------------------


class TestConstants:
    """Verify threshold constants have expected values."""

    def test_auto_none_threshold(self):
        assert AUTO_NONE_THRESHOLD == 100

    def test_auto_bus_threshold(self):
        assert AUTO_BUS_THRESHOLD == 1000

    def test_auto_max_hv_buses(self):
        assert AUTO_MAX_HV_BUSES == 64
