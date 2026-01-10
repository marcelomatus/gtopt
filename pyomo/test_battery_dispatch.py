"""
Unit tests for battery dispatch optimization.
"""
import json
import os
import sys
import tempfile
from pathlib import Path

import pytest

# Add the pyomo directory to the path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from battery_dispatch import (
    ConfigLoader,
    BatteryDispatchModel,
    BatteryDispatchSolver,
    ResultsHandler,
)


def create_test_config_file() -> str:
    """Create a temporary test configuration file."""
    config = {
        "battery": {
            "name": "test_battery",
            "storage_capacity_mwh": 100.0,
            "max_charge_rate_mw": 50.0,
            "max_discharge_rate_mw": 50.0,
            "charge_efficiency": 0.95,
            "discharge_efficiency": 0.95,
            "initial_soc_mwh": 50.0,
            "min_soc_mwh": 10.0,
            "max_soc_mwh": 90.0
        },
        "time_series": {
            "time_resolution_hours": 1.0,
            "marginal_costs_usd_per_mwh": [
                30.0, 25.0, 20.0, 15.0,  # Low prices: charge
                50.0, 60.0, 70.0, 80.0,  # High prices: discharge
                30.0, 25.0
            ],
            "time_periods": [
                "2024-01-01T00:00", "2024-01-01T01:00",
                "2024-01-01T02:00", "2024-01-01T03:00",
                "2024-01-01T04:00", "2024-01-01T05:00",
                "2024-01-01T06:00", "2024-01-01T07:00",
                "2024-01-01T08:00", "2024-01-01T09:00"
            ]
        },
        "solver": "cbc",
        "output_file": "test_results.json"
    }

    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(config, f, indent=2)
        return f.name


def test_config_loader():
    """Test configuration loading."""
    config_file = create_test_config_file()

    try:
        config = ConfigLoader.from_file(config_file)

        assert config.battery.name == "test_battery"
        assert config.battery.storage_capacity_mwh == 100.0
        assert config.battery.charge_efficiency == 0.95
        assert config.time_series.time_resolution_hours == 1.0
        assert len(config.time_series.marginal_costs_usd_per_mwh) == 10
        assert config.solver_name == "cbc"
    finally:
        Path(config_file).unlink()


def test_model_build():
    """Test model building."""
    config_file = create_test_config_file()

    try:
        config = ConfigLoader.from_file(config_file)
        model = BatteryDispatchModel(config)

        # Build should not raise exceptions
        model.build()

        assert model.model is not None
        assert len(model.model.T) == 10  # 10 time periods

        # Check variables exist
        assert hasattr(model.model, 'charge')
        assert hasattr(model.model, 'discharge')
        assert hasattr(model.model, 'soc')
        assert hasattr(model.model, 'objective')

    finally:
        Path(config_file).unlink()


def test_model_variables():
    """Test variable extraction."""
    config_file = create_test_config_file()

    try:
        config = ConfigLoader.from_file(config_file)
        model = BatteryDispatchModel(config)
        model.build()

        # Set some dummy values for testing
        for t in model.model.T:
            model.model.charge[t].value = t * 1.0
            model.model.discharge[t].value = t * 0.5
            model.model.soc[t].value = 50.0 + t * 2.0

        charge, discharge, soc = model.get_variables()

        assert len(charge) == 10
        assert len(discharge) == 10
        assert len(soc) == 10
        assert charge[5] == 5.0
        assert discharge[5] == 2.5
        assert soc[5] == 60.0

    finally:
        Path(config_file).unlink()


def test_results_handler():
    """Test results handling."""
    results = {
        "status": "ok",
        "termination_condition": "optimal",
        "objective_value_usd": -1234.56,
        "charge_mw": [10.0, 20.0, 0.0],
        "discharge_mw": [0.0, 0.0, 30.0],
        "soc_mwh": [50.0, 70.0, 40.0],
        "time_resolution_hours": 1.0,
        "time_periods": ["t1", "t2", "t3"],
        "marginal_costs_usd_per_mwh": [30.0, 25.0, 40.0],
    }

    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        output_path = f.name

    try:
        # Test JSON output
        ResultsHandler.to_json(results, output_path)

        # Verify file exists and contains data
        assert Path(output_path).exists()

        with open(output_path, 'r', encoding='utf-8') as f:
            saved = json.load(f)

        assert "metadata" in saved
        assert "results" in saved
        assert saved["results"]["status"] == "ok"
        assert saved["results"]["objective_value_usd"] == -1234.56

        # Test summary (should not raise exceptions)
        ResultsHandler.print_summary(results)

    finally:
        if Path(output_path).exists():
            Path(output_path).unlink()


@pytest.mark.skip(reason="Requires CBC solver installed")
def test_solver_integration():
    """Integration test with solver (requires CBC installed)."""
    config_file = create_test_config_file()

    try:
        config = ConfigLoader.from_file(config_file)
        solver = BatteryDispatchSolver(config)

        # This will fail if CBC not installed, but that's OK for unit tests
        results = solver.solve()

        assert "status" in results
        assert "charge_mw" in results
        assert "discharge_mw" in results
        assert "soc_mwh" in results

        # Verify lengths match
        n_periods = len(config.time_series.marginal_costs_usd_per_mwh)
        assert len(results["charge_mw"]) == n_periods
        assert len(results["discharge_mw"]) == n_periods
        assert len(results["soc_mwh"]) == n_periods

    finally:
        Path(config_file).unlink()


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
