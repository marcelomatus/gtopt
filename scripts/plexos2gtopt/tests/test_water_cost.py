"""Water-cost (future-cost FCF') integration in compare_with_plexos.

The compare tool reports an FCF' $ + Operational+FCF' $ line, computed on a
basis common to gtopt and PLEXOS.  The boundary cut is the single cost-to-go
``alpha = rhs + Σ slope·vol_end`` (slope negative).  Rebasing it to a brim-full
reservoir with ``c = rhs + Σ slope·emax`` yields

    FCF' = alpha - c = Σ_r |slope_r| · (emax_r - efin_r)   >= 0

— the future cost of the water not yet stored up to full.  ``c`` is identical
on both sides (same cut, same emax) so it cancels in the comparison; only each
side's own ``efin`` differs.  These tests pin the helpers behind that line.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.compare_with_plexos import (
    _read_boundary_cut_coeffs,
    _read_reservoir_emax,
    _reservoir_water_cost,
)


def test_reservoir_water_cost_emax_referenced() -> None:
    """FCF' = Σ |coef|·(emax − efin), always >= 0 (rebased to brim-full)."""
    coef = {"ELTORO": 10.0, "RALCO": 5.0}
    emax = {"ELTORO": 100.0, "RALCO": 70.0}
    vols = {
        "ELTORO": {"eini": 50.0, "efin": 80.0},  # (100-80)*10 = 200
        "RALCO": {"eini": 50.0, "efin": 60.0},  # (70-60)*5  =  50
        "NOTINCUT": {"eini": 99.0, "efin": 0.0},  # not in coef -> skipped
    }
    assert _reservoir_water_cost(vols, coef, emax) == 250.0  # 200 + 50


def test_reservoir_water_cost_nonnegative_even_on_refill() -> None:
    """A refill above eini still yields a positive FCF' (emax reference)."""
    coef = {"R": 4.0}
    emax = {"R": 200.0}
    vols = {"R": {"eini": 10.0, "efin": 150.0}}  # refill, yet (200-150)*4 = 200
    assert _reservoir_water_cost(vols, coef, emax) == 200.0


def test_reservoir_water_cost_missing_emax_skips() -> None:
    """A reservoir absent from emax is skipped (graceful, no crash)."""
    coef = {"A": 1.0, "B": 2.0}
    emax = {"A": 10.0}  # B has no emax
    vols = {"A": {"efin": 4.0}, "B": {"efin": 1.0}}
    assert _reservoir_water_cost(vols, coef, emax) == 6.0  # only A: (10-4)*1


def test_reservoir_water_cost_empty_coef() -> None:
    """No boundary coeffs -> zero FCF' (graceful, no crash)."""
    assert _reservoir_water_cost({"X": {"efin": 1.0}}, {}, {"X": 5.0}) == 0.0


def test_read_boundary_cut_coeffs(tmp_path: Path) -> None:
    """scene/rhs columns dropped; reservoir coeffs returned as |value|."""
    (tmp_path / "boundary_cuts.csv").write_text(
        "scene,rhs,ELTORO,RALCO\n0,1234.5,-10.0,5.0\n"
    )
    # First candidate is None (skipped), second is the dir holding the file.
    assert _read_boundary_cut_coeffs(None, tmp_path) == {
        "ELTORO": 10.0,
        "RALCO": 5.0,
    }


def test_read_boundary_cut_coeffs_missing(tmp_path: Path) -> None:
    """Absent boundary_cuts.csv -> empty dict (water line is then skipped)."""
    assert not _read_boundary_cut_coeffs(tmp_path)


def test_read_reservoir_emax_scalar_and_profile(tmp_path: Path) -> None:
    """emax read from the bundle JSON; scalar kept, per-block profile MAX'd."""
    import json

    bundle = {
        "system": {
            "reservoir_array": [
                {"name": "ELTORO", "emax": 100.0},  # scalar
                {"name": "CANUT", "emax": [[5.0, 8.0, 6.0]]},  # profile -> max 8
                {"name": "NOEMAX", "eini": 1.0},  # no emax -> skipped
            ]
        }
    }
    (tmp_path / "case.json").write_text(json.dumps(bundle))
    assert _read_reservoir_emax(tmp_path) == {"ELTORO": 100.0, "CANUT": 8.0}


def test_read_reservoir_emax_skips_planning_and_provenance(tmp_path: Path) -> None:
    """planning.json / *.provenance.json are not PLEXOS-source bundles."""
    import json

    bundle = {"system": {"reservoir_array": [{"name": "R", "emax": 9.0}]}}
    (tmp_path / "planning.json").write_text(json.dumps(bundle))
    (tmp_path / "x.provenance.json").write_text(json.dumps(bundle))
    assert _read_reservoir_emax(tmp_path) == {}
    # A direct .json file path is read as-is.
    f = tmp_path / "case.json"
    f.write_text(json.dumps(bundle))
    assert _read_reservoir_emax(f) == {"R": 9.0}


def test_read_reservoir_emax_missing(tmp_path: Path) -> None:
    """No bundle JSON with a reservoir_array -> empty dict."""
    assert _read_reservoir_emax(tmp_path) == {}


_RENDER_REQUIRED = {
    "block_count": 1.0,
    "hours_covered": 24.0,
    "load_mwh": 0.0,
    "gen_mwh": 0.0,
}


def test_render_solution_compare_includes_fcf_rows() -> None:
    """With water_cost_usd stashed, the cost table gains FCF' $ +
    Operational+FCF' $ rows."""
    from io import StringIO

    from rich.console import Console

    from plexos2gtopt.compare_with_plexos import _render_solution_compare

    p = {**_RENDER_REQUIRED, "gen_cost_srmc_usd": 1000.0, "water_cost_usd": 200.0}
    g = {**_RENDER_REQUIRED, "op_cost_usd": 900.0, "water_cost_usd": 150.0}
    buf = StringIO()
    _render_solution_compare(p, g, Console(file=buf, width=200))
    out = buf.getvalue()
    assert "FCF'" in out
    assert "Operational + FCF'" in out


def test_render_solution_compare_omits_fcf_without_coeffs() -> None:
    """No water_cost_usd (no boundary_cuts.csv) -> no FCF' rows, base table
    still renders."""
    from io import StringIO

    from rich.console import Console

    from plexos2gtopt.compare_with_plexos import _render_solution_compare

    buf = StringIO()
    _render_solution_compare(
        {**_RENDER_REQUIRED, "gen_cost_srmc_usd": 1000.0},
        {**_RENDER_REQUIRED, "op_cost_usd": 900.0},
        Console(file=buf, width=200),
    )
    out = buf.getvalue()
    assert "FCF'" not in out
    assert "Operational + FCF'" not in out
    assert "TOTAL Operational $" in out
