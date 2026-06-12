"""Builder: ``batt_test_case_{b,c,d,e,f}`` (Battery degeneracy) variant.

The PowerSimulations.jl ``batt_test_case_*_sys`` family carries FIVE
sub-variants (named b, c, d, e, f) of a tiny single-bus battery test
fixture, exercising different corner cases of the Battery LP:

* Single REF bus ``nodeA``
* One ``PowerLoad`` (active = 0.4 p.u. for b/c/d/e, 0.2 p.u. for f)
* One ``RenewableDispatch`` wind unit
* One ``EnergyReservoirStorage`` battery with:
    storage_capacity = 7.0 MWh
    rating = 7.0 MW
    pmax_charge = pmax_discharge = 2.0 MW (input_active_power_limits)
    efficiency = (in = 0.80, out = 0.90)           ← ASYMMETRIC η
    storage_target = 0.2 p.u. (final-block SoC floor)
    initial_storage_capacity_level varies per subcase

Per-subcase differences (from
``PowerSystemCaseBuilder.jl/src/library/psitest_library.jl`` lines
5568-6011):

  case b: SoC_ini = 5.0,  surplus_cost = 10,    shortage_cost = 0.001
          wind = [0.5, 0.7, 0.8]   target = [0.4, 0.4, 0.1]
  case c: SoC_ini = 2.0,  surplus_cost = 0,     shortage_cost = 50
          wind = [0.9, 0.7, 0.8]   target = [0.0, 0.0, 0.4]
  case d: SoC_ini = 2.0,  surplus_cost = -10,   shortage_cost = 0
          wind = [0.9, 0.7, 0.8, 0.1]  target = [0,0,0,0] (4 blocks)
  case e: SoC_ini = 2.0,  surplus_cost = 50,    shortage_cost = 50
          wind = [0.9, 0.7, 0.8]   target = [0.2, 0.2, 0.0]
  case f: SoC_ini = 2.0,  surplus_cost = -5,    shortage_cost = 50
          wind = [0.9, 0.7, 0.8]   target = [0.0, 0.0, 0.3]   load = 0.2

gtopt has no direct ``storage_target`` (== final-block SoC) bound
field that varies per block, but it has ``efin`` (terminal SoC
floor) which maps onto the last-block target after scaling.  The
``energy_shortage_cost`` maps onto ``Battery.efin_cost`` (the soft
SoC-floor penalty); ``energy_surplus_cost`` has no direct gtopt
peer, so we either price it via ``ecost`` (when positive — penalty
for SoC above target, broadly interpreted as a storage usage cost)
or leave it unset (when negative — that's a reward for surplus,
gtopt doesn't model that natively).

The fixture is *deliberately degenerate*: with three blocks, a
tightly-constrained 2-MW charge/discharge cap, and asymmetric η,
the LP is dominated by the SoC trajectory.  Each subcase pins a
different operating regime:

* **b**: high initial SoC + surplus penalty → LP should DISCHARGE
  to hit the low end-target.
* **c**: low initial SoC + high end-target + shortage penalty →
  LP should CHARGE to reach the target (even via expensive wind
  curtailment in early blocks).
* **d**: 4-block extended horizon with surplus reward → tests SoC
  carrying behaviour.
* **e**: symmetric high penalties on BOTH sides → forces LP to
  hit the per-block target exactly.
* **f**: half-load + asymmetric penalties → tests with light
  loading.

The C++ side (``test_sienna_5bus_battery_degeneracy_port.cpp``)
verifies the LP solves cleanly for each subcase, and pins the
per-subcase dispatch direction (charge dominant, discharge dominant,
or balanced).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from sienna_to_gtopt._reader import SiennaCase

VALID_SUBCASES = ("b", "c", "d", "e", "f")

# In Sienna p.u. the base is 100 MVA but ``storage_capacity`` is
# already in MWh (not p.u.), so 7.0 means 7 MWh of usable battery
# energy.  Same for the 2.0 MW power ratings (they ARE p.u. of
# base_power = 100.0, so 0.02 p.u. but the storage ratings are
# specified in actual units — see PSY EnergyReservoirStorage docs).
# We pin the LP to the actual-MW values for clarity.
BATTERY_CAPACITY_MWH = 7.0
BATTERY_EMIN_MWH = 0.10
BATTERY_PMAX_CHARGE = 2.0
BATTERY_PMAX_DISCHARGE = 2.0
BATTERY_ETA_IN = 0.80
BATTERY_ETA_OUT = 0.90

# Sienna's per-p.u. load × base_power (100 MVA) = MW.  All cases use
# 0.4 p.u. except case f at 0.2 — but we apply the same scale ourselves
# so the JSON is human-readable as MW.  We pick "load × 50" so the
# 2 MW battery is interesting against the load: 0.4 * 50 = 20 MW load.
LOAD_SCALE = 50.0


@dataclass(frozen=True)
class BatterySubcaseSpec:
    """Per-subcase parameter set extracted from the Julia fixtures."""

    subcase: str
    soc_ini: float  # initial SoC (MWh)
    load_pu_base: float  # active load in p.u. (× LOAD_SCALE → MW)
    wind_profile: tuple[float, ...]  # per-block wind p.u. (× wind_capacity → MW)
    target_profile: tuple[float, ...]  # per-block storage target (p.u. → MWh)
    energy_shortage_cost: float  # $/MWh — shortfall below target
    energy_surplus_cost: float  # $/MWh — surplus above target


SUBCASE_SPECS: dict[str, BatterySubcaseSpec] = {
    "b": BatterySubcaseSpec(
        subcase="b",
        soc_ini=5.0,
        load_pu_base=0.4,
        wind_profile=(0.5, 0.7, 0.8),
        target_profile=(0.4, 0.4, 0.1),
        energy_shortage_cost=0.001,
        energy_surplus_cost=10.0,
    ),
    "c": BatterySubcaseSpec(
        subcase="c",
        soc_ini=2.0,
        load_pu_base=0.4,
        wind_profile=(0.9, 0.7, 0.8),
        target_profile=(0.0, 0.0, 0.4),
        energy_shortage_cost=50.0,
        energy_surplus_cost=0.0,
    ),
    "d": BatterySubcaseSpec(
        subcase="d",
        soc_ini=2.0,
        load_pu_base=0.4,
        wind_profile=(0.9, 0.7, 0.8, 0.1),
        target_profile=(0.0, 0.0, 0.0, 0.0),
        energy_shortage_cost=0.0,
        energy_surplus_cost=-10.0,
    ),
    "e": BatterySubcaseSpec(
        subcase="e",
        soc_ini=2.0,
        load_pu_base=0.4,
        wind_profile=(0.9, 0.7, 0.8),
        target_profile=(0.2, 0.2, 0.0),
        energy_shortage_cost=50.0,
        energy_surplus_cost=50.0,
    ),
    "f": BatterySubcaseSpec(
        subcase="f",
        soc_ini=2.0,
        load_pu_base=0.2,
        wind_profile=(0.9, 0.7, 0.8),
        target_profile=(0.0, 0.0, 0.3),
        energy_shortage_cost=50.0,
        energy_surplus_cost=-5.0,
    ),
}

# Wind unit p.u. → MW scale.  Picked so the wind plant alone can NEVER
# cover the full load every block — forces SoC to do real work.
WIND_CAPACITY_MW = 30.0


def build_battery_degeneracy(
    case: SiennaCase,  # pylint: disable=unused-argument
    *,
    subcase: str = "b",
) -> dict[str, Any]:
    """Emit the gtopt JSON for a battery degeneracy subcase.

    Parameters
    ----------
    case
        Parsed 5-bus bundle — unused (the batt_test_case_* fixtures
        are single-bus and synthesized entirely in Julia source), but
        accepted to keep the ``VariantBuilder`` signature uniform.
    subcase
        One of ``"b"``, ``"c"``, ``"d"``, ``"e"``, ``"f"``.
    """

    if subcase not in SUBCASE_SPECS:
        raise ValueError(
            f"unknown battery-degeneracy subcase {subcase!r}; "
            f"valid={list(SUBCASE_SPECS)}"
        )
    spec = SUBCASE_SPECS[subcase]
    n_blocks = len(spec.wind_profile)
    assert len(spec.target_profile) == n_blocks

    bus_array = [{"uid": 1, "name": "nodeA"}]

    # Single ``PowerLoad`` at p.u. × LOAD_SCALE.
    load_mw = spec.load_pu_base * LOAD_SCALE
    demand_array = [
        {
            "uid": 1,
            "name": "Bus1Load",
            "bus": 1,
            "capacity": load_mw,
        }
    ]

    # Single wind unit, capacity-scaled.  We bake the per-block wind
    # profile into pmax (a 2-D ``[stage][block]`` schedule) so the
    # LP sees the same time-shape as the Sienna fixture.  In the
    # absence of a profile parser we just use the inline form.
    wind_pmax_per_block = [round(w * WIND_CAPACITY_MW, 6) for w in spec.wind_profile]
    generator_array: list[dict[str, Any]] = [
        {
            "uid": 1,
            "name": "WindBusC",
            "bus": 1,
            "gcost": 0.220,  # Sienna LinearCurve(0.220) — matches Julia
            "pmax": [wind_pmax_per_block],  # [stage][block]
            "capacity": WIND_CAPACITY_MW,
        },
        # Backup thermal so the LP is feasible when wind + battery
        # together can't carry the load (corner cases d/e).
        {
            "uid": 2,
            "name": "thermal_backup",
            "bus": 1,
            "gcost": 100.0,
            "capacity": 200.0,
        },
    ]

    # End-of-horizon storage target (MWh) — corresponds to the LAST
    # entry of target_profile × capacity.  In gtopt this maps to
    # ``Battery.efin`` (a >=  constraint on terminal SoC) with the
    # ``energy_shortage_cost`` priced via ``efin_cost``.
    efin_mwh = spec.target_profile[-1] * BATTERY_CAPACITY_MWH

    battery: dict[str, Any] = {
        "uid": 1,
        "name": "Bat2",
        "type": "li-ion",
        "bus": 1,
        "input_efficiency": BATTERY_ETA_IN,
        "output_efficiency": BATTERY_ETA_OUT,
        "emin": BATTERY_EMIN_MWH,
        "emax": BATTERY_CAPACITY_MWH,
        "capacity": BATTERY_CAPACITY_MWH,
        "pmax_charge": BATTERY_PMAX_CHARGE,
        "pmax_discharge": BATTERY_PMAX_DISCHARGE,
        "eini": spec.soc_ini,
        "efin": efin_mwh,
        # Decouple stages: the daily_cycle default would add a
        # 24/duration scaling we don't want for a 3-block fixture.
        "daily_cycle": False,
    }
    if spec.energy_shortage_cost > 0.0:
        # Soft floor on terminal SoC: ``SoC_end + slack >= efin``,
        # slack priced at shortage_cost $/MWh.
        battery["efin_cost"] = spec.energy_shortage_cost

    # The Sienna ``energy_surplus_cost`` (penalty for SoC > target)
    # has no direct gtopt peer — when positive we approximate it via
    # ``ecost`` (per-MWh SoC usage cost); when negative (a reward),
    # we drop it to avoid emitting a negative objective coefficient
    # that could destabilize the LP.
    if spec.energy_surplus_cost > 0.0:
        battery["ecost"] = spec.energy_surplus_cost

    system = {
        "name": f"SiennaBattTestCase{spec.subcase.upper()}",
        "bus_array": bus_array,
        "demand_array": demand_array,
        "generator_array": generator_array,
        "battery_array": [battery],
    }

    simulation = {
        "block_array": [{"uid": i + 1, "duration": 1.0} for i in range(n_blocks)],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": n_blocks}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }

    return {
        "options": {
            "model_options": {
                "use_single_bus": True,
                "use_kirchhoff": False,
                "demand_fail_cost": 1000.0,
                "scale_objective": 1.0,
            }
        },
        "simulation": simulation,
        "system": system,
    }


__all__ = ["build_battery_degeneracy", "SUBCASE_SPECS", "VALID_SUBCASES"]
