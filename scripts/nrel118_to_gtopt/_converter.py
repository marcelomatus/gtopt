"""Core conversion logic for the NREL-118 → gtopt port.

The NREL-118 test system (Pena, Martinez-Anido, Hodge 2017) ships per-
generator data in `Generators.csv` with a five-band incremental heat-rate
model:

    HR_Base [MMBTU/hr]      — no-load fuel consumption
    HR_Inc_Band_k [BTU/kWh] — incremental fuel rate over MW band k
    Load_Point_Band_k [MW]  — cumulative MW breakpoint at the top of band k
    Min Stable Level [MW]   — pmin

gtopt's `expand_fuel_emission_sources` (see `source/system.cpp`) requires a
SCALAR heat_rate; piecewise specifications are skipped with a WARN.  We
collapse to a single pmax-weighted segment-average heat_rate at converter
time, which captures the average operating fuel intensity at full load.

The formula at output = pmax is:

    fuel_consumed [MMBTU/hr] = HR_Base
                             + Σ_k HR_Inc[k] [BTU/kWh] × band_width_k [MW]
                                                       × 1e-3 [MMBTU/MWh ÷ BTU/kWh]

    heat_rate [MMBTU/MWh]   = fuel_consumed / pmax

    heat_rate [GJ/MWh]      = heat_rate [MMBTU/MWh] × 1.055
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# 1 MMBTU = 1.055 GJ; 1 BTU/kWh = 1.055e-3 MMBTU/MWh × 1.055 GJ/MMBTU
MMBTU_PER_GJ = 1.055
BTU_PER_KWH_TO_GJ_PER_MWH = MMBTU_PER_GJ * 1.0e-3  # 0.001055 GJ/MWh per BTU/kWh

# IPCC AR6 WG3 Table A.III.2 combustion emission factors (tCO2/GJ).
# Renewable / nuclear carriers are explicit zeros so the converter can
# emit a Fuel + EmissionFactor row without per-MWh CO2 contribution.
IPCC_AR6_TCO2_PER_GJ: dict[str, float] = {
    "coal": 0.0946,  # subcritical bituminous
    "natural_gas": 0.0561,  # CT/CC/ICE common
    "diesel": 0.0741,  # gas/diesel oil
    "biomass": 0.0,  # biogenic-zero per IPCC AFOLU
    "nuclear": 0.0,
    "geothermal": 0.0,
    "hydro": 0.0,
    "wind": 0.0,
    "solar": 0.0,
    "other": 0.0741,  # conservative — treat unknown thermal as diesel-class
}


# Map the leading word(s) of a NREL-118 generator name to a fuel kind.
# The CSV row's first column carries human-readable names like
# "Biomass 01", "CC NG 02", "CT Oil 03" — sufficient to fingerprint the
# fuel without a separate carrier table.
def unit_type_to_fuel(name: str) -> str:
    """Return one of the keys of `IPCC_AR6_TCO2_PER_GJ`."""

    lower = name.lower()
    if lower.startswith("biomass"):
        return "biomass"
    if lower.startswith("hydro"):
        return "hydro"
    if lower.startswith("wind"):
        return "wind"
    if lower.startswith("solar"):
        return "solar"
    if lower.startswith("geo"):
        return "geothermal"
    if "nuc" in lower:
        return "nuclear"
    if "coal" in lower:
        return "coal"
    if "oil" in lower or "diesel" in lower:
        return "diesel"
    if " ng " in lower or lower.endswith(" ng") or "natural gas" in lower:
        return "natural_gas"
    return "other"


@dataclass
class NrelGen:
    """Parsed row from NREL-118 ``Generators.csv``."""

    name: str
    bus: str
    pmax: float
    pmin: float
    vom: float  # $/MWh
    fuel: str
    heat_rate_gj_per_mwh: float = 0.0  # 0 ⇒ renewable / no fuel
    is_renewable: bool = False  # wind/solar/hydro/geo


@dataclass
class Conversion:
    """End-to-end conversion artefacts (used by both the CLI and tests)."""

    week: int
    n_hours: int
    bus_count: int
    gen_count: int
    line_count: int
    total_load_mw: list[float]  # length = n_hours
    fuels: list[dict[str, Any]] = field(default_factory=list)
    generators: list[NrelGen] = field(default_factory=list)
    renewables_profile: dict[str, list[float]] = field(default_factory=dict)


def collapse_piecewise_heat_rate(
    hr_base_mmbtu_per_hr: float,
    hr_inc_bands_btu_per_kwh: list[float],
    load_points_mw: list[float],
    pmin: float,
    pmax: float,
) -> float:
    """Reduce NREL-118 piecewise HR to a SCALAR heat_rate at pmax.

    Returns the pmax-weighted segment-average heat_rate in **GJ/MWh**.
    Renewable / hydro rows ship empty HR columns; callers must filter
    those out *before* invoking this function (the renewable check is
    NOT this function's responsibility).

    The bands form a cumulative segment chain starting at ``pmin``:
    segment k covers ``[load_points[k-1], load_points[k]]`` with
    ``load_points[0] = pmin``.  Incremental band rates apply to the
    extra MW added in that segment relative to the previous breakpoint.
    """

    if pmax <= 0.0:
        return 0.0

    # Total fuel consumed at full load (MMBTU/hr).  Start with the no-
    # load contribution; this is the "Heat Rate Base" PLEXOS column.
    fuel_mmbtu_per_hr = hr_base_mmbtu_per_hr

    prev_breakpoint = pmin if pmin > 0.0 else 0.0
    for band_idx, breakpoint_mw in enumerate(load_points_mw):
        if breakpoint_mw <= prev_breakpoint:
            continue  # empty / missing band
        band_width = breakpoint_mw - prev_breakpoint
        if band_idx >= len(hr_inc_bands_btu_per_kwh):
            break
        inc_btu_per_kwh = hr_inc_bands_btu_per_kwh[band_idx]
        # BTU/kWh × MW = BTU/kWh × 1000 kW = 1000 BTU/h.
        # Convert to MMBTU/hr by dividing 1e6: ÷ 1e3.
        fuel_mmbtu_per_hr += inc_btu_per_kwh * band_width * 1.0e-3
        prev_breakpoint = breakpoint_mw

    heat_rate_mmbtu_per_mwh = fuel_mmbtu_per_hr / pmax
    return heat_rate_mmbtu_per_mwh * MMBTU_PER_GJ


def _float_or(value: str, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _parse_generator_row(
    header_index: dict[str, int],
    row: list[str],
) -> NrelGen | None:
    """Parse one NREL-118 ``Generators.csv`` row to a ``NrelGen`` instance."""

    name = row[header_index["Generator Name"]].strip()
    if not name:
        return None
    bus = row[header_index["bus of connection"]].strip()
    pmax = _float_or(row[header_index["Max Capacity (MW)"]])
    if pmax <= 0.0:
        return None  # offline / placeholder rows
    pmin = _float_or(row[header_index["Min Stable Level (MW)"]])
    vom = _float_or(row[header_index["VO&M Charge ($/MWh)"]])

    fuel_kind = unit_type_to_fuel(name)
    is_renewable = fuel_kind in {"wind", "solar", "hydro", "geothermal"}

    # Collapse piecewise HR for fueled units; renewables stay 0.
    heat_rate_gj_per_mwh = 0.0
    if not is_renewable and fuel_kind != "nuclear":
        hr_base = _float_or(row[header_index["Heat Rate Base (MMBTU/hr)"]])
        inc_bands = [
            _float_or(row[header_index[f"Heat Rate Inc Band {k} (BTU/kWh)"]])
            for k in range(1, 6)
        ]
        load_points = [
            _float_or(row[header_index[f"Load Point Band {k} (MW)"]])
            for k in range(1, 6)
        ]
        heat_rate_gj_per_mwh = collapse_piecewise_heat_rate(
            hr_base, inc_bands, load_points, pmin, pmax
        )

    return NrelGen(
        name=name,
        bus=bus,
        pmax=pmax,
        pmin=pmin,
        vom=vom,
        fuel=fuel_kind,
        heat_rate_gj_per_mwh=heat_rate_gj_per_mwh,
        is_renewable=is_renewable,
    )


def parse_generators(generators_csv: Path) -> list[NrelGen]:
    """Read all rows of NREL-118 ``Generators.csv`` and return parsed gens."""

    with generators_csv.open(newline="") as fp:
        reader = csv.reader(fp)
        header = next(reader)
        header_index = {col: idx for idx, col in enumerate(header)}
        gens: list[NrelGen] = []
        for row in reader:
            if not row or not row[0].strip():
                continue
            parsed = _parse_generator_row(header_index, row)
            if parsed is not None:
                gens.append(parsed)
    return gens


def parse_buses(buses_csv: Path) -> list[str]:
    """Return the ordered list of bus names from ``Buses.csv``."""

    with buses_csv.open(newline="") as fp:
        reader = csv.reader(fp)
        header = next(reader)
        try:
            bus_col = header.index("Bus Name")
        except ValueError:
            bus_col = 0
        return [row[bus_col].strip() for row in reader if row and row[bus_col].strip()]


def parse_load_series(load_csv: Path, week: int) -> list[float]:
    """Slice ``168 × 1 h`` hours starting at the chosen week from a load CSV.

    NREL-118's hourly load CSVs are MW values keyed by datetime; we ignore
    the date and just take a contiguous slice.  Week 1 starts at hour 0
    (Jan 1 00:00); week 2 starts at hour 168, etc.  Returns MW values.
    """

    hour0 = (week - 1) * 168
    hours = []
    with load_csv.open(newline="") as fp:
        reader = csv.reader(fp)
        next(reader)  # skip header
        for idx, row in enumerate(reader):
            if idx < hour0:
                continue
            if idx >= hour0 + 168:
                break
            hours.append(_float_or(row[1]))
    return hours


def _sum_load_files(cache_dir: Path, week: int) -> list[float]:
    """Sum the regional NREL-118 load files into a single system load slice."""

    totals: list[float] = []
    for region in ("R1", "R2", "R3"):
        path = cache_dir / f"Load{region}DA.csv"
        if not path.exists():
            continue
        region_load = parse_load_series(path, week)
        if not totals:
            totals = list(region_load)
        else:
            for idx, value in enumerate(region_load):
                if idx < len(totals):
                    totals[idx] += value
    return totals


def convert(cache_dir: Path, week: int = 2) -> Conversion:
    """High-level conversion: read CSVs, slice the chosen week, build state."""

    generators = parse_generators(cache_dir / "Generators.csv")
    buses = parse_buses(cache_dir / "Buses.csv")
    n_lines = _count_csv_rows(cache_dir / "Lines.csv")
    total_load_mw = _sum_load_files(cache_dir, week)
    n_hours = len(total_load_mw)

    fuels = []
    for fuel_kind, combustion in IPCC_AR6_TCO2_PER_GJ.items():
        if combustion == 0.0:
            # Renewables/nuclear: no fuel emission row is needed in the
            # gtopt JSON, but we keep them for downstream inspection.
            fuels.append({"name": fuel_kind, "combustion_tco2_per_gj": 0.0})
        else:
            fuels.append({"name": fuel_kind, "combustion_tco2_per_gj": combustion})

    return Conversion(
        week=week,
        n_hours=n_hours,
        bus_count=len(buses),
        gen_count=len(generators),
        line_count=n_lines,
        total_load_mw=total_load_mw,
        fuels=fuels,
        generators=generators,
    )


def _count_csv_rows(path: Path) -> int:
    if not path.exists():
        return 0
    with path.open(newline="") as fp:
        reader = csv.reader(fp)
        next(reader, None)
        return sum(1 for row in reader if row and row[0].strip())


def to_gtopt_json(
    conversion: Conversion, renewables_share: float = 0.0
) -> dict[str, Any]:
    """Materialise the gtopt JSON from a ``Conversion``.

    Single-bus aggregation; piecewise HR already collapsed to scalar.

    ``renewables_share`` ∈ [0, 1]:  if > 0, scales every non-renewable
    generator's `capacity` by `(1 − renewables_share)` and adds an
    aggregate solar+wind generator at capacity = total peak load × share.
    Mirrors the 0 % vs 33 % comparison in Peña, Martinez-Anido, Hodge 2017.
    """

    nh = conversion.n_hours
    bus = {"uid": 1, "name": "bus"}
    demand = {
        "uid": 1,
        "name": "load",
        "bus": 1,
        "capacity": max(conversion.total_load_mw) if conversion.total_load_mw else 1.0,
    }
    demand_profile = None
    if conversion.total_load_mw:
        peak = max(conversion.total_load_mw)
        if peak > 0.0:
            demand_profile = {
                "uid": 1,
                "name": "load_profile",
                "demand": 1,
                "profile": [[[v / peak for v in conversion.total_load_mw]]],
            }

    # CO2 pollutant + global zone.
    emissions = [{"uid": 1, "name": "co2"}]
    zones = [
        {
            "uid": 1,
            "name": "global_co2",
            "emissions": [{"emission": 1, "weight": 1.0}],
        }
    ]

    # Fuel records (one per nonzero IPCC factor used by some generator).
    used_fuels = {
        gen.fuel for gen in conversion.generators if gen.heat_rate_gj_per_mwh > 0
    }
    fuel_uid = {}
    fuels = []
    for idx, fuel_name in enumerate(sorted(used_fuels), start=1):
        fuel_uid[fuel_name] = idx
        fuels.append(
            {
                "uid": idx,
                "name": fuel_name,
                "price": 0.0,  # vom on generator already covers dispatch cost
                "emission_factors": [
                    {"emission": 1, "combustion": IPCC_AR6_TCO2_PER_GJ[fuel_name]}
                ],
            }
        )

    # Generators.  Apply the renewables_share derate to thermals and
    # synthesise a single aggregate renewable on the same bus.
    derate = 1.0 - renewables_share
    gens_json = []
    for idx, gen in enumerate(conversion.generators, start=1):
        cap = gen.pmax * derate if not gen.is_renewable else gen.pmax
        entry: dict[str, Any] = {
            "uid": idx,
            "name": gen.name,
            "bus": 1,
            "gcost": gen.vom,
            "capacity": cap,
        }
        if gen.heat_rate_gj_per_mwh > 0 and gen.fuel in fuel_uid:
            entry["fuel"] = fuel_uid[gen.fuel]
            entry["heat_rate"] = gen.heat_rate_gj_per_mwh
        gens_json.append(entry)

    if renewables_share > 0.0 and conversion.total_load_mw:
        peak_load = max(conversion.total_load_mw)
        gens_json.append(
            {
                "uid": len(gens_json) + 1,
                "name": "aggregate_renewables",
                "bus": 1,
                "gcost": 0.0,
                "capacity": peak_load * renewables_share,
            }
        )

    sim = {
        "block_array": [{"uid": h + 1, "duration": 1.0} for h in range(nh)],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": nh}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }
    system = {
        "name": f"NREL118Week{conversion.week}",
        "bus_array": [bus],
        "demand_array": [demand],
        "emission_array": emissions,
        "emission_zone_array": zones,
        "fuel_array": fuels,
        "generator_array": gens_json,
    }
    if demand_profile is not None:
        system["demand_profile_array"] = [demand_profile]

    return {"simulation": sim, "system": system}


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as fp:
        json.dump(payload, fp, indent=2)
