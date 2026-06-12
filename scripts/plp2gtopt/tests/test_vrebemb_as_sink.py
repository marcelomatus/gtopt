"""Tests for vrebemb reservoir spill handling (and the now-inert
``--vrebemb-as-sink`` flag).

Every reservoir except the never-drain sentinels
(``_DRAIN_KILLED_RESERVOIRS``, e.g. ELTORO) has its spill carried *solely*
by the reservoir's own storage-drain column (``Reservoir.spillway_cost = 0``
— free — with finite C++-default capacity), regardless of plpvrebemb.dat
membership.  These reservoirs emit **no** draining ``_ver`` waterway —
neither to a downstream ``ser_ver`` central nor to a synthetic
``<name>_ocean`` drain.  This mirrors plexos2gtopt, which collapses
``Vert_*`` spill waterways onto the reservoir/junction rather than keeping
draining arcs.

Because drained reservoirs no longer emit any ``_ver`` arc, the historic
``--vrebemb-as-sink`` flag (which redirected that arc to an ocean drain)
is now a no-op for its target population.  Its CLI plumbing is retained
for backward compatibility; the behavioural tests below assert the new
reservoir-spillway behaviour holds regardless of the flag.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

import pytest

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


def _reservoir(system: Dict[str, Any], name: str) -> Dict[str, Any]:
    """Return the unique reservoir element named ``name``."""
    matches = [r for r in system["reservoir_array"] if r["name"] == name]
    assert len(matches) == 1, f"expected one reservoir {name!r}, got {matches}"
    return matches[0]


def _no_ver_arcs(system: Dict[str, Any], name_prefix: str) -> None:
    """Assert no ``<name_prefix>_ver_*`` waterway exists in the system."""
    matches = [
        w
        for w in system["waterway_array"]
        if w["name"].startswith(name_prefix + "_ver_")
    ]
    assert matches == [], f"vrebemb reservoir leaked _ver arc(s): {matches}"


# ---------------------------------------------------------------------------
# Core behaviour: vrebemb reservoir carries spill on the reservoir spillway,
# never on a _ver waterway — regardless of ser_ver or the flag.
# ---------------------------------------------------------------------------


def test_vrebemb_reservoir_ser_ver_has_no_ver_arc_and_spillway_cost():
    """vrebemb embalse with ser_ver > 0: no ``_ver`` arc; spillway on reservoir.

    The spill is represented by ``Reservoir.spillway_cost = 0`` (free) with
    the C++-default finite capacity; ``spillway_capacity`` is omitted from
    the JSON.  Every reservoir except the never-drain sentinels gets the
    drain, so the vrebemb cost is irrelevant; the flag value is too.
    """
    for flag in (True, False):
        cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2)
        sink = _serie("Sink", 2)
        system = _run(
            [cent, sink],
            vrebemb_costs={"Dam": 5000.0},
            options={"vrebemb_as_sink": flag},
        )

        _no_ver_arcs(system, "Dam")
        # No synthetic ocean drain was synthesised for the spill side.
        assert not [j for j in system["junction_array"] if j["name"] == "Dam_ocean"]
        res = _reservoir(system, "Dam")
        assert res.get("spillway_cost") == 0.0
        assert "spillway_capacity" not in res


def test_vrebemb_reservoir_terminal_has_no_ver_arc():
    """vrebemb embalse with ser_ver = 0 (terminal): no ``_ver`` arc.

    The spill rides the reservoir storage-drain column (drained embalse),
    so no ``_ver`` waterway is emitted.  Because this central is also
    terminal on the GEN side (``ser_hid = 0`` with no downstream consumer
    of its gen Waterway), the terminal gen path collapses onto the source
    junction's own drain column — a genuine basin exit, not the spurious
    in-cascade self-drain the old reservoir-drain model forbade.
    """
    cent = _embalse_in_network("Term", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"Term": 5000.0},
        options={"vrebemb_as_sink": True},
    )

    _no_ver_arcs(system, "Term")
    res = _reservoir(system, "Term")
    assert res.get("spillway_cost") == 0.0
    assert "spillway_capacity" not in res
    # No synthetic ocean junction is created for the terminal gen path.
    assert not [j for j in system["junction_array"] if j["name"].endswith("_ocean")]
    # The source junction sheds its terminal gen surplus via its own
    # drain column (drain_capacity = PotMax / Rendi = 100 / 0.85).
    src = next(j for j in system["junction_array"] if j["name"] == "Term")
    assert src.get("drain") is True
    assert src["drain_capacity"] == pytest.approx(100.0 / 0.85)


# ---------------------------------------------------------------------------
# Non-vrebemb centrals are untouched: they keep their _ver routing.
# ---------------------------------------------------------------------------


def test_non_vrebemb_serie_keeps_ver_arc():
    """Non-vrebemb serie central: ``_ver`` still targets ser_ver."""
    cent = _serie("CentA", 1, ser_hid=2, ser_ver=2, vert_max=50.0)
    sink = _serie("Sink", 2)
    # Empty vrebemb mapping ⇒ get_cost("CentA") returns None.
    system = _run(
        [cent, sink],
        vrebemb_costs={},
        options={"vrebemb_as_sink": True},
    )

    ver = [w for w in system["waterway_array"] if w["name"].startswith("CentA_ver_")]
    assert len(ver) == 1
    assert ver[0]["junction_b"] == "Sink"
    assert not ver[0]["junction_b"].endswith("_ocean")


# ---------------------------------------------------------------------------
# Asymmetric vrebemb reservoir: still no _ver arc (spill on the reservoir).
# ---------------------------------------------------------------------------


def test_vrebemb_asymmetric_reservoir_has_no_ver_arc():
    """Asymmetric vrebemb reservoir (ser_hid != ser_ver): no ``_ver`` arc.

    The spill is on the reservoir spillway column; the asymmetric
    ``ser_ver`` routing is reproduced by the reservoir's ``spill_junction``
    wiring in the C++ ReservoirLP, not by a parallel arc.
    """
    cent = _embalse_in_network("Asym", 1, ser_hid=2, ser_ver=3)
    cent_a = _serie("CentA", 2)
    cent_b = _serie("CentB", 3)
    system = _run(
        [cent, cent_a, cent_b],
        vrebemb_costs={"Asym": 5000.0},
        options={"vrebemb_as_sink": True},
    )

    _no_ver_arcs(system, "Asym")
    res = _reservoir(system, "Asym")
    assert res.get("spillway_cost") == 0.0


def test_vrebemb_with_drop_spillway_still_no_ver_arc():
    """``--drop-spillway-waterway`` on a vrebemb reservoir: still no ``_ver`` arc.

    Both code paths converge on "no draining ``_ver`` arc"; the
    drop-spillway flag additionally marks the source junction as a drain.
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

    ver_arcs = [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    assert ver_arcs == [], f"unexpected _ver arcs: {ver_arcs}"


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
