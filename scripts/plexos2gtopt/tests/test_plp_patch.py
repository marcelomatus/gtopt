"""Tests for the PLP ``embalses.csv`` cross-reference patcher.

Covers:

* ``load_embalses_csv`` parsing of the CEN-published schema (plain
  + ``.xz``).
* ``auto_detect_embalses`` probing of sibling paths.
* ``patch_case_with_plp_embalses`` synthesising a missing reservoir
  + emitting the matching junction.
* CLI gating: ``--plexos-legacy`` and ``--no-plp-embalses`` both
  short-circuit the patch.
"""

from __future__ import annotations

import logging
import lzma
from pathlib import Path

import pytest

from plexos2gtopt._plp_patch import (
    PlpEmbalsesEntry,
    auto_detect_embalses,
    find_compressed_path,
    load_embalses_csv,
    patch_case_with_plp_embalses,
)
from plexos2gtopt.entities import (
    BoundaryCutSpec,
    BundleSpec,
    JunctionSpec,
    PlexosCase,
    ReservoirSpec,
    TurbineSpec,
)


# ── Fixtures ──────────────────────────────────────────────────────────────


_EMB_CSV_TEXT = (
    "#Numero, Nombre, Tipo, VolMin, VolMax, VolMinNECF, VolMaxNECF, "
    "FEscala, FactRendim\n"
    "   1,LMAULE                                          ,A,"
    "           0.00,     1453409.30,           0.00,     1453409.30,"
    "   9,   12.994,\n"
    "   2,CIPRESES                                        ,E,"
    "        4716.36,      174660.33,        4716.36,      174660.33,"
    "   8,    9.288,\n"
    "  10,PILMAIQUEN                                      ,E,"
    "           0.00,      289680.30,           0.00,      289680.30,"
    "   8,    0.905,\n"
)


@pytest.fixture
def embalses_csv(tmp_path: Path) -> Path:
    """Write a small ``embalses.csv`` in CEN's published schema."""
    p = tmp_path / "embalses.csv"
    p.write_text(_EMB_CSV_TEXT, encoding="utf-8")
    return p


@pytest.fixture
def embalses_csv_xz(tmp_path: Path) -> Path:
    """Write a small ``embalses.csv.xz`` (compressed) in CEN's schema."""
    p = tmp_path / "embalses.csv.xz"
    with lzma.open(p, "wt", encoding="utf-8") as fh:
        fh.write(_EMB_CSV_TEXT)
    return p


def _make_case_with_missing_pilmaiquen(
    with_pilmaiquen_chain: bool = True,
) -> PlexosCase:
    """Return a :class:`PlexosCase` whose boundary cut references
    PILMAIQUEN but whose ``reservoir_array`` does not.

    By default an existing ``TurbineSpec`` references PILMAIQUEN so the
    strict patcher accepts the splice.  Pass ``with_pilmaiquen_chain=False``
    to exercise the orphan-skip path (real-bundle case: no hydro chain).
    """
    bundle = BundleSpec(bundle_date="20260422")
    turbines: tuple[TurbineSpec, ...] = (
        TurbineSpec(generator_name="CIPRESES_U1", reservoir_name="CIPRESES"),
    )
    if with_pilmaiquen_chain:
        turbines = turbines + (
            TurbineSpec(generator_name="PILMAIQUEN_U1", reservoir_name="PILMAIQUEN"),
        )
    return PlexosCase(
        bundle=bundle,
        reservoirs=(
            ReservoirSpec(object_id=1, name="L_Maule", emax=100.0, eini=50.0),
            ReservoirSpec(object_id=2, name="CIPRESES", emax=200.0, eini=100.0),
        ),
        junctions=(
            JunctionSpec(name="L_Maule"),
            JunctionSpec(name="CIPRESES"),
        ),
        turbines=turbines,
        boundary_cut=BoundaryCutSpec(
            fcf=1.247e9,
            slopes={
                "L_Maule": 9037.748,
                "CIPRESES": 6683.819,
                "PILMAIQUEN": 576.479,  # missing reservoir
            },
        ),
    )


# ── load_embalses_csv ─────────────────────────────────────────────────────


def test_load_embalses_csv_plain(embalses_csv: Path) -> None:
    """Reads a plain CSV, skips the comment header, returns typed rows."""
    rows = load_embalses_csv(embalses_csv)
    assert len(rows) == 3
    assert isinstance(rows[0], PlpEmbalsesEntry)
    # Trailing whitespace in the name field is stripped.
    assert rows[0].name == "LMAULE"
    assert rows[0].tipo == "A"
    assert rows[0].vmax == pytest.approx(1453409.30)
    assert rows[0].fact_rendim == pytest.approx(12.994)


def test_load_embalses_csv_xz(embalses_csv_xz: Path) -> None:
    """The ``.xz`` variant decompresses transparently."""
    rows = load_embalses_csv(embalses_csv_xz)
    pil = next(r for r in rows if r.name == "PILMAIQUEN")
    assert pil.vmin == pytest.approx(0.0)
    assert pil.vmax == pytest.approx(289680.30)
    assert pil.fact_rendim == pytest.approx(0.905)


def test_load_embalses_csv_missing(tmp_path: Path) -> None:
    """Missing path raises ``FileNotFoundError`` (after compressed probe)."""
    with pytest.raises(FileNotFoundError):
        load_embalses_csv(tmp_path / "nope.csv")


def test_load_embalses_csv_skips_short_rows(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Short rows are skipped with a warning, the rest parse fine."""
    bad = (
        "#Numero, Nombre, Tipo, VolMin, VolMax, VolMinNECF, VolMaxNECF, "
        "FEscala, FactRendim\n"
        "   1,BADROW,E\n"  # too few fields
        "   2,GOODROW,E,0.0,100.0,0.0,100.0,8,1.5,\n"
    )
    p = tmp_path / "embalses.csv"
    p.write_text(bad, encoding="utf-8")
    with caplog.at_level(logging.WARNING, logger="plexos2gtopt._plp_patch"):
        rows = load_embalses_csv(p)
    assert len(rows) == 1
    assert rows[0].name == "GOODROW"
    assert any("short row" in r.getMessage() for r in caplog.records)


# ── find_compressed_path / auto_detect_embalses ───────────────────────────


def test_find_compressed_path_xz_sibling(embalses_csv_xz: Path) -> None:
    """``find_compressed_path`` finds the ``.xz`` sibling."""
    # Pass the plain name; the helper should resolve to the .xz form.
    plain = embalses_csv_xz.with_name("embalses.csv")
    assert not plain.exists()
    resolved = find_compressed_path(plain)
    assert resolved == embalses_csv_xz


def test_auto_detect_embalses_in_bundle_parent(
    tmp_path: Path, embalses_csv: Path
) -> None:
    """Co-located embalses.csv (in the bundle's parent dir) is found."""
    bundle_source = embalses_csv.parent / "PLEXOS20260422.zip"
    # bundle_source need not exist — we only use its .parent.
    found = auto_detect_embalses(bundle_source)
    assert found == embalses_csv


def test_auto_detect_embalses_none_when_absent(tmp_path: Path) -> None:
    """No embalses.csv anywhere → returns None silently."""
    bundle_source = tmp_path / "PLEXOS20260422.zip"
    assert auto_detect_embalses(bundle_source) is None


def test_auto_detect_embalses_none_when_bundle_source_none() -> None:
    """``None`` bundle source ⇒ ``None`` return (no probing)."""
    assert auto_detect_embalses(None) is None


# ── patch_case_with_plp_embalses ──────────────────────────────────────────


def test_patch_adds_missing_reservoir_and_junction(
    embalses_csv: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Missing PILMAIQUEN gets synthesised from the PLP source."""
    case = _make_case_with_missing_pilmaiquen()
    assert len(case.reservoirs) == 2
    with caplog.at_level(logging.INFO, logger="plexos2gtopt._plp_patch"):
        patched = patch_case_with_plp_embalses(case, embalses_csv)
    assert len(patched.reservoirs) == 3
    pil = next(r for r in patched.reservoirs if r.name == "PILMAIQUEN")
    # Source-of-truth check: every patched field matches embalses.csv.
    assert pil.emin == pytest.approx(0.0)
    assert pil.emax == pytest.approx(289680.30)
    assert pil.eini == pytest.approx(289680.30)
    assert pil.efin == pytest.approx(0.0)
    # Synthesised reservoirs get negative object_ids to keep them
    # separated from real PLEXOS-side ids.
    assert pil.object_id < 0
    # The matching Junction is created too (writer wires
    # Reservoir.junction = Reservoir.name).
    assert any(j.name == "PILMAIQUEN" for j in patched.junctions)
    # INFO log surfaces the patch.
    msgs = [r.getMessage() for r in caplog.records]
    assert any("PILMAIQUEN" in m and "synthesized" in m for m in msgs)


def test_patch_noop_when_no_boundary_cut(embalses_csv: Path) -> None:
    """No boundary cut ⇒ nothing to patch ⇒ case returned unchanged."""
    bundle = BundleSpec(bundle_date="20260422")
    case = PlexosCase(
        bundle=bundle,
        reservoirs=(ReservoirSpec(object_id=1, name="L_Maule", emax=100.0),),
    )
    out = patch_case_with_plp_embalses(case, embalses_csv)
    assert out is case  # identity (no-op)


def test_patch_noop_when_no_missing(embalses_csv: Path) -> None:
    """Boundary cut references only known reservoirs ⇒ no-op."""
    bundle = BundleSpec(bundle_date="20260422")
    case = PlexosCase(
        bundle=bundle,
        reservoirs=(ReservoirSpec(object_id=1, name="L_Maule", emax=100.0),),
        boundary_cut=BoundaryCutSpec(fcf=1.0, slopes={"L_Maule": 9.0}),
    )
    out = patch_case_with_plp_embalses(case, embalses_csv)
    assert out is case


def test_patch_warns_when_name_not_in_plp(
    embalses_csv: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Slope name absent from embalses.csv ⇒ WARN, no synth."""
    bundle = BundleSpec(bundle_date="20260422")
    case = PlexosCase(
        bundle=bundle,
        reservoirs=(),
        boundary_cut=BoundaryCutSpec(fcf=1.0, slopes={"UNKNOWN_NAME": 5.0}),
    )
    with caplog.at_level(logging.WARNING, logger="plexos2gtopt._plp_patch"):
        out = patch_case_with_plp_embalses(case, embalses_csv)
    assert len(out.reservoirs) == 0
    assert any(
        "UNKNOWN_NAME" in r.getMessage() and r.levelno == logging.WARNING
        for r in caplog.records
    )


def test_patch_skips_orphan_reservoir_no_existing_turbine(
    embalses_csv: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Strict policy: PLP knows the reservoir but no Turbine references
    it ⇒ skip with WARN (would otherwise inflate boundary_cuts.csv
    with a slope that gates nothing).  Real-bundle case: PILMAIQUEN
    has no Storage / Waterway / Turbine in the PLEXOS XML."""
    case = _make_case_with_missing_pilmaiquen(with_pilmaiquen_chain=False)
    with caplog.at_level(logging.WARNING, logger="plexos2gtopt._plp_patch"):
        out = patch_case_with_plp_embalses(case, embalses_csv)
    # Orphan was skipped; reservoir count stays at 2.
    assert len(out.reservoirs) == 2
    assert not any(r.name == "PILMAIQUEN" for r in out.reservoirs)
    # WARN log surfaces the skip with the reason.
    msgs = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert any("PILMAIQUEN" in m and "no existing Turbine" in m for m in msgs), msgs


# ── CLI gating ────────────────────────────────────────────────────────────


def test_cli_flags_registered() -> None:
    """``--plp-embalses`` and ``--no-plp-embalses`` parse without error."""
    from plexos2gtopt.main import make_parser

    parser = make_parser()

    args = parser.parse_args(["--plp-embalses", "/tmp/embalses.csv.xz", "bundle.zip"])
    assert args.plp_embalses == Path("/tmp/embalses.csv.xz")
    assert args.no_plp_embalses is False

    args = parser.parse_args(["--no-plp-embalses", "bundle.zip"])
    assert args.no_plp_embalses is True
    assert args.plp_embalses is None

    # Defaults: no flag set.
    args = parser.parse_args(["bundle.zip"])
    assert args.plp_embalses is None
    assert args.no_plp_embalses is False


def test_cli_flags_mutually_exclusive() -> None:
    """Passing both --plp-embalses and --no-plp-embalses errors out."""
    from plexos2gtopt.main import make_parser

    parser = make_parser()
    with pytest.raises(SystemExit):
        parser.parse_args(
            [
                "--plp-embalses",
                "/tmp/x.csv",
                "--no-plp-embalses",
                "bundle.zip",
            ]
        )


def test_maybe_patch_short_circuits_on_plexos_legacy(
    embalses_csv: Path,
) -> None:
    """``--plexos-legacy`` always disables the patch."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    options = {
        "plexos_legacy": True,
        "plp_embalses": embalses_csv,  # explicit path, but legacy wins
    }
    out = _maybe_patch_with_plp_embalses(case, options, embalses_csv.parent)
    # Identity: legacy short-circuits before any patching.
    assert out is case
    assert len(out.reservoirs) == 2


def test_maybe_patch_short_circuits_on_no_plp_embalses(
    embalses_csv: Path,
) -> None:
    """``--no-plp-embalses`` disables auto-detect AND explicit-path
    patching alike."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    options = {
        "no_plp_embalses": True,
        "plp_embalses": embalses_csv,  # ignored
    }
    out = _maybe_patch_with_plp_embalses(case, options, embalses_csv.parent)
    assert out is case
    assert len(out.reservoirs) == 2


def test_maybe_patch_auto_detect_picks_up_colocated(
    embalses_csv: Path,
) -> None:
    """Auto-detect finds embalses.csv next to the bundle source."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    options: dict = {}
    # ``input_path`` is the bundle source; embalses_csv lives in the
    # same directory.
    fake_bundle = embalses_csv.parent / "PLEXOS20260422.zip"
    out = _maybe_patch_with_plp_embalses(case, options, fake_bundle)
    assert len(out.reservoirs) == 3
    assert any(r.name == "PILMAIQUEN" for r in out.reservoirs)


def test_maybe_patch_explicit_path_takes_precedence(
    embalses_csv_xz: Path,
) -> None:
    """Explicit ``--plp-embalses`` overrides auto-detect."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    options = {"plp_embalses": embalses_csv_xz}
    # input_path points to a directory with NO embalses.csv, but the
    # explicit option still wires the patch.
    out = _maybe_patch_with_plp_embalses(case, options, Path("/nonexistent/bundle.zip"))
    assert any(r.name == "PILMAIQUEN" for r in out.reservoirs)


def test_maybe_patch_explicit_path_missing_raises(tmp_path: Path) -> None:
    """An explicit path that doesn't exist raises ``FileNotFoundError``."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    options = {"plp_embalses": tmp_path / "missing.csv"}
    with pytest.raises(FileNotFoundError):
        _maybe_patch_with_plp_embalses(case, options, tmp_path)


def test_maybe_patch_no_args_no_files_noop(tmp_path: Path) -> None:
    """Defaults + no colocated CSV ⇒ silent no-op (legacy WARN path)."""
    from plexos2gtopt.plexos2gtopt import _maybe_patch_with_plp_embalses

    case = _make_case_with_missing_pilmaiquen()
    out = _maybe_patch_with_plp_embalses(case, {}, tmp_path / "bundle.zip")
    # No patch happened; the boundary cut still has 3 slopes but only
    # 2 reservoirs in the case (writer will WARN about PILMAIQUEN).
    assert out is case
    assert len(out.reservoirs) == 2


# ── boundary_cuts.csv emission after patch ────────────────────────────────


def test_boundary_cuts_emits_patched_reservoir(
    embalses_csv: Path, tmp_path: Path
) -> None:
    """End-to-end: after patching, the writer emits the new slope row."""
    import csv as _csv

    from plexos2gtopt.gtopt_writer import write_boundary_cut_csv

    case = _make_case_with_missing_pilmaiquen()
    patched = patch_case_with_plp_embalses(case, embalses_csv)
    reservoir_names = frozenset(r.name for r in patched.reservoirs)
    assert patched.boundary_cut is not None
    out_name = write_boundary_cut_csv(patched.boundary_cut, reservoir_names, tmp_path)
    assert out_name == "boundary_cuts.csv"
    with (tmp_path / "boundary_cuts.csv").open(encoding="utf-8") as fh:
        rows = list(_csv.reader(fh))
    # Header + one data row, header carries all THREE slopes now.
    assert len(rows) == 2
    assert "PILMAIQUEN" in rows[0]
    # PILMAIQUEN coefficient = -576.479.
    pil_idx = rows[0].index("PILMAIQUEN")
    assert float(rows[1][pil_idx]) == pytest.approx(-576.479)
