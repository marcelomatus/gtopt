"""
Pyomo optimization models.

This module provides two optimization models:
1. Simple mixed-integer linear programming example
2. Battery dispatch optimization for energy storage
"""

import sys
import argparse
from typing import Dict, Any

# Import battery dispatch modules
try:
    from battery_dispatch import ConfigLoader, BatteryDispatchSolver, ResultsHandler

    BATTERY_DISPATCH_AVAILABLE = True
except ImportError:
    BATTERY_DISPATCH_AVAILABLE = False

from pyomo.environ import (
    ConcreteModel,
    Var,
    Objective,
    Constraint,
    SolverFactory,
    NonNegativeReals,
    NonNegativeIntegers,
    maximize,
    value,
)


def _create_optimization_model() -> ConcreteModel:
    """Create the simple optimization model."""
    model = ConcreteModel(name="HBMaule_Optimization")

    # Decision variables
    model.x = Var(domain=NonNegativeIntegers, doc="Integer decision variable")
    model.y = Var(domain=NonNegativeReals, doc="Continuous decision variable")

    # Define constants to avoid magic numbers
    obj_x_coeff = 2
    obj_y_coeff = 3
    constraint1_rhs = 6
    constraint2_rhs = 1
    constraint1_y_coeff = 2

    # Objective function: maximize 2x + 3y
    model.obj = Objective(
        expr=obj_x_coeff * model.x + obj_y_coeff * model.y,
        sense=maximize,
        doc="Maximize the objective function",
    )

    # Constraints
    model.c1 = Constraint(
        expr=model.x + constraint1_y_coeff * model.y <= constraint1_rhs,
        doc="First constraint: x + 2y ≤ 6",
    )
    model.c2 = Constraint(
        expr=model.x - model.y >= constraint2_rhs,
        doc="Second constraint: x - y ≥ 1",
    )

    return model


def _solve_model(model: ConcreteModel, solver_name: str = "glpk") -> Dict[str, Any]:
    """Solve the optimization model."""
    solver = SolverFactory(solver_name)
    if solver is None:
        raise RuntimeError(f"Solver '{solver_name}' is not available.")

    try:
        result = solver.solve(model, tee=False)
    except (ValueError, TypeError, AttributeError, RuntimeError) as e:
        error_msg = f"Solver '{solver_name}' failed to solve the model: {e}"
        raise RuntimeError(error_msg) from e

    x_val = value(model.x) if model.x.value is not None else None
    y_val = value(model.y) if model.y.value is not None else None
    obj_val = value(model.obj.expr) if model.obj.expr is not None else None

    return {
        "solver_status": str(result.solver.status),
        "termination_condition": str(result.solver.termination_condition),
        "success": result.solver.termination_condition == "optimal",
        "x_value": x_val,
        "y_value": y_val,
        "objective_value": obj_val,
    }


def _display_results(results: Dict[str, Any]) -> None:
    """Display the optimization results."""
    print("\n" + "=" * 50)
    print("SIMPLE OPTIMIZATION RESULTS")
    print("=" * 50)

    print(f"\nSolver Status: {results['solver_status']}")
    print(f"Termination Condition: {results['termination_condition']}")

    if results["success"]:
        print("\n✓ Optimal solution found!")
        print("\nDecision Variables:")
        if results["x_value"] is not None:
            print(f"  x (integer) = {results['x_value']:.2f}")
        else:
            print("  x (integer) = None")
        if results["y_value"] is not None:
            print(f"  y (continuous) = {results['y_value']:.2f}")
        else:
            print("  y (continuous) = None")
        if results["objective_value"] is not None:
            print(f"\nObjective Value: {results['objective_value']:.2f}")
        else:
            print("\nObjective Value: None")
    else:
        print("\n✗ No optimal solution found.")
        error_msg = (
            "  The problem may be infeasible, unbounded,"
            " or the solver encountered an error."
        )
        print(error_msg)

    print("\n" + "=" * 50)


def run_simple_example() -> int:
    """
    Run the original simple optimization example.

    Returns:
        Exit code (0 for success, 1 for error)
    """
    default_solver = "glpk"

    print("Simple Optimization Model")
    print("=" * 50)
    print("Problem: maximize 2x + 3y")
    print("Subject to:")
    print("  x + 2y ≤ 6")
    print("  x - y ≥ 1")
    print("  x ∈ ℤ⁺ (non-negative integers)")
    print("  y ∈ ℝ⁺ (non-negative reals)")
    print("=" * 50)

    try:
        model = _create_optimization_model()
        results = _solve_model(model, solver_name=default_solver)
        _display_results(results)
        return 0 if results["success"] else 1

    except RuntimeError as e:
        print(f"\nError: {e}", file=sys.stderr)
        print("\nPlease ensure you have a suitable solver installed.")
        print("You can install GLPK or use another solver like 'cbc'.")
        return 1
    except (ValueError, TypeError, AttributeError, ImportError) as e:
        print(f"\nUnexpected error: {e}", file=sys.stderr)
        return 1


def run_battery_dispatch(config_file: str, output_file: str = None) -> int:
    """
    Run battery dispatch optimization.

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
        config = ConfigLoader.from_file(config_file)
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except (KeyError, ValueError) as e:
        print(f"Invalid configuration: {e}", file=sys.stderr)
        return 1

    if output_file:
        config.output_file = output_file

    try:
        solver = BatteryDispatchSolver(config)
        results = solver.solve()
    except RuntimeError as e:
        print(f"Solver error: {e}", file=sys.stderr)
        print("\nMake sure CBC solver is installed:", file=sys.stderr)
        print("  conda install -c conda-forge coincbc", file=sys.stderr)
        print("  or", file=sys.stderr)
        print("  apt-get install coinor-cbc", file=sys.stderr)
        return 1

    ResultsHandler.print_summary(results)
    ResultsHandler.to_json(results, config.output_file)

    return 0


def _setup_argparse() -> argparse.ArgumentParser:
    """Set up command line argument parser."""
    parser = argparse.ArgumentParser(
        description="Pyomo Optimization Models",
        epilog="Examples:\n"
        "  python hbmaule.py simple          # Run simple example\n"
        "  python hbmaule.py battery config.json  # Run battery dispatch",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subparsers = parser.add_subparsers(dest="command", help="Command to run")

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

    # If no arguments, show help
    if len(sys.argv) == 1:
        parser.print_help()
        return 0

    args = parser.parse_args()

    if args.command == "simple":
        return run_simple_example()
    if args.command == "battery":
        return run_battery_dispatch(args.config_file, args.output)

    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
