"""
Battery dispatch optimization model using Pyomo.
"""

from typing import List, Tuple

import pyomo.environ as pyo
from pyomo.environ import ConcreteModel, Var, Objective, Constraint, NonNegativeReals

from .config import OptimizationConfig


class BatteryDispatchModel:
    """Pyomo model for battery dispatch optimization."""

    def __init__(self, config: OptimizationConfig):
        self.config = config
        self.model = None
        self.n_periods = len(config.time_series.marginal_costs_usd_per_mwh)
        self.time_durations = config.time_series.time_durations_hours

    def build(self) -> None:
        """Build the Pyomo optimization model."""
        m = ConcreteModel(name="BatteryDispatch")

        # Time periods
        m.T = pyo.RangeSet(0, self.n_periods - 1)

        # Decision variables
        # Charge power (MW) at each time period
        m.charge = Var(
            m.T,
            within=NonNegativeReals,
            bounds=(0, self.config.battery.max_charge_rate_mw),
        )

        # Discharge power (MW) at each time period
        m.discharge = Var(
            m.T,
            within=NonNegativeReals,
            bounds=(0, self.config.battery.max_discharge_rate_mw),
        )

        # State of charge (MWh) at each time period
        m.soc = Var(
            m.T,
            within=NonNegativeReals,
            bounds=(self.config.battery.min_soc_mwh, self.config.battery.max_soc_mwh),
        )

        # Binary variable to prevent simultaneous charge/discharge
        m.charge_binary = Var(m.T, within=pyo.Binary)
        m.discharge_binary = Var(m.T, within=pyo.Binary)

        # Objective: minimize total cost
        marginal_costs = self.config.time_series.marginal_costs_usd_per_mwh
        time_durations = self.time_durations

        def objective_rule(model):
            # Cost = sum over time of (discharge - charge) * marginal_cost * duration
            # Discharge earns money (negative cost), charge costs money
            # Multiply by duration to account for variable time intervals
            return sum(
                (model.discharge[t] - model.charge[t])
                * marginal_costs[t]
                * time_durations[t]
                for t in model.T
            )

        m.objective = Objective(rule=objective_rule, sense=pyo.minimize)

        # Constraints

        # SOC evolution
        def soc_evolution_rule(model, t):
            if t == 0:
                # Initial SOC
                # Energy = power * time, so multiply by duration
                return model.soc[t] == (
                    self.config.battery.initial_soc_mwh
                    + model.charge[t]
                    * self.config.battery.charge_efficiency
                    * time_durations[t]
                    - model.discharge[t]
                    / self.config.battery.discharge_efficiency
                    * time_durations[t]
                )
            # Subsequent periods
            return model.soc[t] == (
                model.soc[t - 1]
                + model.charge[t]
                * self.config.battery.charge_efficiency
                * time_durations[t]
                - model.discharge[t]
                / self.config.battery.discharge_efficiency
                * time_durations[t]
            )

        m.soc_evolution = Constraint(m.T, rule=soc_evolution_rule)

        # Final SOC constraint (optional: could be same as initial)
        def final_soc_rule(model):
            return model.soc[self.n_periods - 1] >= self.config.battery.initial_soc_mwh

        m.final_soc = Constraint(rule=final_soc_rule)

        # Prevent simultaneous charge and discharge
        def no_simultaneous_rule(model, t):
            return model.charge_binary[t] + model.discharge_binary[t] <= 1

        m.no_simultaneous = Constraint(m.T, rule=no_simultaneous_rule)

        # Link binary variables to power variables
        # Power limits remain in MW (power), not affected by time duration
        def charge_linking_rule(model, t):
            return (
                model.charge[t]
                <= model.charge_binary[t] * self.config.battery.max_charge_rate_mw
            )

        def discharge_linking_rule(model, t):
            return (
                model.discharge[t]
                <= model.discharge_binary[t] * self.config.battery.max_discharge_rate_mw
            )

        m.charge_linking = Constraint(m.T, rule=charge_linking_rule)
        m.discharge_linking = Constraint(m.T, rule=discharge_linking_rule)

        self.model = m

    def get_variables(self) -> Tuple[List[float], List[float], List[float]]:
        """Extract solution variables."""
        if self.model is None:
            raise RuntimeError("Model not built. Call build() first.")

        charge = [pyo.value(self.model.charge[t]) for t in self.model.T]
        discharge = [pyo.value(self.model.discharge[t]) for t in self.model.T]
        soc = [pyo.value(self.model.soc[t]) for t in self.model.T]

        return charge, discharge, soc
