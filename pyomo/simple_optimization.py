"""
Simple optimization model class.
"""

import sys
from typing import Dict, Any
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


class SimpleOptimization:
    """Handles the simple mixed-integer optimization example."""

    def __init__(self, solver_name: str = "glpk"):
        """Initialize with optional solver name."""
        self.solver_name = solver_name
        self.model = None

    def create_model(self) -> ConcreteModel:
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

        self.model = model
        return model

    def solve(self) -> Dict[str, Any]:
        """Solve the optimization model."""
        if self.model is None:
            self.create_model()

        solver = SolverFactory(self.solver_name)
        if solver is None:
            raise RuntimeError(f"Solver '{self.solver_name}' is not available.")

        try:
            result = solver.solve(self.model, tee=False)
        except (ValueError, TypeError, AttributeError, RuntimeError) as exc:
            error_msg = f"Solver '{self.solver_name}' failed to solve the model: {exc}"
            raise RuntimeError(error_msg) from exc

        x_val = value(self.model.x) if self.model.x.value is not None else None
        y_val = value(self.model.y) if self.model.y.value is not None else None
        obj_val = (
            value(self.model.obj.expr) if self.model.obj.expr is not None else None
        )

        return {
            "solver_status": str(result.solver.status),
            "termination_condition": str(result.solver.termination_condition),
            "success": result.solver.termination_condition == "optimal",
            "x_value": x_val,
            "y_value": y_val,
            "objective_value": obj_val,
        }

    def display_results(self, results: Dict[str, Any]) -> None:
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

    def run(self) -> int:
        """
        Run the complete simple optimization example.

        Returns:
            Exit code (0 for success, 1 for error)
        """
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
            self.create_model()
            results = self.solve()
            self.display_results(results)
            return 0 if results["success"] else 1

        except RuntimeError as exc:
            print(f"\nError: {exc}", file=sys.stderr)
            print("\nPlease ensure you have a suitable solver installed.")
            print("You can install GLPK or use another solver like 'cbc'.")
            return 1
        except (ValueError, TypeError, AttributeError, ImportError) as exc:
            print(f"\nUnexpected error: {exc}", file=sys.stderr)
            return 1
