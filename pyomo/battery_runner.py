"""
Battery dispatch runner class.
"""

import sys
from typing import Optional, Any, TYPE_CHECKING

if TYPE_CHECKING:
    from battery_dispatch.config import (
        OptimizationConfig,
        BatteryConfig,
        TimeSeriesConfig,
    )
    from battery_dispatch import ConfigLoader, BatteryDispatchSolver, ResultsHandler

# Import battery dispatch modules
try:
    from battery_dispatch import ConfigLoader, BatteryDispatchSolver, ResultsHandler
    from battery_dispatch.config import (
        OptimizationConfig,
        BatteryConfig,
        TimeSeriesConfig,
    )

    BATTERY_DISPATCH_AVAILABLE = True
except ImportError:
    BATTERY_DISPATCH_AVAILABLE = False
    # Create dummy types for type checking when battery_dispatch is not available
    class BatteryConfig:  # type: ignore
        """Dummy BatteryConfig class."""
        max_charge_rate_mw: float = 0.0
        max_discharge_rate_mw: float = 0.0
        min_soc_mwh: float = 0.0
        max_soc_mwh: float = 0.0
        initial_soc_mwh: float = 0.0
        charge_efficiency: float = 0.0
        discharge_efficiency: float = 0.0

    class TimeSeriesConfig:  # type: ignore
        """Dummy TimeSeriesConfig class."""
        marginal_costs_usd_per_mwh: list[float]
        time_durations_hours: list[float]
        time_periods: list[int]
        
        def __init__(self) -> None:
            self.marginal_costs_usd_per_mwh = []
            self.time_durations_hours = []
            self.time_periods = []

    class OptimizationConfig:  # type: ignore
        """Dummy config class for type hints."""
        def __init__(self, battery: BatteryConfig, time_series: TimeSeriesConfig) -> None:
            self.battery = battery
            self.time_series = time_series
            self.output_file: str = ""
            self.solver_name: str = "cbc"

    class ConfigLoader:  # type: ignore
        """Dummy loader class for type hints."""
        @staticmethod
        def from_file(filepath: str) -> OptimizationConfig:
            """Dummy method."""
            return OptimizationConfig(
                battery=BatteryConfig(),
                time_series=TimeSeriesConfig()
            )

    class BatteryDispatchSolver:  # type: ignore
        """Dummy solver class for type hints."""
        def __init__(self, config: OptimizationConfig) -> None:
            """Dummy init."""
            pass
        
        def solve(self) -> dict[str, Any]:
            """Dummy solve."""
            return {}

    class ResultsHandler:  # type: ignore
        """Dummy results handler for type hints."""
        @staticmethod
        def print_summary(results: dict[str, Any]) -> None:
            """Dummy method."""
            pass
        
        @staticmethod
        def to_json(results: dict[str, Any], output_path: str) -> None:
            """Dummy method."""
            pass


class BatteryDispatchRunner:
    """Handles running battery dispatch optimization."""

    def __init__(self) -> None:
        """Initialize the runner."""
        self.config: Optional[OptimizationConfig] = None
        self.results: Optional[dict[str, Any]] = None

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

        if output_file and self.config:
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
        if self.config is None:
            raise RuntimeError("Configuration not available.")

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
