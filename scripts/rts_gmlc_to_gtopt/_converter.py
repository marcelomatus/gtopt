"""Core conversion logic for RTS-GMLC → gtopt.

RTS-GMLC ``RTS_Data/SourceData/gen.csv`` carries:

    PMax MW                 — pmax
    PMin MW                 — pmin
    HR_avg_0                — heat rate at minimum stable level (BTU/kWh)
    HR_incr_{1..4}          — incremental BTU/kWh on bands 1..4
    Output_pct_{0..4}       — cumulative MW fractions of pmax per band
    VOM                     — variable O&M ($/MWh)
    Fuel Price $/MMBTU      — fuel price (single value)
    Emissions CO2 Lbs/MMBTU — fuel-side CO2 per unit fuel energy

Unit conversion:

    1 lb = 0.4536 kg = 0.0004536 t
    1 MMBTU = 1.055 GJ

So the CSV "Emissions CO2 Lbs/MMBTU" value × 0.0004536 / 1.055 →
tCO2/GJ.  Quick sanity: subbituminous coal at 210 lbs/MMBTU →
210 × 0.0004536 / 1.055 ≈ 0.0903 tCO2/GJ — within ~5 % of IPCC's
~0.096 tCO2/GJ value for subbituminous coal.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Conversion factor LBS/MMBTU → tCO2/GJ.
LBS_PER_MMBTU_TO_TCO2_PER_GJ = 0.4536e-3 / 1.055  # ≈ 0.0004299

# Pollutants tracked by RTS-GMLC.  Order matters: index 0 = CO2 (the main
# target).  The converter emits one Emission per entry and a fuel-side
# combustion factor for each.
POLLUTANTS: tuple[tuple[str, str], ...] = (
    ("co2", "Emissions CO2 Lbs/MMBTU"),
    ("so2", "Emissions SO2 Lbs/MMBTU"),
    ("nox", "Emissions NOX Lbs/MMBTU"),
    ("ch4", "Emissions CH4 Lbs/MMBTU"),
    ("n2o", "Emissions N2O Lbs/MMBTU"),
    ("co", "Emissions CO Lbs/MMBTU"),
    ("vocs", "Emissions VOCs Lbs/MMBTU"),
)


@dataclass
class RtsGen:
    """Parsed row from RTS-GMLC ``gen.csv``."""

    name: str
    bus_id: int
    unit_type: str
    fuel_kind: str
    pmax: float
    pmin: float
    vom: float  # $/MWh
    fuel_price_per_mmbtu: float
    heat_rate_gj_per_mwh: float
    combustion_tco2_per_gj: float
    pollutant_combustion: dict[str, float] = field(default_factory=dict)
    is_renewable: bool = False


@dataclass
class Conversion:
    day: int
    n_hours: int
    bus_count: int
    gen_count: int
    line_count: int
    total_load_mw: list[float]
    generators: list[RtsGen] = field(default_factory=list)


def _float_or(value: str, default: float = 0.0) -> float:
    try:
        if value == "":
            return default
        return float(value)
    except (TypeError, ValueError):
        return default


def _collapse_heat_rate(
    hr_avg_0: float,
    hr_incr: list[float],
    output_pct: list[float],
    pmax: float,
) -> float:
    """Reduce RTS-GMLC piecewise HR to a SCALAR heat_rate at pmax.

    Returns GJ/MWh.  ``output_pct`` is cumulative fraction of pmax for
    the top of each band; ``hr_avg_0`` is the average HR at the first
    breakpoint (often = pmin), and ``hr_incr[k]`` is the incremental
    BTU/kWh added moving from band k-1 to band k.

    The pmax-weighted segment-average HR at full load is the energy-
    weighted sum of average HRs across all segments, divided by pmax.
    For the first segment we use ``hr_avg_0`` directly; for k ≥ 1 we
    accumulate the incremental contributions.
    """

    if pmax <= 0.0 or not output_pct:
        return 0.0

    # Cumulative fuel consumption (BTU/h) at the top of each band.
    # Segment 0: from 0 to output_pct[0]·pmax at average HR hr_avg_0.
    cum_fuel_btu_per_h = output_pct[0] * pmax * 1.0e3 * hr_avg_0
    prev_pct = output_pct[0]
    for k, incr in enumerate(hr_incr, start=1):
        if k >= len(output_pct):
            break
        delta_pct = output_pct[k] - prev_pct
        if delta_pct <= 0.0:
            continue
        cum_fuel_btu_per_h += delta_pct * pmax * 1.0e3 * incr
        prev_pct = output_pct[k]

    # Total fuel at full load (BTU/h) / pmax (MW = 1000 kW = 1000 kWh/h)
    # = BTU/kWh.  Then convert to GJ/MWh: BTU/kWh × 1.055e-3 = GJ/MWh.
    heat_rate_btu_per_kwh = cum_fuel_btu_per_h / (pmax * 1.0e3)
    return heat_rate_btu_per_kwh * 1.055e-3


_RENEWABLE_UNIT_TYPES = {
    "PV",
    "RTPV",
    "WIND",
    "HYDRO",
    "ROR",
    "CSP",
    "STORAGE",
    "SYNC_COND",
}


def _parse_gen_row(header_index: dict[str, int], row: list[str]) -> RtsGen | None:
    name = row[header_index["GEN UID"]].strip()
    if not name:
        return None
    unit_type = row[header_index["Unit Type"]].strip()
    fuel = row[header_index["Fuel"]].strip()
    pmax = _float_or(row[header_index["PMax MW"]])
    if pmax <= 0.0:
        return None
    pmin = _float_or(row[header_index["PMin MW"]])
    vom = _float_or(row[header_index["VOM"]])
    fuel_price = _float_or(row[header_index["Fuel Price $/MMBTU"]])
    bus_id = int(_float_or(row[header_index["Bus ID"]]))

    is_renewable = unit_type in _RENEWABLE_UNIT_TYPES or unit_type == "NUCLEAR"
    # Nuclear isn't "renewable" but has no combustion CO2 — group with
    # renewables for the heat-rate skip.

    heat_rate_gj_per_mwh = 0.0
    combustion_tco2_per_gj = 0.0
    pollutant_combustion: dict[str, float] = {}

    if unit_type not in _RENEWABLE_UNIT_TYPES:
        hr_avg_0 = _float_or(row[header_index["HR_avg_0"]])
        hr_incr = [_float_or(row[header_index[f"HR_incr_{k}"]]) for k in range(1, 5)]
        output_pct = [_float_or(row[header_index[f"Output_pct_{k}"]]) for k in range(5)]
        heat_rate_gj_per_mwh = _collapse_heat_rate(hr_avg_0, hr_incr, output_pct, pmax)
        co2_lbs_per_mmbtu = _float_or(row[header_index["Emissions CO2 Lbs/MMBTU"]])
        combustion_tco2_per_gj = co2_lbs_per_mmbtu * LBS_PER_MMBTU_TO_TCO2_PER_GJ
        for tag, col in POLLUTANTS:
            lbs_per_mmbtu = _float_or(row[header_index[col]])
            pollutant_combustion[tag] = lbs_per_mmbtu * LBS_PER_MMBTU_TO_TCO2_PER_GJ

    return RtsGen(
        name=name,
        bus_id=bus_id,
        unit_type=unit_type,
        fuel_kind=fuel,
        pmax=pmax,
        pmin=pmin,
        vom=vom,
        fuel_price_per_mmbtu=fuel_price,
        heat_rate_gj_per_mwh=heat_rate_gj_per_mwh,
        combustion_tco2_per_gj=combustion_tco2_per_gj,
        pollutant_combustion=pollutant_combustion,
        is_renewable=is_renewable,
    )


def parse_generators(gen_csv: Path) -> list[RtsGen]:
    with gen_csv.open(newline="") as fp:
        reader = csv.reader(fp)
        header = next(reader)
        header_index = {col: idx for idx, col in enumerate(header)}
        gens: list[RtsGen] = []
        for row in reader:
            if not row or not row[0].strip():
                continue
            parsed = _parse_gen_row(header_index, row)
            if parsed is not None:
                gens.append(parsed)
    return gens


def parse_buses(bus_csv: Path) -> list[int]:
    with bus_csv.open(newline="") as fp:
        reader = csv.reader(fp)
        header = next(reader)
        try:
            col = header.index("Bus ID")
        except ValueError:
            col = 0
        return [int(_float_or(row[col])) for row in reader if row and row[col].strip()]


def _count_csv_rows(path: Path) -> int:
    if not path.exists():
        return 0
    with path.open(newline="") as fp:
        reader = csv.reader(fp)
        next(reader, None)
        return sum(1 for row in reader if row and row[0].strip())


def synthetic_day_load(peak_mw: float, day: int) -> list[float]:
    """Generate a 24-hour synthetic daily load shape at the chosen peak.

    RTS-GMLC's full annual timeseries lives in 73 separate hourly CSVs;
    pulling them all is overkill for a 24-hour port test.  We use a
    canonical "tested" daily shape — morning ramp, noon dip, evening
    peak — scaled to ``peak_mw``.  The ``day`` parameter modulates the
    overall scale by 0.9..1.0 to give different days different totals.
    """

    pattern = [
        0.65,
        0.62,
        0.60,
        0.58,
        0.58,
        0.60,  # 0-6 night/dawn
        0.70,
        0.80,
        0.86,
        0.90,
        0.92,
        0.93,  # 6-12 morning ramp
        0.92,
        0.91,
        0.90,
        0.91,
        0.94,
        0.98,  # 12-18 afternoon
        1.00,
        0.97,
        0.92,
        0.85,
        0.78,
        0.71,  # 18-24 evening peak/falloff
    ]
    scale = peak_mw * (0.9 + 0.1 * ((day - 1) % 10) / 9.0)
    return [v * scale for v in pattern]


def convert(cache_dir: Path, day: int = 1) -> Conversion:
    generators = parse_generators(cache_dir / "gen.csv")
    buses = parse_buses(cache_dir / "bus.csv")
    n_lines = _count_csv_rows(cache_dir / "branch.csv")
    # Aggregate pmax of all thermals + renewables as the system peak ref.
    aggregate_peak = sum(g.pmax for g in generators)
    # RTS-GMLC actual peak load is ~2 850 MW; we want roughly 70% of
    # the aggregate capacity for realistic dispatch tightness.
    peak_load = min(aggregate_peak * 0.65, 8000.0)
    total_load_mw = synthetic_day_load(peak_load, day)

    return Conversion(
        day=day,
        n_hours=len(total_load_mw),
        bus_count=len(buses),
        gen_count=len(generators),
        line_count=n_lines,
        total_load_mw=total_load_mw,
        generators=generators,
    )


def to_gtopt_json(
    conversion: Conversion,
    include_pollutants: tuple[str, ...] = ("co2",),
) -> dict[str, Any]:
    """Materialise a single-bus 24-hour gtopt JSON with per-gen CO2 (+ optional
    multi-pollutant) accounting."""

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

    pollutant_uid: dict[str, int] = {}
    emissions = []
    zones = []
    for idx, tag in enumerate(include_pollutants, start=1):
        pollutant_uid[tag] = idx
        emissions.append({"uid": idx, "name": tag})
        zones.append(
            {
                "uid": idx,
                "name": f"global_{tag}",
                "emissions": [{"emission": idx, "weight": 1.0}],
            }
        )

    # One fuel per generator (per-gen rates differ).  Each Fuel carries
    # one emission_factor per included pollutant.
    fuels: list[dict[str, Any]] = []
    gens_json: list[dict[str, Any]] = []
    fuel_uid = 1
    for gen_idx, gen in enumerate(conversion.generators, start=1):
        entry: dict[str, Any] = {
            "uid": gen_idx,
            "name": gen.name,
            "bus": 1,
            "gcost": gen.vom
            + gen.heat_rate_gj_per_mwh * gen.fuel_price_per_mmbtu / 1.055,
            "capacity": gen.pmax,
        }
        if gen.heat_rate_gj_per_mwh > 0 and any(
            gen.pollutant_combustion.get(t, 0.0) > 0 for t in include_pollutants
        ):
            factor_rows = []
            for tag in include_pollutants:
                combustion = gen.pollutant_combustion.get(tag, 0.0)
                if combustion > 0:
                    factor_rows.append(
                        {"emission": pollutant_uid[tag], "combustion": combustion}
                    )
            fuels.append(
                {
                    "uid": fuel_uid,
                    "name": f"{gen.name}_fuel",
                    "price": 0.0,  # already folded into gcost above
                    "emission_factors": factor_rows,
                }
            )
            entry["fuel"] = fuel_uid
            entry["heat_rate"] = gen.heat_rate_gj_per_mwh
            fuel_uid += 1
        gens_json.append(entry)

    sim = {
        "block_array": [{"uid": h + 1, "duration": 1.0} for h in range(nh)],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": nh}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }
    system = {
        "name": f"RTSGMLCDay{conversion.day}",
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
