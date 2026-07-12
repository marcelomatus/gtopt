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
    NodeSpec,
)
from plexos2gtopt.gtopt_writer import (
    _compute_default_slack_cost,
    augment_el1_with_soft_caps,
    build_battery_array,
    build_bus_array,
    build_demand_array,
    build_emission_array,
    build_fuel_array,
    build_generator_array,
    build_line_array,
    soft_penalty_cost,
)


def test_bus_array_voltage_from_native_property() -> None:
    """A NodeSpec with a native PLEXOS ``Voltage`` property wins over
    any kV encoded in the name (118-Bus convention: names are pure bus
    numbers, voltage lives in the property)."""
    nodes = (NodeSpec(object_id=1, name="001", voltage=138.0),)
    out = build_bus_array(nodes)
    assert out == [{"uid": 1, "name": "001", "voltage": 138.0}]


def test_bus_array_voltage_parsed_from_name() -> None:
    """CEN convention: no ``Voltage`` property, kV encoded in the NAME."""
    nodes = (
        NodeSpec(object_id=1, name="Salar110"),
        NodeSpec(object_id=2, name="Tamaya033"),  # leading zeros → 33
        NodeSpec(object_id=3, name="CNavia220_Aux_D"),  # embedded level
    )
    out = build_bus_array(nodes)
    assert [b["voltage"] for b in out] == [110.0, 33.0, 220.0]


def test_bus_array_voltage_omitted_when_unresolvable() -> None:
    """No native property AND no kV in the name → the field is OMITTED
    (never fabricated) so downstream tooling can tell 'unknown' apart."""
    nodes = (
        NodeSpec(object_id=1, name="NORTE"),
        NodeSpec(object_id=2, name="101"),  # RTS-96 bus number, not a kV
    )
    out = build_bus_array(nodes)
    assert all("voltage" not in b for b in out)
    assert out[0] == {"uid": 1, "name": "NORTE"}


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


def test_el0_extended_soft_caps_inflate_tmax() -> None:
    """--el0-lines extended: a soft-capped line inflates tmax above nominal."""
    lines = (
        LineSpec(
            object_id=3,
            name="el0_soft",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            tmin_ab=-100.0,
            units=1,
            enforce_limits=1,
            soft_cap=True,  # how extract_lines flags EL=0 under "extended"
        ),
    )
    out = build_line_array(lines)
    # Soft cap: tmax_ab is the inflated HARD cap (> nominal), tmax_normal_ab
    # the free-band threshold (also > nominal), and loss_envelope preserves
    # the true nominal rating (100).
    assert out[0]["tmax_ab"] > 100.0
    assert out[0]["tmax_normal_ab"] > 100.0
    assert out[0]["loss_envelope"] == 100.0
    assert "overload_penalty" in out[0]


def test_soft_cap_loss_envelope_extends_under_flag(monkeypatch) -> None:
    """``--loss-extend-overload`` (env ``GTOPT_LOSS_EXTEND_OVERLOAD=1``)
    MUST widen the writer-emitted ``loss_envelope`` by the hard-cap
    headroom factor on soft-cap lines.  Before #44 the parsers side
    sized K_i for the extended envelope but the writer hardcoded
    ``loss_envelope = orig_env`` regardless of the flag — the C++
    side then collapsed the K segments back into ``[0, tmax_normal]``,
    silently wasting the K growth.

    Regression contract:

      * OFF / unset → envelope == original rating.
      * ON, regular soft_cap  → envelope == 2× original rating.
      * ON, soft_cap_lifted   → envelope == 4× original rating.
      * EL=2 hard-cap line    → envelope unchanged regardless of flag
        (LP can never flow past tmax, so wider envelope is moot).
    """
    monkeypatch.delenv("GTOPT_LOSS_EXTEND_OVERLOAD", raising=False)

    lines = (
        # EL=2 hard cap — extending the envelope is a no-op for these.
        LineSpec(
            object_id=10,
            name="hard_cap",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            resistance=0.05,  # piecewise loss path
            enforce_limits=2,
            units=1,
        ),
        # Regular soft cap — 2× hard headroom.
        LineSpec(
            object_id=11,
            name="soft_reg",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            resistance=0.05,
            enforce_limits=1,
            soft_cap=True,
            units=1,
        ),
        # Lifted soft cap — 4× hard headroom.
        LineSpec(
            object_id=12,
            name="soft_lifted",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            resistance=0.05,
            enforce_limits=1,
            soft_cap=True,
            soft_cap_lifted=True,
            units=1,
        ),
    )

    off = {ln["name"]: ln for ln in build_line_array(lines)}
    assert off["hard_cap"]["loss_envelope"] == 100.0
    assert off["soft_reg"]["loss_envelope"] == 100.0
    assert off["soft_lifted"]["loss_envelope"] == 100.0

    monkeypatch.setenv("GTOPT_LOSS_EXTEND_OVERLOAD", "1")
    on = {ln["name"]: ln for ln in build_line_array(lines)}
    # Hard cap: unchanged (LP can't flow past tmax — the wider envelope
    # would be vacuous and could under-resolve I²R at the rated point).
    assert on["hard_cap"]["loss_envelope"] == 100.0
    # Soft cap: envelope = orig × hard_f (2× regular, 4× lifted).  Pull
    # the factors via the actual writer constants so the test tracks
    # future tweaks instead of pinning to literal numbers.
    from plexos2gtopt.gtopt_writer import (
        _LINE_SOFT_HARD_FACTOR,
        _LINE_LIFTED_HARD_FACTOR,
    )

    assert on["soft_reg"]["loss_envelope"] == 100.0 * _LINE_SOFT_HARD_FACTOR
    assert on["soft_lifted"]["loss_envelope"] == 100.0 * _LINE_LIFTED_HARD_FACTOR
    # Soft-cap envelopes strictly widen under the flag.
    assert on["soft_reg"]["loss_envelope"] > off["soft_reg"]["loss_envelope"]
    assert on["soft_lifted"]["loss_envelope"] > off["soft_lifted"]["loss_envelope"]


def test_el0_strict_acts_like_el2_hard_cap() -> None:
    """--el0-lines strict: the line is a plain hard cap at the nominal rating.

    extract_lines emits ``enforce_limits=2`` + ``soft_cap=False`` in strict
    mode, so the writer keeps tmax at the nominal rating with no free band.
    """
    lines = (
        LineSpec(
            object_id=4,
            name="el0_strict",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            tmin_ab=-100.0,
            units=1,
            enforce_limits=2,
            soft_cap=False,
        ),
    )
    out = build_line_array(lines)
    assert out[0]["tmax_ab"] == 100.0  # nominal, not inflated
    assert "tmax_normal_ab" not in out[0]  # no soft-cap band
    assert "overload_penalty" not in out[0]
    assert "enforce_level" not in out[0]  # EL=2 is the schema default


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


def test_battery_emits_max_cycles_day_and_capacity() -> None:
    """max_cycles_day > 0 (with emax > 0) emits both ``capacity`` (= emax,
    the usable energy) and ``max_cycles_day`` so gtopt builds the daily
    energy-throughput limit row."""
    batts = (
        BatterySpec(
            object_id=20,
            name="bess_cyc",
            bus_name="bus_a",
            emax=100.0,
            eini=50.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            max_cycles_day=1.0,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert entry["max_cycles_day"] == 1.0
    assert entry["capacity"] == 100.0


def test_battery_omits_max_cycles_day_when_zero() -> None:
    """No ``max_cycles_day`` / ``capacity`` keys when the cycle limit is
    unset (0.0) — keeps the daily-cycle row out of the LP."""
    batts = (
        BatterySpec(
            object_id=21,
            name="bess_nocyc",
            bus_name="bus_a",
            emax=100.0,
            pmax_charge=50.0,
            pmax_discharge=50.0,
        ),
    )
    entry = build_battery_array(batts)[0]
    assert "max_cycles_day" not in entry
    assert "capacity" not in entry


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


def test_build_fuel_array_emits_canonical_type_tag() -> None:
    """``build_fuel_array`` surfaces ``FuelSpec.type_tag`` as the gtopt
    ``Fuel.type`` field — the same canonical family tag the parser
    derives via ``fuel_family_of_fuel`` and the generator-side suffix
    matcher uses for orphan recovery.  The field is emitted
    unconditionally so downstream consumers always see a non-null
    classification.
    """
    fuels = (
        FuelSpec(object_id=1, name="Diesel_Collahuasi", type_tag="diesel"),
        FuelSpec(object_id=2, name="Gas_Kelar_A", type_tag="gas"),
        FuelSpec(object_id=3, name="FuelOil_Norgener", type_tag="fuel_oil"),
        FuelSpec(object_id=4, name="Biomasa_CMPCLaja_B1", type_tag="biomasa"),
        # Hand-rolled fuel name that doesn't match any CEN prefix —
        # parser-side default ``"other"`` round-trips into the JSON.
        FuelSpec(object_id=5, name="virtual_fuel", type_tag="other"),
    )
    out = build_fuel_array(fuels)
    assert [e["type"] for e in out] == [
        "diesel",
        "gas",
        "fuel_oil",
        "biomasa",
        "other",
    ]


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


# ---------------------------------------------------------------------------
# Fuel.min_offtake (PLEXOS Min Offtake / take-or-pay reproduction)
# ---------------------------------------------------------------------------


def test_fuel_min_offtake_emitted_when_set() -> None:
    """``FuelSpec.min_offtake`` ≥ 0 lands on the JSON as ``min_offtake``.

    Default per-stage SUM mode mirrors the ``max_offtake`` semantics —
    no ``min_offtake_per_block`` field is emitted (gtopt's FuelLP
    defaults to per-stage SUM).  Take-or-pay floors operate on a
    horizon-wide budget basis identical to the upper-bound family.
    """
    fuels = (
        FuelSpec(
            object_id=1,
            name="Gas_Quintero_A",
            price=10.0,
            min_offtake=1500.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["min_offtake"] == 1500.0
    # No cost ⇒ no soft-floor slack on the LP (gtopt-native hard floor).
    assert "min_offtake_cost" not in out[0]
    assert "min_offtake_per_block" not in out[0]


def test_fuel_min_offtake_soft_emits_cost() -> None:
    """``min_offtake_cost`` rides alongside the floor as the shortfall
    slack price ($/fuel-unit).  Mirrors the PLEXOS Min Offtake Penalty
    default; the parser injects $1000/fuel-unit when the bundle ships
    a floor without an explicit penalty.
    """
    fuels = (
        FuelSpec(
            object_id=2,
            name="Gas_NuevaRenca_GN_A",
            price=12.0,
            min_offtake=2000.0,
            min_offtake_cost=1000.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["min_offtake"] == 2000.0
    assert out[0]["min_offtake_cost"] == 1000.0


def test_fuel_min_offtake_explicit_zero_propagates() -> None:
    """``min_offtake == 0.0`` IS emitted (PLEXOS-style explicit zero
    floor).  Semantically the row is trivially satisfied but the LP
    still gets the symbol so any UC that references the slack column
    can resolve."""
    fuels = (
        FuelSpec(
            object_id=3,
            name="Gas_Colbun_GN_C",
            price=14.0,
            min_offtake=0.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["min_offtake"] == 0.0


def test_fuel_min_offtake_absent_when_none() -> None:
    """``min_offtake == None`` omits all min-side fields (no floor binds).

    Default state for every fuel across the 14 cached CEN PCP bundles
    (2025-10 → 2026-05) — no CEN bundle in the archive populates any
    Min Offtake property.
    """
    fuels = (
        FuelSpec(
            object_id=4,
            name="Gas_Quintero_C",
            price=14.0,
            min_offtake=None,
            min_offtake_cost=None,
        ),
    )
    out = build_fuel_array(fuels)
    assert "min_offtake" not in out[0]
    assert "min_offtake_cost" not in out[0]
    assert "min_offtake_per_block" not in out[0]


def test_fuel_ranged_pair_min_and_max_coexist() -> None:
    """Both ``min_offtake`` and ``max_offtake`` set on the same fuel
    emit both fields side-by-side on the JSON entry — gtopt's FuelLP
    then materialises two separate LP rows sharing the offtake DV
    LHS, mirroring the ``Commitment::{min,max}_starts`` pattern.
    """
    fuels = (
        FuelSpec(
            object_id=5,
            name="Gas_NuevaRenca_GN_A",
            price=10.0,
            min_offtake=200.0,
            min_offtake_cost=1000.0,
            max_offtake=5000.0,
            max_offtake_cost=500.0,
        ),
    )
    out = build_fuel_array(fuels)
    assert out[0]["min_offtake"] == 200.0
    assert out[0]["min_offtake_cost"] == 1000.0
    assert out[0]["max_offtake"] == 5000.0
    assert out[0]["max_offtake_cost"] == 500.0


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


def test_generator_with_fuel_but_zero_heat_rate_still_emits_fk() -> None:
    """Emission-audit gap #1: when a Generator carries a Fuel
    membership but ``heat_rate == 0`` (no rated heat rate in PLEXOS
    XML), the writer must STILL emit the Fuel FK so:

    * gtopt's ``Fuel.max_offtake`` cap row includes this generator
      (with a zero coefficient — no effect today, but binding once a
      per-stage heat_rate schedule is later supplied);
    * gtopt's ``System::expand_fuel_emission_sources`` can attach a
      per-MWh CO2 source row as soon as a heat_rate becomes non-zero.

    Pre-fix the writer dropped the FK on this path because the
    ``elif`` condition required ``heat_rate > 0``.
    """
    fuels = (
        FuelSpec(
            object_id=1,
            name="Diesel_North",
            price=80.0,
        ),
    )
    gens = (
        GeneratorSpec(
            object_id=11,
            name="DieselPlant_NoHR",
            bus_name="b1",
            pmax=50.0,
            heat_rate=0.0,  # not rated in this hypothetical PLEXOS XML
            vom_charge=2.0,
            fuel_transport=0.5,
            fuel_names=("Diesel_North",),
        ),
    )
    gen_out = build_generator_array(gens, fuels)
    entry = gen_out[0]
    # Fuel FK preserved
    assert entry["fuel"] == "Diesel_North"
    # heat_rate is NOT emitted when it would be 0 (mutex with segments
    # would otherwise be a concern; emitting a zero scalar is also
    # noise in the JSON).
    assert "heat_rate" not in entry
    # gcost is the non-fuel part only — when heat_rate is unset,
    # gtopt's ``primary_slope_cost_at`` (generator_lp.cpp:151-154)
    # collapses to just gcost, so no double-counting risk.
    assert entry["gcost"] == 2.0 + 0.5


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
    # Explicit ``piecewise`` (the converter default is now Coffrin
    # ``tangent_signed_flow`` — see test_line_losses_mode.py); this case still
    # exercises the piecewise layout + adaptive-K path.
    out = build_line_array(lines, line_losses_mode="piecewise")
    # The varying DLR profile materialises as a [[per-hour]] matrix.
    assert isinstance(out[0]["tmax_ab"], list)
    assert out[0]["tmax_ab"] == [[100.0] * 12 + [200.0] * 12]
    # Resistance + voltage + piecewise loss mode.  When LineSpec.loss_segments
    # is unset (as in this fixture) the writer falls back to the historic
    # uniform-K default of 4.  Post-2026-05-29 the converter's adaptive
    # cube-root rule (--loss-error-pct 0.01) stamps per-line overrides
    # in extract_lines with ceiling 6, but build_line_array called
    # directly does not exercise that path.
    assert out[0]["resistance"] == 0.05
    assert out[0]["voltage"] == 10.0
    assert out[0]["line_losses_mode"] == "piecewise"
    assert out[0]["loss_segments"] == 4
    # Default layout (post-2026-05) is `midpoint` (de-biased secant);
    # emitted only when non-`uniform` so the JSON stays minimal.
    assert out[0].get("loss_pwl_layout") == "midpoint"


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
    out = build_line_array(lines, line_losses_mode="piecewise")
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


def test_generator_emits_type_and_description() -> None:
    """F5/D: every generator carries a coarse tech ``type`` and a
    standardized ``description`` (source + field units)."""
    fuels = (FuelSpec(object_id=1, name="coal", price=50.0),)
    gens = (
        GeneratorSpec(
            object_id=1,
            name="THERM",
            bus_name="b",
            pmax=100.0,
            heat_rate=0.3,
            fuel_names=("coal",),
        ),
        GeneratorSpec(object_id=2, name="SOLAR", bus_name="b", pmax=50.0),
    )
    out = build_generator_array(gens, fuels=fuels)
    by = {e["name"]: e for e in out}
    # Inline GeneratorSpec construction (no type_tag set) — writer's
    # safety-net falls back to the legacy binary classification
    # (fuel signal → "thermal", else "renewable").
    assert by["THERM"]["type"] == "thermal"
    assert by["SOLAR"]["type"] == "renewable"
    for e in out:
        assert "[MW]" in e["description"]
        assert "(File:" in e["description"]


def test_generator_emits_hierarchical_type_when_tag_explicit() -> None:
    """When ``GeneratorSpec.type_tag`` carries a canonical primary-energy
    tag, the writer composes the hierarchical ``Generator.type`` string
    (``"thermal:<family>"`` / ``"renewable:<tech>"``).  The sub-tag
    matches ``FuelSpec.type_tag`` exactly — same taxonomy on both sides.
    """
    fuels = (FuelSpec(object_id=1, name="Diesel_X", price=1000.0),)
    gens = (
        GeneratorSpec(
            object_id=1,
            name="diesel_peaker",
            bus_name="b",
            pmax=10.0,
            heat_rate=0.3,
            fuel_names=("Diesel_X",),
            type_tag="diesel",
        ),
        GeneratorSpec(
            object_id=2,
            name="ccgt",
            bus_name="b",
            pmax=400.0,
            heat_rate=0.2,
            fuel_names=("Gas_X",),
            type_tag="gas",
        ),
        GeneratorSpec(object_id=3, name="solar", bus_name="b", type_tag="solar"),
        GeneratorSpec(object_id=4, name="wind", bus_name="b", type_tag="wind"),
        GeneratorSpec(object_id=5, name="hydro", bus_name="b", type_tag="hydro"),
        # Unclassified renewable falls through to bare ``"renewable"``.
        GeneratorSpec(object_id=6, name="other_ren", bus_name="b"),
        # Unclassified thermal (no canonical family) falls through to
        # bare ``"thermal"`` via the writer's has-fuel safety net.
        GeneratorSpec(
            object_id=7,
            name="mystery_thermal",
            bus_name="b",
            heat_rate=0.4,
            type_tag="thermal",
        ),
    )
    out = build_generator_array(gens, fuels=fuels)
    by = {e["name"]: e["type"] for e in out}
    assert by["diesel_peaker"] == "thermal:diesel"
    assert by["ccgt"] == "thermal:gas"
    assert by["solar"] == "renewable:solar"
    assert by["wind"] == "renewable:wind"
    assert by["hydro"] == "renewable:hydro"
    assert by["other_ren"] == "renewable"
    assert by["mystery_thermal"] == "thermal"
    # Backward-compat invariant: every value starts with one of the
    # two top-level families.
    for t in by.values():
        assert t.startswith(("thermal", "renewable")), t


def test_build_provenance_documents_element_classes() -> None:
    """F5/B: the provenance sidecar documents each element class with its
    PLEXOS source, units, transforms, and emitted count."""
    from plexos2gtopt.gtopt_writer import build_provenance

    planning = {
        "options": {"model_options": {"demand_fail_cost": 467.19}},
        "system": {
            "generator_array": [{"name": "g1"}, {"name": "g2"}],
            "line_array": [{"name": "l1"}],
            "user_constraint_array": [{"name": "uc1"}],
        },
    }
    prov = build_provenance(planning, source_bundle="DATOS.zip")
    assert prov["source_bundle"] == "DATOS.zip"
    assert prov["demand_fail_cost"] == 467.19
    assert "soft_penalty" in prov["global_transforms"]
    gen = prov["elements"]["Generator"]
    assert gen["count"] == 2
    assert gen["plexos_source"] == "Generator"
    assert gen["units"]["pmax"] == "MW"
    assert gen["units"]["pmin_fcost"] == "$/MWh"
    assert prov["elements"]["Line"]["count"] == 1
    assert prov["elements"]["UserConstraint"]["count"] == 1
    # Classes with no emitted elements still document units/source (count 0).
    assert prov["elements"]["Battery"]["count"] == 0
    assert prov["elements"]["Reservoir"]["units"]["vmax"] == "hm³"


def test_write_provenance_creates_valid_json(tmp_path) -> None:
    """F5/B: write_provenance emits a parseable JSON sidecar with the
    per-class records and counts derived from the planning."""
    import json as _json

    from plexos2gtopt.gtopt_writer import build_provenance, write_provenance

    prov = build_provenance(
        {
            "options": {"model_options": {"demand_fail_cost": 467.19}},
            "system": {"generator_array": [{"name": "g1"}, {"name": "g2"}]},
        },
        source_bundle="DATOS.zip",
    )
    path = write_provenance(prov, tmp_path / "case.provenance.json")
    assert path.exists()
    data = _json.loads(path.read_text())
    assert data["source_bundle"] == "DATOS.zip"
    assert data["elements"]["Generator"]["count"] == 2
    assert "soft_penalty" in data["global_transforms"]
    assert "rhs_custom" in data["global_transforms"]


def test_build_emission_array_co2() -> None:
    """emission_array['co2'] is emitted iff a fuel carries a CO2 rate."""
    no_co2 = (FuelSpec(object_id=1, name="coal", price=4.0),)
    assert not build_emission_array(no_co2)
    with_co2 = (FuelSpec(object_id=1, name="coal", price=4.0, co2_rate=0.34),)
    assert build_emission_array(with_co2) == [{"uid": 1, "name": "co2"}]
    # upstream-only rate also triggers the pollutant definition.
    up_only = (FuelSpec(object_id=2, name="lng", co2_upstream_rate=0.05),)
    assert build_emission_array(up_only) == [{"uid": 1, "name": "co2"}]


def test_build_user_constraint_array_omits_directive_when_unset() -> None:
    """Legacy ``UserConstraintSpec`` (``directive=None``) round-trips
    unchanged — the writer must NOT emit a ``directive`` JSON key.

    Regression contract for Step 4a (#53): the scaffold landed in #52
    treats the directive as optional everywhere; existing JSON
    consumers must not see an unexpected key.
    """
    from plexos2gtopt.entities import UserConstraintSpec
    from plexos2gtopt.gtopt_writer import build_user_constraint_array

    specs = (
        UserConstraintSpec(
            name="legacy_uc",
            expression='generator("G1").generation <= 100',
            penalty=10.0,
        ),
    )
    out = build_user_constraint_array(specs)
    assert "directive" not in out[0]
    assert out[0]["penalty"] == 10.0


def test_build_user_constraint_array_emits_regrange_directive() -> None:
    """When a ``UserConstraintSpec`` carries a ``ConstraintDirective``
    the writer MUST emit a ``directive`` sibling field on the JSON
    entry — with the ``kind`` discriminator and only the populated
    optional fields.

    Regression contract for Step 4a (#53):
      * ``kind`` always emitted.
      * Populated fields (``penalty`` / ``scope`` / ``window_hours``)
        emitted when non-None.
      * Unset optional fields are OMITTED so the JSON wire form
        stays minimal and gtopt-side ``daw::json`` ``*_null``
        bindings stay happy.
    """
    from plexos2gtopt.entities import ConstraintDirective, UserConstraintSpec
    from plexos2gtopt.gtopt_writer import build_user_constraint_array

    specs = (
        UserConstraintSpec(
            name="regrange_uc",
            expression='commitment("uc_G1").status <= 0',
            penalty=1000.0,
            directive=ConstraintDirective(
                kind="regrange",
                penalty=1000.0,
            ),
        ),
        UserConstraintSpec(
            name="reserve_prov_sum_uc",
            expression=(
                'reserve_provision("p1").up + reserve_provision("p2").up '
                '+ reserve_provision("p3").up >= 10'
            ),
            penalty=1000.0,
            directive=ConstraintDirective(
                kind="reserve_prov_sum",
                penalty=1000.0,
            ),
        ),
        UserConstraintSpec(
            name="daily_budget_uc",
            expression='sum(generator(all: fuel="GAS").generation) <= 800',
            penalty=0.0,
            directive=ConstraintDirective(
                kind="daily_budget",
                scope="fuel:GAS",
            ),
        ),
        UserConstraintSpec(
            name="max_starts_window_uc",
            expression='sum(commitment("uc_G1").startup) <= 5',
            penalty=0.0,
            directive=ConstraintDirective(
                kind="max_starts_window",
                window_hours=24,
            ),
        ),
    )
    out = {entry["name"]: entry for entry in build_user_constraint_array(specs)}

    # RegRange — kind + penalty only; no scope / window_hours leakage.
    assert out["regrange_uc"]["directive"] == {
        "kind": "regrange",
        "penalty": 1000.0,
    }
    # ReserveProvSum — same shape, different discriminator.
    assert out["reserve_prov_sum_uc"]["directive"] == {
        "kind": "reserve_prov_sum",
        "penalty": 1000.0,
    }
    # DailyBudget — kind + scope, penalty omitted (None on directive).
    assert out["daily_budget_uc"]["directive"] == {
        "kind": "daily_budget",
        "scope": "fuel:GAS",
    }
    # MaxStartsWindow — kind + window_hours only.
    assert out["max_starts_window_uc"]["directive"] == {
        "kind": "max_starts_window",
        "window_hours": 24,
    }


# ---------------------------------------------------------------------------
# write_boundary_cut_csv — silent-drop bug C3 regression guard
#
# ``Hydro_StoWaterValues.csv`` defines a water-value slope per reservoir + an
# FCF intercept; the writer filters to bundle reservoirs (``reservoir_array``
# membership) and emits one column per kept slope.  A real CEN PCP example:
# PILMAIQUEN is a PLEXOS Generator only (no Storage object), so its $576/CMD
# slope cannot be attached and is dropped — historically at INFO level, which
# silently lost the row from ``boundary_cuts.csv`` and removed the terminal-
# value coupling on the implicit reservoir.  These tests pin down:
#   (1) the drop is WARN (not INFO), so the omission surfaces in normal logs
#   (2) the emitted row count equals the number of MATCHED slopes (= input
#       slopes minus dropped non-bundle entries)
#   (3) ``None`` is still returned when every slope drops
# ---------------------------------------------------------------------------


def test_write_boundary_cut_csv_emits_row_per_bundle_reservoir(tmp_path) -> None:
    """Row count == #(slopes that match a bundle reservoir)."""
    import csv as _csv

    from plexos2gtopt.entities import BoundaryCutSpec
    from plexos2gtopt.gtopt_writer import write_boundary_cut_csv

    cut = BoundaryCutSpec(
        fcf=1.247e9,
        slopes={
            "L_Maule": 9037.748,
            "CIPRESES": 6683.819,
            "PILMAIQUEN": 576.479,  # non-bundle (Generator-only in PLEXOS)
        },
    )
    reservoirs = frozenset({"L_Maule", "CIPRESES"})  # no PILMAIQUEN
    out_name = write_boundary_cut_csv(cut, reservoirs, tmp_path)
    assert out_name == "boundary_cuts.csv"
    with (tmp_path / "boundary_cuts.csv").open(encoding="utf-8") as fh:
        rows = list(_csv.reader(fh))
    # Header + 1 data row.
    assert len(rows) == 2
    # Header carries one column per MATCHED slope (PILMAIQUEN absent).
    assert rows[0] == ["scene", "rhs", "L_Maule", "CIPRESES"]
    # Coefficients are stored as -water_value (more storage → lower cost).
    assert rows[1][0] == "0"
    assert float(rows[1][1]) == pytest.approx(1.247e9)
    assert float(rows[1][2]) == pytest.approx(-9037.748)
    assert float(rows[1][3]) == pytest.approx(-6683.819)


def test_parse_water_value_factor_parses_pairs() -> None:
    """``--water-value-factor`` spec → {reservoir: factor}, strict on errors."""
    from plexos2gtopt.gtopt_writer import parse_water_value_factor

    assert parse_water_value_factor("COLBUN:0.9,RALCO:0.85") == {
        "COLBUN": 0.9,
        "RALCO": 0.85,
    }
    assert not parse_water_value_factor(None)
    assert not parse_water_value_factor("")
    assert parse_water_value_factor(" COLBUN : 0.9 ") == {"COLBUN": 0.9}
    for bad in ("COLBUN", "COLBUN:x", "COLBUN:-1", ":0.9"):
        with pytest.raises(ValueError):
            parse_water_value_factor(bad)


def test_write_boundary_cut_csv_applies_water_value_factor(tmp_path) -> None:
    """``water_value_factor`` multiplies named slopes; unlisted keep 1.0."""
    import csv as _csv

    from plexos2gtopt.entities import BoundaryCutSpec
    from plexos2gtopt.gtopt_writer import write_boundary_cut_csv

    cut = BoundaryCutSpec(
        fcf=1.0e9,
        slopes={"COLBUN": 2658.467, "RALCO": 3440.711, "PEHUENCHE": 3053.312},
    )
    reservoirs = frozenset({"COLBUN", "RALCO", "PEHUENCHE"})
    write_boundary_cut_csv(
        cut,
        reservoirs,
        tmp_path,
        water_value_factor={"COLBUN": 0.9, "RALCO": 0.85},
    )
    with (tmp_path / "boundary_cuts.csv").open(encoding="utf-8") as fh:
        rows = list(_csv.reader(fh))
    hdr, data = rows[0], rows[1]
    val = {c: float(data[hdr.index(c)]) for c in ("COLBUN", "RALCO", "PEHUENCHE")}
    # Coefficient is -water_value; the factor multiplies the magnitude.
    assert val["COLBUN"] == pytest.approx(-2658.467 * 0.9)
    assert val["RALCO"] == pytest.approx(-3440.711 * 0.85)
    assert val["PEHUENCHE"] == pytest.approx(-3053.312)  # unlisted → 1.0


def test_write_boundary_cut_csv_warns_on_non_bundle_drop(
    tmp_path, caplog: pytest.LogCaptureFixture
) -> None:
    """Dropping a slope (e.g. PILMAIQUEN) emits a WARN-level log so the
    omission surfaces in operator-facing logs — previously INFO, which the
    standard converter run silenced and lost terminal-value couplings without
    a peep (bug C3 from the PLEXOS audit).
    """
    import logging as _logging

    from plexos2gtopt.entities import BoundaryCutSpec
    from plexos2gtopt.gtopt_writer import write_boundary_cut_csv

    cut = BoundaryCutSpec(
        fcf=1.0,
        slopes={"R1": 2.0, "PILMAIQUEN": 576.479},
    )
    with caplog.at_level(_logging.WARNING, logger="plexos2gtopt.gtopt_writer"):
        write_boundary_cut_csv(cut, frozenset({"R1"}), tmp_path)
    drop_warnings = [
        r
        for r in caplog.records
        if r.levelno == _logging.WARNING
        and "PILMAIQUEN" in r.getMessage()
        and "no matching reservoir_array" in r.getMessage()
    ]
    assert drop_warnings, (
        "expected a WARN log naming PILMAIQUEN when a water-value slope "
        f"has no bundle reservoir; got: {[r.getMessage() for r in caplog.records]}"
    )


def test_write_boundary_cut_csv_returns_none_when_no_match(
    tmp_path, caplog: pytest.LogCaptureFixture
) -> None:
    """All slopes drop → no CSV, ``None`` returned, single WARN explaining."""
    import logging as _logging

    from plexos2gtopt.entities import BoundaryCutSpec
    from plexos2gtopt.gtopt_writer import write_boundary_cut_csv

    cut = BoundaryCutSpec(fcf=1.0, slopes={"PILMAIQUEN": 576.479})
    with caplog.at_level(_logging.WARNING, logger="plexos2gtopt.gtopt_writer"):
        result = write_boundary_cut_csv(cut, frozenset({"R1"}), tmp_path)
    assert result is None
    assert not (tmp_path / "boundary_cuts.csv").exists()
    matched = [
        r
        for r in caplog.records
        if r.levelno == _logging.WARNING
        and "none of" in r.getMessage()
        and "PILMAIQUEN" in r.getMessage()
    ]
    assert matched, (
        f"expected a WARN log, got: {[r.getMessage() for r in caplog.records]}"
    )


@pytest.mark.parametrize("heat_rate", [0.0, 0.5, 9.0])
def test_scalar_path_cost_decomposition_invariant(heat_rate: float) -> None:
    """Emission-audit gap #1 widening: ANY ``primary_fuel`` takes the
    scalar (FK-preserving) path, regardless of ``heat_rate``.

    The semantic invariant this test pins is that the LP cost
    coefficient on ``generation`` is identical to what the legacy
    baked-gcost path would have emitted:

    * Old baked path (``heat_rate == 0`` BEFORE gap-#1 widening):
      ``entry["gcost"] = 0 × fuel.price + vom + fuel_transport``
      ``→ vom + fuel_transport``;  Fuel FK was dropped, ``heat_rate``
      not emitted.

    * New scalar path (``primary_fuel is not None``):
      ``entry["fuel"] = primary_fuel``; ``entry["gcost"] = vom +
      fuel_transport``;  ``entry["heat_rate"]`` only when > 0.

    gtopt's ``primary_slope_cost_at`` (generator_lp.cpp:151-154)
    reconstitutes the full coefficient as
    ``fuel.price × heat_rate + gcost`` — when ``heat_rate`` is unset
    (heat_rate == 0 case) it collapses to just ``gcost``, matching the
    legacy baked value byte-for-byte.  For ``heat_rate > 0`` the scalar
    path emits the raw heat_rate so gtopt re-multiplies by fuel.price
    at LP-build — same LP coefficient as a hypothetical baked path
    would have produced.  No double-counting on any branch.

    Parameterization spans zero (the gap-#1 trigger), small (0.5), and
    large (9.0) heat rates — the last exercises the scalar path
    properly emitting the heat_rate scalar.
    """
    vom_charge = 2.0
    fuel_transport = 0.5

    fuels = (
        FuelSpec(
            object_id=1,
            name="TestFuel",
            price=80.0,
        ),
    )
    gens = (
        GeneratorSpec(
            object_id=100,
            name="ThermalUnit",
            bus_name="b1",
            pmax=50.0,
            heat_rate=heat_rate,
            vom_charge=vom_charge,
            fuel_transport=fuel_transport,
            fuel_names=("TestFuel",),
        ),
    )
    gen_out = build_generator_array(gens, fuels)
    entry = gen_out[0]

    # FK always preserved when primary_fuel is set — the whole point of
    # gap #1 widening.  Pre-fix the heat_rate == 0 case dropped the FK.
    assert entry["fuel"] == "TestFuel", (
        f"Fuel FK must be preserved at heat_rate={heat_rate}"
    )
    # gcost is the non-fuel part only — NEVER pre-bakes ``heat_rate ×
    # fuel.price`` regardless of the heat_rate value.  This is the
    # decomposition invariant the test exists to lock down.
    assert entry["gcost"] == pytest.approx(vom_charge + fuel_transport), (
        f"gcost must be non-fuel only at heat_rate={heat_rate}; "
        f"got {entry['gcost']}, expected {vom_charge + fuel_transport}"
    )
    # heat_rate emitted iff > 0 (zero is omitted as redundant noise).
    if heat_rate > 0.0:
        assert "heat_rate" in entry, (
            f"heat_rate must be emitted when > 0 (heat_rate={heat_rate})"
        )
        # Raw value — NO pre-multiplication by anything.
        assert entry["heat_rate"] == pytest.approx(heat_rate)
    else:
        assert "heat_rate" not in entry, (
            "heat_rate must be omitted when == 0 (gap-#1 contract)"
        )
