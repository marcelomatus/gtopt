"""Unit tests for the Python-side reservoir water-value pricing.

Exercises the shared :class:`gtopt_shared.water_values.WaterValueResolver`
wiring in ``plexos2gtopt.gtopt_writer``: the central-shape adapter, the
boundary-cut OVERWRITE vs auto ANCHOR estimate, ``build_reservoir_array``'s
``efin_cost`` emission (gated by ``soft_storage_bounds``), and the global
default-water-fail fallback.
"""

from __future__ import annotations

from plexos2gtopt.entities import (
    BoundaryCutSpec,
    ReservoirSpec,
    TurbineSpec,
)
from plexos2gtopt.gtopt_writer import (
    _PF_TO_CMD_EFFICIENCY,
    apply_default_water_fail,
    build_reservoir_array,
    build_water_value_resolver,
)


def _gens(*gcosts: float) -> list[dict[str, float]]:
    return [{"gcost": gc} for gc in gcosts]


def _demands(*volls: float) -> list[dict[str, float]]:
    return [{"fcost": v} for v in volls]


def test_resolver_anchor_from_generator_and_demand_costs() -> None:
    """ANCHOR = 0.75·avg_thermal_gcost + 0.25·min_falla_gcost, sourced
    from the already-built generator/demand entries."""
    resolver, numbers = build_water_value_resolver(
        reservoirs=(ReservoirSpec(object_id=1, name="A", emax=100.0),),
        turbines=(),
        generator_entries=_gens(20.0, 40.0),  # avg = 30
        demand_entries=_demands(500.0, 1000.0),  # min = 500
        boundary_cut=None,
    )
    # 0.75 * 30 + 0.25 * 500 = 22.5 + 125 = 147.5
    assert resolver.anchor == 147.5
    assert resolver.is_active is True
    assert numbers == {"A": 1}


def test_resolver_no_thermal_anchor_zero() -> None:
    """Anchor falls back to 0 when there are no thermal generators (a
    pure-renewable bundle) — the efin_cost gate then stays off."""
    resolver, _ = build_water_value_resolver(
        reservoirs=(ReservoirSpec(object_id=1, name="A", emax=100.0),),
        turbines=(),
        generator_entries=[],
        demand_entries=_demands(1000.0),
        boundary_cut=None,
    )
    assert resolver.anchor == 0.0


def test_reservoir_emits_auto_efin_cost_in_cmd_units() -> None:
    """A reservoir with a turbine + efin but no boundary cut gets the auto
    ``ANCHOR × production_factor × 24`` estimate in $/CMD."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="A", emax=100.0, eini=50.0, efin=40.0),
    )
    turbines = (
        TurbineSpec(
            generator_name="A_gen",
            reservoir_name="A",
            production_factor=2.0,  # MW per m³/s
        ),
    )
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),  # anchor = 0.75*30 + 0.25*500 = 147.5
        boundary_cut=None,
    )
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    # auto $/CMD = anchor * pf * 24 = 147.5 * 2 * 24 = 7080.0
    assert out[0]["efin"] == 40.0
    assert out[0]["efin_cost"] == 7080.0
    # Adapter scale sanity: efficiency * 1e6/3600 == pf * 24.
    assert abs(2.0 * _PF_TO_CMD_EFFICIENCY * 1e6 / 3600 - 2.0 * 24) < 1e-9


def test_soft_storage_bounds_true_prices_efin() -> None:
    """``soft_storage_bounds=True`` (the default): a reservoir with efin and
    a positive anchor gets a SOFT priced ``efin_cost`` slack."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="A", emax=100.0, eini=50.0, efin=40.0),
    )
    turbines = (
        TurbineSpec(generator_name="A_gen", reservoir_name="A", production_factor=2.0),
    )
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=None,
    )
    out = build_reservoir_array(
        reservoirs,
        soft_storage_bounds=True,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    assert out[0]["efin"] == 40.0
    assert out[0]["efin_cost"] == 7080.0  # anchor * pf * 24 = 147.5 * 2 * 24


def test_soft_storage_bounds_false_keeps_hard_efin() -> None:
    """``soft_storage_bounds=False``: the ``efin`` HARD floor is still emitted
    but NO ``efin_cost`` slack price is set (hard ``vol_end >= efin``)."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="A", emax=100.0, eini=50.0, efin=40.0),
        ReservoirSpec(object_id=2, name="B", emax=100.0, eini=50.0, efin=30.0),
    )
    turbines = (
        TurbineSpec(generator_name="A_gen", reservoir_name="A", production_factor=2.0),
        TurbineSpec(generator_name="B_gen", reservoir_name="B", production_factor=1.0),
    )
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=None,
    )
    out = build_reservoir_array(
        reservoirs,
        soft_storage_bounds=False,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    # Every reservoir keeps its hard efin floor; NONE gets a soft price.
    assert out[0]["efin"] == 40.0
    assert out[1]["efin"] == 30.0
    assert all("efin_cost" not in entry for entry in out)


def test_reservoir_without_turbine_gets_no_auto_efin_cost() -> None:
    """A reservoir with efin but NO turbine has zero cascade production factor,
    so the auto estimate is 0 and no ``efin_cost`` is emitted (the hard efin
    floor remains)."""
    reservoirs = (ReservoirSpec(object_id=1, name="A", emax=100.0, efin=40.0),)
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=(),  # no turbine ⇒ cascade_lost_pf == 0
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),  # anchor = 147.5 > 0
        boundary_cut=None,
    )
    assert resolver.anchor > 0.0
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    assert out[0]["efin"] == 40.0
    assert "efin_cost" not in out[0]


def test_soft_storage_bounds_false_suppresses_cut_price() -> None:
    """``soft_storage_bounds=False`` dominates even the boundary-cut OVERWRITE:
    a reservoir present in the cut still gets a HARD efin floor, no slack."""
    reservoirs = (ReservoirSpec(object_id=1, name="A", emax=100.0, efin=40.0),)
    turbines = (
        TurbineSpec(generator_name="A_gen", reservoir_name="A", production_factor=2.0),
    )
    boundary_cut = BoundaryCutSpec(fcf=1.0e6, slopes={"A": 1234.5})
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=boundary_cut,
    )
    out = build_reservoir_array(
        reservoirs,
        soft_storage_bounds=False,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    assert out[0]["efin"] == 40.0
    assert "efin_cost" not in out[0]


def test_reservoir_boundary_cut_overwrites_auto_estimate() -> None:
    """When a boundary cut prices the reservoir, its lower-bound water
    value ($/CMD) OVERWRITES the auto estimate."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="A", emax=100.0, eini=50.0, efin=40.0),
    )
    turbines = (
        TurbineSpec(generator_name="A_gen", reservoir_name="A", production_factor=2.0),
    )
    # Cut slope is the (positive) water value in $/CMD.
    boundary_cut = BoundaryCutSpec(fcf=1.0e6, slopes={"A": 1234.5})
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=boundary_cut,
    )
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    # cut LOWER bound = water value itself (single coeff -1234.5 → -max = 1234.5).
    assert out[0]["efin_cost"] == 1234.5


def test_reservoir_water_value_factor_scales_cut() -> None:
    """``--water-value-factor`` scales the cut slope before pricing."""
    reservoirs = (ReservoirSpec(object_id=1, name="A", emax=100.0, efin=40.0),)
    turbines = (
        TurbineSpec(generator_name="A_gen", reservoir_name="A", production_factor=2.0),
    )
    boundary_cut = BoundaryCutSpec(fcf=1.0e6, slopes={"A": 1000.0})
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=boundary_cut,
        water_value_factor={"A": 0.5},
    )
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    assert out[0]["efin_cost"] == 500.0


def test_never_drain_reservoir_gets_no_efin_cost() -> None:
    """``never_drain`` (ELTORO) keeps a HARD efin floor with NO soft price."""
    reservoirs = (
        ReservoirSpec(
            object_id=1,
            name="ELTORO",
            emax=12000.0,
            eini=12000.0,
            efin=11000.0,
            never_drain=True,
        ),
    )
    turbines = (
        TurbineSpec(
            generator_name="ELTORO_gen", reservoir_name="ELTORO", production_factor=2.0
        ),
    )
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=None,
    )
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    assert out[0]["efin"] == 11000.0
    assert "efin_cost" not in out[0]


def test_cascade_stops_at_next_reservoir_own_pf_fallback() -> None:
    """In PLEXOS every storage is an ``embalse``, so the shared resolver's
    cascade walk stops at the NEXT reservoir (exclusive) — each reservoir is
    priced on its OWN production factor (the documented junction-lost-pf
    fallback), NOT the summed downstream chain."""
    reservoirs = (
        ReservoirSpec(object_id=1, name="UP", emax=100.0, efin=40.0),
        ReservoirSpec(object_id=2, name="DOWN", emax=100.0, efin=40.0),
    )
    turbines = (
        TurbineSpec(
            generator_name="UP_gen",
            reservoir_name="UP",
            production_factor=1.0,
            tail_reservoir_name="DOWN",
        ),
        TurbineSpec(
            generator_name="DOWN_gen",
            reservoir_name="DOWN",
            production_factor=3.0,
        ),
    )
    resolver, numbers = build_water_value_resolver(
        reservoirs=reservoirs,
        turbines=turbines,
        generator_entries=_gens(30.0),
        demand_entries=_demands(500.0),
        boundary_cut=None,
    )
    # Cascade walk stops at DOWN (the next embalse), so UP contributes only
    # its own pf=1; DOWN contributes its own pf=3.  Anchor = 147.5.
    assert resolver.cascade_lost_pf(numbers["UP"]) == 1.0 * _PF_TO_CMD_EFFICIENCY
    assert resolver.cascade_lost_pf(numbers["DOWN"]) == 3.0 * _PF_TO_CMD_EFFICIENCY
    out = build_reservoir_array(
        reservoirs,
        water_value_resolver=resolver,
        reservoir_numbers=numbers,
    )
    by_name = {r["name"]: r for r in out}
    assert by_name["UP"]["efin_cost"] == 147.5 * 1 * 24
    assert by_name["DOWN"]["efin_cost"] == 147.5 * 3 * 24


def test_apply_default_water_fail_patches_unpriced_reservoirs() -> None:
    """The global default = max(efin_cost) is applied to reservoirs that
    carry efin but were left un-priced."""
    entries = [
        {"name": "A", "efin": 40.0, "efin_cost": 1000.0},
        {"name": "B", "efin": 50.0},  # un-priced
        {"name": "C", "efin": 0.0},  # no terminal target → untouched
    ]
    patched = apply_default_water_fail(entries)
    assert patched == 1
    assert entries[1]["efin_cost"] == 1000.0
    assert "efin_cost" not in entries[2]


def test_apply_default_water_fail_noop_without_priced_reservoir() -> None:
    """No-op when no reservoir carries a positive efin_cost (e.g. non-hydro
    or anchor==0 bundle)."""
    entries = [{"name": "A", "efin": 40.0}]
    assert apply_default_water_fail(entries) == 0
    assert "efin_cost" not in entries[0]
    assert apply_default_water_fail([]) == 0
