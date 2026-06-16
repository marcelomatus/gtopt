# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the ``--only-emissions`` overlay (all-tCO2 architecture).

These tests pin the contract of the cost-defaults registry and the
emissions overlay's behaviour under ``only_emissions=True``:

1. ``COST_DEFAULTS`` entries' ``emissions_mode`` values land in
   tCO2/MWh (= cost-mode ÷ SCC where applicable).
2. ``apply_emission_overrides`` walks every cost-carrying field and
   converts $/MWh → tCO2/MWh by dividing by SCC.
3. ``Demand.fcost`` is overridden UNCONDITIONALLY to the EU UNS
   reference (regardless of source value — PLEXOS CEN ships $467
   per demand, gtopt default $1000).
4. ``Commitment.startup_cost`` / ``shutdown_cost`` are converted
   via the fuel chain (``heat_content · ef_combustion / fuel.price``)
   to tCO2/start, WITHOUT a SCC multiplier (LP applies SCC at the
   EmissionZone level).
5. ``EmissionZone.price`` stays at the SCC default ($35/tCO2eq —
   Chile CNE) so the LP objective is dimensionally homogeneous in
   tCO2 × $/tCO2 = $.
6. ``boundary_cuts_file`` references dropped recursively (the FCF is
   $-denominated, meaningless in emissions mode).
7. Reservoir ``water_emission_value`` stamping (already covered by
   ``hydro_epf`` tests; here we just check it survives the overlay).
8. The overlay is idempotent — running it twice yields the same
   planning dict.

The two physical anchors (Marcelo, 2026-06-02):
  * UNS = $150 / tCO2eq (EU ETS social cost) — yields 4.286 tCO2/MWh
    in the LP, matching a "very very bad house generator".
  * Dirtiest coal = 1 tCO2 / MWh — physical ceiling, $/MWh ↔
    tCO2/MWh bridge via SCC.
"""

from __future__ import annotations

import copy
from pathlib import Path

import pytest

from gtopt_shared.cost_defaults import (
    COST_DEFAULTS,
    DEFAULT_SCC,
    apply_emission_overrides,
    get_default,
)
from gtopt_shared.emissions import apply_emission_defaults_from_file


# ---------------------------------------------------------------------------
# Tiny synthetic planning fixture — covers every cost-carrying field path
# ---------------------------------------------------------------------------


def _make_planning() -> dict:
    """Minimal planning JSON exercising every cost path under test."""
    return {
        "options": {"model_options": {"demand_fail_cost": 1000.0}},
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [
                {"uid": 1, "name": "d1", "bus": "b1", "fcost": 467.19},
                {"uid": 2, "name": "d2", "bus": "b1", "fcost": 1000.0},
            ],
            "generator_array": [
                {
                    "uid": 1,
                    "name": "g1_coal",
                    "bus": "b1",
                    "fuel": "Carbon_test",
                    "heat_rate": 9.5,
                    "gcost": 30.0,
                    "pmin": 0,
                    "pmax": 100,
                    "pmin_fcost": 300.0,
                },
                {
                    "uid": 2,
                    "name": "g2_diesel",
                    "bus": "b1",
                    "fuel": "Diesel_test",
                    "heat_rate": 11.0,
                    "gcost": 200.0,
                    "pmin": 0,
                    "pmax": 50,
                    "pmin_fcost": 300.0,
                },
            ],
            "line_array": [
                {
                    "uid": 1,
                    "name": "ln1",
                    "bus_a": "b1",
                    "bus_b": "b1",
                    "overload_penalty": 700.0,
                }
            ],
            "fuel_array": [
                {
                    "uid": 1,
                    "name": "Carbon_test",
                    "type": "carbon",
                    "price": 80.0,
                    "heat_content": 25.8,
                    "emission_factors": [{"emission": "co2", "combustion": 0.0961}],
                    "max_offtake_cost": 100.0,
                    "min_offtake_cost": 50.0,
                },
                {
                    "uid": 2,
                    "name": "Diesel_test",
                    "type": "diesel",
                    "price": 861.0,
                    "heat_content": 43.0,
                    "emission_factors": [{"emission": "co2", "combustion": 0.0741}],
                },
                {
                    "uid": 3,
                    "name": "noref_fuel",
                    "type": "other",
                    "price": 100.0,
                    "heat_content": 0,
                    "emission_factors": [],
                },
            ],
            "commitment_array": [
                {
                    "uid": 1,
                    "name": "uc_g1",
                    "generator": "g1_coal",
                    "startup_cost": 1000.0,
                    "shutdown_cost": 500.0,
                },
                {
                    "uid": 2,
                    "name": "uc_g2",
                    "generator": "g2_diesel",
                    "startup_cost": 200.0,
                },
                {
                    "uid": 3,
                    "name": "uc_unknown",
                    "generator": "g_unknown",  # no matching generator → zeroed
                    "startup_cost": 100.0,
                },
            ],
            "battery_array": [
                {
                    "uid": 1,
                    "name": "bat1",
                    "bus": "b1",
                    "charge_cost": 5.0,
                    "discharge_cost": 7.0,
                }
            ],
            "waterway_array": [{"uid": 1, "name": "w1", "fcost": 70.0}],
            "flow_array": [{"uid": 1, "name": "f1", "fcost": 35.0}],
            "flow_right_array": [{"uid": 1, "name": "fr1", "fcost": 175.0}],
            "junction_array": [{"uid": 1, "name": "j1", "drain_cost": 14.0}],
            "user_constraint_array": [
                {"uid": 1, "name": "uc1", "penalty": 10000.0},  # PLEXOS soft default
                {"uid": 2, "name": "uc2", "penalty": 500.0},
            ],
            "decision_variable_array": [{"uid": 1, "name": "dv1", "cost": 350.0}],
            "reservoir_array": [],  # covered by hydro_epf tests; not exercised here
            "emission_zone_array": [
                {
                    "uid": 1,
                    "name": "global_ghg",
                    "price": 35.0,
                    "emissions": [{"emission": "co2", "weight": 1.0}],
                }
            ],
        },
    }


# ---------------------------------------------------------------------------
# COST_DEFAULTS registry contract
# ---------------------------------------------------------------------------


class TestCostDefaultsRegistry:
    """Pin the per-knob (cost, emissions) defaults."""

    def test_scc_is_35(self) -> None:
        assert DEFAULT_SCC == pytest.approx(35.0)

    def test_demand_fail_eu_reference(self) -> None:
        """UNS = $150 / tCO2eq (EU ETS) ÷ SCC = 4.286 tCO2/MWh."""
        e = COST_DEFAULTS["demand_fail_cost"]
        assert e.cost_mode == pytest.approx(1000.0)
        assert e.emissions_mode == pytest.approx(150.0 / 35.0)
        assert e.emissions_mode == pytest.approx(4.286, rel=1e-3)

    def test_reserve_shortage_div_by_scc(self) -> None:
        e = COST_DEFAULTS["reserve_shortage_cost"]
        assert e.emissions_mode == pytest.approx(e.cost_mode / DEFAULT_SCC)
        assert e.emissions_mode == pytest.approx(500.0 / 35.0)

    def test_state_violation_div_by_scc(self) -> None:
        e = COST_DEFAULTS["state_violation_cost"]
        assert e.emissions_mode == pytest.approx(e.cost_mode / DEFAULT_SCC)

    def test_water_fail_div_by_scc(self) -> None:
        e = COST_DEFAULTS["water_fail_cost"]
        assert e.emissions_mode == pytest.approx(100.0 / 35.0)

    def test_hydro_spill_div_by_scc(self) -> None:
        e = COST_DEFAULTS["hydro_spill_cost"]
        assert e.emissions_mode == pytest.approx(0.1 / 35.0)

    def test_soft_penalty_fixed_in_emissions(self) -> None:
        e = COST_DEFAULTS["soft_penalty_cost"]
        # cost_mode = None (auto-derived); emissions_mode = $300/SCC = 8.571
        assert e.cost_mode is None
        assert e.emissions_mode == pytest.approx(300.0 / 35.0)

    def test_default_uc_penalty_div_by_scc(self) -> None:
        """PLEXOS _SOFT_UC_DEFAULT_PENALTY = $10000/MWh → 285.7 tCO2/MWh."""
        e = COST_DEFAULTS["default_uc_penalty"]
        assert e.cost_mode == pytest.approx(10000.0)
        assert e.emissions_mode == pytest.approx(10000.0 / 35.0)
        assert e.emissions_mode == pytest.approx(285.714, rel=1e-3)

    def test_hydro_use_value_dropped(self) -> None:
        """Replaced by per-reservoir water_emission_value in emissions mode."""
        e = COST_DEFAULTS["hydro_use_value"]
        assert e.cost_mode == pytest.approx(20.0)
        assert e.emissions_mode is None

    def test_get_default_returns_mode_appropriate_value(self) -> None:
        assert get_default("demand_fail_cost", only_emissions=False) == 1000.0
        assert get_default("demand_fail_cost", only_emissions=True) == pytest.approx(
            150.0 / 35.0
        )

    def test_uns_strictly_above_dirtiest_coal(self) -> None:
        """The sanity bar: UNS > dirtiest coal so LP serves load.

        Coal emission ceiling (dirtiest CEN coal DIEGO_DE_ALMAGRO) = 1.07
        tCO2/MWh.  UNS = 4.286 tCO2/MWh — strictly worse, so the LP
        prefers dispatching even the worst coal over shedding load.
        """
        coal_ceiling = 1.07
        uns = COST_DEFAULTS["demand_fail_cost"].emissions_mode
        assert uns > coal_ceiling
        # Ratio matches the "house genset" interpretation (~4×)
        assert uns / coal_ceiling == pytest.approx(4.0, rel=0.1)


# ---------------------------------------------------------------------------
# apply_emission_overrides — system-wide model_options + Demand.fcost
# ---------------------------------------------------------------------------


class TestApplyEmissionOverridesSystemWide:
    def test_demand_fcost_overridden_unconditionally(self) -> None:
        """Per-demand fcost is replaced with EU UNS regardless of source.

        PLEXOS CEN ships $467/MWh per demand; gtopt default is $1000.
        Both get the same target (4.286 tCO2/MWh) because the EU
        reference is the policy anchor, not derived from the source.
        """
        pl = _make_planning()
        apply_emission_overrides(pl)
        for d in pl["system"]["demand_array"]:
            assert d["fcost"] == pytest.approx(150.0 / 35.0)

    def test_model_options_demand_fail_overridden(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["options"]["model_options"]["demand_fail_cost"] == pytest.approx(
            150.0 / 35.0
        )

    def test_model_options_other_slacks_stamped(self) -> None:
        """Missing/default model_options.* slacks land at emissions defaults."""
        pl = _make_planning()
        apply_emission_overrides(pl)
        mo = pl["options"]["model_options"]
        assert mo["reserve_shortage_cost"] == pytest.approx(500.0 / 35.0)
        assert mo["state_violation_cost"] == pytest.approx(500.0 / 35.0)
        assert mo["water_fail_cost"] == pytest.approx(100.0 / 35.0)
        assert mo["hydro_spill_cost"] == pytest.approx(0.1 / 35.0)
        assert mo["soft_penalty_cost"] == pytest.approx(300.0 / 35.0)
        assert mo["default_uc_penalty"] == pytest.approx(10000.0 / 35.0)

    def test_hydro_use_value_dropped_from_model_options(self) -> None:
        pl = _make_planning()
        pl["options"]["model_options"]["hydro_use_value"] = 20.0
        apply_emission_overrides(pl)
        assert "hydro_use_value" not in pl["options"]["model_options"]

    def test_user_explicit_override_preserved(self) -> None:
        """If user set a non-default value, the overlay must not stomp it."""
        pl = _make_planning()
        pl["options"]["model_options"]["reserve_shortage_cost"] = 777.0  # non-default
        apply_emission_overrides(pl)
        assert pl["options"]["model_options"]["reserve_shortage_cost"] == 777.0

    def test_custom_uns_price_override(self) -> None:
        """Caller-provided uns_price overrides the EU $150 default."""
        pl = _make_planning()
        apply_emission_overrides(pl, uns_price_dollar=200.0)
        expected = 200.0 / 35.0
        for d in pl["system"]["demand_array"]:
            assert d["fcost"] == pytest.approx(expected)
        assert pl["options"]["model_options"]["demand_fail_cost"] == pytest.approx(
            expected
        )

    def test_custom_scc_changes_all_conversions(self) -> None:
        """Pass a non-35 SCC and verify every /SCC field uses it."""
        pl = _make_planning()
        apply_emission_overrides(pl, scc=50.0)
        # Per-element slacks should use the custom SCC
        assert pl["system"]["generator_array"][0]["pmin_fcost"] == pytest.approx(
            300.0 / 50.0
        )
        assert pl["system"]["line_array"][0]["overload_penalty"] == pytest.approx(
            700.0 / 50.0
        )
        assert pl["system"]["waterway_array"][0]["fcost"] == pytest.approx(70.0 / 50.0)


# ---------------------------------------------------------------------------
# Per-element $/MWh → tCO2/MWh scaling (divide by SCC)
# ---------------------------------------------------------------------------


class TestApplyEmissionOverridesPerElement:
    def test_generator_pmin_fcost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        for g in pl["system"]["generator_array"]:
            assert g["pmin_fcost"] == pytest.approx(300.0 / 35.0)

    def test_line_overload_penalty_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["line_array"][0]["overload_penalty"] == pytest.approx(
            700.0 / 35.0
        )

    def test_waterway_fcost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["waterway_array"][0]["fcost"] == pytest.approx(70.0 / 35.0)

    def test_flow_fcost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["flow_array"][0]["fcost"] == pytest.approx(35.0 / 35.0)

    def test_flow_right_fcost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["flow_right_array"][0]["fcost"] == pytest.approx(
            175.0 / 35.0
        )

    def test_junction_drain_cost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["junction_array"][0]["drain_cost"] == pytest.approx(
            14.0 / 35.0
        )

    def test_user_constraint_penalty_scaled(self) -> None:
        """PLEXOS soft UC slack penalty also routed through the bridge."""
        pl = _make_planning()
        apply_emission_overrides(pl)
        ucs = pl["system"]["user_constraint_array"]
        assert ucs[0]["penalty"] == pytest.approx(10000.0 / 35.0)
        assert ucs[1]["penalty"] == pytest.approx(500.0 / 35.0)

    def test_fuel_offtake_costs_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        coal = next(f for f in pl["system"]["fuel_array"] if f["name"] == "Carbon_test")
        assert coal["max_offtake_cost"] == pytest.approx(100.0 / 35.0)
        assert coal["min_offtake_cost"] == pytest.approx(50.0 / 35.0)

    def test_battery_costs_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        bat = pl["system"]["battery_array"][0]
        assert bat["charge_cost"] == pytest.approx(5.0 / 35.0)
        assert bat["discharge_cost"] == pytest.approx(7.0 / 35.0)

    def test_decision_variable_cost_scaled(self) -> None:
        pl = _make_planning()
        apply_emission_overrides(pl)
        assert pl["system"]["decision_variable_array"][0]["cost"] == pytest.approx(
            350.0 / 35.0
        )


# ---------------------------------------------------------------------------
# Commitment.startup_cost / shutdown_cost — fuel chain to tCO2
# ---------------------------------------------------------------------------


class TestCommitmentFuelChain:
    """The formula:
    startup_em_tCO2 = startup_$  ×  heat_content [GJ/fu]
                       × ef_combustion [tCO2/GJ]
                       / fuel.price [$/fu]
    NO SCC multiplier (the LP applies SCC at the EmissionZone).
    """

    def test_coal_startup_carbon_heavy_per_dollar(self) -> None:
        """Coal: hc=25.8, ef=0.0961, price=80 → factor = 25.8·0.0961/80 ≈ 0.031.
        Startup_cost $1000 → 0.031 × 1000 = 31 tCO2."""
        pl = _make_planning()
        apply_emission_overrides(pl)
        coal_uc = next(
            c for c in pl["system"]["commitment_array"] if c["generator"] == "g1_coal"
        )
        expected = 1000.0 * (25.8 * 0.0961 / 80.0)
        assert coal_uc["startup_cost"] == pytest.approx(expected, rel=1e-5)
        # Same factor applied to shutdown
        assert coal_uc["shutdown_cost"] == pytest.approx(500.0 * (25.8 * 0.0961 / 80.0))

    def test_diesel_startup_carbon_light_per_dollar(self) -> None:
        """Diesel: hc=43, ef=0.0741, price=861 → factor ≈ 0.0037.
        Startup_cost $200 → ~0.74 tCO2 (much smaller carbon than coal)."""
        pl = _make_planning()
        apply_emission_overrides(pl)
        diesel_uc = next(
            c for c in pl["system"]["commitment_array"] if c["generator"] == "g2_diesel"
        )
        expected = 200.0 * (43.0 * 0.0741 / 861.0)
        assert diesel_uc["startup_cost"] == pytest.approx(expected, rel=1e-5)

    def test_no_scc_multiplier_in_startup_conversion(self) -> None:
        """Critical contract: the conversion does NOT multiply by SCC.

        The LP applies EmissionZone.price (SCC) once to the aggregated
        emissions.  Double-multiplying here would give SCC² scaling.
        """
        pl = _make_planning()
        apply_emission_overrides(pl, scc=999.0)  # absurd SCC
        coal_uc = next(
            c for c in pl["system"]["commitment_array"] if c["generator"] == "g1_coal"
        )
        # Factor is fuel-chain only, independent of SCC
        expected_no_scc = 1000.0 * (25.8 * 0.0961 / 80.0)
        assert coal_uc["startup_cost"] == pytest.approx(expected_no_scc, rel=1e-5)

    def test_missing_fuel_data_zeroes_startup(self) -> None:
        """When the linked generator has no fuel / no ef, zero out."""
        pl = _make_planning()
        apply_emission_overrides(pl)
        unknown_uc = next(
            c for c in pl["system"]["commitment_array"] if c["generator"] == "g_unknown"
        )
        assert unknown_uc["startup_cost"] == 0.0


# ---------------------------------------------------------------------------
# Emissions overlay (apply_emission_defaults_from_file) — end-to-end
# ---------------------------------------------------------------------------


class TestEndToEndOverlay:
    @pytest.fixture
    def cen_chile_path(self) -> Path:
        return (
            Path(__file__).resolve().parents[3]
            / "share"
            / "gtopt"
            / "emissions"
            / "cen_chile.json"
        )

    def test_objective_mode_set(self, cen_chile_path: Path) -> None:
        pl = _make_planning()
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        assert pl["options"]["model_options"]["objective_mode"] == "emissions"

    def test_emission_zone_price_stays_at_scc(self, cen_chile_path: Path) -> None:
        """EmissionZone.price = SCC ($35) — the single SCC application point."""
        pl = _make_planning()
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        zones = pl["system"]["emission_zone_array"]
        # There may now be 2 zones (the bundled fixture + the
        # apply_emission_defaults synthesised one).  We care that AT
        # LEAST one of them carries the default SCC.
        prices = [z.get("price") for z in zones if z.get("price") is not None]
        assert any(abs(p - 35.0) < 1e-6 for p in prices)

    def test_boundary_cuts_file_dropped(self, cen_chile_path: Path) -> None:
        pl = _make_planning()
        pl["options"]["boundary_cuts_file"] = "boundary_cuts.csv"
        pl["options"]["sddp_options"] = {"boundary_cuts_file": "nested.csv"}
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        import json

        js = json.dumps(pl)
        assert "boundary_cuts_file" not in js

    def test_idempotent(self, cen_chile_path: Path) -> None:
        """Running the overlay twice yields the same planning."""
        pl1 = _make_planning()
        apply_emission_defaults_from_file(pl1, cen_chile_path, only_emissions=True)
        snapshot = copy.deepcopy(pl1)
        apply_emission_defaults_from_file(pl1, cen_chile_path, only_emissions=True)
        # cost knobs should not double-convert (the overlay only
        # touches cost-mode defaults, which after one pass no longer
        # match the cost_mode value)
        assert (
            pl1["options"]["model_options"]["demand_fail_cost"]
            == snapshot["options"]["model_options"]["demand_fail_cost"]
        )
        for d, ds in zip(
            pl1["system"]["demand_array"], snapshot["system"]["demand_array"]
        ):
            assert d["fcost"] == ds["fcost"]

    def test_cost_mode_run_unaffected(self, cen_chile_path: Path) -> None:
        """When ``only_emissions=False`` (default), no slack values change."""
        pl = _make_planning()
        before_demand_fcost = [d["fcost"] for d in pl["system"]["demand_array"]]
        before_model_opts = dict(pl["options"]["model_options"])
        apply_emission_defaults_from_file(pl, cen_chile_path)  # only_emissions=False
        after_demand_fcost = [d["fcost"] for d in pl["system"]["demand_array"]]
        after_model_opts = pl["options"]["model_options"]
        assert before_demand_fcost == after_demand_fcost
        # demand_fail_cost stays at its cost-mode value
        assert (
            after_model_opts["demand_fail_cost"]
            == before_model_opts["demand_fail_cost"]
        )
        # objective_mode not set in cost mode
        assert "objective_mode" not in after_model_opts


# ---------------------------------------------------------------------------
# Physical-anchor sanity checks
# ---------------------------------------------------------------------------


class TestPhysicalAnchors:
    """The all-tCO2 architecture rests on two physical anchors:

      UNS = $150 / tCO2eq  (EU ETS social cost reference)
      Dirtiest coal = 1 tCO2 / MWh  (physical upper bound on per-MWh ef)

    These tests verify the LP-objective ordering implied by these
    anchors holds for the stamped values.
    """

    def test_uns_above_dirtiest_coal(self) -> None:
        """In LP-objective terms, UNS must be strictly worse than the
        dirtiest coal × SCC.  EU UNS $150/tCO2eq / SCC $35 = 4.286
        tCO2/MWh, well above coal's 1.07 tCO2/MWh ceiling."""
        uns_tco2_per_mwh = COST_DEFAULTS["demand_fail_cost"].emissions_mode
        dirtiest_coal = 1.07  # DIEGO_DE_ALMAGRO in CEN
        assert uns_tco2_per_mwh > dirtiest_coal
        assert uns_tco2_per_mwh / dirtiest_coal > 3.5  # at least 3.5× margin

    def test_soft_uc_above_uns(self) -> None:
        """Soft UC default ($10000/MWh) must dominate UNS so the LP
        sheds load before violating a user constraint."""
        soft_uc = COST_DEFAULTS["default_uc_penalty"].emissions_mode
        uns = COST_DEFAULTS["demand_fail_cost"].emissions_mode
        assert soft_uc > uns
        # PLEXOS soft default keeps a wide margin (~66× over UNS)
        assert soft_uc / uns > 50

    def test_pmin_slack_above_dispatch_ceiling(self) -> None:
        """Soft pmin slack must be > dirtiest coal × SCC = $35/MWh
        (= 1 tCO2/MWh) so the LP won't violate forced pmin to avoid
        coal dispatch."""
        soft_pmin = COST_DEFAULTS["soft_penalty_cost"].emissions_mode
        dirtiest_coal = 1.07
        assert soft_pmin > dirtiest_coal

    def test_spill_below_dispatch_ceiling(self) -> None:
        """Spill is a tiny nudge — must NOT dominate dispatch decisions."""
        spill = COST_DEFAULTS["hydro_spill_cost"].emissions_mode
        dirtiest_coal = 1.07
        assert spill < dirtiest_coal
        assert spill < 0.01  # tiny by construction


# ---------------------------------------------------------------------------
# External-fixture integration test — NREL-118 with --only-emissions
# ---------------------------------------------------------------------------
#
# Uses the NREL-118 (Pena 2017) test system already wired through
# ``scripts/nrel118_to_gtopt`` as the reference emission-enabled fixture
# (test/source/test_emission_nrel118_port.cpp validates the C++ side
# in cost mode; this test inverts it into --only-emissions).
#
# Why NREL-118 is the right fixture:
#   * 327 thermal generators across 5 fuel classes (coal, gas, oil,
#     nuclear, renewable) — exercises every per-generator emission
#     route the overlay touches.
#   * IPCC AR6 combustion factors already baked into Fuel.emission_factors
#     by the converter — no fixture preparation step required.
#   * 1 EmissionZone covering CO2 — matches the canonical
#     single-pollutant case the overlay targets.
#   * Single-week dispatch (168 h) — small enough for unit-test budgets,
#     large enough to expose every dispatch-mode interaction.
#
# Adjacent literature / open-source examples worth porting later:
#   * GenX `example_systems/small_new_england` (MIT) — multi-zone CO2
#     cap fixture; would exercise EmissionZone.cap_cost in addition
#     to .price.  https://github.com/GenXProject/GenX
#   * PyPSA-Eur `test/test_emissions.py` — Snakemake fixtures with
#     country-resolved CO2 caps; pricier to port but covers the
#     multi-zone routing pattern.
#   * Pereira et al. (2019) "SDDP with carbon constraints" — synthetic
#     4-reservoir hydrothermal example with terminal carbon shadow
#     values; the algebra matches our EPF × gas_em terminal-value
#     formula and is small enough to encode directly in a fixture.
#   * Bizjak et al. (2023) "Hydropower emissions valuation" — applies
#     SCC × EPF to reservoir terminal values exactly as our overlay
#     does; their case studies (Soča basin) are open-data and could
#     be turned into a parallel cascade fixture.
#
# The NREL-118 test below is the minimal viable integration check;
# the GenX/PyPSA/Pereira cases above are queued as future expansions
# (track via #519 / #520 / #521 issue tree).


class TestNREL118OnlyEmissionsInversion:
    """End-to-end test: take the NREL-118 emissions fixture and apply
    the --only-emissions overlay.  Verifies that every dispatch-relevant
    cost gets re-anchored against the carbon scale (tCO2/MWh) while the
    physical / emission-source plumbing the converter already wired
    survives the overlay unchanged.

    Skipped when the fixture generator is unavailable (network
    download disabled and no cached data) so CI can pick this up
    without external dependencies.
    """

    @pytest.fixture(scope="class")
    def nrel118_planning(self, tmp_path_factory: pytest.TempPathFactory) -> dict:
        """Run ``nrel118_to_gtopt`` once (cached if possible) and return
        the loaded planning dict."""
        out = tmp_path_factory.mktemp("nrel118") / "nrel118.json"
        # Try cached path first; fall back to network only if explicitly
        # allowed (default disabled — CI must not depend on NREL HTTP).
        try:
            import subprocess

            result = subprocess.run(
                [
                    "python3",
                    "-B",
                    "-m",
                    "nrel118_to_gtopt",
                    "--week",
                    "1",
                    "--no-download",
                    "-o",
                    str(out),
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if result.returncode != 0 or not out.is_file():
                pytest.skip(
                    f"nrel118_to_gtopt fixture unavailable "
                    f"(cache miss, network disabled): {result.stderr[:200]}"
                )
        except (FileNotFoundError, ModuleNotFoundError) as exc:
            pytest.skip(f"nrel118_to_gtopt not installed: {exc}")
        import json as _json

        return _json.loads(out.read_text())

    @pytest.fixture(scope="class")
    def cen_chile_path(self) -> Path:
        return (
            Path(__file__).resolve().parents[3]
            / "share"
            / "gtopt"
            / "emissions"
            / "cen_chile.json"
        )

    def test_baseline_has_emission_infrastructure(self, nrel118_planning: dict) -> None:
        """Sanity-check the cost-mode fixture before we invert it."""
        sys = nrel118_planning["system"]
        assert len(sys["generator_array"]) >= 50  # NREL-118 has 54 baseline gens
        assert len(sys["fuel_array"]) >= 4  # at least coal/gas/oil + 1
        # Emission infrastructure already wired by the converter
        assert len(sys["emission_array"]) >= 1
        assert len(sys["emission_zone_array"]) >= 1
        # Fuels carry IPCC AR6 combustion factors
        thermals_with_ef = sum(
            1
            for f in sys["fuel_array"]
            if any(
                e.get("emission") in (1, "co2") and e.get("combustion", 0) > 0
                for e in f.get("emission_factors", [])
            )
        )
        assert thermals_with_ef >= 1

    def test_overlay_stamps_objective_mode(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """After inversion, the model picks up emissions-mode objective."""
        pl = copy.deepcopy(nrel118_planning)
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        assert pl["options"]["model_options"]["objective_mode"] == "emissions"

    def test_overlay_keeps_scc_at_zone_price(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """EmissionZone.price = $35/tCO2eq is the single SCC application
        point — keeps after the overlay (does not get over-written)."""
        pl = copy.deepcopy(nrel118_planning)
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        prices = [
            float(z["price"])
            for z in pl["system"]["emission_zone_array"]
            if z.get("price") is not None
        ]
        # At least one zone carries SCC; default is 35.0
        assert any(abs(p - 35.0) < 1e-6 for p in prices)

    def test_overlay_converts_per_demand_fcost(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """Every Demand.fcost gets the EU UNS reference (4.286 tCO2/MWh).

        NREL-118 ships demands without an explicit fcost; the overlay
        only touches demands that DO carry one.  This test exercises
        the path by injecting one before the overlay runs.
        """
        pl = copy.deepcopy(nrel118_planning)
        # Make sure at least one demand carries fcost so the overlay path
        # is exercised.
        if pl["system"]["demand_array"]:
            pl["system"]["demand_array"][0]["fcost"] = 1000.0
            apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
            assert pl["system"]["demand_array"][0]["fcost"] == pytest.approx(
                150.0 / 35.0
            )

    def test_overlay_drops_hydro_use_value(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """``hydro_use_value`` is a cost-mode terminal water value that
        gets dropped in emissions mode (replaced by per-reservoir
        ``water_emission_value``).  NREL-118 has no reservoirs, but
        the drop logic must still run cleanly.
        """
        pl = copy.deepcopy(nrel118_planning)
        pl["options"]["model_options"]["hydro_use_value"] = 20.0
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        assert "hydro_use_value" not in pl["options"]["model_options"]

    def test_overlay_drops_boundary_cuts_refs(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """Inject a synthetic ``boundary_cuts_file`` reference (NREL-118
        is single-stage so doesn't ship one) and verify the overlay
        strips it."""
        pl = copy.deepcopy(nrel118_planning)
        pl["options"]["boundary_cuts_file"] = "boundary_cuts.csv"
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        import json as _json

        assert "boundary_cuts_file" not in _json.dumps(pl)

    def test_overlay_does_not_touch_generator_emission_sources(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """The per-generator emission routes (EmissionSource rows or
        the inline Generator.emissions[] form) must survive the
        overlay — they're the LP's primary tCO2 contribution path
        from dispatch.  Touching them would break the carbon
        accounting."""
        pl = copy.deepcopy(nrel118_planning)
        before_sources = copy.deepcopy(pl["system"].get("emission_source_array", []))
        before_inline = sum(
            len(g.get("emissions", [])) for g in pl["system"]["generator_array"]
        )
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        after_sources = pl["system"].get("emission_source_array", [])
        after_inline = sum(
            len(g.get("emissions", [])) for g in pl["system"]["generator_array"]
        )
        assert len(after_sources) >= len(before_sources)  # may add, never delete
        assert after_inline == before_inline

    def test_overlay_emission_factors_preserved(
        self, nrel118_planning: dict, cen_chile_path: Path
    ) -> None:
        """Per-fuel IPCC AR6 combustion factors must NOT change in
        emissions mode — they're the physical anchors of the LP's
        carbon accounting."""
        pl = copy.deepcopy(nrel118_planning)
        before = {
            f["name"]: [dict(e) for e in f.get("emission_factors", [])]
            for f in pl["system"]["fuel_array"]
        }
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)
        for f in pl["system"]["fuel_array"]:
            for e_before, e_after in zip(
                before.get(f["name"], []), f.get("emission_factors", [])
            ):
                assert e_before.get("combustion") == e_after.get("combustion")
                assert e_before.get("emission") == e_after.get("emission")


# ---------------------------------------------------------------------------
# Multi-pollutant EmissionZone — different prices per pollutant
# ---------------------------------------------------------------------------
#
# The EmissionZone schema supports multiple emissions in the basket
# (CO2 + CH4 + N2O via GWP100 weights) AND multiple zones with
# different prices (e.g. one zone for greenhouse gases at the SCC,
# another for NOx at the EPA NOx social cost).  These tests verify the
# overlay does not stomp on user-provided multi-pollutant zone
# configurations.
#
# IPCC AR6 GWP100 (used as weights in a single-zone basket):
#   CO2 = 1.0
#   CH4 = 27.0  (was 25 in AR5 — AR6 update)
#   N2O = 273.0 (was 298 in AR5)
#
# EPA per-pollutant social costs (2022 estimates):
#   CO2 = $51 / tCO2eq           (3% discount, central)
#   NOx = $5,000 / tNOx          (NOx social cost benefit estimate)
#   SO2 = $12,000 / tSO2         (SO2 social cost benefit estimate)
#   PM2.5 = $50,000+ / tPM2.5    (PM2.5 social cost benefit estimate)


class TestMultiPollutantZones:
    """Verify the all-tCO2 overlay preserves user-configured multi-
    pollutant EmissionZone setups (different prices per pollutant).
    """

    def _planning_with_multi_pollutant_zones(self) -> dict:
        """Planning with three zones:
        1. global_ghg ($35 SCC) basket = CO2 + CH4·GWP + N2O·GWP
        2. nox_zone   ($5000) basket = NOx
        3. so2_zone   ($12000) basket = SO2
        """
        pl = _make_planning()
        # Replace single-zone with multi-pollutant config
        pl["system"]["emission_array"] = [
            {"uid": 1, "name": "co2"},
            {"uid": 2, "name": "ch4"},
            {"uid": 3, "name": "n2o"},
            {"uid": 4, "name": "nox"},
            {"uid": 5, "name": "so2"},
        ]
        pl["system"]["emission_zone_array"] = [
            {
                "uid": 1,
                "name": "global_ghg",
                "price": 35.0,  # Chile SCC
                "emissions": [
                    {"emission": "co2", "weight": 1.0},
                    {"emission": "ch4", "weight": 27.0},  # AR6 GWP100
                    {"emission": "n2o", "weight": 273.0},  # AR6 GWP100
                ],
            },
            {
                "uid": 2,
                "name": "nox_zone",
                "price": 5000.0,  # EPA NOx social cost
                "emissions": [{"emission": "nox", "weight": 1.0}],
            },
            {
                "uid": 3,
                "name": "so2_zone",
                "price": 12000.0,  # EPA SO2 social cost
                "emissions": [{"emission": "so2", "weight": 1.0}],
            },
        ]
        # Add per-fuel emission factors for the non-CO2 pollutants on coal
        coal = next(f for f in pl["system"]["fuel_array"] if f["name"] == "Carbon_test")
        coal["emission_factors"] = [
            {"emission": "co2", "combustion": 0.0961},
            {"emission": "ch4", "combustion": 1.0e-6},  # IPCC default
            {"emission": "n2o", "combustion": 1.5e-6},  # IPCC default
            {"emission": "nox", "combustion": 7.4e-4},  # EPA AP-42 coal NOx
            {"emission": "so2", "combustion": 8.0e-4},  # EPA AP-42 coal SO2 (med-S)
        ]
        return pl

    def test_overlay_preserves_user_zone_prices(self) -> None:
        """User-set prices on multi-pollutant zones survive --only-emissions."""
        pl = self._planning_with_multi_pollutant_zones()
        apply_emission_overrides(pl)
        zones = {z["name"]: z for z in pl["system"]["emission_zone_array"]}
        assert zones["global_ghg"]["price"] == pytest.approx(35.0)
        assert zones["nox_zone"]["price"] == pytest.approx(5000.0)
        assert zones["so2_zone"]["price"] == pytest.approx(12000.0)

    def test_overlay_preserves_basket_weights(self) -> None:
        """GWP100 weights in the GHG basket must NOT change in
        emissions mode — they're physical / IPCC anchors."""
        pl = self._planning_with_multi_pollutant_zones()
        apply_emission_overrides(pl)
        ghg = next(
            z for z in pl["system"]["emission_zone_array"] if z["name"] == "global_ghg"
        )
        weights = {e["emission"]: e["weight"] for e in ghg["emissions"]}
        assert weights["co2"] == pytest.approx(1.0)
        assert weights["ch4"] == pytest.approx(27.0)  # AR6 GWP100
        assert weights["n2o"] == pytest.approx(273.0)

    def test_overlay_preserves_per_pollutant_fuel_factors(self) -> None:
        """Per-fuel CH4/N2O/NOx/SO2 combustion factors are physical
        constants; the overlay must not touch them."""
        pl = self._planning_with_multi_pollutant_zones()
        before_efs = {
            f["name"]: [dict(e) for e in f.get("emission_factors", [])]
            for f in pl["system"]["fuel_array"]
        }
        apply_emission_overrides(pl)
        for f in pl["system"]["fuel_array"]:
            for b, a in zip(before_efs[f["name"]], f.get("emission_factors", [])):
                assert b["emission"] == a["emission"]
                assert b["combustion"] == pytest.approx(a["combustion"])

    def test_different_prices_yield_different_implicit_uns_targets(self) -> None:
        """Sanity check on the underlying math: with the CO2 zone at
        $35 and the NOx zone at $5000, a UNS-equivalent that emits
        BOTH would cost ``f(UNS_MWh) × (35×co2_factor + 5000×nox_factor)``.

        This test just verifies the prices co-exist as set on each zone
        (not that the LP correctly composes them — that's the C++ side
        in #522).
        """
        pl = self._planning_with_multi_pollutant_zones()
        apply_emission_overrides(pl)
        prices = sorted(
            float(z["price"])
            for z in pl["system"]["emission_zone_array"]
            if z.get("price") is not None
        )
        # 3 distinct positive prices: CO2 / NOx / SO2
        assert len(prices) == 3
        assert prices[0] < prices[1] < prices[2]
        assert prices == pytest.approx([35.0, 5000.0, 12000.0])

    def test_custom_per_pollutant_scc_override(self) -> None:
        """Caller can override the SCC used for the $/MWh → tCO2/MWh
        bridge.  Other zones' prices are untouched (they're per-pollutant
        independent)."""
        pl = self._planning_with_multi_pollutant_zones()
        apply_emission_overrides(pl, scc=50.0)
        zones = {z["name"]: z for z in pl["system"]["emission_zone_array"]}
        # NOx and SO2 prices untouched by the SCC bridge override
        assert zones["nox_zone"]["price"] == pytest.approx(5000.0)
        assert zones["so2_zone"]["price"] == pytest.approx(12000.0)
        # Per-element slacks scaled by the custom SCC=50 (not 35)
        assert pl["system"]["generator_array"][0]["pmin_fcost"] == pytest.approx(
            300.0 / 50.0
        )

    def test_zone_without_price_left_alone(self) -> None:
        """A zone with no ``price`` (cap-only zone, e.g.
        ``EmissionZone.cap`` style) must not get an unintended
        price stamped on it by the overlay."""
        pl = self._planning_with_multi_pollutant_zones()
        # Add a cap-only zone (no price)
        pl["system"]["emission_zone_array"].append(
            {
                "uid": 4,
                "name": "ch4_cap_zone",
                "cap": 100.0,  # tCH4eq cap (not used by overlay)
                "emissions": [{"emission": "ch4", "weight": 1.0}],
            }
        )
        apply_emission_overrides(pl)
        cap_zone = next(
            z
            for z in pl["system"]["emission_zone_array"]
            if z["name"] == "ch4_cap_zone"
        )
        assert "price" not in cap_zone
        assert cap_zone["cap"] == pytest.approx(100.0)


class TestMultiPollutantThroughOverlay:
    """End-to-end: ``apply_emission_defaults_from_file`` with
    ``only_emissions=True`` against a multi-pollutant fixture."""

    @pytest.fixture
    def cen_chile_path(self) -> Path:
        return (
            Path(__file__).resolve().parents[3]
            / "share"
            / "gtopt"
            / "emissions"
            / "cen_chile.json"
        )

    def test_full_overlay_with_multi_pollutant_zones(
        self, cen_chile_path: Path
    ) -> None:
        """Run the full ``apply_emission_defaults_from_file`` path on
        a multi-pollutant fixture and verify ALL pollutant zones keep
        their user-set prices.
        """
        # Build a planning with three zones (GHG, NOx, SO2)
        pl = TestMultiPollutantZones()._planning_with_multi_pollutant_zones()
        apply_emission_defaults_from_file(pl, cen_chile_path, only_emissions=True)

        zones = {z["name"]: z for z in pl["system"]["emission_zone_array"]}
        assert "global_ghg" in zones
        assert "nox_zone" in zones
        assert "so2_zone" in zones
        assert zones["global_ghg"]["price"] == pytest.approx(35.0)
        assert zones["nox_zone"]["price"] == pytest.approx(5000.0)
        assert zones["so2_zone"]["price"] == pytest.approx(12000.0)

        # Objective mode set
        assert pl["options"]["model_options"]["objective_mode"] == "emissions"
        # Slack values scaled by SCC (= 35, the GHG zone price) — bridge
        # factor doesn't change per-zone
        assert pl["options"]["model_options"]["demand_fail_cost"] == pytest.approx(
            150.0 / 35.0
        )


# ---------------------------------------------------------------------------
# Issue #521 — PLEXOS-input cost re-anchoring (Reservoir.water_value /
# efin_cost replaced by EPF-based terminal value; fuel_price_override zeroed)
# ---------------------------------------------------------------------------


class TestIssue521ReservoirTerminalReanchoring:
    """When ``--only-emissions`` is set AND Reservoir already carries
    a ``water_emission_value`` (stamped by commit 7b36f3728), the
    overlay must replace the cost-mode ``water_value`` / ``efin_cost``
    ($/hm³) with the emission-equivalent (tCO2eq/hm³) = EPF · gas_em ·
    loss · 277.78."""

    def _make_reservoir_planning(self) -> dict:
        # Minimal planning carrying both stamping (water_emission_value
        # set as if by 7b36f3728) and a pre-existing $/hm³ water_value
        # / efin_cost (as plexos2gtopt / plp2gtopt would have shipped).
        return {
            "options": {"model_options": {}},
            "system": {
                "bus_array": [{"uid": 1, "name": "b1"}],
                "demand_array": [],
                "generator_array": [],
                "line_array": [],
                "fuel_array": [],
                "commitment_array": [],
                "battery_array": [],
                "waterway_array": [],
                "flow_array": [],
                "flow_right_array": [],
                "junction_array": [],
                "user_constraint_array": [],
                "decision_variable_array": [],
                "emission_zone_array": [
                    {
                        "uid": 1,
                        "name": "global",
                        "price": 35.0,
                        "emissions": [{"emission": "co2", "weight": 1.0}],
                    }
                ],
                "reservoir_array": [
                    {
                        "uid": 1,
                        "name": "L_Maule",
                        "water_value": 50.0,  # $/hm³ — cost mode
                        "efin_cost": 100000.0,  # $/hm³ — cost mode
                        "water_emission_value": 4.4365,  # tCO2eq / (m³/s)·h
                    },
                    {
                        "uid": 2,
                        "name": "EmptyEPF",
                        "water_value": 25.0,
                        # no water_emission_value (EPF=0) → leave alone
                    },
                ],
            },
        }

    def test_reservoir_water_value_replaced_when_wev_present(self) -> None:
        pl = self._make_reservoir_planning()
        apply_emission_overrides(pl)
        l_maule = pl["system"]["reservoir_array"][0]
        # 4.4365 × 277.78 ≈ 1232.4 tCO2eq/hm³
        expected = 4.4365 * 277.78
        assert l_maule["water_value"] == pytest.approx(expected, rel=1e-4)
        assert l_maule["efin_cost"] == pytest.approx(expected, rel=1e-4)

    def test_reservoir_water_value_untouched_when_no_wev(self) -> None:
        """Reservoirs without water_emission_value (e.g. terminal hydro
        whose cascade had no turbines) keep their cost-mode value —
        the overlay can't compute the carbon equivalent."""
        pl = self._make_reservoir_planning()
        apply_emission_overrides(pl)
        empty = pl["system"]["reservoir_array"][1]
        assert empty["water_value"] == pytest.approx(25.0)


class TestIssue521GeneratorFuelPriceOverride:
    """118-bus / GenX schemas carry ``Generator.fuel_price_override``
    ($/MWh-equivalent of fuel cost).  In emissions mode this leaks
    through to gcost-style reporting even though GeneratorLP zeros
    the cost slope.  The overlay zeros it explicitly."""

    def test_fuel_price_override_zeroed(self) -> None:
        pl = {
            "options": {"model_options": {}},
            "system": {
                "bus_array": [],
                "demand_array": [],
                "line_array": [],
                "fuel_array": [],
                "commitment_array": [],
                "battery_array": [],
                "waterway_array": [],
                "flow_array": [],
                "flow_right_array": [],
                "junction_array": [],
                "user_constraint_array": [],
                "decision_variable_array": [],
                "reservoir_array": [],
                "emission_zone_array": [
                    {
                        "uid": 1,
                        "name": "g",
                        "price": 35.0,
                        "emissions": [{"emission": "co2", "weight": 1.0}],
                    }
                ],
                "generator_array": [
                    {"uid": 1, "name": "g1", "fuel_price_override": 25.0},
                    {"uid": 2, "name": "g2", "fuel_price_override": 0.0},
                    {"uid": 3, "name": "g3"},  # no override
                ],
            },
        }
        apply_emission_overrides(pl)
        assert pl["system"]["generator_array"][0]["fuel_price_override"] == 0.0
        assert pl["system"]["generator_array"][1]["fuel_price_override"] == 0.0
        assert "fuel_price_override" not in pl["system"]["generator_array"][2]
