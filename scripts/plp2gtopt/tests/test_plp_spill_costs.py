"""Tests for the opt-in ``--plp-spill-costs`` converter option.

Default (flag OFF): every drained reservoir's storage-drain column stays
FREE (``Reservoir.spillway_cost = 0``) — pricing physical rebalse pollutes
marginal water values, so the free drain is the safe default.

Flag ON (PLP-parity studies only): the drain is priced at the RAW
plpvrebemb.dat ``Costo de Rebalse`` (fallback plpmat.dat ``CVert``, else
0.0), with **no** ``--spillway-cost-cap`` clamp.  The mapping is
unit-identical — PLP's ``FO(qrb) = CostoReb·edur/(ScaleObj·FactTasa)``
(genpdreb.f) equals gtopt's ``spillway_cost·duration·discount /
scale_objective`` — so the price passes through unscaled ($/(m³/s·h)).
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

from ..junction_writer import JunctionWriter
from .test_junction_writer import MockCentralParser
from .test_vrebemb_as_sink import MockVrebembParser, _embalse_in_network


class MockPlpmatParser:  # pylint: disable=too-few-public-methods
    """Minimal plpmat stand-in exposing the global ``CVert`` price."""

    def __init__(self, vert_cost: float):
        """Initialise with the global ``CVert`` value."""
        self.vert_cost = vert_cost


def _run(
    centrals: List[Dict[str, Any]],
    *,
    vrebemb_costs: Optional[Dict[str, float]] = None,
    plpmat_vert_cost: Optional[float] = None,
    options: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Run JunctionWriter with the given spill-cost sources; return system."""
    base_options: Dict[str, Any] = {"drop_spillway_waterway": False}
    if options is not None:
        base_options.update(options)
    writer = JunctionWriter(
        central_parser=MockCentralParser(centrals),
        vrebemb_parser=(
            MockVrebembParser(vrebemb_costs) if vrebemb_costs is not None else None
        ),
        plpmat_parser=(
            MockPlpmatParser(plpmat_vert_cost) if plpmat_vert_cost is not None else None
        ),
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


# ---------------------------------------------------------------------------
# Flag OFF (default): drain stays free regardless of vrebemb membership.
# ---------------------------------------------------------------------------


def test_default_off_keeps_free_drain():
    """Default (no option key): spillway_cost = 0 even for vrebemb members."""
    cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2)
    sink = _embalse_in_network("Sink", 2, ser_hid=0, ser_ver=0)
    system = _run([cent, sink], vrebemb_costs={"Dam": 5000.0})
    assert _reservoir(system, "Dam").get("spillway_cost") == 0.0


def test_explicit_off_keeps_free_drain():
    """``plp_spill_costs = False``: identical to the default free drain."""
    cent = _embalse_in_network("Dam", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"Dam": 5000.0},
        plpmat_vert_cost=0.01,
        options={"plp_spill_costs": False},
    )
    assert _reservoir(system, "Dam").get("spillway_cost") == 0.0


# ---------------------------------------------------------------------------
# Flag ON: raw vrebemb price → CVert fallback → 0.0; never clamped.
# ---------------------------------------------------------------------------


def test_on_emits_raw_vrebemb_price():
    """Flag ON: the drain carries the RAW Costo de Rebalse (no cap)."""
    cent = _embalse_in_network("Dam", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"Dam": 5000.0},
        plpmat_vert_cost=0.01,
        # vert_cost_cap present and small: must NOT clamp the drain price.
        options={"plp_spill_costs": True, "vert_cost_cap": 500.0},
    )
    res = _reservoir(system, "Dam")
    assert res.get("spillway_cost") == 5000.0
    # The C++-default finite capacity still applies (field omitted).
    assert "spillway_capacity" not in res


def test_on_small_prices_pass_through():
    """Flag ON: tiny PLP symmetry-breaker prices (0.01/0.1) pass through."""
    cent = _embalse_in_network("Dam", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"Dam": 0.1},
        plpmat_vert_cost=0.01,
        options={"plp_spill_costs": True},
    )
    assert _reservoir(system, "Dam").get("spillway_cost") == 0.1


def test_on_falls_back_to_cvert():
    """Flag ON, reservoir not in vrebemb: global plpmat CVert applies."""
    cent = _embalse_in_network("Dam", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={},  # get_cost -> None
        plpmat_vert_cost=0.01,
        options={"plp_spill_costs": True},
    )
    assert _reservoir(system, "Dam").get("spillway_cost") == 0.01


def test_on_no_sources_emits_free_drain():
    """Flag ON with no vrebemb / CVert source: drain stays free (0.0)."""
    cent = _embalse_in_network("Dam", 1, ser_hid=0, ser_ver=0)
    system = _run([cent], options={"plp_spill_costs": True})
    assert _reservoir(system, "Dam").get("spillway_cost") == 0.0


def test_on_never_drain_sentinel_unaffected():
    """Flag ON: ELTORO (never-drain sentinel) still emits no drain fields."""
    cent = _embalse_in_network("ELTORO", 1, ser_hid=0, ser_ver=0)
    system = _run(
        [cent],
        vrebemb_costs={"ELTORO": 5000.0},
        options={"plp_spill_costs": True},
    )
    res = _reservoir(system, "ELTORO")
    assert "spillway_cost" not in res
    assert "spillway_capacity" not in res


def test_on_vertmax_reservoir_keeps_ver_waterway_cvert():
    """Flag ON: VertMax>0 reservoir keeps its CVert-priced ``_ver`` arc.

    The drain is disabled (spill rides the physical ``_ver`` waterway,
    PLP's per-block ``qv``), and PLP prices ``qv`` at CVert — not at the
    vrebemb ``Costo de Rebalse`` — so the arc's ``fcost`` stays CVert.
    """
    cent = _embalse_in_network("Dam", 1, ser_hid=2, ser_ver=2, vert_max=50.0)
    sink = _embalse_in_network("Sink", 2, ser_hid=0, ser_ver=0)
    system = _run(
        [cent, sink],
        vrebemb_costs={"Dam": 5000.0},
        plpmat_vert_cost=0.01,
        options={"plp_spill_costs": True},
    )
    res = _reservoir(system, "Dam")
    assert "spillway_cost" not in res  # drain disabled
    ver = [w for w in system["waterway_array"] if w["name"].startswith("Dam_ver_")]
    assert len(ver) == 1
    assert ver[0]["fcost"] == 0.01  # PLP qv price = CVert, unchanged


# ---------------------------------------------------------------------------
# CLI plumbing
# ---------------------------------------------------------------------------


def test_cli_flag_default_is_false():
    """CLI default for ``--plp-spill-costs`` is False (opt-in)."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case"])
    assert args.plp_spill_costs is False


def test_cli_flag_boolean_optional_action():
    """``--plp-spill-costs`` / ``--no-plp-spill-costs`` both parse."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    parser = make_parser()
    assert parser.parse_args(["plp_case", "--plp-spill-costs"]).plp_spill_costs is True
    assert (
        parser.parse_args(["plp_case", "--no-plp-spill-costs"]).plp_spill_costs is False
    )


def test_cli_flag_passes_through_build_options():
    """``build_options`` propagates the parsed value to the options dict."""
    from plp2gtopt.main import build_options, make_parser  # noqa: PLC0415

    args_default = make_parser().parse_args(["plp_case"])
    args_on = make_parser().parse_args(["plp_case", "--plp-spill-costs"])
    assert build_options(args_default)["plp_spill_costs"] is False
    assert build_options(args_on)["plp_spill_costs"] is True
