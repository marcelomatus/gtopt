"""Tests for the ``--vrebemb-as-sink`` mode.

This flag is **opt-in** (default: False).  When enabled, JunctionWriter
redirects the ``_ver`` waterway of any central listed in
``plpvrebemb.dat`` to a synthetic ``<name>_ocean`` drain and drops both
``fmax`` and ``fcost`` on the arc.  Non-vrebemb centrals are untouched.

The flag restores PLP's ``qrb`` (sink-bound, costed) rebalse semantics:
PLP subtracts ``qrb`` from end-of-stage storage WITHOUT adding it back
at any downstream junction, so the parallel-pipe ``_ver → ser_ver``
model gtopt used previously could route uncapped water through the
spillway arc to feed downstream demand at ``rebalse_cost`` per m³ —
generating "fictitious water".

When both ``--drop-spillway-waterway`` and ``--vrebemb-as-sink`` are
set, the spillway-suppress mode wins (no ``_ver`` arc is emitted).
"""

from __future__ import annotations

import math
from typing import Any, Dict, List, Optional

from ..junction_writer import JunctionWriter
from ..vrebemb_parser import VrebembParser
from .test_junction_writer import MockCentralParser


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


class MockVrebembParser(VrebembParser):
    """Mock VrebembParser for testing — exposes ``get_cost`` from a dict."""

    def __init__(self, costs: Dict[str, float]):
        """Initialise with a mapping of central name -> per-stage rebalse cost."""
        super().__init__("dummy.dat")
        self._costs = costs

    def get_cost(self, name: str) -> Optional[float]:
        """Return cost or None if the central is not in vrebemb."""
        return self._costs.get(name)


def _embalse_in_network(
    name: str,
    number: int,
    *,
    ser_hid: int,
    ser_ver: int,
    vert_max: float = 0.0,
) -> Dict[str, Any]:
    """Build an embalse central with downstream ser_hid / ser_ver."""
    return {
        "number": number,
        "name": name,
        "type": "embalse",
        "bus": 0,
        "pmin": 0,
        "pmax": 100.0,
        "vert_min": 0.0,
        "vert_max": vert_max,
        "efficiency": 0.85,
        "ser_hid": ser_hid,
        "ser_ver": ser_ver,
        "afluent": 0.0,
        "vol_ini": 50.0,
        "vol_fin": 50.0,
        "emin": 0.0,
        "emax": 500.0,
    }


def _serie(
    name: str,
    number: int,
    *,
    ser_hid: int = 0,
    ser_ver: int = 0,
    bus: int = 1,
    vert_max: float = 0.0,
) -> Dict[str, Any]:
    """Build a minimal serie / pasada central dict."""
    return {
        "number": number,
        "name": name,
        "type": "serie",
        "bus": bus,
        "pmin": 0,
        "pmax": 10.0,
        "vert_min": 0.0,
        "vert_max": vert_max,
        "efficiency": 1.0,
        "ser_hid": ser_hid,
        "ser_ver": ser_ver,
        "afluent": 0.0,
    }


def _run(
    centrals: List[Dict[str, Any]],
    *,
    vrebemb_costs: Optional[Dict[str, float]] = None,
    options: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Run JunctionWriter and return the system dict.

    The legacy spillway topology (``_ver`` waterway from
    ``--no-drop-spillway-waterway``) is the baseline these tests
    compare against, so we always pin
    ``drop_spillway_waterway = False`` unless the test overrides it.
    """
    base_options: Dict[str, Any] = {"drop_spillway_waterway": False}
    if options is not None:
        base_options.update(options)
    vrebemb_parser: Optional[MockVrebembParser] = None
    if vrebemb_costs is not None:
        vrebemb_parser = MockVrebembParser(vrebemb_costs)
    writer = JunctionWriter(
        central_parser=MockCentralParser(centrals),
        vrebemb_parser=vrebemb_parser,
        options=base_options,
    )
    result = writer.to_json_array()
    assert result, "expected non-empty system output"
    return result[0]


def _ver_waterway(system: Dict[str, Any], name_prefix: str) -> Dict[str, Any]:
    """Return the unique ``<name_prefix>_ver_*`` waterway."""
    matches = [
        w
        for w in system["waterway_array"]
        if w["name"].startswith(name_prefix + "_ver_")
    ]
    assert len(matches) == 1, (
        f"expected exactly one _ver waterway for {name_prefix!r}, got {matches}"
    )
    return matches[0]


# ---------------------------------------------------------------------------
# Core behaviour: vrebemb central with ser_ver > 0 redirected to ocean
# ---------------------------------------------------------------------------


def test_vrebemb_as_sink_redirects_ver_to_ocean():
    """vrebemb embalse with ser_ver > 0 routes _ver to synthetic ocean drain.

    Flag ON: ``_ver`` is redirected to ``<name>_ocean`` and both
    ``fmax`` and ``fcost`` are dropped from the waterway.  The
    ocean junction is added to the system as a drain.
    """
    cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2)
    sink = _serie("Sink", 2)
    system = _run(
        [cent, sink],
        vrebemb_costs={"Dam": 5000.0},
        options={"vrebemb_as_sink": True},
    )

    ver = _ver_waterway(system, "Dam")
    assert ver["junction_b"].endswith("_ocean")
    assert "fcost" not in ver, f"unexpected fcost: {ver.get('fcost')!r}"
    # fmax is emitted explicitly as ``1e30`` (gtopt's effective-infinity
    # sentinel — clamped to solver infinity at flatten time, JSON-safe
    # unlike Infinity).  Matches the convention at lines ~1050, ~1073,
    # ~1446 of junction_writer.py for other unbounded waterway-flow paths.
    assert ver.get("fmax") == math.inf, f"expected math.inf fmax: {ver.get('fmax')!r}"

    junctions = {j["name"]: j for j in system["junction_array"]}
    drain_name = ver["junction_b"]
    assert drain_name in junctions
    assert junctions[drain_name].get("drain") is True


# ---------------------------------------------------------------------------
# Off (default) — current behaviour preserved for vrebemb centrals
# ---------------------------------------------------------------------------


def test_vrebemb_as_sink_off_preserves_today():
    """Flag OFF: vrebemb _ver still goes to ser_ver with rebalse fcost."""
    cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2)
    sink = _serie("Sink", 2)
    system = _run(
        [cent, sink],
        vrebemb_costs={"Dam": 5000.0},
        options={"vrebemb_as_sink": False},
    )

    ver = _ver_waterway(system, "Dam")
    assert ver["junction_b"] == "Sink"
    assert not ver["junction_b"].endswith("_ocean")
    assert ver.get("fcost") == 5000.0


# ---------------------------------------------------------------------------
# Non-vrebemb centrals are untouched even when the flag is on
# ---------------------------------------------------------------------------


def test_vrebemb_as_sink_non_vrebemb_unchanged():
    """Non-vrebemb serie central with flag ON: _ver still targets ser_ver."""
    cent = _serie("CentA", 1, ser_hid=2, ser_ver=2, vert_max=50.0)
    sink = _serie("Sink", 2)
    # Empty vrebemb mapping ⇒ get_cost("CentA") returns None.
    system = _run(
        [cent, sink],
        vrebemb_costs={},
        options={"vrebemb_as_sink": True},
    )

    ver = _ver_waterway(system, "CentA")
    assert ver["junction_b"] == "Sink"
    assert not ver["junction_b"].endswith("_ocean")


# ---------------------------------------------------------------------------
# Existing-ocean path: vrebemb central with ser_ver = 0 already routes to
# a synthetic drain.  Flag ON must drop fcost (and fmax) on that arc.
# ---------------------------------------------------------------------------


def test_vrebemb_as_sink_existing_ocean_drops_fcost():
    """Vrebemb embalse with ser_ver = 0: spillway encoded as junction drain.

    LMAULE / RAPEL / CANUTILLAR / COLBUN have ser_ver = 0; the
    architectural fix collapses the legacy ``_ver → <central>_ocean``
    arc + ocean Junction into ``Junction{drain: true}`` on the source.
    Under ``--vrebemb-as-sink`` the rebalse cost is also dropped, so
    ``drain_cost`` is omitted (no per-flow penalty) and
    ``drain_capacity`` is omitted (PLP qrb-to-sink semantics:
    uncapped, costless).
    """
    cent = _embalse_in_network("Term", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"Term": 5000.0},
        options={"vrebemb_as_sink": True},
    )

    # No ``_ver`` arc — it collapsed into Junction.drain on the source.
    ver = [w for w in system["waterway_array"] if "Term_ver" in w.get("name", "")]
    assert ver == []
    src = next(j for j in system["junction_array"] if j["name"] == "Term")
    assert src["drain"] is True
    # vrebemb-as-sink + qrb-to-sink: no cap, no cost on the drain column.
    assert "drain_capacity" not in src, src.get("drain_capacity")
    assert "drain_cost" not in src, src.get("drain_cost")


# ---------------------------------------------------------------------------
# Precedence: --drop-spillway-waterway wins over --vrebemb-as-sink
# ---------------------------------------------------------------------------


def test_vrebemb_as_sink_redirects_asymmetric_central_uniformly():
    """Asymmetric and symmetric vrebemb centrals get the same treatment.

    Both ELTORO (asymmetric — ser_ver = ABANICO) and the symmetric
    cases get their ``_ver`` redirected to ``<name>_ocean`` and their
    ``fcost`` dropped.  Downstream pmin obligations are now per-block
    soft FlowRights (with the trajectory carried over from
    plpmance.dat) and downstream Reservoir efin/soft_emin are soft
    slacks priced by the auto water-shortfall resolver, so the
    asymmetric topology no longer needs a tier-2 skip.
    """
    # Asymmetric: ser_hid=2 (CentA), ser_ver=3 (CentB).
    cent = _embalse_in_network("Asym", 1, ser_hid=2, ser_ver=3)
    cent_a = _serie("CentA", 2)
    cent_b = _serie("CentB", 3)
    system = _run(
        [cent, cent_a, cent_b],
        vrebemb_costs={"Asym": 5000.0},
        options={"vrebemb_as_sink": True},
    )

    ver = _ver_waterway(system, "Asym")
    # Topology redirected to ocean — same as for symmetric vrebemb centrals.
    assert ver["junction_b"].endswith("_ocean")
    assert ver["junction_b"] != "CentB"
    # fcost dropped (the redundant vrebemb deterrent has no anti-arbitrage
    # value once water leaves the system).
    assert "fcost" not in ver, f"unexpected fcost: {ver.get('fcost')!r}"
    assert ver.get("fmax") == math.inf


def test_vrebemb_as_sink_with_drop_spillway_takes_precedence():
    """Both flags ON: no _ver waterway emitted, source junction is a drain.

    ``--drop-spillway-waterway`` suppresses every ``_ver`` arc, so
    ``--vrebemb-as-sink`` is a no-op when both are set.  The central's
    own junction takes over as drain (matches today's
    drop-spillway behaviour).
    """
    cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2)
    sink = _serie("Sink", 2)
    system = _run(
        [cent, sink],
        vrebemb_costs={"Dam": 5000.0},
        options={
            "drop_spillway_waterway": True,
            "vrebemb_as_sink": True,
        },
    )

    # No _ver arc at all.
    ver_arcs = [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    assert ver_arcs == [], f"unexpected _ver arcs: {ver_arcs}"

    # Source junction is now a drain (the standard --drop-spillway-waterway
    # behaviour absorbs surplus water there).
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["Dam"].get("drain") is True


# ---------------------------------------------------------------------------
# CLI plumbing
# ---------------------------------------------------------------------------


def test_cli_flag_default_is_false():
    """CLI default for ``--vrebemb-as-sink`` is False (opt-in)."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case"])
    assert args.vrebemb_as_sink is False


def test_cli_flag_can_be_enabled():
    """``--vrebemb-as-sink`` enables the redirect explicitly."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case", "--vrebemb-as-sink"])
    assert args.vrebemb_as_sink is True


def test_cli_flag_no_form_keeps_default():
    """``--no-vrebemb-as-sink`` is accepted and pins the default off."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case", "--no-vrebemb-as-sink"])
    assert args.vrebemb_as_sink is False


def test_cli_flag_passes_through_build_options():
    """``build_options`` propagates the parsed value to the options dict."""
    from plp2gtopt.main import build_options, make_parser  # noqa: PLC0415

    args_default = make_parser().parse_args(["plp_case"])
    args_on = make_parser().parse_args(["plp_case", "--vrebemb-as-sink"])
    assert build_options(args_default)["vrebemb_as_sink"] is False
    assert build_options(args_on)["vrebemb_as_sink"] is True
