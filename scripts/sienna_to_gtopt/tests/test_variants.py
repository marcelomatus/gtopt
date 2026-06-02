"""Schema-shape tests for the three Sienna variant builders.

These tests don't run the LP — they assert that the JSON each
builder emits is well-formed gtopt input:

* required top-level keys are present (``options``, ``simulation``,
  ``system``),
* the system block lists the expected element arrays for that
  variant,
* UIDs are unique within each array,
* the variant-specific knob (cascade adjacency / enforce_level / DC
  flag) lands on the correct elements.
"""

from __future__ import annotations

import json

import pytest

from sienna_to_gtopt import VARIANTS
from sienna_to_gtopt._battery_degeneracy import (
    SUBCASE_SPECS,
    VALID_SUBCASES,
    build_battery_degeneracy,
)
from sienna_to_gtopt._bundle import extract_bundle
from sienna_to_gtopt._cascading_hydro import build_cascading_hydro
from sienna_to_gtopt._fuel_cost_ts import build_fuel_cost_ts
from sienna_to_gtopt._hvdc import build_hvdc
from sienna_to_gtopt._interruptible_load import build_interruptible_load
from sienna_to_gtopt._monitored_line import build_monitored_line
from sienna_to_gtopt._pumped_storage import build_pumped_storage
from sienna_to_gtopt._reader import load_case
from sienna_to_gtopt._wecc_240 import (
    DEFAULT_N_BUS,
    DEFAULT_N_DEMAND,
    DEFAULT_N_GEN,
    DEFAULT_N_LINE,
    build_wecc_240,
)


def _case():
    case_dir = extract_bundle("5bus")
    return load_case(case_dir)


def _assert_top_level(payload):
    assert isinstance(payload, dict)
    for key in ("options", "simulation", "system"):
        assert key in payload, f"missing top-level key {key!r}"
    sim = payload["simulation"]
    assert isinstance(sim.get("block_array"), list) and sim["block_array"]
    assert isinstance(sim.get("stage_array"), list) and sim["stage_array"]
    assert isinstance(sim.get("scenario_array"), list) and sim["scenario_array"]


def _assert_unique_uids(elements, label):
    uids = [e["uid"] for e in elements if "uid" in e]
    assert len(uids) == len(set(uids)), f"duplicate uids in {label}"


def _assert_json_serializable(payload):
    # gtopt input JSON is loaded by daw::json with StrictParsePolicy —
    # so it must be plain-stdlib-json serializable end-to-end.
    text = json.dumps(payload)
    assert isinstance(text, str) and text


def test_variant_registry_complete():
    assert set(VARIANTS.keys()) == {
        "cascading_hydro",
        "monitored_line",
        "hvdc",
        "pumped_storage",
        "battery_degeneracy",
        "interruptible_load",
        "fuel_cost_ts",
        "wecc_240",
    }
    for builder in VARIANTS.values():
        assert callable(builder)


def test_cascading_hydro_schema():
    case = _case()
    payload = build_cascading_hydro(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    # All four hydro-topology primitives populated.
    assert system.get("junction_array")
    assert system.get("waterway_array")
    assert system.get("reservoir_array")
    assert system.get("turbine_array")
    assert system.get("flow_array")

    # 3 hydro plants → 3 junctions + 1 drain.
    assert len(system["junction_array"]) == 4
    drains = [j for j in system["junction_array"] if j.get("drain")]
    assert len(drains) == 1

    # 3 plants × 2 waterways each (turbine + spill) = 6.
    assert len(system["waterway_array"]) == 6
    # 3 reservoirs + 3 turbines + 3 hydro generators.
    assert len(system["reservoir_array"]) == 3
    assert len(system["turbine_array"]) == 3

    # All turbines reference an existing waterway + generator.
    ww_uids = {w["uid"] for w in system["waterway_array"]}
    gen_uids = {g["uid"] for g in system["generator_array"]}
    for turbine in system["turbine_array"]:
        assert turbine["waterway"] in ww_uids
        assert turbine["generator"] in gen_uids
    # Each reservoir attaches to an existing junction.
    j_uids = {j["uid"] for j in system["junction_array"]}
    for rsv in system["reservoir_array"]:
        assert rsv["junction"] in j_uids

    for label in (
        "bus_array",
        "generator_array",
        "junction_array",
        "waterway_array",
        "reservoir_array",
        "turbine_array",
    ):
        _assert_unique_uids(system[label], label)


def test_monitored_line_schema():
    case = _case()
    payload = build_monitored_line(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    lines = system.get("line_array", [])
    # Source bundle has 7 branches → 7 lines.
    assert len(lines) == 7
    # Exactly ONE line carries enforce_level = 2; the rest are 0.
    enforced = [ln for ln in lines if ln.get("enforce_level") == 2]
    relaxed = [ln for ln in lines if ln.get("enforce_level") == 0]
    assert len(enforced) == 1
    assert len(relaxed) == 6
    # The monitored line defaults to the CSV's first row → "branch4".
    assert enforced[0]["name"] == "branch4"
    _assert_unique_uids(lines, "line_array")


def test_monitored_line_custom_uid_override():
    case = _case()
    payload = build_monitored_line(case, monitored_branch_uid="branch7")
    enforced = [
        ln for ln in payload["system"]["line_array"] if ln["enforce_level"] == 2
    ]
    assert len(enforced) == 1
    assert enforced[0]["name"] == "branch7"


def test_hvdc_schema():
    case = _case()
    payload = build_hvdc(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)
    # Sanity: the HVDC variant must enable Kirchhoff (so the LP
    # actually exercises the KVL skip on the DC link).
    assert payload["options"]["model_options"]["use_kirchhoff"] is True

    lines = payload["system"]["line_array"]
    assert len(lines) == 7
    dc = [ln for ln in lines if ln.get("type") == "dc"]
    ac = [ln for ln in lines if ln.get("type") == "ac"]
    assert len(dc) == 1
    assert len(ac) == 6
    # The DC line drops reactance; AC lines keep it.
    assert "reactance" not in dc[0]
    for ln in ac:
        assert ln.get("reactance", 0.0) > 0.0


def test_hvdc_custom_uid_override():
    case = _case()
    payload = build_hvdc(case, dc_branch_uid="branch3")
    dc = [ln for ln in payload["system"]["line_array"] if ln.get("type") == "dc"]
    assert len(dc) == 1
    assert dc[0]["name"] == "branch3"


# ── pumped_storage ───────────────────────────────────────────────────


def test_pumped_storage_schema():
    case = _case()
    payload = build_pumped_storage(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    # The PHES variant defines a single battery, two generators (cheap +
    # peaker), one demand on the primary bus, and uses single-bus mode.
    batteries = system.get("battery_array", [])
    assert len(batteries) == 1
    bat = batteries[0]
    # Round-trip efficiency components — both < 1 (lossy).
    assert 0.0 < bat["input_efficiency"] < 1.0
    assert 0.0 < bat["output_efficiency"] < 1.0
    assert bat["emax"] >= bat["eini"] >= bat["emin"]
    assert bat.get("type") == "pumped"

    gens = system.get("generator_array", [])
    assert len(gens) == 2
    gcosts = sorted(g["gcost"] for g in gens)
    assert gcosts[1] > gcosts[0]  # peaker > cheap

    sim = payload["simulation"]
    # Two-block horizon — see _pumped_storage docstring.
    assert len(sim["block_array"]) == 2

    for label in ("bus_array", "generator_array", "demand_array", "battery_array"):
        _assert_unique_uids(system[label], label)


# ── battery_degeneracy ───────────────────────────────────────────────


@pytest.mark.parametrize("subcase", VALID_SUBCASES)
def test_battery_degeneracy_schema(subcase):
    case = _case()
    payload = build_battery_degeneracy(case, subcase=subcase)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    batteries = system.get("battery_array", [])
    assert len(batteries) == 1
    bat = batteries[0]

    spec = SUBCASE_SPECS[subcase]
    assert bat["eini"] == pytest.approx(spec.soc_ini)
    assert bat["input_efficiency"] != bat["output_efficiency"]  # asymmetric η
    # efin == last target × capacity.
    assert bat["efin"] == pytest.approx(spec.target_profile[-1] * bat["emax"])

    sim = payload["simulation"]
    assert len(sim["block_array"]) == len(spec.wind_profile)

    # Asymmetric η is a corner case the LP must handle (no equal-
    # efficiency degeneracy on the round-trip).
    assert bat["input_efficiency"] < 1.0
    assert bat["output_efficiency"] < 1.0


def test_battery_degeneracy_unknown_subcase_raises():
    case = _case()
    with pytest.raises(ValueError):
        build_battery_degeneracy(case, subcase="z")


# ── interruptible_load ───────────────────────────────────────────────


def test_interruptible_load_schema():
    case = _case()
    payload = build_interruptible_load(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    demands = system.get("demand_array", [])
    # At least one mandatory load (carrying no fcost or high fcost) +
    # exactly ONE interruptible load (carrying our priced fcost).
    interruptible = [d for d in demands if d.get("type") == "interruptible"]
    assert len(interruptible) == 1
    il = interruptible[0]
    # The fcost must be set and below the most-expensive thermal gen
    # (else the LP would never curtail it, defeating the variant).
    assert "fcost" in il
    gen_gcosts = [g["gcost"] for g in system["generator_array"]]
    assert il["fcost"] < max(gen_gcosts)


def test_interruptible_load_custom_fcost_override():
    case = _case()
    payload = build_interruptible_load(case, il_fcost=12.5, il_capacity=33.0)
    il = next(
        d for d in payload["system"]["demand_array"] if d.get("type") == "interruptible"
    )
    assert il["fcost"] == 12.5
    assert il["capacity"] == 33.0


# ── fuel_cost_ts ─────────────────────────────────────────────────────


def test_fuel_cost_ts_schema():
    case = _case()
    payload = build_fuel_cost_ts(case)
    _assert_top_level(payload)
    _assert_json_serializable(payload)

    system = payload["system"]
    fuels = system.get("fuel_array", [])
    assert len(fuels) == 1
    assert "price" in fuels[0]

    gens = system["generator_array"]
    fueled = [g for g in gens if "fuel" in g]
    assert len(fueled) >= 1
    gas_ts = next(g for g in gens if g["name"] == "gas_ts")
    # Per-block gcost schedule — list-of-lists shape.
    assert isinstance(gas_ts["gcost"], list)
    assert isinstance(gas_ts["gcost"][0], list)
    sim = payload["simulation"]
    assert len(sim["block_array"]) == len(gas_ts["gcost"][0])


def test_fuel_cost_ts_custom_horizon():
    case = _case()
    payload = build_fuel_cost_ts(
        case, heat_rate=10.0, fuel_price_per_block=(2.0, 4.0, 1.0)
    )
    sim = payload["simulation"]
    assert len(sim["block_array"]) == 3
    gas_ts = next(
        g for g in payload["system"]["generator_array"] if g["name"] == "gas_ts"
    )
    # heat_rate × price = 10 × [2,4,1] = [20,40,10].
    assert gas_ts["gcost"][0] == [20.0, 40.0, 10.0]


# ── wecc_240 ─────────────────────────────────────────────────────────


def test_wecc_240_default_topology():
    case = _case()
    payload = build_wecc_240(case)
    _assert_top_level(payload)
    # JSON serializability sanity (the payload is large — ~600 elements
    # — so this is the most important guard we have).
    _assert_json_serializable(payload)

    system = payload["system"]
    assert len(system["bus_array"]) == DEFAULT_N_BUS
    assert len(system["generator_array"]) == DEFAULT_N_GEN
    assert len(system["demand_array"]) == DEFAULT_N_DEMAND
    assert len(system["line_array"]) == DEFAULT_N_LINE
    for label in ("bus_array", "generator_array", "demand_array", "line_array"):
        _assert_unique_uids(system[label], label)


def test_wecc_240_small_override_for_speed():
    """Smaller override — used by faster pytest runs."""

    case = _case()
    payload = build_wecc_240(case, n_bus=20, n_gen=10, n_demand=10, n_line=25)
    system = payload["system"]
    assert len(system["bus_array"]) == 20
    assert len(system["generator_array"]) == 10
    assert len(system["demand_array"]) == 10
    assert len(system["line_array"]) == 25


def test_wecc_240_invalid_sizes_raise():
    case = _case()
    # n_line < n_bus — under-connected ring is rejected.
    with pytest.raises(ValueError):
        build_wecc_240(case, n_bus=20, n_gen=10, n_demand=10, n_line=10)
    # n_gen > n_bus.
    with pytest.raises(ValueError):
        build_wecc_240(case, n_bus=5, n_gen=10, n_demand=5, n_line=20)
