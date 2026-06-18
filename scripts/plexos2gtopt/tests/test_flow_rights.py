"""Unit tests for FlowRight extraction + JSON build (Laja irrigation)."""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import FlowRightSpec, TurbineSpec, WaterwaySpec
from plexos2gtopt.gtopt_writer import build_flow_right_array
from plexos2gtopt.parsers import extract_flow_rights
from plexos2gtopt.plexos_loader import PlexosBundle


def _write_bounds(tmp_path: Path, body: str) -> None:
    """Write a Hydro_AntucoBounds.csv body and stub the XML the loader expects."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text("<MasterDataSet/>")
    (tmp_path / "Hydro_AntucoBounds.csv").write_text(body)


# ── extract_flow_rights is intentionally disabled ──────────────────────────
#
# Hydro_AntucoBounds.csv was previously interpreted as junction-level
# irrigation rights, but the PLEXOS DB schema shows those entries
# (ANTUCOmin/ANTUCOmax/ELTOROmax) are actually per-generator turbine
# discharge limits (collection_id=32, Generator→FlowConstraint).  The
# legacy extractor caused phantom infeasibilities by binding them as
# FlowRights on the upstream reservoir.  The extractor now returns an
# empty tuple unconditionally; junction-level rights (when needed) will
# be sourced from a different overlay.  These tests pin that disabled
# behaviour so any future re-enable is an explicit, intentional flip.


def test_extract_flow_rights_returns_empty_when_csv_has_rows(tmp_path: Path) -> None:
    """ELTOROmax row in CSV → extractor still returns ()."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nELTOROmax,2026,4,22,1,37.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="EL_TORO_U1", reservoir_name="ELTORO"),)
    out = extract_flow_rights(bundle, turbines)
    assert not out


def test_extract_flow_rights_returns_empty_with_generator_prefix_row(
    tmp_path: Path,
) -> None:
    """ANTUCOmin row + turbine on POLCURA → still () (disabled)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nANTUCOmin,2026,4,22,1,61.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="ANTUCO_U1", reservoir_name="POLCURA"),)
    out = extract_flow_rights(bundle, turbines)
    assert not out


def test_extract_flow_rights_returns_empty_on_dupes(tmp_path: Path) -> None:
    """Duplicate NAME rows → still () (disabled)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\n"
        "ELTOROmax,2026,4,22,1,30.0\n"
        "ELTOROmax,2026,4,23,1,40.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="EL_TORO_U1", reservoir_name="ELTORO"),)
    out = extract_flow_rights(bundle, turbines)
    assert not out


def test_extract_flow_rights_returns_empty_on_unresolvable(tmp_path: Path) -> None:
    """Bound with no matching reservoir/turbine → still () (disabled)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nBOGUSmax,2026,4,22,1,1.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_flow_rights(bundle, turbines=())
    assert not out


def test_extract_flow_rights_no_csv(tmp_path: Path) -> None:
    """Missing CSV → still () (no error)."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text("<MasterDataSet/>")
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_flow_rights(bundle, turbines=())
    assert not out


# ── build_flow_right_array — JSON shape ────────────────────────────────────


def test_build_flow_right_array_drops_unresolved() -> None:
    """Specs with junction_name=None are silently dropped by the writer."""
    fr = (
        FlowRightSpec(
            name="resolved", junction_name="ELTORO", fmax=37.0, purpose="irrigation"
        ),
        FlowRightSpec(name="orphan", junction_name=None, fmax=99.0),
    )
    out = build_flow_right_array(fr)
    assert len(out) == 1
    assert out[0]["junction_a"] == "ELTORO"
    assert out[0]["purpose"] == "irrigation"
    assert out[0]["fmax"] == 37.0
    assert "fmin" not in out[0]
    # No bypass_junction in topology → no junction_b emitted.
    assert "junction_b" not in out[0]


def test_build_flow_right_array_emits_both_bounds() -> None:
    """fmin + fmax both emitted when set."""
    fr = (
        FlowRightSpec(
            name="both", junction_name="J1", fmin=5.0, fmax=10.0, purpose="env"
        ),
    )
    out = build_flow_right_array(fr)
    assert out[0]["fmin"] == 5.0
    assert out[0]["fmax"] == 10.0


# ── New bypass-via-FlowRight feature (35d3bdb8a) ───────────────────────────
#
# The legacy emission added a synthetic ``bypass_<name>`` Waterway in
# parallel with the FlowRight to provide a pressure-release path when
# the irrigation cap saturated the junction balance.  The new model
# expresses this directly on the FlowRight via ``bypass_junction`` /
# ``bypass_cost`` JSON fields; the LP layer (``flow_right_lp.cpp``)
# adds a per-block bypass column inline.  These tests pin the new
# JSON shape and confirm the synthetic-Waterway path is gone.


def test_build_flow_right_array_emits_explicit_bypass_junction() -> None:
    """When the spec sets bypass_junction, it propagates to JSON verbatim."""
    fr = (
        FlowRightSpec(
            name="eltoro_irr",
            junction_name="ELTORO",
            fmax=37.0,
            bypass_junction="DOWNSTREAM",
            bypass_cost=2.5,
        ),
    )
    out = build_flow_right_array(fr)
    assert len(out) == 1
    assert out[0]["junction_b"] == "DOWNSTREAM"
    assert out[0]["bypass_cost"] == 2.5


def test_build_flow_right_array_auto_resolves_bypass_from_vert_waterway() -> None:
    """When no explicit bypass_junction is set, fall back to the first
    existing ``Vert_*`` spillway's downstream from the FlowRight's junction."""
    fr = (FlowRightSpec(name="eltoro_irr", junction_name="ELTORO", fmax=37.0),)
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="Vert_ELTORO",
            storage_from="ELTORO",
            storage_to="ELTORO_DS",
        ),
    )
    out = build_flow_right_array(fr, waterways=waterways)
    assert len(out) == 1
    assert out[0]["junction_b"] == "ELTORO_DS"
    # Default bypass_cost is 0.0 → not emitted (LP defaults to free
    # pass-through, used freely when no irrigation pressure).
    assert "bypass_cost" not in out[0]


def test_build_flow_right_array_no_bypass_when_no_topology() -> None:
    """No Vert_* waterway and no explicit bypass_junction → no field emitted.

    Preserves pure-consumer semantics for FlowRights on junctions
    that have no downstream pressure-release path (the legacy fallback).
    """
    fr = (FlowRightSpec(name="standalone", junction_name="ISLAND", fmax=10.0),)
    out = build_flow_right_array(fr, waterways=())
    assert len(out) == 1
    assert "junction_b" not in out[0]
    assert "bypass_cost" not in out[0]


def test_build_flow_right_array_no_longer_appends_synthetic_waterway() -> None:
    """Legacy behaviour (synthetic ``bypass_<name>`` Waterway) is gone.

    The ``extra_waterways`` argument is kept for API compatibility but
    must not be mutated — the bypass path lives on the FlowRight JSON
    entry itself now via ``bypass_junction``.
    """
    fr = (FlowRightSpec(name="eltoro_irr", junction_name="ELTORO", fmax=37.0),)
    waterways = (
        WaterwaySpec(
            object_id=1,
            name="Vert_ELTORO",
            storage_from="ELTORO",
            storage_to="ELTORO_DS",
        ),
    )
    extra: list[dict] = []
    build_flow_right_array(fr, waterways=waterways, extra_waterways=extra)
    assert not extra
