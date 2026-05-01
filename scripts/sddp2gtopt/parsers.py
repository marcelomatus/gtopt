"""Per-collection parsers — turn raw ``psrclasses.json`` entities
into the typed specs declared in :mod:`sddp2gtopt.entities`.

Each ``parse_*`` is a pure function that takes a
:class:`PsrClassesLoader` and returns a list (or scalar) of specs.
They deliberately mirror plp2gtopt's parser-per-file convention so
the test layout (``test_parser_<entity>.py``) can be lifted across.
"""

from __future__ import annotations

import logging
from typing import Any

from .entities import (
    DemandSpec,
    FuelSpec,
    GaugingStationSpec,
    HydroSpec,
    StudySpec,
    SystemSpec,
    ThermalSpec,
)
from .psrclasses_loader import PsrClassesLoader


logger = logging.getLogger(__name__)


def _scalar(val: Any, default: float = 0.0) -> float:
    """Coerce a PSR scalar (often wrapped in a one-element list)."""
    if isinstance(val, list):
        if not val:
            return default
        head = val[0]
        return float(head) if isinstance(head, (int, float)) else default
    if isinstance(val, (int, float)):
        return float(val)
    return default


def _int(val: Any, default: int = 0) -> int:
    """Coerce a PSR scalar to ``int``."""
    if isinstance(val, list):
        if not val:
            return default
        head = val[0]
        return int(head) if isinstance(head, (int, float)) else default
    if isinstance(val, (int, float)):
        return int(val)
    return default


def parse_study(loader: PsrClassesLoader) -> StudySpec:
    """Extract :class:`StudySpec` from the (single) ``PSRStudy``.

    Falls back to the dataclass defaults when the entity is missing,
    so downstream parsers can still run on partial cases.
    """
    study = loader.first("PSRStudy") or {}
    return StudySpec(
        initial_year=_int(study.get("Ano_inicial"), 2000),
        initial_stage=_int(study.get("Etapa_inicial"), 1),
        stage_type=_int(study.get("Tipo_Etapa"), 2),
        num_stages=_int(study.get("NumeroEtapas"), 1),
        num_systems=_int(study.get("NumeroSistemas"), 1),
        num_blocks=_int(study.get("NumeroBlocosDemanda"), 1),
        num_series_forward=_int(study.get("Series_Forward"), 1),
        deficit_cost=_scalar(study.get("DeficitCost"), 1000.0),
        discount_rate=_scalar(study.get("TaxaDesconto"), 0.0),
        currency=str(study.get("CurrencyReference") or "$"),
    )


def parse_systems(loader: PsrClassesLoader) -> list[SystemSpec]:
    """One :class:`SystemSpec` per ``PSRSystem`` entity."""
    out: list[SystemSpec] = []
    for ent in loader.entities("PSRSystem"):
        out.append(
            SystemSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=_int(ent.get("reference_id")),
                currency=str(ent.get("UnM", "$")),
            )
        )
    return out


def parse_fuels(loader: PsrClassesLoader) -> list[FuelSpec]:
    """One :class:`FuelSpec` per ``PSRFuel`` entity."""
    out: list[FuelSpec] = []
    for ent in loader.entities("PSRFuel"):
        out.append(
            FuelSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=_int(ent.get("reference_id")),
                cost=_scalar(ent.get("Custo")),
                unit=str(ent.get("UE", "MWh")),
                co2=_scalar(ent.get("EmiCO2")),
                system_ref=ent.get("system"),
            )
        )
    return out


def parse_thermal_plants(loader: PsrClassesLoader) -> list[ThermalSpec]:
    """One :class:`ThermalSpec` per ``PSRThermalPlant`` entity.

    The fuel cost is resolved via the entity's ``fuels`` reference
    list (an array of PSRFuel ``reference_id``) so the writer can
    produce a piecewise gcost when needed. PSR encodes up to three
    cost segments per plant (``G(i)`` + ``CEsp(i,1)``); we collect
    only the non-zero ones.
    """
    fuel_cost = {f.reference_id: f.cost for f in parse_fuels(loader)}
    out: list[ThermalSpec] = []
    for ent in loader.entities("PSRThermalPlant"):
        fuel_refs = [int(r) for r in ent.get("fuels", []) if isinstance(r, int)]
        primary = fuel_cost.get(fuel_refs[0], 0.0) if fuel_refs else 0.0
        segs: list[tuple[float, float]] = []
        for idx in (1, 2, 3):
            cap = _scalar(ent.get(f"G({idx})"))
            cesp = _scalar(ent.get(f"CEsp({idx},1)"))
            if cap > 0 and cesp > 0:
                segs.append((cap, cesp * primary))
        out.append(
            ThermalSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=_int(ent.get("reference_id")),
                pmin=_scalar(ent.get("GerMin")),
                pmax=_scalar(ent.get("GerMax")),
                transport_cost=_scalar(ent.get("CTransp")),
                fuel_refs=fuel_refs,
                g_segments=segs,
                system_ref=ent.get("system"),
            )
        )
    return out


def parse_hydro_plants(loader: PsrClassesLoader) -> list[HydroSpec]:
    """One :class:`HydroSpec` per ``PSRHydroPlant`` entity."""
    out: list[HydroSpec] = []
    for ent in loader.entities("PSRHydroPlant"):
        out.append(
            HydroSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=_int(ent.get("reference_id")),
                p_inst=_scalar(ent.get("PotInst")),
                vmin=_scalar(ent.get("Vmin")),
                vmax=_scalar(ent.get("Vmax")),
                vinic=_scalar(ent.get("Vinic")),
                qmin=_scalar(ent.get("Qmin")),
                qmax=_scalar(ent.get("Qmax")),
                fp_med=_scalar(ent.get("FPMed")),
                station_ref=ent.get("station"),
                system_ref=ent.get("system"),
            )
        )
    return out


def parse_demands(loader: PsrClassesLoader) -> list[DemandSpec]:
    """One :class:`DemandSpec` per ``PSRDemand``.

    ``profile`` is sourced from the matching ``PSRDemandSegment``
    (linked via the ``demand`` reference), specifically the
    ``Demanda(1)`` time-series.
    """
    segments_by_demand: dict[int, dict[str, Any]] = {}
    for seg in loader.entities("PSRDemandSegment"):
        ref = seg.get("demand")
        if isinstance(ref, int):
            segments_by_demand[ref] = seg
    out: list[DemandSpec] = []
    for ent in loader.entities("PSRDemand"):
        ref = _int(ent.get("reference_id"))
        seg = segments_by_demand.get(ref, {})
        profile_raw = seg.get("Demanda(1)") or []
        profile = [float(x) for x in profile_raw if isinstance(x, (int, float))]
        out.append(
            DemandSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=ref,
                duracao_pct=_scalar(ent.get("Duracao(1)"), 100.0),
                system_ref=ent.get("system"),
                profile=profile,
            )
        )
    return out


def parse_gauging_stations(loader: PsrClassesLoader) -> list[GaugingStationSpec]:
    """One :class:`GaugingStationSpec` per ``PSRGaugingStation``."""
    out: list[GaugingStationSpec] = []
    for ent in loader.entities("PSRGaugingStation"):
        vazao_raw = ent.get("Vazao") or []
        vazao = [float(x) for x in vazao_raw if isinstance(x, (int, float))]
        out.append(
            GaugingStationSpec(
                code=_int(ent.get("code")),
                name=str(ent.get("name", "")),
                reference_id=_int(ent.get("reference_id")),
                vazao=vazao,
            )
        )
    return out
