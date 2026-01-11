"""
Configuration handling for battery dispatch optimization.
"""

import json
from dataclasses import dataclass
from typing import List, Optional
from pathlib import Path


@dataclass
class BatteryConfig:
    """Battery technical parameters."""

    name: str
    storage_capacity_mwh: float
    max_charge_rate_mw: float
    max_discharge_rate_mw: float
    charge_efficiency: float  # 0 to 1
    discharge_efficiency: float  # 0 to 1
    initial_soc_mwh: float  # initial state of charge
    min_soc_mwh: float  # minimum state of charge
    max_soc_mwh: float  # maximum state of charge


@dataclass
class TimeSeriesConfig:
    """Time series data configuration."""

    time_durations_hours: List[float]  # duration of each interval in hours
    marginal_costs_usd_per_mwh: List[float]
    time_periods: Optional[List[str]] = None  # optional timestamps

    def __post_init__(self):
        """Validate that all lists have the same length."""
        if len(self.time_durations_hours) != len(self.marginal_costs_usd_per_mwh):
            raise ValueError(
                f"Length of time_durations_hours ({len(self.time_durations_hours)}) "
                f"must match length of marginal_costs_usd_per_mwh "
                f"({len(self.marginal_costs_usd_per_mwh)})"
            )
        if self.time_periods and len(self.time_periods) != len(
            self.marginal_costs_usd_per_mwh
        ):
            raise ValueError(
                f"Length of time_periods ({len(self.time_periods)}) "
                f"must match length of marginal_costs_usd_per_mwh "
                f"({len(self.marginal_costs_usd_per_mwh)})"
            )
        # Validate all durations are positive
        for i, duration in enumerate(self.time_durations_hours):
            if duration <= 0:
                raise ValueError(
                    f"Time duration at index {i} must be positive, got {duration}"
                )


@dataclass
class OptimizationConfig:
    """Overall optimization configuration."""

    battery: BatteryConfig
    time_series: TimeSeriesConfig
    solver_name: str = "cbc"
    output_file: str = "results.json"


class ConfigLoader:
    """Load configuration from JSON file."""

    @staticmethod
    def from_file(filepath: str) -> OptimizationConfig:
        """Load configuration from JSON file."""
        path = Path(filepath)
        if not path.exists():
            raise FileNotFoundError(f"Config file not found: {filepath}")

        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        # Parse battery config
        battery_data = data["battery"]
        battery = BatteryConfig(
            name=battery_data.get("name", "battery"),
            storage_capacity_mwh=battery_data["storage_capacity_mwh"],
            max_charge_rate_mw=battery_data["max_charge_rate_mw"],
            max_discharge_rate_mw=battery_data["max_discharge_rate_mw"],
            charge_efficiency=battery_data["charge_efficiency"],
            discharge_efficiency=battery_data["discharge_efficiency"],
            initial_soc_mwh=battery_data.get("initial_soc_mwh", 0.0),
            min_soc_mwh=battery_data.get("min_soc_mwh", 0.0),
            max_soc_mwh=battery_data.get(
                "max_soc_mwh", battery_data["storage_capacity_mwh"]
            ),
        )

        # Parse time series config
        ts_data = data["time_series"]

        # Handle both old format (time_resolution_hours) and new format (time_durations_hours)
        if "time_durations_hours" in ts_data:
            time_durations = ts_data["time_durations_hours"]
        elif "time_resolution_hours" in ts_data:
            # Convert single resolution to list of identical durations
            resolution = ts_data["time_resolution_hours"]
            n_periods = len(ts_data["marginal_costs_usd_per_mwh"])
            time_durations = [resolution] * n_periods
        else:
            raise ValueError(
                "time_series must contain either 'time_durations_hours' "
                "or 'time_resolution_hours'"
            )

        time_series = TimeSeriesConfig(
            time_durations_hours=time_durations,
            marginal_costs_usd_per_mwh=ts_data["marginal_costs_usd_per_mwh"],
            time_periods=ts_data.get("time_periods"),
        )

        return OptimizationConfig(
            battery=battery,
            time_series=time_series,
            solver_name=data.get("solver", "cbc"),
            output_file=data.get("output_file", "results.json"),
        )
