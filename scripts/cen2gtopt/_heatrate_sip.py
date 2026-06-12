# SPDX-License-Identifier: BSD-3-Clause
"""Inline heat-rate-curve parser for the SIP ``unidades_generadoras`` feed.

The SIP exposes a free-text ``curva_consumo`` field per unit. For ~99 %
of thermals the field is a placeholder (``N/A``, ``Pendiente hasta PES``,
or a CEN ``DEnnnn-yy`` document reference); for a small but growing
minority it carries a structured 2-3 point curve, e.g.::

    9583,8 Kj/kWh a 150MW; 9631,53 Kj/kWh a 112,5MW; 9776,25 Kj/kWh a 90MW

This module extracts the (P, HR) tuples, fits the customary
``fuel(P) = a + b·P`` linear consumption model (one straight line in
GJ/h vs MW, equivalent to the affine heat-rate ``HR(P) = a/P + b``),
and returns a long-form parquet-friendly table keyed by ``id_unidad``.

Coverage today is small (≈1 unit / 140 thermals) but the loader picks
up new rows automatically as CEN populates more entries — no schema
change needed.

Pipeline integration is opt-in: callers fetch the
``unidades_generadoras__v4__findByDate`` parquet and call
:func:`build_heatrate_table` to obtain a dataframe that can be joined
on ``id_unidad`` to refine the fuel-flat ε(P) into a per-dispatch ε(P).

The parser is **strict on units**: it accepts the kJ/kWh and kcal/kWh
glyphs that appear in the SIP today and silently ignores BTU/kWh and
GJ/MWh (none observed in the feed as of 2026-05). That is intentional
— the upstream feed is small and hand-curated, and a silent unit
conversion would be more dangerous than an audit miss.

Refs:
  - SIP unidades-generadoras endpoint: ``/unidades-generadoras/v4``
  - White-paper §discussion (heat-rate availability gap)
"""

from __future__ import annotations

import re
from dataclasses import dataclass

import numpy as np
import pandas as pd

KJ_PER_KCAL = 4.184  # IEC standard
_NUMERIC_TOKEN = re.compile(
    r"(?P<hr>\d{3,5}(?:[,.]\d+)?)\s*"
    r"(?P<unit>kj|kcal)\s*/\s*kwh"
    r"\s*(?:a|@|at)\s*"
    r"(?P<p>\d{1,4}(?:[,.]\d+)?)\s*mw",
    re.IGNORECASE,
)


def _to_float_es(token: str) -> float:
    """Spanish-locale-aware float parse — accepts comma decimals."""
    return float(token.replace(",", "."))


@dataclass(slots=True, frozen=True)
class HeatRatePoint:
    """One operating-point sample from a SIP ``curva_consumo`` string."""

    power_mw: float
    hr_kj_per_kwh: float

    @property
    def fuel_gj_per_h(self) -> float:
        """Fuel-input rate at this operating point, in GJ/h."""
        return self.power_mw * self.hr_kj_per_kwh / 1000.0


@dataclass(slots=True, frozen=True)
class HeatRateFit:
    """Linear fuel-consumption model ``fuel(P) = a + b·P`` (GJ/h vs MW).

    The ``a`` intercept captures the no-load (idle) heat consumption;
    the ``b`` slope is the incremental heat rate. Both are positive
    for any physically valid thermal unit.
    """

    a_gj_per_h: float  # no-load fuel rate [GJ/h]
    b_gj_per_mwh: float  # incremental heat rate [GJ/MWh]
    rsq: float  # coefficient of determination
    n_points: int  # number of (P, HR) samples used

    def hr_kj_per_kwh(self, power_mw: float) -> float:
        """Predicted heat rate at ``power_mw``, in kJ/kWh."""
        if power_mw <= 0.0:
            msg = f"power_mw must be positive, got {power_mw}"
            raise ValueError(msg)
        return 1000.0 * (self.a_gj_per_h / power_mw + self.b_gj_per_mwh)


def parse_curva_consumo(text: str | None) -> list[HeatRatePoint]:
    """Extract ``(P, HR)`` tuples from a SIP ``curva_consumo`` string.

    Returns an empty list for placeholder values (``N/A``, doc refs,
    blank). De-duplicates samples that share the same ``power_mw``,
    keeping the first occurrence.
    """
    if not text:
        return []
    points: list[HeatRatePoint] = []
    seen: set[float] = set()
    for match in _NUMERIC_TOKEN.finditer(text):
        hr = _to_float_es(match.group("hr"))
        if match.group("unit").lower() == "kcal":
            hr *= KJ_PER_KCAL
        p = _to_float_es(match.group("p"))
        if p <= 0.0 or hr <= 0.0:
            continue
        if p in seen:
            continue
        seen.add(p)
        points.append(HeatRatePoint(power_mw=p, hr_kj_per_kwh=hr))
    points.sort(key=lambda pt: pt.power_mw)
    return points


def fit_linear_fuel(points: list[HeatRatePoint]) -> HeatRateFit | None:
    """Least-squares fit of ``fuel(P) = a + b·P`` to ``points``.

    Returns ``None`` if fewer than 2 points are supplied or the fit
    yields a non-physical (negative-slope or negative-intercept)
    result. Single-point inputs cannot identify both ``a`` and ``b``;
    callers needing a 1-point estimate should assume ``a=0`` and
    ``b = HR(P)/1000`` themselves.
    """
    if len(points) < 2:
        return None
    p_arr = np.array([pt.power_mw for pt in points], dtype=float)
    fuel_arr = np.array([pt.fuel_gj_per_h for pt in points], dtype=float)
    # Closed-form OLS for y = a + b*x, more readable than np.linalg.lstsq
    p_mean = p_arr.mean()
    f_mean = fuel_arr.mean()
    cov = np.sum((p_arr - p_mean) * (fuel_arr - f_mean))
    var = np.sum((p_arr - p_mean) ** 2)
    if var <= 0.0:
        return None
    b = cov / var
    a = f_mean - b * p_mean
    if a < 0.0 or b <= 0.0:
        return None
    pred = a + b * p_arr
    ss_res = np.sum((fuel_arr - pred) ** 2)
    ss_tot = np.sum((fuel_arr - f_mean) ** 2)
    rsq = 1.0 - (ss_res / ss_tot) if ss_tot > 0.0 else 1.0
    return HeatRateFit(
        a_gj_per_h=float(a),
        b_gj_per_mwh=float(b),
        rsq=float(rsq),
        n_points=len(points),
    )


def build_heatrate_table(units_df: pd.DataFrame) -> pd.DataFrame:
    """Long-form ``(id_unidad, power_mw, hr_kj_per_kwh)`` table.

    Input is the SIP ``unidades_generadoras`` parquet (must carry
    ``id_unidad``, ``unidad_nombre``, ``curva_consumo``). Output one
    row per ``(id_unidad, P)`` operating point; the table is empty
    if no unit has a structured curve.
    """
    required = {"id_unidad", "unidad_nombre", "curva_consumo"}
    missing = required - set(units_df.columns)
    if missing:
        msg = f"units_df missing required columns: {sorted(missing)}"
        raise ValueError(msg)
    units_df = units_df.drop_duplicates(subset=["id_unidad"], keep="first")
    rows: list[dict[str, object]] = []
    for _, unit_row in units_df.iterrows():
        for pt in parse_curva_consumo(unit_row.get("curva_consumo")):
            rows.append(
                {
                    "id_unidad": int(unit_row["id_unidad"]),
                    "unidad_nombre": str(unit_row["unidad_nombre"]),
                    "power_mw": pt.power_mw,
                    "hr_kj_per_kwh": pt.hr_kj_per_kwh,
                    "fuel_gj_per_h": pt.fuel_gj_per_h,
                }
            )
    return pd.DataFrame(
        rows,
        columns=[
            "id_unidad",
            "unidad_nombre",
            "power_mw",
            "hr_kj_per_kwh",
            "fuel_gj_per_h",
        ],
    )


def build_heatrate_fits(units_df: pd.DataFrame) -> pd.DataFrame:
    """Per-unit linear-fit table from the SIP ``unidades_generadoras`` parquet.

    Only units with **≥ 2** operating points and a physically valid
    fit (``a ≥ 0``, ``b > 0``) are returned. The table is the natural
    join key for the marginal-emission pipeline: callers replace the
    fuel-flat ``ε`` with ``ε(P) = (a/P + b) · EF_fuel`` for any unit
    that appears here, and fall back to fuel-flat for the rest.
    """
    required = {"id_unidad", "unidad_nombre", "curva_consumo"}
    missing = required - set(units_df.columns)
    if missing:
        msg = f"units_df missing required columns: {sorted(missing)}"
        raise ValueError(msg)
    units_df = units_df.drop_duplicates(subset=["id_unidad"], keep="first")
    rows: list[dict[str, object]] = []
    for _, unit_row in units_df.iterrows():
        points = parse_curva_consumo(unit_row.get("curva_consumo"))
        fit = fit_linear_fuel(points)
        if fit is None:
            continue
        rows.append(
            {
                "id_unidad": int(unit_row["id_unidad"]),
                "unidad_nombre": str(unit_row["unidad_nombre"]),
                "a_gj_per_h": fit.a_gj_per_h,
                "b_gj_per_mwh": fit.b_gj_per_mwh,
                "rsq": fit.rsq,
                "n_points": fit.n_points,
                "p_min_mw": min(pt.power_mw for pt in points),
                "p_max_mw": max(pt.power_mw for pt in points),
            }
        )
    return pd.DataFrame(
        rows,
        columns=[
            "id_unidad",
            "unidad_nombre",
            "a_gj_per_h",
            "b_gj_per_mwh",
            "rsq",
            "n_points",
            "p_min_mw",
            "p_max_mw",
        ],
    )
