"""
Pyomo optimization models - Main entry point.

This module provides a command-line interface to run optimization models:
1. Simple mixed-integer linear programming example
2. Battery dispatch optimization for energy storage
"""

import sys
import argparse
from typing import Optional, Type, cast

# Import refactored modules
SimpleOptimization: Optional[Type] = None
BatteryDispatchRunner: Optional[Type] = None

try:
    from simple_optimization import SimpleOptimization as SO
    SimpleOptimization = SO
except ImportError:
    pass

try:
    from battery_runner import BatteryDispatchRunner as BDR
    BatteryDispatchRunner = BDR
except ImportError:
    pass


def _setup_argparse() -> argparse.ArgumentParser:
    """Set up command line argument parser."""
    parser = argparse.ArgumentParser(
        description="Pyomo Optimization Models",
        epilog="Examples:\n"
        "  python hbmaule.py simple          # Run simple example\n"
        "  python hbmaule.py battery config.json  # Run battery dispatch",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subparsers = parser.add_subparsers(dest="command", help="Command to run", required=True)

    # Simple example command
    subparsers.add_parser(
        "simple", help="Run the simple mixed-integer optimization example"
    )

    # Battery dispatch command
    battery_parser = subparsers.add_parser(
        "battery", help="Run battery dispatch optimization"
    )
    battery_parser.add_argument(
        "config_file", type=str, help="Path to JSON configuration file"
    )
    battery_parser.add_argument(
        "--output", type=str, help="Override output file path (optional)"
    )

    return parser


def main() -> int:
    """Main entry point with command line interface."""
    parser = _setup_argparse()

    args = parser.parse_args()

    if args.command == "simple":
        if SimpleOptimization is None:
            print("Error: SimpleOptimization module not found.", file=sys.stderr)
            return 1
        # Create instance and call run method
        optimizer = SimpleOptimization()
        return optimizer.run()

    if args.command == "battery":
        if BatteryDispatchRunner is None:
            print("Error: BatteryDispatchRunner module not found.", file=sys.stderr)
            return 1
        runner = BatteryDispatchRunner()
        # args is guaranteed to have these attributes by argparse
        return runner.run(args.config_file, args.output)

    # This should never be reached due to required=True in subparsers
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
