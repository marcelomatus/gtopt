# SPDX-License-Identifier: BSD-3-Clause
"""Compose declared MC (USD/MWh) and emission factor (kg CO₂/MWh)
per unit per day from CEN data.

Master plan §4.11.1 driver #2 (gas inflexible) + §9.4 normalisation
+ §4.12 (marginal emission intensity).

Input
-----
* ``costo-combustible`` (SIP API) — fuel cost per central per day
  in USD per fuel-unit.
  Columns: ``date_utc, central_name, configuracion, empresa,
  tipo_combustible, costo_combustible``.

* ``topologia/v3/fuelcons`` (operación API) — heat rate per
  generation unit in fuel-units per MWh.
  Columns: ``unit_id, plant_name, fuel, heat_rate, fuel_unit, …``.

* (optional) ``infotecnica/v1/centrales`` — catalogue with
  ``id, nombre, nemotecnico, central_tipo_nombre, propietario_nombre``.

Output
------
DataFrame with columns:
  ``date_utc, configuracion, central_name, plant_name, fuel,
  fuel_unit, heat_rate, fuel_cost, declared_MC,
  emission_factor_kg_per_unit, emission_factor_kg_per_mwh``

Where:
  ``declared_MC                = heat_rate × fuel_cost``       (USD/MWh)
  ``emission_factor_kg_per_mwh = heat_rate × ef_per_fuel_unit`` (kg CO₂/MWh)

Fuel CO₂ emission factors
-------------------------
Per-fuel CO₂ emission factors below are from IPCC 2006 Guidelines
for National Greenhouse Gas Inventories, Volume 2 (Energy), table 1.4
(default emission factors for stationary combustion), normalised to
kg CO₂ per fuel-unit (ton, m³ or MMBTU as documented in the SIP
costo-combustible response).

Biomasa is treated as biogenic-zero per the IPCC convention used
by Chile's CNE for SEN reporting (the carbon released equals the
carbon absorbed during plant growth).

Sources:
  * IPCC 2006 GL Vol. 2 Ch. 1 Tab. 1.4 (default CO₂ EFs).
  * EPA AP-42 Section 1.1 (bituminous coal), 1.3 (fuel oil), 1.4 (gas).
  * ENGIE Chile 2023 emission inventory (validates Carbón ~2.5 t/t,
    Diésel ~3.2 t/t for the SEN's specific fuel grades).

Join logic
----------
The natural join key between ``costo-combustible`` and
``topologia/v3/fuelcons`` is the **plant name**, normalised
(NFD-fold + ASCII-fold + lower + strip-non-alnum). Rows that fail
to join are reported in ``unmatched`` for diagnostics.
"""

from __future__ import annotations

import logging
import unicodedata
from dataclasses import dataclass

import pandas as pd


_LOG = logging.getLogger("cen2gtopt.mc")


# ----------------------------------------------------------------------
# CO₂ emission factors per fuel-unit (kg CO₂ / fuel-unit).
#
# Keys are NFD-folded + lowered + alnum-only (matching ``_norm``) so the
# fuel string from the API ("Diésel", "Carbón", "Gas Natural") maps
# robustly. Fuel-unit alignment with the API: SIP's
# ``costo-combustible.unidad_de_combustible`` is implicit per fuel
# (ton for solids/heavy fuels, m³ for diesel, m³ for natural gas).
# Operación's ``topologia/v3/fuelcons.unidad_de_combustible`` is the
# canonical match.
#
# Values:
#  * Diésel        ~3170 kg CO₂/ton   (IPCC default 2 925 + Chilean
#                                       grade adjustment per ENGIE 2023)
#  * Carbón        ~2530 kg CO₂/ton   (sub-bituminous default; SEN coal
#                                       is mostly imported sub-bit/bit)
#  * Gas Natural   ~2.0 kg CO₂/m³     (IPCC default at 0°C; converted to
#                                       per-Sm³ as published)
#  * Fuel Oil Nro. 6  ~3120 kg CO₂/ton   (IPCC default residual fuel oil)
#  * Biomasa       0   kg CO₂/ton    (biogenic — IPCC Vol. 2 §2.3.1)
#  * GLP / LPG     ~3000 kg CO₂/ton   (IPCC default propane/butane mix)
#  * Otro          NaN — caller must supply a custom factor or accept
#                  NaN emission_rate for that row
# ----------------------------------------------------------------------
_FUEL_EMISSION_FACTORS_KG_PER_UNIT: dict[str, float] = {
    "diesel": 3170.0,  # ton (Diésel)
    "carbon": 2530.0,  # ton (Carbón — sub-bituminous default)
    "gasnatural": 2.0,  # Sm³
    "fueloilnro6": 3120.0,  # ton
    "fueloil": 3120.0,  # ton (synonym)
    "biomasa": 0.0,  # ton — biogenic
    "glp": 3000.0,  # ton — LPG default
    "kerosene": 2530.0,  # ton — proxy
    # Otros / unknown → no entry; caller gets NaN for those rows.
}


def _emission_factor_for_fuel(fuel: object) -> float:
    """Return kg CO₂ per fuel-unit for the given fuel name, or NaN
    when the fuel is unknown / "Otro"."""
    key = _norm(fuel)
    return _FUEL_EMISSION_FACTORS_KG_PER_UNIT.get(key, float("nan"))


def _norm(s: object) -> str:
    """NFD-fold + ASCII-fold + lower + strip-non-alnum."""
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return ""
    nfd = unicodedata.normalize("NFD", str(s))
    no_marks = "".join(c for c in nfd if not unicodedata.combining(c))
    ascii_only = no_marks.encode("ascii", errors="ignore").decode()
    return "".join(ch for ch in ascii_only.lower() if ch.isalnum())


@dataclass(slots=True)
class CompositionResult:
    """Output of :func:`compose_declared_mc`."""

    declared_mc: pd.DataFrame  # the joined frame
    unmatched_costos: pd.DataFrame  # costo-combustible rows with no fuelcons match
    unmatched_fuelcons: pd.DataFrame  # fuelcons rows with no costo match
    n_match: int
    n_total_costos: int
    n_total_fuelcons: int


def compose_declared_mc(
    costos: pd.DataFrame,
    fuelcons: pd.DataFrame,
) -> CompositionResult:
    """Compose ``declared_MC = heat_rate × fuel_cost`` per unit per day.

    Args:
        costos: long-form costo-combustible frame from
            ``cen2gtopt._sip_client.fetch_costo_combustible``. Must
            have at least ``date_utc, central_name, configuracion,
            costo_combustible``.
        fuelcons: long-form fuelcons frame from
            ``cen2gtopt._operacion_client.fetch_topologia_fuelcons``
            (typically concatenated across multiple plants). Must
            have at least ``plant_name, fuel, heat_rate, fuel_unit``.

    Returns:
        :class:`CompositionResult` with the joined table plus
        diagnostic frames for unmatched rows.
    """
    if costos.empty:
        _LOG.warning("compose_declared_mc: costos is empty")
        return CompositionResult(
            declared_mc=pd.DataFrame(),
            unmatched_costos=pd.DataFrame(),
            unmatched_fuelcons=fuelcons.copy(),
            n_match=0,
            n_total_costos=0,
            n_total_fuelcons=len(fuelcons),
        )
    if fuelcons.empty:
        _LOG.warning("compose_declared_mc: fuelcons is empty")
        return CompositionResult(
            declared_mc=pd.DataFrame(),
            unmatched_costos=costos.copy(),
            unmatched_fuelcons=pd.DataFrame(),
            n_match=0,
            n_total_costos=len(costos),
            n_total_fuelcons=0,
        )

    # Build normalised match keys on both sides.
    costos = costos.copy()
    fuelcons = fuelcons.copy()
    # On the costos side, try `central_name`, fallback to `configuracion`.
    costos["_match_central"] = costos["central_name"].map(_norm)
    costos["_match_config"] = costos["configuracion"].map(_norm)
    fuelcons["_match_plant"] = fuelcons["plant_name"].map(_norm)

    # Primary attempt: central_name ↔ plant_name.
    primary = costos.merge(
        fuelcons,
        left_on="_match_central",
        right_on="_match_plant",
        how="inner",
        suffixes=("_costo", "_fuel"),
    )

    # Fallback: configuracion ↔ plant_name (catches some tag-style names).
    matched_keys = set(primary["_match_central"])
    leftover_costos = costos[~costos["_match_central"].isin(matched_keys)]
    fallback = leftover_costos.merge(
        fuelcons,
        left_on="_match_config",
        right_on="_match_plant",
        how="inner",
        suffixes=("_costo", "_fuel"),
    )

    joined = pd.concat([primary, fallback], ignore_index=True)
    if joined.empty:
        return CompositionResult(
            declared_mc=joined,
            unmatched_costos=costos.copy(),
            unmatched_fuelcons=fuelcons.copy(),
            n_match=0,
            n_total_costos=len(costos),
            n_total_fuelcons=len(fuelcons),
        )

    # Compose declared_MC and emission_factor_kg_per_mwh.
    heat_rate_num = pd.to_numeric(joined["heat_rate"], errors="coerce")
    fuel_cost_num = pd.to_numeric(joined["costo_combustible"], errors="coerce")
    joined["declared_MC"] = heat_rate_num * fuel_cost_num

    # IPCC-standard emission intensity per MWh:
    #   ef_per_mwh = heat_rate × ef_per_fuel_unit
    # The fuel-side EF is looked up via _emission_factor_for_fuel
    # (NFD-fold + alnum-only) so "Diésel"/"Carbón"/"Gas Natural"
    # match the catalogue regardless of accent / case.
    ef_per_unit = joined["fuel"].map(_emission_factor_for_fuel)
    joined["emission_factor_kg_per_unit"] = ef_per_unit
    joined["emission_factor_kg_per_mwh"] = heat_rate_num * ef_per_unit

    # Project to a clean schema.
    keep = [
        "date_utc",
        "configuracion",
        "central_name",
        "plant_name",
        "fuel",
        "fuel_unit",
        "heat_rate",
        "costo_combustible",
        "declared_MC",
        "emission_factor_kg_per_unit",
        "emission_factor_kg_per_mwh",
    ]
    keep = [c for c in keep if c in joined.columns]
    joined = joined[keep].rename(columns={"costo_combustible": "fuel_cost"})

    # Compute unmatched diagnostic frames.
    matched_central_keys = set(primary["_match_central"]) | set(
        fallback["_match_central"]
    )
    unmatched_costos = costos[~costos["_match_central"].isin(matched_central_keys)]

    matched_plants = set(joined["plant_name"])
    unmatched_fuelcons = fuelcons[~fuelcons["plant_name"].isin(matched_plants)]

    return CompositionResult(
        declared_mc=joined.reset_index(drop=True),
        unmatched_costos=unmatched_costos.reset_index(drop=True).drop(
            columns=[
                c
                for c in ("_match_central", "_match_config")
                if c in unmatched_costos.columns
            ]
        ),
        unmatched_fuelcons=unmatched_fuelcons.reset_index(drop=True).drop(
            columns=[c for c in ("_match_plant",) if c in unmatched_fuelcons.columns]
        ),
        n_match=len(joined),
        n_total_costos=len(costos),
        n_total_fuelcons=len(fuelcons),
    )


__all__ = ["CompositionResult", "compose_declared_mc"]
