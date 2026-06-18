"""Smoke tests for :mod:`plexos2gtopt`.

These tests are the package's import-clean gate: ``--help`` runs,
``--version`` reports the expected string, the package imports
without side effects, and the writer emits a syntactically valid
planning JSON from an empty :class:`PlexosCase` (the topology
extractors are P1 work; v0 only guarantees the JSON skeleton).
"""

from __future__ import annotations

from pathlib import Path

import pytest

import plexos2gtopt
from plexos2gtopt.entities import BundleSpec, PlexosCase
from plexos2gtopt.gtopt_writer import build_planning, write_planning
from plexos2gtopt.main import main, make_parser


def test_package_version() -> None:
    """The package exposes a 0.x.y version string."""
    assert plexos2gtopt.__version__.startswith("0.")


def test_public_exports() -> None:
    """Top-level re-exports match :data:`plexos2gtopt.__all__`."""
    for name in plexos2gtopt.__all__:
        assert hasattr(plexos2gtopt, name), f"missing public export: {name}"


def test_help_smoke(capsys: pytest.CaptureFixture[str]) -> None:
    """``plexos2gtopt --help`` exits 0 and mentions the core flags."""
    parser = make_parser()
    with pytest.raises(SystemExit) as exc:
        parser.parse_args(["--help"])
    assert exc.value.code == 0
    captured = capsys.readouterr().out
    assert "plexos2gtopt" in captured
    assert "--info" in captured
    assert "--validate" in captured


def test_version_smoke(capsys: pytest.CaptureFixture[str]) -> None:
    """``plexos2gtopt --version`` exits 0 with the package version."""
    parser = make_parser()
    with pytest.raises(SystemExit) as exc:
        parser.parse_args(["--version"])
    assert exc.value.code == 0
    captured = capsys.readouterr().out
    assert plexos2gtopt.__version__ in captured


def test_no_input_errors(capsys: pytest.CaptureFixture[str]) -> None:
    """``main`` without a bundle path exits non-zero with a usage message."""
    with pytest.raises(SystemExit) as exc:
        main([])
    assert exc.value.code != 0
    err = capsys.readouterr().err
    assert "input bundle" in err.lower()


def test_validate_missing_bundle(tmp_path: Path) -> None:
    """``--validate`` on a non-existent path exits non-zero."""
    bogus = tmp_path / "does_not_exist.zip"
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(bogus)])
    assert exc.value.code != 0


def test_build_planning_empty_case() -> None:
    """An empty :class:`PlexosCase` still yields a valid planning JSON."""
    case = PlexosCase(bundle=BundleSpec(bundle_name="empty"))
    planning = build_planning(case, name="empty")
    assert "options" in planning
    assert "simulation" in planning
    assert "system" in planning
    sim = planning["simulation"]
    assert len(sim["block_array"]) == 24
    assert sim["block_array"][0]["duration"] == 1.0
    assert len(sim["stage_array"]) == 1
    assert sim["stage_array"][0]["count_block"] == 24
    # An empty topology collapses to single-bus / no-Kirchhoff (the
    # flag lives inside the nested ``model_options`` block, matching
    # gtopt's reference cases in ``cases/c0`` and ``cases/ieee_4b_ori``).
    model_opts = planning["options"]["model_options"]
    assert model_opts["use_single_bus"] is True
    assert model_opts["use_kirchhoff"] is False


def test_write_planning_round_trip(tmp_path: Path) -> None:
    """Write a planning, parse it back, confirm the round-trip."""
    import json

    case = PlexosCase(bundle=BundleSpec(bundle_name="rt"))
    planning = build_planning(case, name="rt")
    out_path = tmp_path / "rt.json"
    write_planning(planning, out_path)
    assert out_path.is_file()
    reparsed = json.loads(out_path.read_text())
    assert reparsed == planning


def test_install_solver_param_files_copies_bundled_prm(tmp_path: Path) -> None:
    """install_solver_param_files copies every bundled ``*.prm``."""
    from plexos2gtopt import gtopt_writer as gw

    # Point _BUNDLED_SOLVERS_DIR at a controlled fixture so the test
    # passes regardless of which prm files happen to be in the package.
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "cplex.prm").write_text(
        "CPLEX Parameter File Version 22.1.1.0\nCPXPARAM_MIP_Cuts_Gomory   2\n"
    )
    (bundle / "highs.prm").write_text("solver = simplex\n")

    target = tmp_path / "case"
    original = gw._BUNDLED_SOLVERS_DIR
    gw._BUNDLED_SOLVERS_DIR = bundle  # type: ignore[attr-defined]
    try:
        installed = gw.install_solver_param_files(target)
    finally:
        gw._BUNDLED_SOLVERS_DIR = original  # type: ignore[attr-defined]

    assert {p.name for p in installed} == {"cplex.prm", "highs.prm"}
    cplex_dst = target / "solvers" / "cplex.prm"
    assert cplex_dst.is_file()
    assert "CPXPARAM_MIP_Cuts_Gomory" in cplex_dst.read_text()


def test_install_solver_param_files_no_bundle(tmp_path: Path) -> None:
    """When the bundled directory is absent the helper is a no-op."""
    from plexos2gtopt import gtopt_writer as gw

    target = tmp_path / "case"
    original = gw._BUNDLED_SOLVERS_DIR
    gw._BUNDLED_SOLVERS_DIR = tmp_path / "does_not_exist"  # type: ignore[attr-defined]
    try:
        installed = gw.install_solver_param_files(target)
    finally:
        gw._BUNDLED_SOLVERS_DIR = original  # type: ignore[attr-defined]

    assert not installed
    assert not (target / "solvers").exists()


def test_write_planning_invokes_solver_install(tmp_path: Path) -> None:
    """write_planning installs solvers/ alongside the JSON output."""
    from plexos2gtopt import gtopt_writer as gw

    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "cplex.prm").write_text("# minimal\n")

    case = PlexosCase(bundle=BundleSpec(bundle_name="rt"))
    planning = build_planning(case, name="rt")
    out_path = tmp_path / "case" / "rt.json"

    original = gw._BUNDLED_SOLVERS_DIR
    gw._BUNDLED_SOLVERS_DIR = bundle  # type: ignore[attr-defined]
    try:
        write_planning(planning, out_path)
    finally:
        gw._BUNDLED_SOLVERS_DIR = original  # type: ignore[attr-defined]

    assert out_path.is_file()
    assert (out_path.parent / "solvers" / "cplex.prm").is_file()
