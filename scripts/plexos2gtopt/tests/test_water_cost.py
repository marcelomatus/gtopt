"""Water-cost integration in compare_with_plexos.

The compare tool now reports a Water $ + Operational+Water $ line, computed on
a basis common to gtopt and PLEXOS (PLEXOS exposes only net storage):

    water_$ = Σ_r (eini_r - efin_r) · |boundary_coef_r|     # SIGNED

A draw-down is a positive cost; a refill is a negative cost (a credit for
water banked at its FCF value).  These tests pin the two helpers behind that
line.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.compare_with_plexos import (
    _read_boundary_cut_coeffs,
    _reservoir_water_cost,
)


def test_reservoir_water_cost_signed_net_change() -> None:
    """SIGNED net change over boundary-cut reservoirs: draw-down is a cost,
    a refill is a credit (the stored-water FCF value must not be clamped)."""
    coef = {"ELTORO": 10.0, "RALCO": 5.0}
    vols = {
        "ELTORO": {"eini": 100.0, "efin": 80.0},  # drawdown 20 -> +200
        "RALCO": {"eini": 50.0, "efin": 60.0},  # refill 10 -> -50 (credit)
        "NOTINCUT": {"eini": 99.0, "efin": 0.0},  # not in coef -> skipped
    }
    assert _reservoir_water_cost(vols, coef) == 150.0  # 200 - 50


def test_reservoir_water_cost_empty_coef() -> None:
    """No boundary coeffs -> zero water cost (graceful, no crash)."""
    assert _reservoir_water_cost({"X": {"eini": 5.0, "efin": 1.0}}, {}) == 0.0


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


_RENDER_REQUIRED = {
    "block_count": 1.0,
    "hours_covered": 24.0,
    "load_mwh": 0.0,
    "gen_mwh": 0.0,
}


def test_render_solution_compare_includes_water_rows() -> None:
    """With water_cost_usd stashed, the cost table gains Water $ +
    Operational+Water $ rows."""
    from io import StringIO

    from rich.console import Console

    from plexos2gtopt.compare_with_plexos import _render_solution_compare

    p = {**_RENDER_REQUIRED, "gen_cost_srmc_usd": 1000.0, "water_cost_usd": 200.0}
    g = {**_RENDER_REQUIRED, "op_cost_usd": 900.0, "water_cost_usd": 150.0}
    buf = StringIO()
    _render_solution_compare(p, g, Console(file=buf, width=200))
    out = buf.getvalue()
    assert "Water $" in out
    assert "Operational + Water $" in out


def test_render_solution_compare_omits_water_without_coeffs() -> None:
    """No water_cost_usd (no boundary_cuts.csv) -> no water rows, base table
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
    assert "Water $" not in out
    assert "Operational + Water $" not in out
    assert "TOTAL Operational $" in out
