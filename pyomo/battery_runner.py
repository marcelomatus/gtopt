"""
Battery dispatch runner class.
"""

import sys
from typing import Optional

# Import battery dispatch modules
try:
    from battery_dispatch import ConfigLoader, BatteryDispatchSolver, ResultsHandler

    BATTERY_DISPATCH_AVAILABLE = True
except ImportError:
    BATTERY_DISPATCH_AVAILABLE = False


class BatteryDispatchRunner:
    """Handles running battery dispatch optimization."""

    def __init__(self):
        """Initialize the runner."""
        self.config = None
        self.results = None

    def load_config(self, config_file: str, output_file: Optional[str] = None) -> None:
        """Load configuration from file."""
        if not BATTERY_DISPATCH_AVAILABLE:
            raise ImportError("Battery dispatch modules not available.")

        try:
            self.config = ConfigLoader.from_file(config_file)
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"Configuration file not found: {exc}") from exc
        except (KeyError, ValueError) as exc:
            raise ValueError(f"Invalid configuration: {exc}") from exc

        if output_file:
            self.config.output_file = output_file

    def solve(self) -> None:
        """Run the optimization solver."""
        if self.config is None:
            raise RuntimeError("Configuration must be loaded before solving.")

        try:
            solver = BatteryDispatchSolver(self.config)
            self.results = solver.solve()
        except RuntimeError as exc:
            raise RuntimeError(f"Solver error: {exc}") from exc

    def display_and_save_results(self) -> None:
        """Display results and save to file."""
        if self.results is None:
            raise RuntimeError("No results to display.")

        ResultsHandler.print_summary(self.results)
        ResultsHandler.to_json(self.results, self.config.output_file)

    def run(self, config_file: str, output_file: Optional[str] = None) -> int:
        """
        Run the complete battery dispatch optimization.

        Args:
            config_file: Path to JSON configuration file
            output_file: Optional override for output file path

        Returns:
            Exit code (0 for success, 1 for error)
        """
        if not BATTERY_DISPATCH_AVAILABLE:
            print("Error: Battery dispatch modules not available.", file=sys.stderr)
            print(
                "Make sure all battery_dispatch files are in the pyomo directory.",
                file=sys.stderr,
            )
            return 1

        try:
            self.load_config(config_file, output_file)
        except FileNotFoundError as exc:
            print(f"Error: {exc}", file=sys.stderr)
            return 1
        except (KeyError, ValueError) as exc:
            print(f"Invalid configuration: {exc}", file=sys.stderr)
            return 1

        try:
            self.solve()
        except RuntimeError as exc:
            print(f"Solver error: {exc}", file=sys.stderr)
            print("\nMake sure CBC solver is installed:", file=sys.stderr)
            print("  conda install -c conda-forge coincbc", file=sys.stderr)
            print("  or", file=sys.stderr)
            print("  apt-get install coinor-cbc", file=sys.stderr)
            return 1

        try:
            self.display_and_save_results()
        except RuntimeError as exc:
            print(f"Error handling results: {exc}", file=sys.stderr)
            return 1

        return 0
