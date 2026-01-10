"""
Results handling and JSON output for battery dispatch optimization.
"""
import json
from datetime import datetime
from pathlib import Path
from typing import Dict, Any


class ResultsHandler:
    """Handle results output to JSON file."""

    @staticmethod
    def to_json(results: Dict[str, Any], output_path: str) -> None:
        """Write results to JSON file."""
        # Add metadata
        results_with_meta = {
            "metadata": {
                "generated_at": datetime.now().isoformat(),
                "model": "battery_dispatch",
                "time_periods": len(results.get("charge_mw", [])),
            },
            "results": results
        }

        path = Path(output_path)
        path.parent.mkdir(parents=True, exist_ok=True)

        with open(path, 'w', encoding='utf-8') as f:
            json.dump(results_with_meta, f, indent=2, default=str)

        print(f"Results written to: {output_path}")

    @staticmethod
    def print_summary(results: Dict[str, Any]) -> None:
        """Print a summary of results to console."""
        print("\n" + "="*60)
        print("BATTERY DISPATCH OPTIMIZATION RESULTS")
        print("="*60)

        print(f"\nStatus: {results['status']}")
        print(f"Termination: {results['termination_condition']}")
        print(f"Objective Value: ${results['objective_value_usd']:.2f}")

        charge = results['charge_mw']
        discharge = results['discharge_mw']
        soc = results['soc_mwh']
        time_durations = results.get('time_durations_hours', [])

        if charge and discharge and soc:
            print(f"\nTime periods: {len(charge)}")
            
            # Calculate total energy considering variable time durations
            if time_durations and len(time_durations) == len(charge):
                total_charge_energy = sum(c * d for c, d in zip(charge, time_durations))
                total_discharge_energy = sum(d * dur for d, dur in zip(discharge, time_durations))
                print(f"Total charge energy: {total_charge_energy:.2f} MWh")
                print(f"Total discharge energy: {total_discharge_energy:.2f} MWh")
            else:
                print(f"Total charge power: {sum(charge):.2f} MW (sum)")
                print(f"Total discharge power: {sum(discharge):.2f} MW (sum)")
            
            print(f"Max SOC: {max(soc):.2f} MWh")
            print(f"Min SOC: {min(soc):.2f} MWh")
            print(f"Final SOC: {soc[-1]:.2f} MWh")
            
            # Print time duration statistics if available
            if time_durations:
                print(f"\nTime duration statistics:")
                print(f"  Min duration: {min(time_durations):.3f} hours")
                print(f"  Max duration: {max(time_durations):.3f} hours")
                print(f"  Avg duration: {sum(time_durations)/len(time_durations):.3f} hours")

        print("\n" + "="*60)
