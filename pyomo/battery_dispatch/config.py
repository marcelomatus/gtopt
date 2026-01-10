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
    time_resolution_hours: float  # e.g., 1.0, 0.25
    marginal_costs_usd_per_mwh: List[float]
    time_periods: Optional[List[str]] = None  # optional timestamps


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
        
        with open(path, 'r', encoding='utf-8') as f:
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
            max_soc_mwh=battery_data.get("max_soc_mwh", 
                battery_data["storage_capacity_mwh"])
        )
        
        # Parse time series config
        ts_data = data["time_series"]
        time_series = TimeSeriesConfig(
            time_resolution_hours=ts_data["time_resolution_hours"],
            marginal_costs_usd_per_mwh=ts_data["marginal_costs_usd_per_mwh"],
            time_periods=ts_data.get("time_periods")
        )
        
        # Validate time series length
        n_periods = len(time_series.marginal_costs_usd_per_mwh)
        if time_series.time_periods and len(time_series.time_periods) != n_periods:
            raise ValueError("Number of time periods must match marginal costs length")
        
        return OptimizationConfig(
            battery=battery,
            time_series=time_series,
            solver_name=data.get("solver", "cbc"),
            output_file=data.get("output_file", "results.json")
        )
