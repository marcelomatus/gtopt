"""Tests for the ``--drop-spillway-waterway`` mode.

The flag is **opt-in** (default: False) — by default JunctionWriter
emits PLP-faithful ``_ver`` (spillway / vert) waterways with their
``CVert`` / ``Costo de Rebalse`` fcost, matching the historical
PLP topology.

When the option is explicitly enabled (``drop_spillway_waterway = True``),
JunctionWriter must:

* never emit a ``_ver`` (spillway / vert) waterway, regardless of
  ``ser_ver`` value or ``VertMax`` / ``Costo de Rebalse`` settings;
* never allocate a synthetic ``<central>_ocean`` junction for the
  spillway side (the gen-side ocean fallback for ``ser_hid = 0`` is
  unaffected);
* mark the central's own junction as ``drain = True`` so excess water
  exits the system through the junction instead of the missing arc.

The default-off flip (2026-04-28) followed the gtopt_iplp investigation:
suppress-mode topology was implicated in the SDDP elastic-cut degeneracy
chain at LMAULE / ELTORO.  PLP-faithful spillway topology is the safer
default; opt into suppress mode only when LP scaling outweighs routing
fidelity for the case at hand.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any, Dict

import pytest

from plp2gtopt.plp2gtopt import convert_plp_case

from ..junction_writer import JunctionWriter
from .test_junction_writer import MockCentralParser


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _serie(
    name: str,
    number: int,
    *,
    bus: int = 1,
    ser_hid: int = 0,
    ser_ver: int = 0,
    vert_max: float = 0.0,
    pmax: float = 10.0,
    efficiency: float = 1.0,
    afluent: float = 0.0,
    ctype: str = "serie",
) -> Dict[str, Any]:
    """Build a minimal serie / pasada central dict for JunctionWriter."""
    return {
        "number": number,
        "name": name,
        "type": ctype,
        "bus": bus,
        "pmin": 0,
        "pmax": pmax,
        "vert_min": 0.0,
        "vert_max": vert_max,
        "efficiency": efficiency,
        "ser_hid": ser_hid,
        "ser_ver": ser_ver,
        "afluent": afluent,
    }


def _embalse(
    name: str,
    number: int,
    *,
    ser_hid: int = 0,
    ser_ver: int = 0,
    vert_max: float = 1000.0,
) -> Dict[str, Any]:
    """Build a minimal embalse central."""
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


def _run(centrals, *, drop: bool = True) -> Dict[str, Any]:
    """Run JunctionWriter with explicit ``drop_spillway_waterway`` and return system."""
    writer = JunctionWriter(
        central_parser=MockCentralParser(centrals),
        options={"drop_spillway_waterway": drop},
    )
    result = writer.to_json_array()
    assert result, "expected non-empty system output"
    return result[0]


# ---------------------------------------------------------------------------
# Default = False: _ver waterway emitted, source junction is not a drain
# ---------------------------------------------------------------------------


def test_default_keeps_spillway_waterway():
    """The default constructor (no options) must NOT enable suppress mode.

    Default flipped to False on 2026-04-28; suppress mode is now opt-in.
    """
    writer = JunctionWriter(central_parser=MockCentralParser([]))
    assert writer._drop_spillway_waterway is False


# ---------------------------------------------------------------------------
# Opt-in: drop_spillway_waterway=True suppresses every _ver arc
# ---------------------------------------------------------------------------


def test_serie_with_ser_ver_target_no_ver_arc():
    """Serie with ser_ver pointing at a downstream sink: ``_ver`` is suppressed.

    PLP-faithful behaviour would route ``CentA → Sink`` via a costed
    ``_ver`` arc.  In the suppress mode the arc is dropped and CentA's
    own junction takes over as the spill outlet.
    """
    cent = _serie("CentA", 1, ser_hid=2, ser_ver=2, vert_max=50.0)
    sink = _serie("Sink", 2)
    system = _run([cent, sink])

    # No _ver arc anywhere.
    assert not [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    # The gen arc still goes to Sink — now carried by the built-in Turbine
    # (junction_a → junction_b) instead of a ``_gen`` Waterway.
    assert any(
        t.get("junction_a") == "CentA" and t.get("junction_b") == "Sink"
        for t in system["turbine_array"]
    )
    # Source junction is now a drain.
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["CentA"]["drain"] is True


def test_terminal_serie_no_synthetic_ocean_for_spillway():
    """``ser_ver = 0 + VertMax > 0`` no longer triggers a synthetic ocean.

    With drop_spillway_waterway on, the terminal-spillway ocean fallback
    is bypassed.  The gen path used to synthesise its own ocean too,
    but bus > 0 terminal turbines now use the built-in
    ``Turbine.junction_a`` waterway — zero synthetic ocean Junctions,
    zero ``_gen``/``_ver`` Waterways to the ocean.
    """
    cent = _serie("Term", 1, ser_hid=0, ser_ver=0, vert_max=50.0)
    system = _run([cent])

    oceans = [j for j in system["junction_array"] if "_ocean" in j["name"]]
    assert not oceans, "expected no synthetic ocean junctions for terminal turbine"
    # No waterway ends in an ocean — the built-in Turbine waterway
    # debits the central's own junction and drains terminal-style.
    to_ocean = [
        w for w in system["waterway_array"] if w["junction_b"].endswith("_ocean")
    ]
    assert not to_ocean

    # The terminal turbine is emitted with the built-in waterway form.
    turbines = system.get("turbine_array", [])
    assert len(turbines) == 1
    assert turbines[0]["junction_a"] == "Term"
    assert "waterway" not in turbines[0]
    assert "junction_b" not in turbines[0]


def test_no_spillway_fcost_in_suppress_mode():
    """Without the ``_ver`` arc, no PLP CVert/rebalse fcost ends up in the LP."""
    cent = _serie("CentA", 1, ser_hid=0, ser_ver=0, vert_max=50.0)
    system = _run([cent])
    for w in system["waterway_array"]:
        assert "_ver_" not in w["name"], (
            f"unexpected _ver arc {w['name']!r} in suppress mode"
        )
        # ``fcost`` originates from VertMax / CVert / Rebalse on the
        # spillway path; the gen waterway never carries it.  Explicitly
        # check it never sneaks in either.
        assert "fcost" not in w, (
            f"unexpected fcost on waterway {w['name']!r}: {w.get('fcost')!r}"
        )


def test_embalse_with_ser_ver_target_drain_true():
    """Embalse with real ``ser_ver`` no longer emits ``_ver`` either."""
    res = _embalse("Dam", 1, ser_hid=2, ser_ver=2, vert_max=1000.0)
    sink = _serie("Sink", 2)
    system = _run([res, sink])

    assert not [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["Dam"]["drain"] is True


def test_pasada_in_hydro_mode_drain_on_source():
    """Pasada centrals routed through the hydro topology also drain locally."""
    # Pasada centrals only get full junction processing in pasada_mode=hydro.
    cent = _serie(
        "Pas",
        1,
        bus=10,
        ser_hid=0,
        ser_ver=0,
        vert_max=20.0,
        ctype="pasada",
    )
    writer = JunctionWriter(
        central_parser=MockCentralParser([cent]),
        options={"drop_spillway_waterway": True, "pasada_mode": "hydro"},
    )
    result = writer.to_json_array()
    assert result, "pasada_mode=hydro should emit the junction system"
    system = result[0]

    assert not [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["Pas"]["drain"] is True


# ---------------------------------------------------------------------------
# Explicit OFF: legacy spillway topology returns
# ---------------------------------------------------------------------------


def test_legacy_mode_collapses_spill_to_source_drain():
    """``drop_spillway_waterway = False`` keeps the spill, but for
    ``ser_ver = 0`` it is now collapsed onto the SOURCE junction's own
    drain column rather than synthesising a ``<central>_ocean`` node + a
    ``_ver`` arc.  The spill cap (``VertMax``) lands on ``drain_capacity``
    (mass conservation: the surplus still exits).  No ``*_ocean`` junction
    and no ``_ver`` waterway are emitted for the spill-to-sea case.
    """
    cent = _serie("CentA", 1, ser_hid=0, ser_ver=0, vert_max=50.0)
    system = _run([cent], drop=False)

    ver_arcs = [w for w in system["waterway_array"] if "_ver_" in w["name"]]
    # No ``_ver`` arc is emitted: the spill rides the source self-drain.
    assert len(ver_arcs) == 0
    # No synthetic ocean junction is created.
    assert not [j for j in system["junction_array"] if j["name"].endswith("_ocean")]
    junctions = {j["name"]: j for j in system["junction_array"]}
    src = junctions["CentA"]
    # The source junction itself drains, carrying the VertMax spill cap.
    assert src["drain"] is True
    assert src["drain_capacity"] == 50.0


# ---------------------------------------------------------------------------
# CLI plumbing: --drop-spillway-waterway / --no-drop-spillway-waterway
# ---------------------------------------------------------------------------


def test_cli_flag_default_is_false():
    """CLI default for ``--drop-spillway-waterway`` is False (opt-in)."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case"])
    assert args.drop_spillway_waterway is False


def test_cli_flag_can_be_enabled():
    """``--drop-spillway-waterway`` enables suppress mode explicitly."""
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case", "--drop-spillway-waterway"])
    assert args.drop_spillway_waterway is True


def test_cli_flag_no_form_keeps_default():
    """``--no-drop-spillway-waterway`` is accepted and pins the default off.

    Useful for ``~/.gtopt.conf`` overrides where a config file might set
    the flag on; the negative form lets the user force it back off on the
    CLI without depending on the global default.
    """
    from plp2gtopt.main import make_parser  # noqa: PLC0415

    args = make_parser().parse_args(["plp_case", "--no-drop-spillway-waterway"])
    assert args.drop_spillway_waterway is False


def test_cli_flag_passes_through_build_options():
    """``build_options`` propagates the parsed value to the options dict."""
    from plp2gtopt.main import build_options, make_parser  # noqa: PLC0415

    args_default = make_parser().parse_args(["plp_case"])
    args_on = make_parser().parse_args(["plp_case", "--drop-spillway-waterway"])
    assert build_options(args_default)["drop_spillway_waterway"] is False
    assert build_options(args_on)["drop_spillway_waterway"] is True


# ---------------------------------------------------------------------------
# End-to-end integration: convert_plp_case on plp_min_reservoir
# ---------------------------------------------------------------------------


_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMinReservoir = _CASES_DIR / "plp_min_reservoir"


def _make_int_opts(tmp_path: Path, case_name: str, *, drop: bool) -> dict:
    """Build a conversion options dict for the integration tests."""
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": _PLPMinReservoir,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
        "drop_spillway_waterway": drop,
    }


@pytest.mark.integration
def test_integration_drop_on_drops_ver_arcs(tmp_path):
    """End-to-end: ``--drop-spillway-waterway`` (opt-in) omits ``_ver`` arcs.

    The PLP case has Reservoir1 → TurbineGen → ocean.  Both centrals
    are embalse / serie; the suppress mode drops every ``_ver`` arc
    (and the spill-side synthetic ocean junction) while keeping the
    gen path and turbine intact.
    """
    opts = _make_int_opts(tmp_path, "drop_on", drop=True)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys_data = data["system"]
    waterways = sys_data["waterway_array"]

    # No spillway arcs anywhere.
    assert not [w for w in waterways if "_ver_" in w["name"]]

    # No fcost field on any waterway (CVert / Costo de Rebalse only ever
    # decorated the spill side, which is gone).
    assert not [w for w in waterways if "fcost" in w]

    # The PLP-source centrals (Reservoir1 + TurbineGen) carry drain=True.
    junctions = {j["name"]: j for j in sys_data["junction_array"]}
    assert junctions["Reservoir1"]["drain"] is True
    assert junctions["TurbineGen"]["drain"] is True

    # Turbines are still emitted; Reservoir1's generation flow is now on
    # the built-in Turbine (junction_a=Reservoir1, no ``Reservoir1_gen``
    # Waterway).
    assert any(t["name"] == "TurbineGen" for t in sys_data["turbine_array"])
    assert not [w for w in waterways if w["name"].startswith("Reservoir1_gen_")]
    assert any(
        t["name"] == "Reservoir1" and t.get("junction_a") == "Reservoir1"
        for t in sys_data["turbine_array"]
    )


@pytest.mark.integration
def test_integration_default_no_drain_on_source(tmp_path):
    """End-to-end: default (drop=False) keeps the legacy drain rule.

    The plp_min_reservoir case has ``VertMax = 0`` on every central so
    no ``_ver`` arc is emitted in either mode (the legacy spill ocean
    fallback is gated on ``VertMax > 0`` or ``in_vrebemb``).  The
    behavioural difference therefore shows up purely in the
    ``drain`` flag: default mode (PLP-faithful) does NOT mark the
    source junctions as drains because the gen waterway still exists,
    while the opt-in suppress mode would (to absorb the missing
    spillway capacity).
    """
    opts = _make_int_opts(tmp_path, "drop_off", drop=False)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys_data = data["system"]

    junctions = {j["name"]: j for j in sys_data["junction_array"]}
    # Reservoir1 has a gen waterway → TurbineGen.  In legacy mode the
    # source junction is NOT a drain (legacy rule: drain only when
    # both gen and ver are absent).
    assert junctions["Reservoir1"]["drain"] is False
    # TurbineGen has gen → ocean and no spill needed: also not a drain.
    assert junctions["TurbineGen"]["drain"] is False


# ---------------------------------------------------------------------------
# End-to-end comparison: same case converted in both modes.
#
# None of the shipped ``plp_min_*`` cases enable a spillway path (every
# embalse / serie has ``VertMax = 0`` and ``Vertim = 0``), so the legacy
# converter never emits a ``_ver`` waterway on them.  To exercise the
# real comparison — legacy emits ``_ver``, default suppresses it — we
# build a tiny fixture from ``plp_min_reservoir`` and patch
# ``plpcnfce.dat`` so Reservoir1 has ``VertMax = 50`` and routes the
# spill to TurbineGen (``Vertim = 2``).  The patched case lives in
# ``tmp_path``; the shipped fixture is untouched.
# ---------------------------------------------------------------------------


# Reservoir1 line in plp_min_reservoir/plpcnfce.dat — the source-of-truth
# verbatim, used as the ``old_string`` for the in-place fixture patch.
# Keeping the literal here (rather than building one with f-strings) makes
# the test robust against subtle whitespace shifts in the original file.
_RESERVOIR1_VERTMAX_OLD = "             0.0  100.0   000.0   000.0"
_RESERVOIR1_VERTMAX_NEW = "             0.0  100.0   000.0   050.0"
_RESERVOIR1_VERTIM_OLD = "             0.0  1.000      1      2      0    0.0  0020.0"
_RESERVOIR1_VERTIM_NEW = "             0.0  1.000      1      2      2    0.0  0020.0"


def _make_spillway_fixture(tmp_path: Path) -> Path:
    """Copy plp_min_reservoir into tmp_path and enable Reservoir1's spillway.

    The patch sets:
      * ``VertMax = 50``  (was 0)  → unblocks the per-block spill cap.
      * ``Vertim = 2``    (was 0)  → routes spill to TurbineGen instead
                                     of dropping it via a synthetic ocean.

    With this patched case the legacy converter emits a
    ``Reservoir1_ver_1_2`` waterway carrying CVert / fallback fcost; the
    new default suppresses it and marks Reservoir1's junction as a drain.
    """
    src = _PLPMinReservoir
    dst = tmp_path / "plp_min_spillway"
    shutil.copytree(src, dst)
    cnfce = dst / "plpcnfce.dat"
    text = cnfce.read_text(encoding="utf-8")
    assert _RESERVOIR1_VERTMAX_OLD in text, (
        "fixture drift — Reservoir1 VertMax line moved in plp_min_reservoir"
    )
    assert _RESERVOIR1_VERTIM_OLD in text, (
        "fixture drift — Reservoir1 Vertim line moved in plp_min_reservoir"
    )
    text = text.replace(_RESERVOIR1_VERTMAX_OLD, _RESERVOIR1_VERTMAX_NEW, 1)
    text = text.replace(_RESERVOIR1_VERTIM_OLD, _RESERVOIR1_VERTIM_NEW, 1)
    cnfce.write_text(text, encoding="utf-8")
    return dst


def _convert(input_dir: Path, out_dir: Path, name: str, *, drop: bool) -> dict:
    """Run convert_plp_case with the given option and return the system dict."""
    out_dir.mkdir(parents=True, exist_ok=True)
    opts = {
        "input_dir": input_dir,
        "output_dir": out_dir,
        "output_file": out_dir / f"{name}.json",
        "hydrologies": "1",
        "drop_spillway_waterway": drop,
    }
    convert_plp_case(opts)
    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    return data["system"]


@pytest.mark.integration
def test_integration_compare_modes_on_spillway_case(tmp_path):
    """Drained embalse carries its spill on the storage-drain column.

    ``Reservoir1`` is an ``embalse`` and NOT a never-drain sentinel, so it
    now carries its spill on the reservoir storage-drain column
    (``spillway_cost = 0``, ``spillway_capacity`` omitted → C++ +6000 m³/s
    default) in BOTH modes.  No ``_ver`` arc is emitted in either mode —
    keeping a draining ``_ver`` arc next to the drain column would be a
    double escape path.  ``--drop-spillway-waterway`` therefore no longer
    changes the spill topology for a drained reservoir; the patched
    ``VertMax = 50`` / ``Vertim = 2`` fixture no longer yields a legacy
    ``_ver`` arc.  (A NEVER-drain reservoir like ELTORO is the only case
    that still rides a ``_ver`` arc — covered by the unit-level
    ``test_embalse_no_ver_waterway_junction_is_drain``.)
    """
    case_dir = _make_spillway_fixture(tmp_path)

    legacy = _convert(case_dir, tmp_path / "out_legacy", "spill_legacy", drop=False)
    suppress = _convert(case_dir, tmp_path / "out_default", "spill_default", drop=True)

    legacy_ver = [w for w in legacy["waterway_array"] if "_ver_" in w["name"]]
    default_ver = [w for w in suppress["waterway_array"] if "_ver_" in w["name"]]

    # The drain column carries the spill in both modes → no ``_ver`` arc.
    assert legacy_ver == [], f"drained reservoir leaked a legacy _ver arc: {legacy_ver}"
    assert default_ver == [], f"default mode leaked _ver arc(s): {default_ver}"

    # The spill lives on the reservoir storage-drain column (cost 0, finite
    # C++ default capacity) in both modes.
    for system in (legacy, suppress):
        res = next(r for r in system["reservoir_array"] if r["name"] == "Reservoir1")
        assert res.get("spillway_cost") == 0.0
        assert "spillway_capacity" not in res

    # No fcost on any waterway in either mode — fcost only ever decorated
    # the now-removed ``_ver`` spill path.
    assert all("fcost" not in w for w in legacy["waterway_array"])
    assert all("fcost" not in w for w in suppress["waterway_array"])

    # The two modes are now topologically identical for a drained reservoir:
    # same gen waterways, turbines, reservoirs and total waterway count.
    legacy_gen = [w for w in legacy["waterway_array"] if "_gen_" in w["name"]]
    default_gen = [w for w in suppress["waterway_array"] if "_gen_" in w["name"]]
    assert len(legacy_gen) == len(default_gen)
    assert len(legacy["turbine_array"]) == len(suppress["turbine_array"])
    assert len(legacy["reservoir_array"]) == len(suppress["reservoir_array"])
    assert len(legacy["waterway_array"]) == len(suppress["waterway_array"])
