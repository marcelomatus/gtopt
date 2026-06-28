"""Typed dataclasses for parsed SDDP entities.

Each parser in :mod:`sddp2gtopt.parsers` returns one of these. They
deliberately stay close to the PSR semantics (units, code/uid scheme)
so the writer can perform the gtopt translation in one place rather
than having unit conversions scattered across parsers.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class StudySpec:
    """Top-level study parameters, extracted from ``PSRStudy``.

    Attributes:
        initial_year: Calendar year of stage 1 (``Ano_inicial``).
        initial_stage: 1-based index inside the year of the first stage
            (``Etapa_inicial``).
        stage_type: PSR stage type code: 1 = weekly, 2 = monthly,
            3 = trimester, hourly = ``Tipo_Etapa = 4`` in newer cases.
        num_stages: Total number of stages (``NumeroEtapas``).
        num_systems: Number of PSR systems present
            (``NumeroSistemas``).
        num_blocks: Number of demand blocks per stage
            (``NumeroBlocosDemanda``).
        num_series_forward: Forward Monte-Carlo series
            (``Series_Forward``).
        deficit_cost: First-segment deficit cost in k$/MWh (or
            currency / MWh — depends on study) — ``DeficitCost[0]``.
        discount_rate: Annual discount rate as a fraction
            (``TaxaDesconto``).
        currency: ISO-ish currency tag (``CurrencyReference``).
    """

    initial_year: int = 2000
    initial_stage: int = 1
    stage_type: int = 2
    num_stages: int = 1
    num_systems: int = 1
    num_blocks: int = 1
    num_series_forward: int = 1
    deficit_cost: float = 1000.0
    discount_rate: float = 0.0
    currency: str = "$"


@dataclass
class SystemSpec:
    """A power-system area (``PSRSystem``)."""

    code: int
    name: str
    reference_id: int
    currency: str = "$"


@dataclass
class FuelSpec:
    """A fuel record (``PSRFuel``).

    ``cost`` is the first entry of the ``Custo`` vector in fuel-unit
    money per fuel-unit (typically ``$/MWh``).
    """

    code: int
    name: str
    reference_id: int
    cost: float = 0.0
    unit: str = "MWh"
    co2: float = 0.0
    system_ref: int | None = None


@dataclass
class ThermalSpec:
    """A thermal plant (``PSRThermalPlant``).

    ``g_segments`` lists ``(capacity_mw, gen_cost_per_mwh)`` tuples.
    For case0 every plant has a single segment so the list usually has
    length 1, but the writer still emits piecewise gcost when there
    are multiple.
    """

    code: int
    name: str
    reference_id: int
    pmin: float = 0.0
    pmax: float = 0.0
    transport_cost: float = 0.0
    fuel_refs: list[int] = field(default_factory=list)
    g_segments: list[tuple[float, float]] = field(default_factory=list)
    system_ref: int | None = None
    bus_number: int | None = None  # PSR network bus (multi-bus .dat path)
    is_import: bool = False  # backed by an interconnection import fuel (MEX/IMP)


@dataclass
class HydroSpec:
    """A hydro plant (``PSRHydroPlant``).

    Volumes are in ``hm³``; flows in ``m³/s``; ``fp_med`` is the
    average production factor (``MW`` per ``m³/s``).
    """

    code: int
    name: str
    reference_id: int
    p_inst: float = 0.0
    vmin: float = 0.0
    vmax: float = 0.0
    vinic: float = 0.0
    qmin: float = 0.0
    qmax: float = 0.0
    fp_med: float = 0.0
    station_ref: int | None = None
    system_ref: int | None = None
    bus_number: int | None = None  # PSR network bus (multi-bus .dat path)
    gcost: float = 0.0  # water-value opportunity cost [$/MWh] (0 = free)


@dataclass
class DemandSpec:
    """A consumer (``PSRDemand``).

    ``profile`` is the per-stage series (GWh/stage, json path).
    ``block_values`` is an optional explicit per-block MW series for a
    single-stage model (used by the PSR ``.dat`` NCP path, whose demand
    file is hourly over the dispatch horizon); when present the writer
    emits it directly as ``lmax[0]`` instead of normalising ``profile``.
    """

    code: int
    name: str
    reference_id: int
    duracao_pct: float = 100.0
    system_ref: int | None = None
    profile: list[float] = field(default_factory=list)
    block_values: list[float] = field(default_factory=list)
    bus_number: int | None = None  # PSR network bus (multi-bus .dat path)


@dataclass
class GaugingStationSpec:
    """A gauging station + AR-P inflow series (``PSRGaugingStation``).

    ``vazao`` is the flat historical inflow series; the original PSR
    layout is ``[year * 12 + month]`` so ``len(vazao)`` is a multiple
    of 12 in well-formed cases.
    """

    code: int
    name: str
    reference_id: int
    vazao: list[float] = field(default_factory=list)


@dataclass
class BusSpec:
    """A network bus (``dbus.dat``) for the multi-bus DC OPF path.

    ``base_kv`` is taken from the bus-name voltage suffix (``AGU-230`` →
    230); used to convert ohm reactances to per-unit.
    """

    number: int
    name: str
    base_kv: float = 0.0
    area: str = ""


@dataclass
class CircuitSpec:
    """A transmission circuit (line / transformer) from ``dcirc.dat``.

    ``reactance_pu`` is already normalised to per-unit on a 100 MVA base
    by the parser/loader; ``rating`` is the MVA/MW flow cap.
    """

    from_bus: int
    to_bus: int
    name: str = ""
    resistance: float = 0.0
    reactance_pu: float = 0.0
    rating: float = 0.0
