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
