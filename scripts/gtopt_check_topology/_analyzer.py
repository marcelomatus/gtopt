# SPDX-License-Identifier: BSD-3-Clause
"""Graph + topology analysis primitives for gtopt_check_topology.

The analyser ingests a gtopt planning dictionary (the same shape consumed by
``gtopt_check_json``) and produces an :class:`AnalysisReport` covering:

* connectivity     -- islands, isolated buses, stubs (A1/A2/A3)
* KVL loop quality -- fundamental cycles, per-cycle danger score, SOS1
                      candidates, bridge edges (B1-B5)
* numerical class. -- DC links, negative R/X, voltage mixing (C1/C2/C3)

The module is intentionally free of any I/O or rich-rendering code -- those
live in :mod:`gtopt_check_topology._render`.
"""

from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import networkx as nx

# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------

# Reactance below this magnitude (per-unit) is treated as an HVDC / pure DC
# link with controllable power flow -- excluded from cycle_basis because such
# links do not enforce KVL.
DC_REACTANCE_TOL = 1e-12

# Default cycle-danger cut-off (matches the validated prototype score on
# the v0407 PLEXOS bundle).
DEFAULT_DANGER_THRESHOLD = 5

# Cycles larger than this are skipped to keep cycle_basis cheap on dense
# transmission networks.
DEFAULT_MAX_CYCLE_SIZE = 20


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _to_max_float(value: Any) -> float | None:
    """Coerce a JSON numeric / nested-list / time-series to a single float.

    gtopt JSON line capacities (``tmax_ab``) may be a plain float, a flat
    list of per-block values, or a nested list-of-lists (per-stage block
    arrays).  The danger score only needs a representative magnitude, so
    we collapse to the maximum.  Returns ``None`` if no numeric value is
    found.
    """
    if value is None:
        return None
    if isinstance(value, bool):  # bool is an int subclass -- reject explicitly
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, list):
        flat: list[float] = []
        stack: list[Any] = [value]
        while stack:
            item = stack.pop()
            if isinstance(item, list):
                stack.extend(item)
            elif isinstance(item, (int, float)) and not isinstance(item, bool):
                flat.append(float(item))
        return max(flat) if flat else None
    return None


_VOLTAGE_NAME_RE = re.compile(r"(\d{2,4})\b")


def _bus_voltage_from_name(name: str) -> int | None:
    """Heuristic kV extractor for bus names like ``Concepcion110``.

    Returns the first 2-4 digit run found in the name, or ``None`` if no
    such substring exists.
    """
    match = _VOLTAGE_NAME_RE.search(name)
    if match is None:
        return None
    try:
        return int(match.group(1))
    except ValueError:  # pragma: no cover - regex guarantees digit-only
        return None


def _edge_key(a: str, b: str) -> tuple[str, str]:
    """Return a canonical (sorted) bus-pair key for multi-edge collapsing."""
    return (a, b) if a <= b else (b, a)


def _ratio(values: Iterable[float]) -> float:
    """Return ``max(values)/min(values)`` with safe handling of zeros."""
    vs = list(values)
    if not vs:
        return 1.0
    lo = min(vs)
    hi = max(vs)
    if lo <= 0.0:
        return float("inf")
    return hi / lo


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class LineRef:
    """Lightweight reference to a line in the source JSON."""

    uid: Any
    name: str
    bus_a: str
    bus_b: str
    reactance: float
    resistance: float
    tmax: float


@dataclass
class CycleInfo:
    """One fundamental cycle plus its derived danger metrics.

    ``segments`` lists ONE representative line per cycle edge (the
    min-reactance circuit) so that report tables stay readable.
    ``segments_all`` lists EVERY parallel circuit sitting on a cycle
    edge -- this is the source the SOS1 enumeration draws from.
    """

    buses: list[str]
    edges: list[tuple[str, str]]
    segments: list[LineRef]
    segments_all: list[LineRef]
    asym_x: float
    asym_tmax: float
    min_tmax: float
    n_buses: int
    score: int
    kvl_saturation_threshold_mw: float | None = None
    """B7: predicted MW circulation at which the lowest-X edge saturates.

    Computed as ``tmax_of_min_X / (1 - min(X)/sum(X))`` -- a small value
    means even trivial loop circulation will hit a tmax limit and force
    the LP into a binding KVL split.  ``None`` when not computable
    (e.g. degenerate single-segment loop or sum(X) == min(X)).
    """

    def is_dangerous(self, threshold: int = DEFAULT_DANGER_THRESHOLD) -> bool:
        return self.score >= threshold


@dataclass
class IslandInfo:
    """One connected component."""

    buses: list[str]
    has_generator: bool
    has_demand: bool


@dataclass
class VoltageMix:
    """A line whose endpoints sit at different (heuristic) voltage levels."""

    uid: Any
    name: str
    bus_a: str
    bus_b: str
    bus_a_voltage: int | None
    bus_b_voltage: int | None


@dataclass
class NegativeImpedance:
    """A line with a negative reactance or resistance (data error)."""

    uid: Any
    name: str
    reactance: float
    resistance: float


@dataclass
class ReserveZoneIssue:
    """A reserve_zone with no usable provider chain (D1)."""

    zone: str
    issue: str
    n_providers: int


@dataclass
class IslandFeasibility:
    """Per-island demand vs generation feasibility (D2)."""

    island_idx: int
    n_demand_buses: int
    n_gen_buses: int
    sum_pmax: float
    sum_peak_load: float
    issue: str


@dataclass
class BatteryEiniIssue:
    """A battery whose initial state-of-charge looks wrong (D3)."""

    name: str
    issue: str
    eini: float | None
    emin_first: float | None
    emax_first: float | None
    in_ucs: list[str] = field(default_factory=list)


@dataclass
class TimeVaryingCapacityIssue:
    """A line whose ``tmax_ab`` is mostly zero across blocks (E1)."""

    uid: Any
    name: str
    bus_a: str
    bus_b: str
    blocks_at_zero: int
    blocks_near_zero: int
    blocks_total: int
    max_tmax: float


@dataclass
class ReservoirCascadeIssue:
    """Reservoir / waterway topology issue (E2)."""

    name: str
    issue: str
    detail: str = ""


@dataclass
class ParallelCircuitIdentical:
    """A bus pair with multiple identical parallel circuits (B6)."""

    bus_a: str
    bus_b: str
    n_circuits: int
    reactance: float
    tmax: float
    uids: list[Any] = field(default_factory=list)
    on_dangerous_cycle: bool = False


@dataclass
class NetworkGraph:
    """Output of :func:`build_network_graph`.

    Keeps both the simple undirected graph (used for nx.cycle_basis /
    nx.bridges) and the full parallel-edge mapping needed for SOS1
    enumeration.
    """

    graph: nx.Graph
    """Simple graph -- one edge per bus pair (the min-reactance representative)."""

    edge_to_lines: dict[tuple[str, str], list[LineRef]]
    """Every line on each (sorted) bus pair, including parallel circuits."""

    dc_lines: list[LineRef]
    """Lines with |reactance| ≤ DC_REACTANCE_TOL (excluded from cycle_basis)."""

    self_loops: list[LineRef]
    """Lines whose endpoints coincide -- topologically degenerate."""

    skipped_unknown_bus: list[LineRef]
    """Lines referencing buses absent from bus_array."""


@dataclass
class AnalysisReport:
    """Everything :mod:`._render` needs to print or serialize."""

    bundle: str = ""
    n_buses: int = 0
    n_lines: int = 0
    n_cycles: int = 0

    islands: list[IslandInfo] = field(default_factory=list)
    isolated_buses: list[str] = field(default_factory=list)
    stubs: list[LineRef] = field(default_factory=list)

    cycles: list[CycleInfo] = field(default_factory=list)
    bridges: list[LineRef] = field(default_factory=list)

    dc_lines: list[LineRef] = field(default_factory=list)
    negative_impedance: list[NegativeImpedance] = field(default_factory=list)
    voltage_mixing: list[VoltageMix] = field(default_factory=list)

    skipped_unknown_bus: list[LineRef] = field(default_factory=list)
    self_loops: list[LineRef] = field(default_factory=list)

    # Tier-2 -- new collectors
    reserve_zone_coherence: list[ReserveZoneIssue] = field(default_factory=list)
    island_feasibility: list[IslandFeasibility] = field(default_factory=list)
    battery_eini_issues: list[BatteryEiniIssue] = field(default_factory=list)
    time_varying_capacity_issues: list[TimeVaryingCapacityIssue] = field(
        default_factory=list
    )
    reservoir_cascade_issues: list[ReservoirCascadeIssue] = field(default_factory=list)
    parallel_circuit_identical: list[ParallelCircuitIdentical] = field(
        default_factory=list
    )

    danger_threshold: int = DEFAULT_DANGER_THRESHOLD

    # --- derived helpers -------------------------------------------------

    @property
    def dangerous_cycles(self) -> list[CycleInfo]:
        return [c for c in self.cycles if c.is_dangerous(self.danger_threshold)]

    @property
    def benign_cycles(self) -> list[CycleInfo]:
        return [c for c in self.cycles if not c.is_dangerous(self.danger_threshold)]

    @property
    def sos1_candidates(self) -> list[LineRef]:
        """Union of all line UIDs sitting on a dangerous-cycle edge.

        Parallel circuits are included -- if any of the parallel lines
        is part of a dangerous cycle, all of its peers join the SOS1 list.
        """
        seen: set[Any] = set()
        out: list[LineRef] = []
        for cyc in self.dangerous_cycles:
            for seg in cyc.segments_all:
                if seg.uid in seen:
                    continue
                seen.add(seg.uid)
                out.append(seg)
        return out

    @property
    def islands_no_generator(self) -> list[IslandInfo]:
        return [i for i in self.islands if not i.has_generator]


# ---------------------------------------------------------------------------
# Graph builder
# ---------------------------------------------------------------------------


def _line_ref(line: dict[str, Any]) -> LineRef:
    return LineRef(
        uid=line.get("uid"),
        name=str(line.get("name", line.get("uid", "?"))),
        bus_a=str(line.get("bus_a", "")),
        bus_b=str(line.get("bus_b", "")),
        reactance=float(line.get("reactance", 0.0) or 0.0),
        resistance=float(line.get("resistance", 0.0) or 0.0),
        tmax=_to_max_float(line.get("tmax_ab", 0.0)) or 0.0,
    )


def build_network_graph(planning: dict[str, Any]) -> NetworkGraph:
    """Build the undirected graph used for KVL / connectivity analysis.

    * Excludes lines with ``|reactance| <= DC_REACTANCE_TOL`` -- they are
      treated as HVDC / DC links and live in ``dc_lines``.
    * Excludes self-loops (``bus_a == bus_b``).
    * Excludes lines referencing buses absent from ``bus_array``.
    * For multi-edges, the simple graph keeps the min-reactance line as
      its representative.  ``edge_to_lines`` retains every parallel
      circuit so the SOS1 list can later enumerate them all.
    """
    sys = planning.get("system", {}) or {}
    bus_array = sys.get("bus_array", []) or []
    line_array = sys.get("line_array", []) or []

    known_buses: set[str] = set()
    for bus in bus_array:
        name = bus.get("name")
        if name:
            known_buses.add(str(name))

    graph: nx.Graph = nx.Graph()
    for name in known_buses:
        graph.add_node(name)

    edge_to_lines: dict[tuple[str, str], list[LineRef]] = defaultdict(list)
    dc_lines: list[LineRef] = []
    self_loops: list[LineRef] = []
    skipped: list[LineRef] = []

    for raw in line_array:
        ref = _line_ref(raw)
        if not ref.bus_a or not ref.bus_b:
            skipped.append(ref)
            continue
        if ref.bus_a not in known_buses or ref.bus_b not in known_buses:
            skipped.append(ref)
            continue
        if ref.bus_a == ref.bus_b:
            self_loops.append(ref)
            continue
        if abs(ref.reactance) <= DC_REACTANCE_TOL:
            dc_lines.append(ref)
            continue
        key = _edge_key(ref.bus_a, ref.bus_b)
        edge_to_lines[key].append(ref)

    for key, refs in edge_to_lines.items():
        rep = min(refs, key=lambda r: abs(r.reactance))
        graph.add_edge(
            key[0],
            key[1],
            reactance=rep.reactance,
            tmax=rep.tmax,
            uid=rep.uid,
            name=rep.name,
        )

    return NetworkGraph(
        graph=graph,
        edge_to_lines=dict(edge_to_lines),
        dc_lines=dc_lines,
        self_loops=self_loops,
        skipped_unknown_bus=skipped,
    )


# ---------------------------------------------------------------------------
# Danger score
# ---------------------------------------------------------------------------


def cycle_danger_score(
    asym_x: float,
    asym_tmax: float,
    min_tmax: float,
    n_buses: int,
) -> int:
    """Return the prototype-validated cycle danger score.

    The component thresholds were calibrated against the v0407 PLEXOS
    bundle (180 SOS1 candidates across 48 dangerous cycles).  See the
    docstring on :class:`CycleInfo`.
    """
    score = 0
    score += 3 if asym_x >= 5 else 0
    score += 5 if asym_x >= 10 else 0
    score += 5 if asym_x >= 20 else 0
    score += 2 if asym_tmax >= 3 else 0
    score += 3 if asym_tmax >= 10 else 0
    score += 2 if min_tmax < 500 else 0
    score += 3 if min_tmax < 200 else 0
    score += 2 if n_buses <= 4 else 0
    return score


def _cycle_to_edges(cycle: list[str]) -> list[tuple[str, str]]:
    n = len(cycle)
    return [_edge_key(cycle[i], cycle[(i + 1) % n]) for i in range(n)]


# ---------------------------------------------------------------------------
# Connectivity / islands
# ---------------------------------------------------------------------------


def _bus_has_generator_map(planning: dict[str, Any]) -> dict[str, bool]:
    sys = planning.get("system", {}) or {}
    has_gen: dict[str, bool] = defaultdict(bool)

    # Map UIDs back to names so generator bus references that use a UID
    # are still attributed correctly.
    name_by_uid: dict[Any, str] = {}
    for bus in sys.get("bus_array", []) or []:
        uid = bus.get("uid")
        name = bus.get("name")
        if uid is not None and name:
            name_by_uid[uid] = str(name)

    def resolve(ref: Any) -> str | None:
        if ref is None:
            return None
        if isinstance(ref, str):
            return ref
        return name_by_uid.get(ref)

    for gen in sys.get("generator_array", []) or []:
        bid = resolve(gen.get("bus"))
        if bid:
            has_gen[bid] = True
    # Batteries and converters can also inject power -- count them too.
    for bat in sys.get("battery_array", []) or []:
        bid = resolve(bat.get("bus"))
        if bid:
            has_gen[bid] = True
    return dict(has_gen)


def _bus_has_demand_map(planning: dict[str, Any]) -> dict[str, bool]:
    sys = planning.get("system", {}) or {}
    name_by_uid: dict[Any, str] = {}
    for bus in sys.get("bus_array", []) or []:
        uid = bus.get("uid")
        name = bus.get("name")
        if uid is not None and name:
            name_by_uid[uid] = str(name)

    has_dem: dict[str, bool] = defaultdict(bool)
    for dem in sys.get("demand_array", []) or []:
        ref = dem.get("bus")
        bid: str | None = None
        if isinstance(ref, str):
            bid = ref
        elif ref is not None:
            bid = name_by_uid.get(ref)
        if bid:
            has_dem[bid] = True
    return dict(has_dem)


def _collect_islands(
    network: NetworkGraph,
    has_gen: dict[str, bool],
    has_dem: dict[str, bool],
) -> list[IslandInfo]:
    components = [sorted(c, key=str) for c in nx.connected_components(network.graph)]
    components.sort(key=len, reverse=True)
    out: list[IslandInfo] = []
    for comp in components:
        out.append(
            IslandInfo(
                buses=list(comp),
                has_generator=any(has_gen.get(b, False) for b in comp),
                has_demand=any(has_dem.get(b, False) for b in comp),
            )
        )
    return out


# ---------------------------------------------------------------------------
# Numerical / DC classification (C1-C3)
# ---------------------------------------------------------------------------


def _collect_negative_impedance(
    line_array: list[dict[str, Any]],
) -> list[NegativeImpedance]:
    out: list[NegativeImpedance] = []
    for raw in line_array:
        x = float(raw.get("reactance", 0.0) or 0.0)
        r = float(raw.get("resistance", 0.0) or 0.0)
        if x < 0.0 or r < 0.0:
            out.append(
                NegativeImpedance(
                    uid=raw.get("uid"),
                    name=str(raw.get("name", raw.get("uid", "?"))),
                    reactance=x,
                    resistance=r,
                )
            )
    return out


def _collect_voltage_mixing(
    bus_array: list[dict[str, Any]],
    line_array: list[dict[str, Any]],
) -> list[VoltageMix]:
    """Flag lines whose endpoints sit at distinct (heuristic) voltage levels.

    A "transformer" hint is taken from the line's own ``voltage`` field
    when present and != 0 -- in plexos2gtopt exports that field carries
    the tap ratio of a transformer winding and indicates that the
    mismatch is intentional.
    """
    name_voltage: dict[str, int] = {}
    for bus in bus_array:
        name = bus.get("name")
        if not name:
            continue
        v = _bus_voltage_from_name(str(name))
        if v is not None:
            name_voltage[str(name)] = v

    out: list[VoltageMix] = []
    for raw in line_array:
        a = raw.get("bus_a")
        b = raw.get("bus_b")
        if not isinstance(a, str) or not isinstance(b, str):
            continue
        va = name_voltage.get(a)
        vb = name_voltage.get(b)
        if va is None or vb is None or va == vb:
            continue
        # Skip lines that already declare a non-zero ``voltage`` (taken as
        # the transformer-tap hint).  Lines with voltage == 0 or absent
        # are considered untransformed and are flagged.
        tap = raw.get("voltage")
        if isinstance(tap, (int, float)) and tap not in (0, 0.0):
            continue
        out.append(
            VoltageMix(
                uid=raw.get("uid"),
                name=str(raw.get("name", raw.get("uid", "?"))),
                bus_a=a,
                bus_b=b,
                bus_a_voltage=va,
                bus_b_voltage=vb,
            )
        )
    return out


# ---------------------------------------------------------------------------
# Cycles + bridges
# ---------------------------------------------------------------------------


def _collect_cycles(
    network: NetworkGraph,
    max_cycle_size: int,
) -> list[CycleInfo]:
    if network.graph.number_of_edges() == 0:
        return []
    out: list[CycleInfo] = []
    # cycle_basis depends on the root node it picks first; iterate nodes in
    # sorted order so the same JSON input always produces the same cycle
    # basis (otherwise PYTHONHASHSEED makes borderline-score cycles flicker
    # between dangerous/benign across runs).
    root = min(network.graph.nodes) if network.graph.number_of_nodes() else None
    for cyc in nx.cycle_basis(network.graph, root=root):
        if len(cyc) > max_cycle_size:
            continue
        edges = _cycle_to_edges(cyc)
        segments: list[LineRef] = []
        segments_all: list[LineRef] = []
        x_vals: list[float] = []
        t_vals: list[float] = []
        for edge in edges:
            if not network.graph.has_edge(*edge):
                continue
            data = network.graph[edge[0]][edge[1]]
            x_vals.append(abs(float(data["reactance"])))
            t_vals.append(float(data["tmax"]))
            refs = network.edge_to_lines.get(edge, [])
            if refs:
                # min-X representative for display, ALL parallels for SOS1.
                segments.append(min(refs, key=lambda r: abs(r.reactance)))
                segments_all.extend(refs)
        if not x_vals:
            continue
        asym_x = _ratio(x_vals)
        asym_tmax = _ratio(t_vals) if min(t_vals) > 0 else float("inf")
        min_tmax = min(t_vals)
        score = cycle_danger_score(asym_x, asym_tmax, min_tmax, len(cyc))
        # B7 -- predict the loop-circulation MW at which the min-X edge
        # would saturate.  ``pct_through_min_X = 1 - min(X)/sum(X)`` is the
        # KVL-forced share of any loop circulation that flows through the
        # smallest-reactance segment.  Threshold = tmax_min_X / share.
        sum_x = sum(x_vals)
        min_x = min(x_vals)
        sat_threshold_mw: float | None = None
        if 0.0 < min_x < sum_x:
            share = 1.0 - (min_x / sum_x)
            if share > 0.0:
                # Find tmax of the min-X edge (zip back to t_vals).
                tmax_min_x = next(
                    (t for x, t in zip(x_vals, t_vals) if x == min_x),
                    min(t_vals),
                )
                if tmax_min_x > 0:
                    sat_threshold_mw = tmax_min_x / share
        out.append(
            CycleInfo(
                buses=list(cyc),
                edges=edges,
                segments=segments,
                segments_all=segments_all,
                asym_x=asym_x,
                asym_tmax=asym_tmax,
                min_tmax=min_tmax,
                n_buses=len(cyc),
                score=score,
                kvl_saturation_threshold_mw=sat_threshold_mw,
            )
        )
    out.sort(key=lambda c: c.score, reverse=True)
    return out


def _collect_bridges(network: NetworkGraph) -> list[LineRef]:
    out: list[LineRef] = []
    if network.graph.number_of_edges() == 0:
        return out
    seen: set[Any] = set()
    for a, b in nx.bridges(network.graph):
        key = _edge_key(a, b)
        for ref in network.edge_to_lines.get(key, []):
            if ref.uid in seen:
                continue
            seen.add(ref.uid)
            out.append(ref)
    return out


def _collect_stubs(network: NetworkGraph) -> list[LineRef]:
    """Leaf branches -- lines whose endpoint has degree 1 in the simple graph."""
    out: list[LineRef] = []
    seen: set[Any] = set()
    for node in network.graph.nodes:
        if network.graph.degree(node) != 1:
            continue
        for nbr in network.graph.neighbors(node):
            key = _edge_key(node, nbr)
            for ref in network.edge_to_lines.get(key, []):
                if ref.uid in seen:
                    continue
                seen.add(ref.uid)
                out.append(ref)
    return out


# ---------------------------------------------------------------------------
# Tier-2: D1 Reserve-zone coherence
# ---------------------------------------------------------------------------


def _collect_reserve_zone_coherence(
    planning: dict[str, Any],
) -> list[ReserveZoneIssue]:
    """D1 -- every reserve_zone needs at least one usable provider."""
    sys = planning.get("system", {}) or {}
    zones = sys.get("reserve_zone_array", []) or []
    provisions = sys.get("reserve_provision_array", []) or []

    # Pre-aggregate providers per zone, with their max provision capacity.
    by_zone: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for prov in provisions:
        z = prov.get("reserve_zones")
        if isinstance(z, str):
            by_zone[z].append(prov)
        elif isinstance(z, list):
            for zone in z:
                if isinstance(zone, str):
                    by_zone[zone].append(prov)

    out: list[ReserveZoneIssue] = []
    for zone in zones:
        name = zone.get("name")
        if not isinstance(name, str):
            continue
        provs = by_zone.get(name, [])
        if not provs:
            out.append(ReserveZoneIssue(zone=name, issue="no providers", n_providers=0))
            continue
        # Detect "impossible provision": all providers have urmax+drmax == 0.
        has_capacity = False
        for prov in provs:
            for cap_key in ("urmax", "drmax", "up_max", "dn_max"):
                cap = _to_max_float(prov.get(cap_key))
                if cap is not None and cap > 0.0:
                    has_capacity = True
                    break
            if has_capacity:
                break
        if not has_capacity:
            out.append(
                ReserveZoneIssue(
                    zone=name,
                    issue="all providers have zero capacity",
                    n_providers=len(provs),
                )
            )
    return out


# ---------------------------------------------------------------------------
# Tier-2: D2 Per-island generator reachability
# ---------------------------------------------------------------------------


def _pmax_per_bus(planning: dict[str, Any]) -> dict[str, float]:
    """Sum nominal pmax per bus across generators (and batteries discharge)."""
    sys = planning.get("system", {}) or {}
    name_by_uid: dict[Any, str] = {}
    for bus in sys.get("bus_array", []) or []:
        uid = bus.get("uid")
        name = bus.get("name")
        if uid is not None and name:
            name_by_uid[uid] = str(name)

    def resolve(ref: Any) -> str | None:
        if isinstance(ref, str):
            return ref
        if ref is None:
            return None
        return name_by_uid.get(ref)

    out: dict[str, float] = defaultdict(float)
    for gen in sys.get("generator_array", []) or []:
        bid = resolve(gen.get("bus"))
        if not bid:
            continue
        pmax = _to_max_float(gen.get("pmax")) or 0.0
        if pmax > 0:
            out[bid] += pmax
    for bat in sys.get("battery_array", []) or []:
        bid = resolve(bat.get("bus"))
        if not bid:
            continue
        pmax = _to_max_float(bat.get("pmax_discharge")) or 0.0
        if pmax > 0:
            out[bid] += pmax
    return dict(out)


def _peak_load_per_bus(planning: dict[str, Any]) -> dict[str, float]:
    sys = planning.get("system", {}) or {}
    name_by_uid: dict[Any, str] = {}
    for bus in sys.get("bus_array", []) or []:
        uid = bus.get("uid")
        name = bus.get("name")
        if uid is not None and name:
            name_by_uid[uid] = str(name)

    out: dict[str, float] = defaultdict(float)
    for dem in sys.get("demand_array", []) or []:
        ref = dem.get("bus")
        bid: str | None = None
        if isinstance(ref, str):
            bid = ref
        elif ref is not None:
            bid = name_by_uid.get(ref)
        if not bid:
            continue
        peak = _to_max_float(dem.get("lmax")) or 0.0
        if peak > 0:
            out[bid] = max(out[bid], peak) if bid in out else peak
            # NB: ``out[bid] = max(...)`` short-circuits ``defaultdict`` 0.0
            # the first time -- subsequent loads on the same bus stack via max
            # because lmax is already per-bus peak.
    return dict(out)


def _collect_island_feasibility(
    islands: list[IslandInfo],
    pmax_per_bus: dict[str, float],
    peak_per_bus: dict[str, float],
    has_dem_map: dict[str, bool],
) -> list[IslandFeasibility]:
    out: list[IslandFeasibility] = []
    for idx, isl in enumerate(islands):
        n_dem = sum(1 for b in isl.buses if has_dem_map.get(b, False))
        n_gen = sum(1 for b in isl.buses if pmax_per_bus.get(b, 0.0) > 0.0)
        sum_pmax = sum(pmax_per_bus.get(b, 0.0) for b in isl.buses)
        sum_peak = sum(peak_per_bus.get(b, 0.0) for b in isl.buses)
        issue: str | None = None
        if n_dem > 0 and n_gen == 0:
            issue = "demand without generation"
        elif n_gen > 0 and n_dem == 0:
            issue = "generation without demand (curtailment-only)"
        elif n_dem > 0 and n_gen > 0 and sum_peak > 0:
            # gen < load + 10% reserve -> structurally short
            if sum_pmax < sum_peak * 1.10:
                issue = "gen capacity below peak load + 10% reserve"
        if issue is None:
            continue
        out.append(
            IslandFeasibility(
                island_idx=idx,
                n_demand_buses=n_dem,
                n_gen_buses=n_gen,
                sum_pmax=sum_pmax,
                sum_peak_load=sum_peak,
                issue=issue,
            )
        )
    return out


# ---------------------------------------------------------------------------
# Tier-2: D3 Battery eini consistency
# ---------------------------------------------------------------------------


_BATTERY_REF_RE = re.compile(r'battery\(\s*"([^"]+)"\s*\)')


def _batteries_in_uc_expressions(planning: dict[str, Any]) -> dict[str, list[str]]:
    """Map battery-name -> list of UC names that reference it.

    Scans both the in-memory ``user_constraint_array`` (each UC's
    ``expression`` field) and any side-car .pampl files listed under
    ``system.user_constraint_files`` if they can be resolved relative
    to the planning bundle's directory.
    """
    sys = planning.get("system", {}) or {}
    out: dict[str, list[str]] = defaultdict(list)

    for uc in sys.get("user_constraint_array", []) or []:
        name = uc.get("name") or ""
        expr = uc.get("expression") or ""
        if not isinstance(expr, str):
            continue
        for bat in _BATTERY_REF_RE.findall(expr):
            out[bat].append(str(name))

    # Scan side-car pampl files if a containing dir was provided.
    pampl_dir = planning.get("_pampl_dir")
    files = sys.get("user_constraint_files") or []
    if pampl_dir and isinstance(files, list):
        base = Path(pampl_dir)
        for fname in files:
            if not isinstance(fname, str):
                continue
            path = base / fname
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            # Split into "constraint <name>:" blocks so we can attribute the
            # battery() refs to the surrounding UC name.
            current_name = ""
            for line in text.splitlines():
                stripped = line.strip()
                if stripped.startswith("constraint "):
                    # "constraint NAME:" -> grab NAME, strip trailing ':'.
                    rest = stripped[len("constraint ") :].rstrip(":").strip()
                    current_name = rest
                    continue
                if "battery(" not in line:
                    continue
                for bat in _BATTERY_REF_RE.findall(line):
                    if current_name and current_name not in out[bat]:
                        out[bat].append(current_name)
                    elif not current_name:
                        out[bat].append(f"<{path.name}>")
    return dict(out)


def _first_numeric(value: Any) -> float | None:
    """Return the first scalar inside a (possibly nested) numeric value."""
    if value is None:
        return None
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, list):
        for item in value:
            sub = _first_numeric(item)
            if sub is not None:
                return sub
    return None


def _collect_battery_eini_issues(
    planning: dict[str, Any],
) -> list[BatteryEiniIssue]:
    """D3 -- batteries with inconsistent initial state-of-charge."""
    sys = planning.get("system", {}) or {}
    batteries = sys.get("battery_array", []) or []
    bat_in_ucs = _batteries_in_uc_expressions(planning)

    out: list[BatteryEiniIssue] = []
    for bat in batteries:
        name = bat.get("name")
        if not isinstance(name, str):
            continue
        eini = bat.get("eini")
        eini_f = _first_numeric(eini) if eini is not None else None
        emax_f = _to_max_float(bat.get("emax"))
        # NB: emin/emax may be time-varying -- _to_max_float returns the max;
        # for the "<=" comparison we also need the first-block value.
        emin_first = _first_numeric(bat.get("emin"))
        emax_first = _first_numeric(bat.get("emax"))
        in_ucs = bat_in_ucs.get(name, [])

        if eini_f is None:
            # no eini set -- skip (per-block analog or default)
            if (
                emin_first is not None
                and emax_first is not None
                and (emin_first > emax_first)
            ):
                out.append(
                    BatteryEiniIssue(
                        name=name,
                        issue="emin > emax (data error)",
                        eini=None,
                        emin_first=emin_first,
                        emax_first=emax_first,
                        in_ucs=in_ucs,
                    )
                )
            continue
        # eini > emax (using the max across blocks for emax -- the tightest
        # upper bound is the lowest, but if the largest block-cap is still
        # below eini we have a hard data error).
        if emax_f is not None and eini_f > emax_f + 1e-9:
            out.append(
                BatteryEiniIssue(
                    name=name,
                    issue="eini > emax (data error)",
                    eini=eini_f,
                    emin_first=emin_first,
                    emax_first=emax_first,
                    in_ucs=in_ucs,
                )
            )
            continue
        # eini below emin -- only flag with first-block emin to avoid
        # noisy false positives from per-block scheduled minima.
        if emin_first is not None and eini_f < emin_first - 1e-9:
            out.append(
                BatteryEiniIssue(
                    name=name,
                    issue="eini < emin (initial SoC below floor)",
                    eini=eini_f,
                    emin_first=emin_first,
                    emax_first=emax_first,
                    in_ucs=in_ucs,
                )
            )
            continue
        # eini == 0 AND the battery appears in a UC -- BAT_MANZANO_FV /
        # BAT_LA_CABANA_EO case (needs periodic-cycle treatment).
        if eini_f == 0.0 and in_ucs:
            out.append(
                BatteryEiniIssue(
                    name=name,
                    issue="eini=0 with UC reference (needs periodic-cycle)",
                    eini=eini_f,
                    emin_first=emin_first,
                    emax_first=emax_first,
                    in_ucs=in_ucs,
                )
            )
            continue
        # daily_cycle=True without a usable eini.
        if (
            bat.get("daily_cycle") is True
            and emax_first is not None
            and emax_first > 0
            and (eini_f == 0.0 or eini_f > emax_first)
        ):
            out.append(
                BatteryEiniIssue(
                    name=name,
                    issue="daily_cycle=true with inconsistent eini",
                    eini=eini_f,
                    emin_first=emin_first,
                    emax_first=emax_first,
                    in_ucs=in_ucs,
                )
            )
    return out


# ---------------------------------------------------------------------------
# Tier-2: E1 Time-varying capacity sanity
# ---------------------------------------------------------------------------


def _flatten_numeric(value: Any) -> list[float]:
    """Return every numeric leaf inside a (possibly nested) list."""
    out: list[float] = []
    stack: list[Any] = [value]
    while stack:
        item = stack.pop()
        if isinstance(item, list):
            stack.extend(item)
        elif isinstance(item, (int, float)) and not isinstance(item, bool):
            out.append(float(item))
    return out


def _collect_time_varying_capacity_issues(
    line_array: list[dict[str, Any]],
    *,
    zero_pct_threshold: float = 0.5,
) -> list[TimeVaryingCapacityIssue]:
    """E1 -- lines whose ``tmax_ab`` is mostly zero across blocks.

    Only lines whose ``tmax_ab`` is a list (per-block schedule) are
    considered.  Scalar tmax lines are out-of-scope for this check.
    """
    out: list[TimeVaryingCapacityIssue] = []
    for raw in line_array:
        tmax = raw.get("tmax_ab")
        if not isinstance(tmax, list):
            continue
        flat = _flatten_numeric(tmax)
        if not flat:
            continue
        max_t = max(flat)
        if max_t <= 0.0:
            # whole-line zero capacity is a different (and louder) bug,
            # but still report it via E1.
            out.append(
                TimeVaryingCapacityIssue(
                    uid=raw.get("uid"),
                    name=str(raw.get("name", raw.get("uid", "?"))),
                    bus_a=str(raw.get("bus_a", "")),
                    bus_b=str(raw.get("bus_b", "")),
                    blocks_at_zero=len(flat),
                    blocks_near_zero=len(flat),
                    blocks_total=len(flat),
                    max_tmax=0.0,
                )
            )
            continue
        zero = sum(1 for v in flat if v == 0.0)
        near = sum(1 for v in flat if 0.0 < v < 0.10 * max_t)
        if zero / len(flat) > zero_pct_threshold:
            out.append(
                TimeVaryingCapacityIssue(
                    uid=raw.get("uid"),
                    name=str(raw.get("name", raw.get("uid", "?"))),
                    bus_a=str(raw.get("bus_a", "")),
                    bus_b=str(raw.get("bus_b", "")),
                    blocks_at_zero=zero,
                    blocks_near_zero=near,
                    blocks_total=len(flat),
                    max_tmax=max_t,
                )
            )
    return out


# ---------------------------------------------------------------------------
# Tier-2: E2 Reservoir cascade well-formedness
# ---------------------------------------------------------------------------


def _collect_reservoir_cascade_issues(
    planning: dict[str, Any],
) -> list[ReservoirCascadeIssue]:
    """E2 -- structural checks on the water-flow graph."""
    sys = planning.get("system", {}) or {}
    reservoirs = sys.get("reservoir_array", []) or []
    junctions = sys.get("junction_array", []) or []
    waterways = sys.get("waterway_array", []) or []

    junction_names: set[str] = set()
    for j in junctions:
        n = j.get("name")
        if isinstance(n, str):
            junction_names.add(n)

    out: list[ReservoirCascadeIssue] = []

    # 1) Every waterway must reference real junctions.
    water_graph: nx.DiGraph = nx.DiGraph()
    for ww in waterways:
        a = ww.get("junction_a")
        b = ww.get("junction_b")
        name = str(ww.get("name", ww.get("uid", "?")))
        if not isinstance(a, str) or not isinstance(b, str):
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="waterway missing junction_a/junction_b",
                )
            )
            continue
        if a not in junction_names:
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="waterway references unknown junction",
                    detail=f"junction_a='{a}'",
                )
            )
        if b not in junction_names:
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="waterway references unknown junction",
                    detail=f"junction_b='{b}'",
                )
            )
        if isinstance(a, str) and isinstance(b, str):
            water_graph.add_edge(a, b, name=name)

    # 2) Cycles in the water-flow digraph (water should NOT recirculate).
    try:
        for cyc in nx.simple_cycles(water_graph):
            label = "->".join(cyc + [cyc[0]])
            out.append(
                ReservoirCascadeIssue(
                    name="<cascade>",
                    issue="water-flow cycle (water recirculates)",
                    detail=label,
                )
            )
    except nx.NetworkXError:  # pragma: no cover - defensive
        pass

    # 3) Reservoirs with no inflow source AND no initial volume.
    # 4) Reservoirs whose outflow path is empty (water stranded).
    for res in reservoirs:
        name = res.get("name")
        if not isinstance(name, str):
            continue
        junction = res.get("junction")
        if not isinstance(junction, str):
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="reservoir missing junction reference",
                )
            )
            continue
        eini = _first_numeric(res.get("eini"))
        in_degree = water_graph.in_degree(junction) if junction in water_graph else 0
        out_degree = water_graph.out_degree(junction) if junction in water_graph else 0
        if in_degree == 0 and (eini is None or eini == 0.0):
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="no inflow source and no initial volume",
                    detail=f"junction='{junction}'",
                )
            )
        if out_degree == 0:
            out.append(
                ReservoirCascadeIssue(
                    name=name,
                    issue="no outflow path (water stranded)",
                    detail=f"junction='{junction}'",
                )
            )
    return out


# ---------------------------------------------------------------------------
# Tier-2: B6 Parallel-circuit identicality
# ---------------------------------------------------------------------------


def _collect_parallel_circuit_identical(
    network: NetworkGraph,
    dangerous_cycles: list[CycleInfo],
) -> list[ParallelCircuitIdentical]:
    """B6 -- bus pairs with >1 identical parallel circuits (X + tmax match).

    Identicality test uses a strict tolerance because LP degeneracy bites
    only when the equality is exact.  ``on_dangerous_cycle`` upgrades a
    purely-informational entry to a real warning when at least one of the
    parallel UIDs sits on a dangerous-cycle edge.
    """
    dangerous_uids: set[Any] = set()
    for cyc in dangerous_cycles:
        for seg in cyc.segments_all:
            dangerous_uids.add(seg.uid)

    out: list[ParallelCircuitIdentical] = []
    for (a, b), refs in network.edge_to_lines.items():
        if len(refs) < 2:
            continue
        rep_x = refs[0].reactance
        rep_t = refs[0].tmax
        if not all(r.reactance == rep_x and r.tmax == rep_t for r in refs):
            continue
        uids = [r.uid for r in refs]
        out.append(
            ParallelCircuitIdentical(
                bus_a=a,
                bus_b=b,
                n_circuits=len(refs),
                reactance=rep_x,
                tmax=rep_t,
                uids=uids,
                on_dangerous_cycle=any(u in dangerous_uids for u in uids),
            )
        )
    out.sort(key=lambda p: (-p.n_circuits, p.bus_a, p.bus_b))
    return out


# ---------------------------------------------------------------------------
# Orchestrator
# ---------------------------------------------------------------------------


def analyse_planning(
    planning: dict[str, Any],
    *,
    bundle: str = "",
    danger_threshold: int = DEFAULT_DANGER_THRESHOLD,
    max_cycle_size: int = DEFAULT_MAX_CYCLE_SIZE,
) -> AnalysisReport:
    """Run every Tier-1 + Tier-2 topology check and return a populated report."""
    sys = planning.get("system", {}) or {}
    bus_array = sys.get("bus_array", []) or []
    line_array = sys.get("line_array", []) or []

    network = build_network_graph(planning)
    has_gen = _bus_has_generator_map(planning)
    has_dem = _bus_has_demand_map(planning)

    islands = _collect_islands(network, has_gen, has_dem)
    isolated = sorted(n for n in network.graph.nodes if network.graph.degree(n) == 0)
    stubs = _collect_stubs(network)
    cycles = _collect_cycles(network, max_cycle_size)
    bridges = _collect_bridges(network)
    negs = _collect_negative_impedance(line_array)
    mixing = _collect_voltage_mixing(bus_array, line_array)

    # Tier-2 collectors.
    reserve_issues = _collect_reserve_zone_coherence(planning)
    pmax_per_bus = _pmax_per_bus(planning)
    peak_per_bus = _peak_load_per_bus(planning)
    island_issues = _collect_island_feasibility(
        islands, pmax_per_bus, peak_per_bus, has_dem
    )
    battery_issues = _collect_battery_eini_issues(planning)
    tv_capacity_issues = _collect_time_varying_capacity_issues(line_array)
    cascade_issues = _collect_reservoir_cascade_issues(planning)

    # Need the dangerous-cycle set first so B6 can flag overlap.
    dangerous = [c for c in cycles if c.is_dangerous(danger_threshold)]
    parallel_identical = _collect_parallel_circuit_identical(network, dangerous)

    return AnalysisReport(
        bundle=bundle,
        n_buses=len(bus_array),
        n_lines=len(line_array),
        n_cycles=len(cycles),
        islands=islands,
        isolated_buses=isolated,
        stubs=stubs,
        cycles=cycles,
        bridges=bridges,
        dc_lines=network.dc_lines,
        negative_impedance=negs,
        voltage_mixing=mixing,
        skipped_unknown_bus=network.skipped_unknown_bus,
        self_loops=network.self_loops,
        reserve_zone_coherence=reserve_issues,
        island_feasibility=island_issues,
        battery_eini_issues=battery_issues,
        time_varying_capacity_issues=tv_capacity_issues,
        reservoir_cascade_issues=cascade_issues,
        parallel_circuit_identical=parallel_identical,
        danger_threshold=danger_threshold,
    )
