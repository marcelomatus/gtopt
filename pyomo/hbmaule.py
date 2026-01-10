from pyomo.environ import (
    ConcreteModel,
    Var,
    Objective,
    Constraint,
    SolverFactory,
    NonNegativeReals,
    NonNegativeIntegers,
    maximize,
)

# Create a model
model = ConcreteModel()

# Decision variables
model.x = Var(domain=NonNegativeIntegers)
model.y = Var(domain=NonNegativeReals)

# Objective function
model.obj = Objective(expr=2 * model.x + 3 * model.y, sense=maximize)

# Constraints
model.c1 = Constraint(expr=model.x + 2 * model.y <= 6)
model.c2 = Constraint(expr=model.x - model.y >= 1)

# Solve the model
solver = SolverFactory("glpk")
result = solver.solve(model)

# Display results
print(f"Status: {result.solver.status}")
print(f"Optimal value of x: {model.x.value}")
print(f"Optimal value of y: {model.y.value}")
print(f"Maximum objective value: {model.obj()}")
