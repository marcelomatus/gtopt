"""Unit tests for :mod:`plexos2gtopt.gtopt_writer`.

These focus on the per-class array builders — the integration-test
suite covers the end-to-end ``build_planning`` → ``gtopt --lp-only``
path against the real bundle.
"""

from __future__ import annotations

from plexos2gtopt.entities import (
    BatterySpec,
    DemandSpec,
    FuelSpec,
    GeneratorSpec,
    LineSpec,
)
from plexos2gtopt.gtopt_writer import (
    build_battery_array,
    build_demand_array,
    build_fuel_array,
    build_generator_array,
    build_line_array,
)


def test_generator_gcost_thermal() -> None:
    """Thermal generator with a Fuel FK + heat_rate: emit the FK + the
    NON-FUEL part of variable cost as ``gcost``.

    gtopt computes ``effective_gcost = fuel.price × heat_rate + gcost``
    at LP-build (Generator + FuelLP coupling).  Pre-baking
    ``heat_rate × price`` into ``gcost`` here would double-count once
    gtopt resolves the fuel and would also hide the Fuel FK that
    ``FuelLP.max_offtake`` needs to find the generator at cap-row
    build time.  Numerical result is identical (0.25 × 1500 + 8 = 383
    at LP solve), but the JSON now carries ``fuel`` + ``heat_rate``
    explicitly.
    """
    fuels = (FuelSpec(object_id=1, name="diesel", price=1500.0),)
    gens = (
        GeneratorSpec(
            object_id=10,
            name="t1",
            bus_name="b1",
            pmax=100.0,
            heat_rate=0.25,
            vom_charge=8.0,
            fuel_names=("diesel",),
        ),
    )
    out = build_generator_array(gens, fuels)
    assert out[0]["fuel"] == "diesel"
    assert out[0]["heat_rate"] == 0.25
    # gcost is the non-fuel adder only (VOM + transport).  Fuel cost
    # is recovered as `fuel.price × heat_rate` at LP-build.
    assert out[0]["gcost"] == 8.0


def test_generator_gcost_renewable() -> None:
    """Renewable (no Fuel membership) gets gcost = vom_charge only."""
    gens = (
        GeneratorSpec(
            object_id=11,
            name="solar",
            bus_name="b2",
            pmax=50.0,
            heat_rate=0.0,  # PLEXOS ships zero heat-rate for renewables
            vom_charge=0.5,  # minor maintenance cost
            fuel_names=(),
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert out[0]["gcost"] == 0.5


def test_generator_unknown_fuel_does_not_crash() -> None:
    """A thermal whose fuel isn't in the FuelSpec list still emits an entry.

    The cost coefficient falls back to ``vom_charge`` alone — better
    than crashing, and easy to spot in downstream diagnostics.
    """
    gens = (
        GeneratorSpec(
            object_id=12,
            name="orphan",
            bus_name="b3",
            pmax=20.0,
            heat_rate=0.5,
            vom_charge=3.0,
            fuel_names=("missing_fuel",),
        ),
    )
    out = build_generator_array(gens, fuels=(FuelSpec(object_id=1, name="diesel"),))
    # fuel_price lookup misses → 0 → 0.5*0 + 3.0 = 3.0.
    assert out[0]["gcost"] == 3.0


def test_line_uses_tmax_ba_for_reverse() -> None:
    """build_line_array converts negative tmin → positive tmax_ba (EL=2)."""
    lines = (
        LineSpec(
            object_id=1,
            name="l_ab",
            bus_from="a",
            bus_to="b",
            tmax_ab=200.0,
            tmin_ab=-150.0,
            units=1,
            reactance=0.12,
            enforce_limits=2,  # explicit hard cap; EL=1 is unenforced
        ),
    )
    out = build_line_array(lines)
    assert out[0]["tmax_ab"] == 200.0
    assert out[0]["tmax_ba"] == 150.0
    assert out[0]["reactance"] == 0.12


def test_line_units_scales_capacity() -> None:
    """Parallel-line count multiplies both ratings (EL=2)."""
    lines = (
        LineSpec(
            object_id=2,
            name="dbl",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            tmin_ab=-100.0,
            units=2,
            enforce_limits=2,  # explicit hard cap; EL=1 is unenforced
        ),
    )
    out = build_line_array(lines)
    assert out[0]["tmax_ab"] == 200.0
    assert out[0]["tmax_ba"] == 200.0


def test_battery_emits_efficiency_and_power() -> None:
    """P2: build_battery_array writes pmax_charge / input_efficiency."""
    batts = (
        BatterySpec(
            object_id=1,
            name="b1",
            bus_name="bus_a",
            emax=10.0,
            eini=5.0,
            pmax_charge=2.0,
            pmax_discharge=2.0,
            input_efficiency=0.95,
            output_efficiency=0.95,
        ),
    )
    out = build_battery_array(batts)
    assert out[0]["pmax_charge"] == 2.0
    assert out[0]["pmax_discharge"] == 2.0
    assert out[0]["input_efficiency"] == 0.95
    assert out[0]["output_efficiency"] == 0.95


def test_battery_omits_unity_efficiency() -> None:
    """No efficiency keys when both round-trip halves are 1.0."""
    batts = (
        BatterySpec(
            object_id=2,
            name="b2",
            bus_name="bus_a",
            emax=10.0,
            pmax_charge=1.0,
            pmax_discharge=1.0,
        ),
    )
    out = build_battery_array(batts)
    assert "input_efficiency" not in out[0]
    assert "output_efficiency" not in out[0]


# ---------------------------------------------------------------------------
# Battery commitment-pmin plumbing
# ---------------------------------------------------------------------------
#
# PLEXOS ``Min Charge Level`` / ``Min Discharge Level`` are *commitment-
# conditional*: they fire only when the battery is actively
# charging / discharging.  ``build_battery_array`` mirrors this by
# emitting ``pmin_charge`` / ``pmin_discharge`` and flipping
# ``commitment: true`` whenever either pmin is positive.  Without the
# commitment flag, gtopt would treat the floor as an always-on
# constraint and force the battery to operate at >= pmin every block
# (infeasible the moment the LP wants the battery idle).
#
# Coverage: both pmins set, only one set, neither set, AND the static
# floors stay out of the JSON when pmin == 0.


def test_battery_emits_pmin_and_commitment_when_both_set() -> None:
    """Both pmin_charge and pmin_discharge > 0 → both keys plus
    ``commitment: true`` end up on the JSON entry."""
    batts = (
        BatterySpec(
            object_id=10,
            name="bess_committed",
            bus_name="bus_a",
            emax=100.0,
            eini=50.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            pmin_charge=2.5,
            pmin_discharge=2.0,
            input_efficiency=0.95,
            output_efficiency=0.95,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert entry["pmin_charge"] == 2.5
    assert entry["pmin_discharge"] == 2.0
    assert entry["commitment"] is True


def test_battery_omits_commitment_when_no_pmin() -> None:
    """A battery with both pmins = 0 must NOT carry ``commitment``
    or ``pmin_*`` keys — keeping the LP linear (no integer columns)
    and avoiding the per-block ``load >= 0`` no-op floor."""
    batts = (
        BatterySpec(
            object_id=11,
            name="bess_linear",
            bus_name="bus_a",
            emax=100.0,
            eini=50.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            pmin_charge=0.0,
            pmin_discharge=0.0,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert "pmin_charge" not in entry
    assert "pmin_discharge" not in entry
    assert "commitment" not in entry


def test_battery_commitment_emitted_when_only_pmin_charge() -> None:
    """A single non-zero pmin (charge side only) is enough to flip
    ``commitment: true`` — the writer doesn't require both halves."""
    batts = (
        BatterySpec(
            object_id=12,
            name="bess_charge_only",
            bus_name="bus_a",
            emax=100.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            pmin_charge=3.0,
            pmin_discharge=0.0,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert entry["pmin_charge"] == 3.0
    assert "pmin_discharge" not in entry
    assert entry["commitment"] is True


def test_battery_commitment_emitted_when_only_pmin_discharge() -> None:
    """Same as above, but on the discharge side."""
    batts = (
        BatterySpec(
            object_id=13,
            name="bess_discharge_only",
            bus_name="bus_a",
            emax=100.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            pmin_charge=0.0,
            pmin_discharge=4.0,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert "pmin_charge" not in entry
    assert entry["pmin_discharge"] == 4.0
    assert entry["commitment"] is True


def test_fuel_emission_factors_combustion_only() -> None:
    """A diesel fuel with co2_rate=0.074 emits a single combustion row.

    No ``upstream`` key when ``co2_upstream_rate == 0``.  The rate of
    0.074 tCO₂/GJ is the IPCC-default combustion factor for diesel /
    fuel oil (well-to-stack); multiplied by a typical diesel heat
    rate of 10 GJ/MWh that lands at ~0.74 tCO₂/MWh, matching the
    CEN-published marginal emission factors for Chilean diesel fleet.
    """
    fuels = (FuelSpec(object_id=1, name="diesel", price=1500.0, co2_rate=0.074),)
    out = build_fuel_array(fuels)
    assert out[0]["emission_factors"] == [{"emission": "co2", "combustion": 0.074}]


def test_fuel_emission_factors_combustion_and_upstream() -> None:
    """Both combustion + upstream populated → both keys emitted."""
    fuels = (
        FuelSpec(
            object_id=2,
            name="gas",
            price=8.0,
            co2_rate=0.056,
            co2_upstream_rate=0.008,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["emission_factors"] == [
        {"emission": "co2", "combustion": 0.056, "upstream": 0.008}
    ]


def test_fuel_emission_factors_absent_when_zero() -> None:
    """Renewable fuels (zero rates) emit no ``emission_factors`` key."""
    fuels = (FuelSpec(object_id=3, name="biomass", price=0.0),)
    out = build_fuel_array(fuels)
    assert "emission_factors" not in out[0]


def test_fuel_emission_factors_upstream_only() -> None:
    """Pure upstream (e.g. hydrogen WTT) emits no ``combustion`` key."""
    fuels = (
        FuelSpec(
            object_id=4,
            name="h2",
            price=0.0,
            co2_rate=0.0,
            co2_upstream_rate=0.011,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["emission_factors"] == [{"emission": "co2", "upstream": 0.011}]


# ---------------------------------------------------------------------------
# Fuel.max_offtake (PLEXOS FueMaxOffWeek_* reproduction — PR #487)
# ---------------------------------------------------------------------------


def test_fuel_max_offtake_emitted_when_set() -> None:
    """``FuelSpec.max_offtake`` > 0 lands on the JSON as ``max_offtake``
    + ``max_offtake_per_block: true`` (PLEXOS per-period semantics).

    The weekly cap (GJ/week after the TJ→GJ scaling on the parser
    side) is pro-rated by block duration in gtopt's FuelLP — one
    cap row per (scenario, stage, block).
    """
    fuels = (
        FuelSpec(
            object_id=1,
            name="Gas_Quintero_A",
            price=10.0,
            max_offtake=5000.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["max_offtake"] == 5000.0
    assert out[0]["max_offtake_per_block"] is True


def test_fuel_max_offtake_explicit_zero_propagates() -> None:
    """``max_offtake == 0.0`` IS emitted (PLEXOS "shut" signal).

    Distinct from ``None``: the LP must dispatch 0 on every
    generator referencing this fuel within the stage.
    ``max_offtake_per_block`` is also set so the per-block cap
    is 0 across every block (consistent with the "shut" semantic).
    """
    fuels = (
        FuelSpec(
            object_id=2,
            name="Gas_Quintero_B",
            price=12.0,
            max_offtake=0.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["max_offtake"] == 0.0
    assert out[0]["max_offtake_per_block"] is True


def test_fuel_max_offtake_absent_when_none() -> None:
    """``max_offtake == None`` omits both fields (no cap binds)."""
    fuels = (
        FuelSpec(
            object_id=3,
            name="Gas_Quintero_C",
            price=14.0,
            max_offtake=None,
        ),
    )
    out = build_fuel_array(fuels)
    assert "max_offtake" not in out[0]
    assert "max_offtake_per_block" not in out[0]


def test_thermal_generator_emits_fuel_fk_for_offtake_cap_binding() -> None:
    """End-to-end coupling: a thermal generator must emit ``fuel`` +
    ``heat_rate`` so gtopt's ``FuelLP.max_offtake`` cap row can find
    it at LP-build.

    Pre-PR-#489 the writer pre-baked ``heat_rate × price`` into
    ``gcost`` and dropped the Fuel FK — which made the cap row
    silently empty (no generator coupled to the Fuel) and the
    feature ineffective.
    """
    fuels = (
        FuelSpec(
            object_id=1,
            name="Gas_Quintero_A",
            price=10.0,
            max_offtake=5000.0,
        ),
    )
    gens = (
        GeneratorSpec(
            object_id=10,
            name="Quintero_1A",
            bus_name="b1",
            pmax=200.0,
            heat_rate=2.0,
            vom_charge=1.5,
            fuel_transport=0.3,
            fuel_names=("Gas_Quintero_A",),
        ),
    )
    gen_out = build_generator_array(gens, fuels)
    # Fuel FK + heat_rate explicit on the generator entry.
    assert gen_out[0]["fuel"] == "Gas_Quintero_A"
    assert gen_out[0]["heat_rate"] == 2.0
    # Non-fuel cost only.
    assert gen_out[0]["gcost"] == 1.5 + 0.3
    # Fuel side ships the cap; gtopt will sum the heat_rate-weighted
    # generation across this gen (and any others with the same fuel)
    # and bound by `max_offtake`.
    fuel_out = build_fuel_array(fuels)
    assert fuel_out[0]["max_offtake"] == 5000.0


# ---------------------------------------------------------------------------
# Demand fcost (literature audit #3: per-Region VoLL → per-Demand fcost)
# ---------------------------------------------------------------------------


def test_demand_emits_fcost_when_set() -> None:
    """``DemandSpec.fcost > 0`` lands on the JSON as ``fcost``.

    Carries the per-Region VoLL that the parser routed onto this
    Demand via ``_bus_to_region_voll``.  When unset (= 0.0) the field
    is omitted so gtopt's global ``model_options.demand_fail_cost``
    applies — matches PLEXOS Regions with no VoLL property.
    """
    out = build_demand_array(
        (
            DemandSpec(
                name="load_north",
                bus_name="bus_n",
                lmax_profile=(100.0, 120.0),
                fcost=1000.0,
            ),
            DemandSpec(
                name="load_south",
                bus_name="bus_s",
                lmax_profile=(80.0, 90.0),
                fcost=500.0,
            ),
            DemandSpec(
                name="load_orphan",
                bus_name="bus_o",
                lmax_profile=(50.0,),
                fcost=0.0,
            ),
        )
    )
    by_name = {d["name"]: d for d in out}
    assert by_name["load_north"]["fcost"] == 1000.0
    assert by_name["load_south"]["fcost"] == 500.0
    # fcost = 0 → omit so gtopt's global default applies.
    assert "fcost" not in by_name["load_orphan"]
    # lmax still emitted as the 1×N matrix, regardless of fcost.
    assert by_name["load_north"]["lmax"] == [[100.0, 120.0]]


# ---------------------------------------------------------------------------
# T7: DLR profile + resistance → matrix tmax_ab + piecewise loss mode
# ---------------------------------------------------------------------------


def test_build_line_array_dlr_emits_matrix_and_loss_mode() -> None:
    """A varying ``tmax_ab_profile`` (Dynamic Line Rating) lands on
    the JSON as ``[[per-hour]]`` matrix; lines with non-zero
    ``resistance`` AND an enforced cap get gtopt's piecewise loss
    mode (2 segments) + the ``√S_base`` voltage anchor.

    Locks the joint emission so a refactor can't silently drop
    either the DLR matrix shape (would clamp the line to the
    period-1 rating) or the piecewise loss mode (would model the
    line as lossless, undershooting PLEXOS by the corridor's loss
    GWh).
    """
    lines = (
        LineSpec(
            object_id=1,
            name="dlr_corridor",
            bus_from="a",
            bus_to="b",
            tmax_ab=200.0,
            tmax_ab_profile=(100.0,) * 12 + (200.0,) * 12,
            resistance=0.05,
            enforce_limits=2,
            units=1,
        ),
    )
    out = build_line_array(lines)
    # The varying DLR profile materialises as a [[per-hour]] matrix.
    assert isinstance(out[0]["tmax_ab"], list)
    assert out[0]["tmax_ab"] == [[100.0] * 12 + [200.0] * 12]
    # Resistance + voltage + piecewise loss mode (2 segments).
    assert out[0]["resistance"] == 0.05
    assert out[0]["voltage"] == 10.0
    assert out[0]["line_losses_mode"] == "piecewise"
    assert out[0]["loss_segments"] == 2


def test_build_line_array_constant_profile_keeps_scalar_tmax() -> None:
    """A constant ``tmax_ab_profile`` (all-200) collapses to the
    scalar ``tmax_ab``; resistance + piecewise loss mode still emit
    because the line carries a cap.
    """
    lines = (
        LineSpec(
            object_id=2,
            name="flat",
            bus_from="a",
            bus_to="b",
            tmax_ab=200.0,
            tmax_ab_profile=(200.0,) * 24,
            resistance=0.05,
            enforce_limits=2,
            units=1,
        ),
    )
    out = build_line_array(lines)
    # No matrix — the constant profile collapses to a scalar.
    assert out[0]["tmax_ab"] == 200.0
    assert out[0]["resistance"] == 0.05
    assert out[0]["line_losses_mode"] == "piecewise"
