"""Unit tests for :mod:`plexos2gtopt.gtopt_writer`.

These focus on the per-class array builders — the integration-test
suite covers the end-to-end ``build_planning`` → ``gtopt --lp-only``
path against the real bundle.
"""

from __future__ import annotations

import pytest

from plexos2gtopt.entities import (
    BatterySpec,
    DemandSpec,
    FuelSpec,
    GeneratorSpec,
    LineSpec,
)
from plexos2gtopt.gtopt_writer import (
    _compute_default_slack_cost,
    augment_el1_with_soft_caps,
    build_battery_array,
    build_demand_array,
    build_fuel_array,
    build_generator_array,
    build_line_array,
    soft_penalty_cost,
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
    only (no ``max_offtake_per_block`` field — gtopt's FuelLP defaults
    to per-stage SUM, which matches the PLEXOS weekly-budget
    semantic: ``Σ_blocks heat_rate × gen × dur ≤ weekly_cap``).
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
    # Per-stage SUM is the default — no per_block field emitted.
    # Per-block (pro-rated uniform per-hour cap) was used in earlier
    # versions but reduces to an effective power limit that removes
    # the LP's flexibility to time-shift fuel use within the week.
    assert "max_offtake_per_block" not in out[0]


def test_fuel_max_offtake_explicit_zero_propagates() -> None:
    """``max_offtake == 0.0`` IS emitted (PLEXOS "shut" signal).

    Distinct from ``None``: the LP must dispatch 0 on every
    generator referencing this fuel within the stage.  Per-stage SUM
    mode means a single cap row per (scenario, stage) at 0 GJ — the
    LP is forced to keep total fuel use at 0 across the horizon.
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
    assert "max_offtake_per_block" not in out[0]


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
    mode (4 segments, the post-2026-05 default) + the ``√S_base``
    voltage anchor.

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
    # Resistance + voltage + piecewise loss mode (4 segments default).
    assert out[0]["resistance"] == 0.05
    assert out[0]["voltage"] == 10.0
    assert out[0]["line_losses_mode"] == "piecewise"
    assert out[0]["loss_segments"] == 4
    # Default layout (post-2026-05) is `tangent`; emitted only when
    # non-`uniform` so the JSON stays minimal for the older default.
    assert out[0].get("loss_pwl_layout") == "tangent"


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


# ---------------------------------------------------------------------------
# Soft-EL=1 augmenter — pins the default behaviour that auto-converts every
# PLEXOS EL=1 line into a soft-capped line (rated as tmax_normal, 3× rated
# as the hard cap, overload_penalty as the per-MWh slack price).
# ---------------------------------------------------------------------------


def test_augment_el1_with_soft_caps_mutates_only_el1() -> None:
    """The helper softens EL=1 lines in place and leaves EL=0/EL=2 alone."""
    line_entries = [
        {
            "uid": 1,
            "name": "el0_a",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ab": 100.0,
            "tmax_ba": 100.0,
            "enforce_level": 0,
        },
        {
            "uid": 2,
            "name": "el1_b",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ab": 200.0,
            "tmax_ba": 200.0,
            "enforce_level": 1,
        },
        {
            "uid": 3,
            "name": "el2_c",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ab": 300.0,
            "tmax_ba": 300.0,
            "enforce_level": 2,
        },
        # EL=1 with B→A leg only
        {
            "uid": 4,
            "name": "el1_d",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ba": 50.0,
            "enforce_level": 1,
        },
    ]
    n = augment_el1_with_soft_caps(line_entries, overload_penalty=285.81)
    # Only the two EL=1 entries get softened.  uid=4 has no tmax_ab so
    # the soft mode falls through and it is NOT counted.  Pin the count
    # so a refactor that accidentally softens EL=0/2 or skips the no-ab
    # case fails.
    assert n == 1
    assert len(line_entries) == 4  # mutated in place, no new entries

    el0 = line_entries[0]
    assert el0["tmax_ab"] == 100.0  # untouched
    assert "tmax_normal_ab" not in el0
    assert "overload_penalty" not in el0

    el1 = line_entries[1]
    assert el1["tmax_normal_ab"] == 200.0  # soft target = original rated
    assert el1["tmax_ab"] == 600.0  # 3× rated headroom
    assert el1["tmax_normal_ba"] == 200.0
    assert el1["tmax_ba"] == 600.0
    assert el1["overload_penalty"] == 285.81
    # EL stays at 1; the new tmax_ab becomes the hard cap and the soft
    # mechanism (slack column priced at overload_penalty) kicks in via
    # the LP — not via EL flip.
    assert el1["enforce_level"] == 1

    el2 = line_entries[2]
    assert el2["tmax_ab"] == 300.0  # untouched
    assert "tmax_normal_ab" not in el2

    el1_no_ab = line_entries[3]
    assert "tmax_normal_ab" not in el1_no_ab
    assert "overload_penalty" not in el1_no_ab


def test_augment_el1_skips_zero_rated() -> None:
    """An EL=1 line with tmax_ab=0 is skipped (no soft mode emitted —
    would be a divide-by-something on an empty corridor)."""
    line_entries = [
        {
            "uid": 1,
            "name": "empty_el1",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ab": 0.0,
            "enforce_level": 1,
        },
    ]
    n = augment_el1_with_soft_caps(line_entries, overload_penalty=285.81)
    assert n == 0
    assert "tmax_normal_ab" not in line_entries[0]


def test_augment_el1_custom_headroom_factor() -> None:
    """The headroom_factor knob controls how much overflow the LP can
    push into the slack band.  Used for stress-testing under heavier
    overflow regimes; default 3× balances the loss-PWL accuracy (which
    is currently anchored on tmax_ab, NOT tmax_normal_ab — see
    gtopt_writer docstring) against soft-band capacity."""
    line_entries = [
        {
            "uid": 1,
            "name": "el1",
            "bus_a": "A",
            "bus_b": "B",
            "tmax_ab": 100.0,
            "enforce_level": 1,
        }
    ]
    augment_el1_with_soft_caps(
        line_entries, overload_penalty=285.81, headroom_factor=10.0
    )
    assert line_entries[0]["tmax_ab"] == 1000.0  # 10× rated
    assert line_entries[0]["tmax_normal_ab"] == 100.0


def test_compute_default_slack_cost_recipe() -> None:
    """slack_cost = min(max(gcost) + 1, min(VoLL) - 1)."""
    demand = [
        {"uid": 1, "name": "d1", "fcost": 467.19},
        {"uid": 2, "name": "d2", "fcost": 1000.0},
    ]
    generator = [
        {"uid": 1, "name": "g1", "gcost": 100.0},
        {"uid": 2, "name": "g2", "gcost": 5.0},
        {"uid": 3, "name": "g3", "gcost": 0.1},
    ]
    cost = _compute_default_slack_cost(demand, generator)
    # max(gcost)=100 → 101; min(fcost)=467.19 → 466.19; min = 101.
    assert cost == pytest.approx(100.0 + 1.0)


def test_compute_default_slack_cost_handles_per_block_matrix() -> None:
    """fcost / gcost may be scalar, per-block list, or per-block matrix;
    flattener picks the min/max across positive entries."""
    demand = [{"uid": 1, "name": "d1", "fcost": [[500.0, 450.0, 480.0]]}]
    generator = [
        {"uid": 1, "name": "g1", "gcost": [[10.0, 80.0, 25.0]]},
    ]
    cost = _compute_default_slack_cost(demand, generator)
    # max positive gcost = 80 → 81; min positive fcost = 450 → 449; min = 81.
    assert cost == pytest.approx(80.0 + 1.0)


def test_compute_default_slack_cost_fallback_when_empty() -> None:
    """No demands or no generators → returns the fallback constant."""
    assert _compute_default_slack_cost([], [], fallback=42.0) == 42.0
    assert (
        _compute_default_slack_cost(
            [{"uid": 1, "name": "d", "fcost": 500}], [], fallback=7.0
        )
        == 7.0
    )


# ---------------------------------------------------------------------------
# Gen_FixedLoad + Gen_IniGeneration extraction (PLEXOS forced-dispatch CSVs)
#
# `Generator.Fixed Load` (Gen_FixedLoad.csv) hard-equality-pins
# `Generation[t] = Fixed Load[t]` on must-take renewables and run-of-
# river hydro.  plexos2gtopt translates this into JSON
# `pmin = pmax = fixed_load` per block, forcing the LP to dispatch
# exactly the published profile.
#
# `Generator.Initial Generation` (Gen_IniGeneration.csv) is the
# warm-start dispatch level at t=0; captured on `GeneratorSpec` but
# NOT emitted to gtopt JSON (no schema field yet — rolling-horizon
# state continuity is a follow-up).
# ---------------------------------------------------------------------------


def test_generator_fixed_load_emits_pmax_as_cap_matrix() -> None:
    """A variable Fixed Load profile lands on the JSON as a per-block
    ``pmax`` cap at the published value, with ``pmin`` left at 0
    (curtailable) and omitted.
    """
    gens = (
        GeneratorSpec(
            object_id=10,
            name="forced_renewable",
            bus_name="bus_a",
            pmax=100.0,
            heat_rate=0.0,
            vom_charge=0.0,
            fuel_names=(),
            fixed_load_profile=(
                10.0,
                20.0,
                30.0,
                40.0,
                50.0,
                60.0,
                50.0,
                40.0,
                30.0,
                20.0,
                10.0,
                5.0,
            ),
        ),
    )
    out = build_generator_array(gens, fuels=())
    # pmin is 0 (curtailable) and omitted as a redundant zero.
    assert "pmin" not in out[0]
    # pmax caps dispatch at the FixedLoad value per block.
    assert isinstance(out[0]["pmax"], list)
    assert out[0]["pmax"] == [
        [
            10.0,
            20.0,
            30.0,
            40.0,
            50.0,
            60.0,
            50.0,
            40.0,
            30.0,
            20.0,
            10.0,
            5.0,
        ]
    ]
    assert len(out[0]["pmax"]) == 1
    assert len(out[0]["pmax"][0]) == 12


def test_generator_fixed_load_scalar_when_constant_profile() -> None:
    """A constant Fixed Load profile (e.g. a 24h baseload forced unit)
    collapses to a scalar ``pmax`` cap, with ``pmin`` left at 0
    (curtailable) and omitted.
    """
    gens = (
        GeneratorSpec(
            object_id=11,
            name="forced_baseload",
            bus_name="bus_a",
            pmax=80.0,
            fixed_load_profile=(50.0,) * 24,
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert "pmin" not in out[0]
    assert out[0]["pmax"] == 50.0


def test_generator_fixed_load_thermal_pins_pmin_eq_pmax_matrix() -> None:
    """A thermal unit (real fuel + heat rate) with a Fixed Load profile
    is a forced-dispatch / commitment trajectory: PLEXOS pins the
    generation variable, so we emit ``pmin == pmax == fixed_load`` per
    block at the non-zero blocks (the COCHRANE_1 start-up trajectory
    case).  The floor is made SOFT via ``pmin_fcost`` =
    ``min(max(gcost)+1, min(VoLL)-1)`` so a transmission/commitment limit
    can't make the LP infeasible.  Renewables stay curtailable — this is
    the non-renewable arm of the tech-dependent rule.
    """
    fuels = (FuelSpec(object_id=1, name="coal", price=50.0),)
    gens = (
        GeneratorSpec(
            object_id=20,
            name="forced_thermal",
            bus_name="bus_a",
            pmax=250.0,
            heat_rate=0.30,
            vom_charge=20.0,  # gcost field = vom (fuel cost rides the FK)
            fuel_names=("coal",),
            # Start-up trajectory: positive for the first 3 blocks, then 0.
            fixed_load_profile=(100.0, 150.0, 200.0, 0.0, 0.0, 0.0),
        ),
    )
    out = build_generator_array(gens, fuels=fuels)
    # Non-renewable ⇒ hard equality: pmin == pmax on the forced blocks,
    # and at fixed_load=0 blocks pmax falls back to the rating cap while
    # pmin returns to the always-on floor (0 here).
    assert out[0]["pmin"] == [[100.0, 150.0, 200.0, 0.0, 0.0, 0.0]]
    assert out[0]["pmax"] == [[100.0, 150.0, 200.0, 250.0, 250.0, 250.0]]
    # Soft floor: pmin_fcost = max(gcost field) + 1.  Only one generator
    # (gcost field = vom_charge = 20), so the floor is 20 + 1.
    assert out[0]["pmin_fcost"] == pytest.approx(20.0 + 1.0)


def test_generator_fixed_load_thermal_override_pins_pmin_eq_pmax() -> None:
    """A unit with a per-unit fuel-price override (no Fuel FK) but a
    positive heat rate also counts as non-renewable → ``pmin == pmax``
    with a soft ``pmin_fcost`` floor at ``max(gcost) + 1``.  Override
    units take the legacy baked-gcost path, so their gcost field INCLUDES
    the fuel cost (``heat_rate × price + vom``).
    """
    gens = (
        GeneratorSpec(
            object_id=21,
            name="forced_virtual_thermal",
            bus_name="bus_a",
            pmax=80.0,
            heat_rate=0.4,
            vom_charge=8.0,
            fuel_price_override=30.0,
            fixed_load_profile=(60.0,) * 6,
        ),
    )
    out = build_generator_array(gens, fuels=())
    # Constant forced value collapses to a scalar on both sides.
    assert out[0]["pmin"] == 60.0
    assert out[0]["pmax"] == 60.0
    # Legacy baked gcost field = 0.4 × 30 + 8 = 20; +1 → 21.
    assert out[0]["pmin_fcost"] == pytest.approx(0.4 * 30.0 + 8.0 + 1.0)


def test_generator_fixed_load_soft_pmin_uses_system_max_cost() -> None:
    """``pmin_fcost`` is the SYSTEM-WIDE ``max(gcost) + 1``, not the
    forced unit's own cost.  A cheap forced thermal gets the floor of the
    most expensive generator (a costly peaker) plus one.
    """
    fuels = (FuelSpec(object_id=1, name="coal", price=50.0),)
    gens = (
        # Cheap forced unit (gcost field = vom = 5) carrying the Fixed Load.
        GeneratorSpec(
            object_id=30,
            name="cheap_forced",
            bus_name="bus_a",
            pmax=200.0,
            heat_rate=0.30,
            vom_charge=5.0,
            fuel_names=("coal",),
            fixed_load_profile=(80.0,) * 4,
        ),
        # Expensive peaker: override/legacy path → gcost field = 1.0 × 100
        # = 100; no Fixed Load.  Sets the system-wide max.
        GeneratorSpec(
            object_id=31,
            name="expensive_peaker",
            bus_name="bus_b",
            pmax=50.0,
            heat_rate=1.0,
            fuel_price_override=100.0,
        ),
    )
    out = build_generator_array(gens, fuels=fuels)
    forced = next(e for e in out if e["name"] == "cheap_forced")
    # Floor uses the peaker's gcost 100, NOT the forced unit's own 5.
    assert forced["pmin_fcost"] == pytest.approx(100.0 + 1.0)
    # The peaker itself has no Fixed Load → no soft floor.
    peaker = next(e for e in out if e["name"] == "expensive_peaker")
    assert "pmin_fcost" not in peaker


def test_generator_fixed_load_soft_pmin_capped_below_voll() -> None:
    """``pmin_fcost`` is capped at ``min(VoLL) - 1`` so the forced-floor
    obligation never outranks load-serving — even when ``max(gcost) + 1``
    would exceed the value of lost load.  An explicit override wins over
    both.
    """
    gens = (
        # Legacy/override path → gcost field = 1.0 × 100 = 100.
        GeneratorSpec(
            object_id=40,
            name="forced_expensive",
            bus_name="bus_a",
            pmax=100.0,
            heat_rate=1.0,
            fuel_price_override=100.0,
            fixed_load_profile=(40.0,) * 3,
        ),
    )
    # VoLL = 50 → cap = 49, below the uncapped 101.
    out = build_generator_array(gens, demand_voll=50.0)
    assert out[0]["pmin_fcost"] == pytest.approx(50.0 - 1.0)
    # Without a VoLL the uncapped max(gcost)+1 = 101 applies.
    out_uncapped = build_generator_array(gens)
    assert out_uncapped[0]["pmin_fcost"] == pytest.approx(100.0 + 1.0)
    # An explicit override wins over the computed value and the VoLL cap.
    out_override = build_generator_array(
        gens, demand_voll=50.0, soft_penalty_override=7.5
    )
    assert out_override[0]["pmin_fcost"] == pytest.approx(7.5)


def test_soft_penalty_cost_formula() -> None:
    """Unit-test the shared ``soft_penalty_cost`` helper directly."""
    # max(gcost) + 1, no VoLL cap.
    assert soft_penalty_cost([10.0, 50.0, 30.0], []) == pytest.approx(51.0)
    # Capped at min(VoLL) - 1 when that is the binding term.
    assert soft_penalty_cost([10.0, 50.0, 30.0], [40.0, 80.0]) == pytest.approx(39.0)
    # Nested lists are flattened; non-positive gcosts ignored.
    assert soft_penalty_cost([[10.0, 0.0], [20.0]], [100.0]) == pytest.approx(21.0)
    # Empty gcost pool → floor of 1.0.
    assert soft_penalty_cost([], []) == pytest.approx(1.0)
    # Explicit override short-circuits everything.
    assert soft_penalty_cost([10.0], [40.0], override=999.0) == pytest.approx(999.0)


def test_generator_fixed_load_renewable_has_no_soft_pmin() -> None:
    """A zero-cost renewable (no fuel, zero heat rate) with a Fixed Load
    profile stays a curtailable cap: no ``pmin`` floor and therefore no
    ``pmin_fcost`` soft-floor penalty.
    """
    gens = (
        GeneratorSpec(
            object_id=22,
            name="forced_renewable",
            bus_name="bus_a",
            pmax=100.0,
            heat_rate=0.0,
            fuel_names=(),
            fixed_load_profile=(10.0, 20.0, 30.0),
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert "pmin" not in out[0]
    assert "pmin_fcost" not in out[0]


def test_generator_no_fixed_load_omits_zero_pmin() -> None:
    """Without ``fixed_load_profile`` and with default ``pmin=0``
    the writer omits the zero floor — no need to emit redundant
    ``"pmin": 0`` when the LP default is already unbounded-below.
    """
    gens = (
        GeneratorSpec(
            object_id=12,
            name="free_dispatch",
            bus_name="bus_a",
            pmax=200.0,
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert out[0]["pmax"] == 200.0
    assert "pmin" not in out[0]


def test_generator_initial_generation_not_emitted_to_json() -> None:
    """``GeneratorSpec.initial_generation`` is captured for downstream
    diagnostic tooling but intentionally NOT emitted to the gtopt
    JSON: the Generator schema has no ``initial_generation`` field
    yet (rolling-horizon state continuity is a follow-up), and
    emitting an unknown member trips daw::json's strict reader.
    Pin the omission so a future refactor doesn't accidentally
    re-introduce the strict-reader regression.
    """
    gens = (
        GeneratorSpec(
            object_id=13,
            name="warm_start_diag",
            bus_name="bus_a",
            pmax=100.0,
            initial_generation=42.5,
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert "initial_generation" not in out[0]


def test_commitment_initial_power_emitted_when_nonzero() -> None:
    """``CommitmentSpec.initial_power`` (= PLEXOS ``Initial Generation``)
    lands on the JSON as ``Commitment.initial_power`` — the dispatch
    level at t=-1 used for first-block ramp / commitment continuity.

    Pin the emission so a refactor can't accidentally drop it (which
    would silently revert to gtopt's legacy ``p_prev = 0`` cold-start
    behaviour, breaking hot-start UC.jl / PLEXOS test cases).
    """
    from plexos2gtopt.entities import CommitmentSpec
    from plexos2gtopt.gtopt_writer import build_commitment_array

    commits = (
        CommitmentSpec(
            generator_name="hot_start_unit",
            startup_cost=1000.0,
            initial_status=1.0,
            initial_hours=24.0,
            initial_power=85.5,
            pmin=50.0,
        ),
    )
    out = build_commitment_array(commits, lp_relax=False)
    assert out[0]["initial_power"] == 85.5


def test_commitment_omits_initial_power_when_zero() -> None:
    """``initial_power = 0`` (the default for cold-start units) keeps
    the JSON clean — emitting an explicit 0 would suggest a deliberate
    cold-start when in fact the field was just unset by the parser.
    """
    from plexos2gtopt.entities import CommitmentSpec
    from plexos2gtopt.gtopt_writer import build_commitment_array

    commits = (
        CommitmentSpec(
            generator_name="cold_start_unit",
            startup_cost=1000.0,
            pmin=50.0,
            # initial_power left at default 0.0
        ),
    )
    out = build_commitment_array(commits, lp_relax=False)
    assert "initial_power" not in out[0]
