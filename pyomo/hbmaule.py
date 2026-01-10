"""
A Pyomo model for solving a mixed-integer linear programming example.
This model demonstrates a simple optimization problem with integer and continuous variables.
"""

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
import sys


def create_optimization_model():
    """
    Create and configure the optimization model.
    
    Returns:
        ConcreteModel: Configured Pyomo model
    """
    model = ConcreteModel(name="HBMaule_Optimization")
    
    # Decision variables
    model.x = Var(domain=NonNegativeIntegers, doc="Integer decision variable")
    model.y = Var(domain=NonNegativeReals, doc="Continuous decision variable")
    
    # Objective function: maximize 2x + 3y
    model.obj = Objective(
        expr=2 * model.x + 3 * model.y,
        sense=maximize,
        doc="Maximize the objective function"
    )
    
    # Constraints
    model.c1 = Constraint(
        expr=model.x + 2 * model.y <= 6,
        doc="First constraint: x + 2y ≤ 6"
    )
    model.c2 = Constraint(
        expr=model.x - model.y >= 1,
        doc="Second constraint: x - y ≥ 1"
    )
    
    return model


def solve_model(model, solver_name="glpk"):
    """
    Solve the optimization model.
    
    Args:
        model (ConcreteModel): Pyomo model to solve
        solver_name (str): Name of the solver to use
    
    Returns:
        dict: Results dictionary containing solution information
    """
    # Check if solver is available
    solver = SolverFactory(solver_name)
    if solver is None:
        raise RuntimeError(f"Solver '{solver_name}' is not available.")
    
    # Check if the solver is actually available by trying to get its name
    # Some solvers may be None, others may be unavailable
    try:
        # Solve the model
        result = solver.solve(model, tee=False)  # tee=False suppresses solver output
    except Exception as e:
        raise RuntimeError(f"Solver '{solver_name}' failed to solve the model: {e}")
    
    # Collect results
    x_val = value(model.x) if model.x.value is not None else None
    y_val = value(model.y) if model.y.value is not None else None
    obj_val = value(model.obj.expr) if model.obj.expr is not None else None
    
    results = {
        "solver_status": str(result.solver.status),
        "termination_condition": str(result.solver.termination_condition),
        "success": result.solver.termination_condition == "optimal",
        "x_value": x_val,
        "y_value": y_val,
        "objective_value": obj_val,
    }
    
    return results


def display_results(results):
    """
    Display the optimization results in a formatted way.
    
    Args:
        results (dict): Results dictionary from solve_model
    """
    print("\n" + "="*50)
    print("OPTIMIZATION RESULTS")
    print("="*50)
    
    print(f"\nSolver Status: {results['solver_status']}")
    print(f"Termination Condition: {results['termination_condition']}")
    
    if results['success']:
        print("\n✓ Optimal solution found!")
        print(f"\nDecision Variables:")
        if results['x_value'] is not None:
            print(f"  x (integer) = {results['x_value']:.2f}")
        else:
            print(f"  x (integer) = None")
        if results['y_value'] is not None:
            print(f"  y (continuous) = {results['y_value']:.2f}")
        else:
            print(f"  y (continuous) = None")
        if results['objective_value'] is not None:
            print(f"\nObjective Value: {results['objective_value']:.2f}")
        else:
            print(f"\nObjective Value: None")
        
        # Display constraint satisfaction
        print("\nConstraint Analysis:")
        # Note: In a real application, you would compute constraint values here
        print("  (Values would be computed with the solution)")
    else:
        print("\n✗ No optimal solution found.")
        print("  The problem may be infeasible, unbounded, or the solver encountered an error.")
    
    print("\n" + "="*50)


def main():
    """
    Main function to run the optimization example.
    """
    print("HBMaule Optimization Model")
    print("="*50)
    print("Problem: maximize 2x + 3y")
    print("Subject to:")
    print("  x + 2y ≤ 6")
    print("  x - y ≥ 1")
    print("  x ∈ ℤ⁺ (non-negative integers)")
    print("  y ∈ ℝ⁺ (non-negative reals)")
    print("="*50)
    
    try:
        # Create the model
        model = create_optimization_model()
        
        # Solve the model
        results = solve_model(model, solver_name="glpk")
        
        # Display results
        display_results(results)
        
        # Return appropriate exit code
        return 0 if results['success'] else 1
        
    except RuntimeError as e:
        print(f"\nError: {e}", file=sys.stderr)
        print("\nPlease ensure you have a suitable solver installed.")
        print("You can install GLPK or use another solver like 'cbc'.")
        return 1
    except Exception as e:
        print(f"\nUnexpected error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
