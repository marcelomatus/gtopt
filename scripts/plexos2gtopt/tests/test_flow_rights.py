"""Unit tests for P7 FlowRight overlay (Laja irrigation)."""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import FlowRightSpec, TurbineSpec
from plexos2gtopt.gtopt_writer import build_flow_right_array
from plexos2gtopt.parsers import extract_flow_rights
from plexos2gtopt.plexos_loader import PlexosBundle


def _write_bounds(tmp_path: Path, body: str) -> None:
    """Write a Hydro_AntucoBounds.csv body and stub the XML the loader expects."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text("<MasterDataSet/>")
    (tmp_path / "Hydro_AntucoBounds.csv").write_text(body)


def test_extract_flow_rights_resolves_via_reservoir_name(tmp_path: Path) -> None:
    """ELTOROmax → ELTORO reservoir (direct name match)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nELTOROmax,2026,4,22,1,37.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="EL_TORO_U1", reservoir_name="ELTORO"),)
    out = extract_flow_rights(bundle, turbines)
    assert len(out) == 1
    assert out[0].name == "irrigation_ELTOROmax"
    assert out[0].junction_name == "ELTORO"
    assert out[0].fmax == 37.0
    assert out[0].fmin == 0.0


def test_extract_flow_rights_resolves_via_generator_prefix(tmp_path: Path) -> None:
    """ANTUCOmin → POLCURA (turbine prefix points at upstream reservoir)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nANTUCOmin,2026,4,22,1,61.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="ANTUCO_U1", reservoir_name="POLCURA"),)
    out = extract_flow_rights(bundle, turbines)
    assert len(out) == 1
    assert out[0].junction_name == "POLCURA"
    assert out[0].fmin == 61.0
    assert out[0].fmax == 0.0


def test_extract_flow_rights_drops_first_dupe_row(tmp_path: Path) -> None:
    """Same-NAME rows on later days are ignored (single-day PCP horizon)."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\n"
        "ELTOROmax,2026,4,22,1,30.0\n"
        "ELTOROmax,2026,4,23,1,40.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    turbines = (TurbineSpec(generator_name="EL_TORO_U1", reservoir_name="ELTORO"),)
    out = extract_flow_rights(bundle, turbines)
    assert len(out) == 1
    assert out[0].fmax == 30.0  # first day wins


def test_extract_flow_rights_missing_junction(tmp_path: Path) -> None:
    """A bound with no matching reservoir / turbine gets junction=None."""
    _write_bounds(
        tmp_path,
        "NAME,YEAR,MONTH,DAY,PERIOD,Value\nBOGUSmax,2026,4,22,1,1.0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_flow_rights(bundle, turbines=())
    assert len(out) == 1
    assert out[0].junction_name is None


def test_extract_flow_rights_no_csv(tmp_path: Path) -> None:
    """Missing CSV returns an empty tuple, no error."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text("<MasterDataSet/>")
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_flow_rights(bundle, turbines=())
    assert not out


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
    assert out[0]["junction"] == "ELTORO"
    assert out[0]["purpose"] == "irrigation"
    assert out[0]["fmax"] == 37.0
    assert "fmin" not in out[0]


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
