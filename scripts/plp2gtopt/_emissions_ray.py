# SPDX-License-Identifier: BSD-3-Clause
"""Build a synthetic emissions ray (Benders-cut equivalent) for the
``--only-emissions`` LP horizon (issue #520).

In pure-emissions mode the dollar-denominated ``boundary_cuts.csv``
(cost-mode FCF) is dropped because the LP objective is in tCO2eq, not
$.  Without a terminal value the LP would drain every reservoir down
to ``emin`` under a finite horizon.

This module synthesises a SINGLE-ROW emissions ray in the same
``boundary_cuts.csv`` schema gtopt's cut machinery already loads.
Per-reservoir slopes are computed as:

    slope_r  =  EPF[r]            [MW / (m³/s)]
              · gas_em             [tCO2eq / MWh, default 0.5]
              · loss_mult          [dimensionless, default 0.95]
              · hours_per_year     [8760 h/yr]
              · NPV_factor         [yr, = 1 / (1 − 1/(1+r))   for perpetuity]

For a 5 % annual discount rate and an infinite horizon (perpetuity)
the NPV factor → 21.0 yr.  L_Maule (EPF=9.34) thus gets:

    slope = 9.34 · 0.5 · 0.95 · 8760 · 21.0
          ≈ 815 470 tCO2eq per (m³/s)-cumec held back forever

The cut row is then ``rhs = 0`` (intercept absorbed into the linear
form) with ``slope_r`` on each reservoir column.  gtopt loads it via
the existing ``cut_directory`` + ``boundary_cuts_file`` mechanism.

Caller responsibility (plp2gtopt's writer pipeline):

  1. Build the EPF map via ``gtopt_shared.hydro_epf.epf_per_reservoir``.
  2. Call :func:`build_emissions_ray` → ``(rhs, slopes_dict)``.
  3. Write the CSV via :func:`planos_writer.write_boundary_cuts_csv`
     or just emit a tiny CSV directly with this module's helper.
  4. Stamp ``planning['options']['boundary_cuts_file'] =
     "boundary_cuts.csv"`` so gtopt picks it up.

Conventions match commit 7b36f3728 (EPF-based reservoir terminal
value already stamped as ``Reservoir.water_emission_value``); this
module just expresses the same per-reservoir tCO2/(m³/s)·h figure as a
single linear cut over the horizon-end storage state.
"""

from __future__ import annotations

import csv
import logging
import math
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Defaults — kept in sync with gtopt_shared.hydro_epf module
# ---------------------------------------------------------------------------

_DEFAULT_GAS_EM_TCO2_PER_MWH: float = 0.5
"""Long-term marginal-replacement emission rate (gas) for Chile."""

_DEFAULT_LOSS_MULT: float = 0.95
"""Efficiency / loss multiplier applied to the gas-equivalent rate."""

_HOURS_PER_YEAR: float = 8760.0
"""Hours in a year."""

_DEFAULT_DISCOUNT_RATE: float = 0.05
"""Annual discount rate (CNE reference for hydro least-cost dispatch)."""


def npv_factor(
    discount_rate: float = _DEFAULT_DISCOUNT_RATE,
    horizon_years: float | None = None,
) -> float:
    """Return the multi-year NPV multiplier for a unit annual cash flow.

    For a finite horizon ``N`` years (an annuity):
        NPV = Σ_{t=1..N} 1/(1+r)^t  =  (1 − (1+r)^-N) / r

    For an infinite horizon (perpetuity, ``horizon_years=None``):
        NPV → 1 / r   for r > 0

    Returns ``1.0`` when ``discount_rate <= 0`` and horizon is finite
    (the LP gets a "single-period" terminal value with no discounting).
    """
    if discount_rate <= 0.0:
        return float(horizon_years) if horizon_years else 1.0
    if horizon_years is None or horizon_years <= 0:
        return 1.0 / discount_rate
    return (1.0 - math.pow(1.0 + discount_rate, -horizon_years)) / discount_rate


def build_emissions_ray(
    epf_by_reservoir: dict[str, float],
    *,
    gas_em: float = _DEFAULT_GAS_EM_TCO2_PER_MWH,
    loss_mult: float = _DEFAULT_LOSS_MULT,
    discount_rate: float = _DEFAULT_DISCOUNT_RATE,
    horizon_years: float | None = None,
) -> tuple[float, dict[str, float]]:
    """Return ``(rhs, slopes)`` for the synthetic emissions ray.

    Parameters
    ----------
    epf_by_reservoir
        ``{name: EPF}`` map in MW per (m³/s), produced by
        ``gtopt_shared.hydro_epf.epf_per_reservoir``.
    gas_em
        Replacement emission rate in tCO2eq per MWh (default 0.5).
    loss_mult
        Round-trip / loss multiplier (default 0.95).
    discount_rate
        Annual discount rate (default 0.05).
    horizon_years
        Horizon in years; ``None`` = perpetuity (default).

    Returns
    -------
    (rhs, slopes)
        ``rhs`` = 0.0 (linear-in-storage cut).  ``slopes`` is
        ``{reservoir_name: slope}`` in tCO2eq per (m³/s)-cumec held
        for the full horizon.  Reservoirs with EPF=0 (terminal hydros
        with no downstream turbines) are omitted from ``slopes``.
    """
    npv = npv_factor(discount_rate=discount_rate, horizon_years=horizon_years)
    annual_em = gas_em * loss_mult * _HOURS_PER_YEAR  # tCO2eq / (m³/s) / yr
    rhs = 0.0  # linear in storage — intercept absorbed
    slopes: dict[str, float] = {}
    for name, epf in epf_by_reservoir.items():
        epf_f = float(epf or 0.0)
        if epf_f <= 0.0:
            continue
        slopes[name] = epf_f * annual_em * npv
    return rhs, slopes


def write_emissions_ray_csv(
    path: Path | str,
    epf_by_reservoir: dict[str, float],
    *,
    reservoir_order: list[str] | None = None,
    gas_em: float = _DEFAULT_GAS_EM_TCO2_PER_MWH,
    loss_mult: float = _DEFAULT_LOSS_MULT,
    discount_rate: float = _DEFAULT_DISCOUNT_RATE,
    horizon_years: float | None = None,
) -> Path:
    """Compute + write a single-row emissions-ray CSV.

    Schema matches gtopt's ``boundary_cuts.csv``:

        iteration,scene,rhs,Reservoir1,Reservoir2,...
        1,0,<rhs>,<slope1>,<slope2>,...

    Reservoirs absent from ``epf_by_reservoir`` (or with EPF=0) get
    zero slopes — gtopt treats those terminal volumes as unpriced.

    Parameters
    ----------
    path
        Output CSV path.
    epf_by_reservoir
        EPF map from ``gtopt_shared.hydro_epf.epf_per_reservoir``.
    reservoir_order
        Optional explicit column order.  Defaults to sorted EPF keys.
    gas_em, loss_mult, discount_rate, horizon_years
        See :func:`build_emissions_ray`.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    rhs, slopes = build_emissions_ray(
        epf_by_reservoir,
        gas_em=gas_em,
        loss_mult=loss_mult,
        discount_rate=discount_rate,
        horizon_years=horizon_years,
    )
    if reservoir_order is None:
        reservoir_order = sorted(epf_by_reservoir)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["iteration", "scene", "rhs", *reservoir_order])
        w.writerow([1, 0, rhs, *[slopes.get(r, 0.0) for r in reservoir_order]])
    logger.info(
        "synthetic emissions ray written: %s (%d reservoirs, NPV=%.2f)",
        path,
        sum(1 for r in reservoir_order if slopes.get(r, 0.0) > 0),
        npv_factor(discount_rate=discount_rate, horizon_years=horizon_years),
    )
    return path


def stamp_boundary_cuts_file_ref(
    planning: dict[str, Any],
    csv_relative_path: str = "boundary_cuts.csv",
) -> None:
    """Set ``planning['options']['boundary_cuts_file']`` so gtopt picks
    up the synthetic ray at solve time.

    Idempotent.  Does not overwrite a user-set value.
    """
    opts = planning.setdefault("options", {})
    if "boundary_cuts_file" not in opts:
        opts["boundary_cuts_file"] = csv_relative_path


__all__ = [
    "build_emissions_ray",
    "write_emissions_ray_csv",
    "stamp_boundary_cuts_file_ref",
    "npv_factor",
]
