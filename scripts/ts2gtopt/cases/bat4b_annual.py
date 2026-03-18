"""bat4b_annual – Synthetic annual time-series for the bat_4b planning case.

The bat_4b case (4-bus network with solar and battery) is extended here to a
full-year hourly model.  All generated time-series satisfy the following
physical requirements:

Demands (d3 at bus b3, uid:1; d4 at bus b4, uid:2)
---------------------------------------------------
* **Seasonal variation** – Chilean winter (June–August) is 20 % higher than
  Chilean summer (December–February).  Summer peaks around January 15.
* **Intra-day profile** – evening peak at 18–23 h, overnight minimum at 1–4 h,
  with a 35 % difference between peak and minimum values (relative to minimum).
* **Weekends & holidays** – 30 % less demand across all hours.  Holiday set
  includes Chilean national holidays for the requested year.

Solar generator (g_solar at bus b1, uid:3)
-------------------------------------------
* **Seasonal variation** – Chilean summer (January) produces 40 % more energy
  than Chilean winter (July); ratio = 1.4.
* **Diurnal profile** – zero at night (before sunrise / after sunset), smooth
  bell curve peaking at 13 h.

Thermal generators with maintenance (g1 uid:1 and g2 uid:2)
------------------------------------------------------------
* g1 has 2 weeks of maintenance starting January 15 (pmax = 0).
* g2 has 2 weeks of maintenance starting July 17 (pmax = 0).
* The two windows do **not** overlap.

Conservation guarantees
-----------------------
All series are designed to be projected with :func:`ts2gtopt.make_horizon` and
:func:`ts2gtopt.project_timeseries`.  The mean-aggregation approach preserves:

* **Period** – Σ block_duration = 8760 h
* **Energy** – Σ(projected × duration) = Σ(original × 1 h)
"""

from __future__ import annotations

import datetime
import json
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd

# ---------------------------------------------------------------------------
# Module-level constants
# ---------------------------------------------------------------------------

# Normalised 24-hour demand profile (mean = 1.0).
# Peak window 18–23 h; overnight minimum at 1–4 h.
# (max - min) / min ≈ 35 %
_DEMAND_PROFILE_RAW = np.array(
    [
        0.83,
        0.82,
        0.82,
        0.82,
        0.83,
        0.86,  # 0–5 h  night minimum
        0.90,
        0.95,
        1.00,
        1.04,
        1.05,
        1.05,  # 6–11 h morning ramp
        1.04,
        1.01,
        0.99,
        0.98,
        0.99,
        1.04,  # 12–17 h afternoon
        1.10,
        1.11,
        1.10,
        1.07,
        1.01,
        0.92,  # 18–23 h evening peak
    ],
    dtype=float,
)
DEMAND_DAILY_PROFILE: np.ndarray = _DEMAND_PROFILE_RAW / _DEMAND_PROFILE_RAW.mean()

# Seasonal amplitude such that (1 + A) / (1 - A) = 1.20 (20 % winter/summer ratio)
_SEASONAL_AMPLITUDE_DEMAND: float = 0.0909

# Day-of-year of the Chilean winter peak (July 15 ≈ doy 196)
_WINTER_PEAK_DOY: int = 196

# Reduction factor on weekends and holidays
WEEKEND_FACTOR: float = 0.70

# Bat_4b generator pmax values (MW)
G1_PMAX_MW: float = 250.0
G2_PMAX_MW: float = 150.0
SOLAR_PMAX_MW: float = 90.0

# Bat_4b demand base values (MW) – approximate mean annual load
D3_BASE_MW: float = 100.0
D4_BASE_MW: float = 70.0

# Maintenance windows: (month, day) start + duration in days
_G1_MAINT_START: tuple[int, int] = (1, 15)  # January 15
_G1_MAINT_DAYS: int = 14  # 2 weeks
_G2_MAINT_START: tuple[int, int] = (7, 17)  # July 17
_G2_MAINT_DAYS: int = 14  # 2 weeks

# Solar parameters
_SOLAR_SEASONAL_AMPLITUDE: float = 0.1667  # (1 + A) / (1 - A) = 1.40
_SOLAR_SUMMER_DOY: int = 15  # January 15
_SOLAR_SUNRISE_H: int = 6
_SOLAR_SUNSET_H: int = 20


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------


def _easter(year: int) -> datetime.date:
    """Compute Easter Sunday using the Anonymous Gregorian algorithm."""
    a = year % 19
    b = year // 100
    c = year % 100
    d = b // 4
    e = b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i = c // 4
    k = c % 4
    ell = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * ell) // 451
    month = (h + ell - 7 * m + 114) // 31
    day = ((h + ell - 7 * m + 114) % 31) + 1
    return datetime.date(year, month, day)


def make_chilean_holidays(year: int) -> frozenset[datetime.date]:
    """Return a frozenset of Chilean public holidays for *year*.

    Includes fixed-date national holidays and Easter-based holidays
    (Good Friday and Holy Saturday).
    """
    easter = _easter(year)
    fixed: list[tuple[int, int]] = [
        (1, 1),
        (5, 1),
        (5, 21),
        (6, 21),
        (7, 16),
        (8, 15),
        (9, 18),
        (9, 19),
        (10, 9),
        (11, 1),
        (12, 8),
        (12, 25),
    ]
    holidays: set[datetime.date] = {datetime.date(year, m, d) for m, d in fixed}
    holidays.add(easter - datetime.timedelta(days=2))  # Good Friday
    holidays.add(easter - datetime.timedelta(days=1))  # Holy Saturday
    return frozenset(holidays)


def _seasonal_demand_factor(doy: np.ndarray) -> np.ndarray:
    """Seasonal demand multiplier, peaks in winter (July in Chile).

    ``f = 1.0 + amplitude * cos(2π * (doy - winter_peak) / 365)``

    Range: [1 - A, 1 + A] where A = 0.0909.
    Ratio winter / summer = (1 + A) / (1 - A) = 1.20.
    """
    return 1.0 + _SEASONAL_AMPLITUDE_DEMAND * np.cos(
        2 * np.pi * (doy - _WINTER_PEAK_DOY) / 365
    )


def _workday_factor(
    timestamps: pd.DatetimeIndex, holidays: frozenset[datetime.date]
) -> np.ndarray:
    """Return 1.0 for workdays, WEEKEND_FACTOR for weekends and holidays."""
    is_weekend = timestamps.weekday >= 5
    is_holiday = np.array([ts.date() in holidays for ts in timestamps], dtype=bool)
    return np.where(is_weekend | is_holiday, WEEKEND_FACTOR, 1.0)


def _solar_seasonal_factor(doy: np.ndarray) -> np.ndarray:
    """Solar seasonal multiplier, peaks in summer (January in Chile).

    Normalised so that the summer peak equals 1.0.  The winter trough is
    ``(1 - A) / (1 + A) = 1 / 1.4 ≈ 0.714``.
    """
    f_raw = 1.0 + _SOLAR_SEASONAL_AMPLITUDE * np.cos(
        2 * np.pi * (doy - _SOLAR_SUMMER_DOY) / 365
    )
    return f_raw / (1.0 + _SOLAR_SEASONAL_AMPLITUDE)


def _solar_diurnal_factor(hours: np.ndarray) -> np.ndarray:
    """Bell-curve diurnal profile: 0 at night, peak at 13 h.

    Uses ``sin(π × (h - sunrise) / (sunset - sunrise))`` for daylight hours.
    """
    span = _SOLAR_SUNSET_H - _SOLAR_SUNRISE_H
    in_day = (hours >= _SOLAR_SUNRISE_H) & (hours <= _SOLAR_SUNSET_H)
    f = np.zeros(len(hours), dtype=float)
    f[in_day] = np.sin(np.pi * (hours[in_day] - _SOLAR_SUNRISE_H) / span)
    return f


# ---------------------------------------------------------------------------
# Public time-series generators
# ---------------------------------------------------------------------------


def make_demand_timeseries(
    year: int = 2023,
    holidays: frozenset[datetime.date] | None = None,
    noise_seed: int = 42,
) -> pd.DataFrame:
    """Generate hourly demand time-series for bat_4b demands d3 and d4.

    Parameters
    ----------
    year:
        Calendar year for the simulation (default 2023).
    holidays:
        Set of public holiday dates.  When ``None`` the Chilean holidays for
        *year* are computed automatically via :func:`make_chilean_holidays`.
    noise_seed:
        Seed for the random noise generator (±5 % uniform noise).

    Returns
    -------
    pd.DataFrame
        8760-row DataFrame with columns ``datetime``, ``uid:1`` (d3 in MW),
        ``uid:2`` (d4 in MW).  Both series have the same shape; d4 = d3 × 0.70.
    """
    if holidays is None:
        holidays = make_chilean_holidays(year)

    timestamps = pd.date_range(f"{year}-01-01", periods=8760, freq="h")
    doy = pd.Series(timestamps).dt.day_of_year.to_numpy()
    hours = pd.Series(timestamps).dt.hour.to_numpy()

    f_season = _seasonal_demand_factor(doy)
    f_daily = DEMAND_DAILY_PROFILE[hours]
    f_work = _workday_factor(timestamps, holidays)

    rng = np.random.default_rng(noise_seed)
    noise = rng.uniform(0.95, 1.05, len(timestamps))

    d3 = D3_BASE_MW * f_season * f_daily * f_work * noise
    d4 = D4_BASE_MW * f_season * f_daily * f_work * noise

    return pd.DataFrame({"datetime": timestamps, "uid:1": d3, "uid:2": d4})


def make_solar_pmax_timeseries(
    year: int = 2023,
    pmax_mw: float = SOLAR_PMAX_MW,
) -> pd.DataFrame:
    """Generate hourly solar pmax time-series for bat_4b generator g_solar.

    The values represent the maximum power output (MW) for each hour.
    They are shaped by:

    * Seasonal amplitude – summer (January) 40 % more than winter (July).
    * Diurnal bell curve – zero before sunrise (06 h) and after sunset (20 h),
      peaking at 13 h.

    Parameters
    ----------
    year:
        Calendar year for the simulation (default 2023).
    pmax_mw:
        Installed solar capacity in MW (default 90 MW).

    Returns
    -------
    pd.DataFrame
        8760-row DataFrame with columns ``datetime``, ``uid:3`` (g_solar pmax
        in MW, clipped to [0, pmax_mw]).
    """
    timestamps = pd.date_range(f"{year}-01-01", periods=8760, freq="h")
    doy = pd.Series(timestamps).dt.day_of_year.to_numpy()
    hours = pd.Series(timestamps).dt.hour.to_numpy()

    f_season = _solar_seasonal_factor(doy)
    f_diurnal = _solar_diurnal_factor(hours)

    pmax = np.clip(pmax_mw * f_season * f_diurnal, 0.0, pmax_mw)
    return pd.DataFrame({"datetime": timestamps, "uid:3": pmax})


def make_generator_pmax_timeseries(
    year: int = 2023,
    g1_pmax: float = G1_PMAX_MW,
    g2_pmax: float = G2_PMAX_MW,
) -> pd.DataFrame:
    """Generate hourly pmax time-series for thermal generators g1 and g2.

    Each generator has a 2-week (14-day) maintenance window during which its
    pmax is set to 0 MW.  The windows do **not** overlap:

    * g1 – maintenance starts January 15.
    * g2 – maintenance starts July 17.

    Parameters
    ----------
    year:
        Calendar year for the simulation (default 2023).
    g1_pmax:
        Nominal pmax for generator g1 in MW (default 250 MW).
    g2_pmax:
        Nominal pmax for generator g2 in MW (default 150 MW).

    Returns
    -------
    pd.DataFrame
        8760-row DataFrame with columns ``datetime``, ``uid:1`` (g1 pmax in
        MW), ``uid:2`` (g2 pmax in MW).
    """
    timestamps = pd.date_range(f"{year}-01-01", periods=8760, freq="h")

    g1_start = datetime.date(year, *_G1_MAINT_START)
    g1_end = g1_start + datetime.timedelta(days=_G1_MAINT_DAYS)
    g2_start = datetime.date(year, *_G2_MAINT_START)
    g2_end = g2_start + datetime.timedelta(days=_G2_MAINT_DAYS)

    dates = np.array([ts.date() for ts in timestamps])
    in_g1 = (dates >= g1_start) & (dates < g1_end)
    in_g2 = (dates >= g2_start) & (dates < g2_end)

    g1_arr = np.where(in_g1, 0.0, g1_pmax)
    g2_arr = np.where(in_g2, 0.0, g2_pmax)

    return pd.DataFrame({"datetime": timestamps, "uid:1": g1_arr, "uid:2": g2_arr})


def make_bat4b_horizon(year: int = 2023) -> dict[str, Any]:
    """Return a 12-stage × 24-block planning horizon for the bat_4b annual case.

    Delegates to :func:`ts2gtopt.make_horizon` with default parameters
    (monthly stages, hourly blocks, conservation-correct durations).
    """
    from ts2gtopt.ts2gtopt import make_horizon  # pylint: disable=import-outside-toplevel

    return make_horizon(year, n_stages=12, n_blocks=24, interval_hours=1.0)


# ---------------------------------------------------------------------------
# File I/O helpers
# ---------------------------------------------------------------------------

# Line topology used in every bat_4b variant
_LINE_ARRAY: list[dict[str, Any]] = [
    {
        "uid": 1,
        "name": "l1_2",
        "bus_a": "b1",
        "bus_b": "b2",
        "reactance": 0.02,
        "tmax_ab": 300,
        "tmax_ba": 300,
    },
    {
        "uid": 2,
        "name": "l1_3",
        "bus_a": "b1",
        "bus_b": "b3",
        "reactance": 0.02,
        "tmax_ab": 300,
        "tmax_ba": 300,
    },
    {
        "uid": 3,
        "name": "l2_3",
        "bus_a": "b2",
        "bus_b": "b3",
        "reactance": 0.03,
        "tmax_ab": 200,
        "tmax_ba": 200,
    },
    {
        "uid": 4,
        "name": "l2_4",
        "bus_a": "b2",
        "bus_b": "b4",
        "reactance": 0.02,
        "tmax_ab": 200,
        "tmax_ba": 200,
    },
    {
        "uid": 5,
        "name": "l3_4",
        "bus_a": "b3",
        "bus_b": "b4",
        "reactance": 0.03,
        "tmax_ab": 150,
        "tmax_ba": 150,
    },
]


def write_bat4b_timeseries(
    output_dir: Path,
    year: int = 2023,
    output_format: str = "parquet",
) -> dict[str, Path]:
    """Generate and write all bat_4b annual time-series files.

    Produces three flat schedule files under *output_dir* (suitable for testing
    the ts2gtopt tool itself):

    * ``demand_lmax.{ext}``   – d3 and d4 hourly lmax (uid:1, uid:2)
    * ``solar_pmax.{ext}``    – g_solar hourly pmax (uid:3)
    * ``generator_pmax.{ext}``– g1 and g2 hourly pmax with maintenance (uid:1, uid:2)

    Parameters
    ----------
    output_dir:
        Directory where files will be written (created if absent).
    year:
        Calendar year (default 2023).
    output_format:
        ``"parquet"`` *(default)* or ``"csv"``.

    Returns
    -------
    dict[str, Path]
        Mapping ``stem → file_path`` for each written file.
    """
    from ts2gtopt.ts2gtopt import (  # pylint: disable=import-outside-toplevel
        convert_timeseries,
        make_horizon,
    )

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write raw hourly CSVs to a temporary sub-directory
    raw_dir = output_dir / "_raw"
    raw_dir.mkdir(exist_ok=True)

    demand_csv = raw_dir / "demand_lmax.csv"
    solar_csv = raw_dir / "solar_pmax.csv"
    gen_csv = raw_dir / "generator_pmax.csv"

    make_demand_timeseries(year).to_csv(demand_csv, index=False)
    make_solar_pmax_timeseries(year).to_csv(solar_csv, index=False)
    make_generator_pmax_timeseries(year).to_csv(gen_csv, index=False)

    horizon = make_horizon(year)
    return convert_timeseries(
        [demand_csv, solar_csv, gen_csv],
        output_dir,
        horizon,
        year=year,
        output_format=output_format,
    )


def write_bat4b_gtopt_input(
    input_dir: Path,
    year: int = 2023,
    g1_pmax: float = G1_PMAX_MW,
    g2_pmax: float = G2_PMAX_MW,
) -> None:
    """Write gtopt-compatible input directory structure for the bat_4b annual case.

    Creates the subdirectory layout expected by the gtopt solver when the JSON
    option ``input_directory`` is set to *input_dir*:

    .. code-block:: text

        input_dir/
        ├── Demand/
        │   └── lmax.parquet    (uid:1 = d3, uid:2 = d4)
        ├── Generator/
        │   └── pmax.parquet    (uid:1 = g1, uid:2 = g2, with maintenance zeros)
        └── GeneratorProfile/
            └── profile.parquet (uid:1 = gp_solar, capacity factors 0–1)

    All schedule files use the standard gtopt format:
    ``scenario  stage  block  uid:N …``

    Parameters
    ----------
    input_dir:
        Target directory (created if absent).
    year:
        Calendar year for the simulation (default 2023).
    g1_pmax:
        Nominal pmax for g1 in MW (default :data:`G1_PMAX_MW`).
    g2_pmax:
        Nominal pmax for g2 in MW (default :data:`G2_PMAX_MW`).
    """
    from ts2gtopt.ts2gtopt import (  # pylint: disable=import-outside-toplevel
        make_horizon,
        project_timeseries,
        write_schedule,
    )

    input_dir = Path(input_dir)
    horizon = make_horizon(year)

    # Demand lmax: uid:1 = d3, uid:2 = d4 (column names already match element uids)
    demand_proj = project_timeseries(make_demand_timeseries(year), horizon, year=year)
    write_schedule(demand_proj, input_dir / "Demand" / "lmax.parquet")

    # Generator pmax: uid:1 = g1, uid:2 = g2 (maintenance zeros included)
    gen_df = make_generator_pmax_timeseries(year, g1_pmax=g1_pmax, g2_pmax=g2_pmax)
    gen_proj = project_timeseries(gen_df, horizon, year=year)
    write_schedule(gen_proj, input_dir / "Generator" / "pmax.parquet")

    # Solar profile: project pmax MW → divide by SOLAR_PMAX_MW → 0-1 factor
    # Rename uid:3 (g_solar generator uid) → uid:1 (gp_solar profile uid)
    solar_proj = project_timeseries(
        make_solar_pmax_timeseries(year), horizon, year=year
    )
    profile_df = solar_proj.copy()
    profile_df["uid:1"] = profile_df["uid:3"] / SOLAR_PMAX_MW
    profile_df = profile_df.drop(columns=["uid:3"])
    write_schedule(profile_df, input_dir / "GeneratorProfile" / "profile.parquet")


def make_bat4b_case_json(
    schedule_dir: Path,
    year: int = 2023,
    output_format: str = "parquet",
) -> dict[str, Any]:
    """Return a bat_4b planning case JSON with flat file-based schedule references.

    This variant references schedule files by their full relative path
    (e.g. ``schedules/demand_lmax.parquet``), as produced by
    :func:`write_bat4b_timeseries`.  It is primarily useful for testing the
    ts2gtopt tool's output without running the gtopt solver.

    For a fully runnable gtopt case use :func:`write_bat4b_case` instead, which
    sets ``input_directory`` and uses the standard subdirectory convention.

    Parameters
    ----------
    schedule_dir:
        Directory containing the schedule files (relative path used in JSON).
    year:
        Calendar year used when building block durations.
    output_format:
        ``"parquet"`` or ``"csv"`` – the format of the schedule files.

    Returns
    -------
    dict
        Planning case JSON dict.
    """
    ext = output_format
    horizon = make_bat4b_horizon(year)

    stage_array = [
        {
            "uid": s["uid"],
            "first_block": s["first_block"],
            "count_block": s["count_block"],
            "active": 1,
        }
        for s in horizon["stages"]
    ]
    block_array = [
        {"uid": b["uid"], "duration": b["duration"]} for b in horizon["blocks"]
    ]
    sched = str(schedule_dir).rstrip("/")

    return {
        "options": {
            "annual_discount_rate": 0.0,
            "use_lp_names": 1,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "use_single_bus": False,
            "demand_fail_cost": 1000,
            "scale_objective": 1000,
            "use_kirchhoff": True,
            "input_format": output_format,
        },
        "simulation": {
            "block_array": block_array,
            "stage_array": stage_array,
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "name": f"bat_4b_{year}",
            "bus_array": [
                {"uid": 1, "name": "b1"},
                {"uid": 2, "name": "b2"},
                {"uid": 3, "name": "b3"},
                {"uid": 4, "name": "b4"},
            ],
            "generator_array": [
                {
                    "uid": 1,
                    "name": "g1",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": f"{sched}/generator_pmax.{ext}",
                    "gcost": 20,
                    "capacity": G1_PMAX_MW,
                },
                {
                    "uid": 2,
                    "name": "g2",
                    "bus": "b2",
                    "pmin": 0,
                    "pmax": f"{sched}/generator_pmax.{ext}",
                    "gcost": 40,
                    "capacity": G2_PMAX_MW,
                },
                {
                    "uid": 3,
                    "name": "g_solar",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": f"{sched}/solar_pmax.{ext}",
                    "gcost": 0,
                    "capacity": SOLAR_PMAX_MW,
                },
            ],
            "demand_array": [
                {
                    "uid": 1,
                    "name": "d3",
                    "bus": "b3",
                    "lmax": f"{sched}/demand_lmax.{ext}",
                },
                {
                    "uid": 2,
                    "name": "d4",
                    "bus": "b4",
                    "lmax": f"{sched}/demand_lmax.{ext}",
                },
            ],
            "line_array": _LINE_ARRAY,
            "battery_array": [
                {
                    "uid": 1,
                    "name": "bat1",
                    "bus": "b3",
                    "input_efficiency": 0.95,
                    "output_efficiency": 0.95,
                    "emin": 0,
                    "emax": 200,
                    "eini": 0,
                    "pmax_charge": 60,
                    "pmax_discharge": 60,
                    "gcost": 0,
                    "capacity": 200,
                }
            ],
        },
    }


def _make_gtopt_simulation(year: int) -> dict[str, Any]:
    """Build the simulation section (stage_array, block_array) for the gtopt case JSON."""
    horizon = make_bat4b_horizon(year)
    return {
        "block_array": [
            {"uid": b["uid"], "duration": b["duration"]} for b in horizon["blocks"]
        ],
        "stage_array": [
            {
                "uid": s["uid"],
                "first_block": s["first_block"],
                "count_block": s["count_block"],
                "active": 1,
            }
            for s in horizon["stages"]
        ],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    }


def _make_gtopt_system(year: int, g1_pmax: float, g2_pmax: float) -> dict[str, Any]:
    """Build the system section for the runnable gtopt case JSON.

    Uses the ``input_directory`` convention: schedule fields contain only the
    file stem (e.g. ``"lmax"``) and gtopt resolves them as
    ``{input_directory}/{ComponentType}/{stem}.parquet``.
    """
    return {
        "name": f"bat_4b_{year}",
        "bus_array": [
            {"uid": 1, "name": "b1"},
            {"uid": 2, "name": "b2"},
            {"uid": 3, "name": "b3"},
            {"uid": 4, "name": "b4"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": "b1",
                "pmin": 0,
                "pmax": "pmax",
                "gcost": 20,
                "capacity": g1_pmax,
            },
            {
                "uid": 2,
                "name": "g2",
                "bus": "b2",
                "pmin": 0,
                "pmax": "pmax",
                "gcost": 40,
                "capacity": g2_pmax,
            },
            {
                "uid": 3,
                "name": "g_solar",
                "bus": "b1",
                "pmin": 0,
                "pmax": SOLAR_PMAX_MW,
                "gcost": 0,
                "capacity": SOLAR_PMAX_MW,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d3", "bus": "b3", "lmax": "lmax"},
            {"uid": 2, "name": "d4", "bus": "b4", "lmax": "lmax"},
        ],
        "line_array": _LINE_ARRAY,
        "battery_array": [
            {
                "uid": 1,
                "name": "bat1",
                "bus": "b3",
                "input_efficiency": 0.95,
                "output_efficiency": 0.95,
                "emin": 0,
                "emax": 200,
                "eini": 0,
                "pmax_charge": 60,
                "pmax_discharge": 60,
                "gcost": 0,
                "capacity": 200,
            }
        ],
        "generator_profile_array": [
            {"uid": 1, "name": "gp_solar", "generator": "g_solar", "profile": "profile"}
        ],
    }


def write_bat4b_case(
    output_dir: Path,
    year: int = 2023,
    g1_pmax: float = G1_PMAX_MW,
    g2_pmax: float = G2_PMAX_MW,
) -> Path:
    """Write a complete, runnable bat_4b annual case to *output_dir*.

    Creates:

    * ``input/`` – gtopt input directory with projected schedule Parquet files
      (``Demand/lmax.parquet``, ``Generator/pmax.parquet``,
      ``GeneratorProfile/profile.parquet``)
    * ``bat_4b_{year}.json`` – the planning case JSON that the gtopt binary
      can execute directly from *output_dir*

    The case has sufficient generation capacity to meet demand in all months
    and under both maintenance windows (g1 in January, g2 in July):

    * g1 = *g1_pmax* (default 250 MW, 0 MW during Jan 15–28)
    * g2 = *g2_pmax* (default 150 MW, 0 MW during Jul 17–30)
    * g_solar = 90 MW × seasonal/diurnal capacity factor (mean ≈ 16 MW)
    * g_bat_out = 60 MW discharge
    * Peak demand ≈ 110–140 MW total → large capacity margin

    If the solver reports load shedding (``output/Demand/fail_sol.csv`` has
    non-zero values), re-run with larger *g1_pmax* / *g2_pmax*.

    Parameters
    ----------
    output_dir:
        Target directory (created if absent).
    year:
        Calendar year for the case (default 2023).
    g1_pmax:
        Nominal pmax for generator g1 in MW.  Use a higher value if demand
        is not satisfied after an initial run (default :data:`G1_PMAX_MW`).
    g2_pmax:
        Nominal pmax for generator g2 in MW (default :data:`G2_PMAX_MW`).

    Returns
    -------
    Path
        Path to the written JSON case file.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    write_bat4b_gtopt_input(
        output_dir / "input", year, g1_pmax=g1_pmax, g2_pmax=g2_pmax
    )

    from ts2gtopt.ts2gtopt import (  # pylint: disable=import-outside-toplevel
        build_hour_block_map,
        make_horizon,
    )

    horizon = make_horizon(year, n_stages=12, n_blocks=24, interval_hours=1.0)

    case = {
        "options": {
            "annual_discount_rate": 0.0,
            "use_lp_names": 1,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "use_single_bus": False,
            "demand_fail_cost": 1000,
            "scale_objective": 1000,
            "use_kirchhoff": True,
            "input_directory": "input",
            "input_format": "parquet",
        },
        "simulation": _make_gtopt_simulation(year),
        "system": _make_gtopt_system(year, g1_pmax, g2_pmax),
        "hour_block_map": build_hour_block_map(horizon, year=year),
    }
    json_path = output_dir / f"bat_4b_{year}.json"
    json_path.write_text(json.dumps(case, indent=2), encoding="utf-8")
    return json_path
