"""Unit tests for :mod:`plexos2gtopt.gtopt_writer`.

These focus on the per-class array builders — the integration-test
suite covers the end-to-end ``build_planning`` → ``gtopt --lp-only``
path against the real bundle.
"""

from __future__ import annotations

from plexos2gtopt.entities import (
    BatterySpec,
    FuelSpec,
    GeneratorSpec,
    LineSpec,
)
from plexos2gtopt.gtopt_writer import (
    build_battery_array,
    build_fuel_array,
    build_generator_array,
    build_line_array,
)


def test_generator_gcost_thermal() -> None:
    """P3: thermal cost = heat_rate × fuel_price + vom_charge."""
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
    # 0.25 * 1500 + 8 = 383
    assert out[0]["gcost"] == 383.0


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
    """build_line_array converts negative tmin → positive tmax_ba."""
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
        ),
    )
    out = build_line_array(lines)
    assert out[0]["tmax_ab"] == 200.0
    assert out[0]["tmax_ba"] == 150.0
    assert out[0]["reactance"] == 0.12


def test_line_units_scales_capacity() -> None:
    """Parallel-line count multiplies both ratings."""
    lines = (
        LineSpec(
            object_id=2,
            name="dbl",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            tmin_ab=-100.0,
            units=2,
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
