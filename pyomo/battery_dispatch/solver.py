"""
Solver wrapper for battery dispatch optimization.
"""

import logging
from typing import Dict, Any

import pyomo.environ as pyo

from .config import OptimizationConfig
from .model import BatteryDispatchModel

logger = logging.getLogger(__name__)


class BatteryDispatchSolver:
    """Solver for battery dispatch optimization."""

    def __init__(self, config: OptimizationConfig):
        self.config = config
        self.model_wrapper = BatteryDispatchModel(config)
        self.solution = None

    def solve(self) -> Dict[str, Any]:
        """Solve the optimization problem."""
        # Build the model
        self.model_wrapper.build()
        model = self.model_wrapper.model

        # Setup solver
        solver = pyo.SolverFactory(self.config.solver_name)

        if solver is None:
            raise RuntimeError(
                f"Solver '{self.config.solver_name}' not available. "
                f"Install with 'conda install -c conda-forge coincbc' or similar."
            )

        # Solve
        logger.info("Solving with %s...", self.config.solver_name)
        result = solver.solve(model, tee=True)

        # Check solution status
        if result.solver.termination_condition == pyo.TerminationCondition.optimal:
            logger.info("Optimal solution found.")
            self.solution = result
        else:
            logger.warning(
                "Solver terminated with: %s", result.solver.termination_condition
            )
            self.solution = result

        # Extract results
        charge, discharge, soc = self.model_wrapper.get_variables()
        objective_value = pyo.value(model.objective)

        results = {
            "status": str(result.solver.status),
            "termination_condition": str(result.solver.termination_condition),
            "objective_value_usd": objective_value,
            "charge_mw": charge,
            "discharge_mw": discharge,
            "soc_mwh": soc,
            "time_durations_hours": self.config.time_series.time_durations_hours,
            "time_periods": self.config.time_series.time_periods,
            "marginal_costs_usd_per_mwh": self.config.time_series.marginal_costs_usd_per_mwh,
        }

        return results
