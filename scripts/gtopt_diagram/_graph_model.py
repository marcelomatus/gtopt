# SPDX-License-Identifier: BSD-3-Clause
"""Graph model data classes for gtopt network diagrams.

Provides the core data structures used to represent diagram elements:

- :class:`FilterOptions` — controls element-reduction strategies for large diagrams.
- :class:`Node` — a single node (bus, generator, demand, etc.).
- :class:`Edge` — a directed or undirected edge between two nodes.
- :class:`GraphModel` — a collection of nodes and edges with a title.
"""

from __future__ import annotations

from dataclasses import dataclass, field


# Auto-reduction thresholds (element count)
# When --aggregate auto (default): pick a strategy based on total element count.
AUTO_NONE_THRESHOLD = 100  # < 100 elements  -> show everything individually
AUTO_BUS_THRESHOLD = 1000  # 100-999 elements -> aggregate per bus
# >= 1000 elements: aggregate per type + smart voltage threshold (aggressive)

# Smart voltage threshold: target at most this many visible buses after reduction.
AUTO_MAX_HV_BUSES = 64  # target bus count for aggressive auto mode


@dataclass
class FilterOptions:
    """Controls element-reduction strategies for large gtopt diagrams.

    Aggregation modes (``--aggregate``):
      ``auto``    Automatically choose based on element count (default):
                    < 100  -> ``none``
                    100-999 -> ``bus``
                    >= 1000  -> ``type`` + smart voltage threshold that keeps
                               at most ``AUTO_MAX_HV_BUSES`` visible buses
      ``none``    Show every individual element (best for small cases).
      ``bus``     Collapse all generators at each bus into one summary node.
      ``type``    Collapse generators by type (hydro/solar/wind/thermal/BESS)
                  within each bus -- one node per (bus, type) pair.
      ``global``  One node per generator type for the whole system.

    Additional filters:
      ``no_generators``     If True, omit all generator nodes (topology-only view).
      ``top_gens``          Keep only the top-N generators by pmax per bus (0 = all).
      ``filter_types``      List of generator types to include (empty = all).
      ``focus_buses``       Show only elements reachable within ``focus_hops`` hops
                            from the named buses.
      ``max_nodes``         Hard cap: if node count would exceed this, auto-upgrade
                            to the next aggregation mode.
      ``hide_isolated``     Remove nodes with no edges.
      ``compact``           Suppress detail labels (show only name/type/count).
      ``voltage_threshold`` Lump buses (and their lines) below this voltage [kV]
                            into their nearest high-voltage neighbour.  Buses
                            without a ``voltage`` field are never lumped.
                            0 = disabled (default).
    """

    aggregate: str = "auto"  # auto | none | bus | type | global
    no_generators: bool = False  # omit all generator nodes
    top_gens: int = 0  # 0 = no limit
    filter_types: list[str] = field(default_factory=list)
    focus_buses: list[str] = field(default_factory=list)
    focus_hops: int = 2
    max_nodes: int = 0  # 0 = no limit
    hide_isolated: bool = False
    compact: bool = False
    voltage_threshold: float = 0.0  # kV; 0 = disabled
    # Draw "logical / LP-internal" entities that have no physical
    # counterpart: ``Commitment``, ``SimpleCommitment``, ``ReserveZone``,
    # ``ReserveProvision``, ``UserConstraint``, ``DecisionVariable``,
    # ``Emission`` family, ``Generator/Demand`` profile references.
    # Library default is True (backwards-compatible for callers and
    # tests).  The ``gtopt_diagram`` CLI overrides this to False by
    # default since the abstract LP rows clutter the topology view —
    # opt back in with ``--draw-logical``.
    draw_logical_elements: bool = True
    # Cluster-prefix names that the interactive HTML auto-collapses on
    # load via ``network.cluster()`` — the user sees one bubble per
    # cluster, double-click to expand.  Empty by default so the
    # grouping acts as a **layout hint only** (vis.js physics gathers
    # members spatially via the shared ``group`` field; graphviz lays
    # them out inside a subgraph box).  Opt into collapse-on-load for
    # any tier via ``--collapse <prefix[,prefix...]>`` —
    # e.g. ``--collapse substation,plant`` to bubble those up but keep
    # basins and communities expanded.  ``--collapse all`` collapses
    # everything.
    collapse_groups: list[str] = field(default_factory=list)


@dataclass
class Node:
    """A single node in the diagram graph."""

    node_id: str
    label: str
    kind: str
    tooltip: str = ""
    cluster: str = ""  # "electrical" or "hydro"
    # Optional **outer** grouping label that wraps one or more
    # ``subcluster`` subgraphs.  Used for Basins (hydro drainage area
    # containing reservoirs + plants + waterways).  Empty string means
    # "no super-cluster" — the node renders at the same level as a
    # subcluster does today.  Render order:
    # ``cluster > super_cluster > subcluster > node``.
    super_cluster: str = ""
    # Optional inner grouping label.  Currently used to render a
    # ``Plant`` as a graphviz subgraph nested inside its containing
    # ``cluster`` so the configuration variants of one physical machine
    # (e.g. ATA_CC_1_ConfTGA / _ConfTGB / _ConfTV) appear visually
    # bundled.  Set to ``"plant:<name>"`` on member generators.
    subcluster: str = ""
    size: float = 0.0  # 0 = use default from _PYVIS_SIZE_MAP
    # True when the node is an **articulation point** of the AC graph
    # — removing it would split the network into ≥2 disconnected
    # islands.  Rendered with a red border + warning badge in the
    # tooltip.  Set by ``TopologyBuilder._critical_topology``.
    is_critical: bool = False


@dataclass
class Edge:
    """An edge (arc) connecting two nodes in the diagram graph."""

    src: str
    dst: str
    label: str = ""
    style: str = "solid"  # solid | dashed | dotted
    color: str = ""
    directed: bool = True
    weight: float = 1.0
    # True when the edge is a **bridge** of the AC graph — removing it
    # would split the network into ≥2 disconnected islands (the classic
    # N-1 single-line failure mode).  Rendered in red with a "BRIDGE"
    # decoration.  Set by ``TopologyBuilder._critical_topology``.
    is_critical: bool = False


@dataclass
class GraphModel:
    """A graph of nodes and edges representing a gtopt network diagram."""

    title: str = "gtopt Network"
    nodes: list[Node] = field(default_factory=list)
    edges: list[Edge] = field(default_factory=list)

    def add_node(self, n: Node) -> None:
        """Append a node to the graph."""
        self.nodes.append(n)

    def add_edge(self, e: Edge) -> None:
        """Append an edge to the graph."""
        self.edges.append(e)
