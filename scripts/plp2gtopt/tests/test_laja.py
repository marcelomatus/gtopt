# -*- coding: utf-8 -*-

"""Tests for Laja irrigation agreement parser and writer."""

import json
from pathlib import Path

import pytest

from plp2gtopt.compressed_open import find_compressed_path
from plp2gtopt.laja_parser import LajaParser
from plp2gtopt.laja_writer import LajaWriter, _zones_to_bound_rule_segments


# Path to actual PLP test data (compressed)
_SUPPORT_DIR = Path(__file__).parent.parent.parent.parent / "support"
_PLP_2Y = _SUPPORT_DIR / "plp" / "2_years"


@pytest.fixture()
def laja_parser():
    """Parse the 2-year PLP Laja convention file."""
    resolved = find_compressed_path(_PLP_2Y / "plplajam.dat")
    if resolved is None:
        pytest.skip("plplajam.dat not found in support/plp/2_years")
    parser = LajaParser(resolved)
    parser.parse()
    return parser


@pytest.fixture()
def laja_config(laja_parser):
    """Return parsed Laja convention config."""
    return laja_parser.config


class TestVolumeZoneToSegments:
    """Test the volume zone → bound_rule segment conversion."""

    def test_laja_irrigation_segments(self):
        """Verify Laja irrigation volume zone conversion.

        PLP formula: Rights = 570 + 0.00*V1 + 0.40*V2 + 0.40*V3 + 0.25*V4
        Zone widths: 1200, 170, 530, 3682

        Expected piecewise-linear:
          V in [0, 1200):    R = 570 + 0*V = 570
          V in [1200, 1370): R = 570 + 0.40*(V-1200)
                             = 570 - 480 + 0.40*V = 90 + 0.40*V
          V in [1370, 1900): R = 570 + 0.40*170 + 0.40*(V-1370)
                             = 570 + 68 - 548 + 0.40*V = 90 + 0.40*V
                             (same slope, different constant computation)
          V in [1900, 5582]: R = 570 + 68 + 0.40*530 + 0.25*(V-1900)
                             = 570 + 68 + 212 - 475 + 0.25*V = 375 + 0.25*V
        """
        segments = _zones_to_bound_rule_segments(
            base=570,
            factors=[0.00, 0.40, 0.40, 0.25],
            widths=[1200, 170, 530, 3682],
            vol_muerto=0,
        )
        assert len(segments) == 4

        # Zone 1: V >= 0, slope=0, constant=570
        assert segments[0]["volume"] == pytest.approx(0)
        assert segments[0]["slope"] == pytest.approx(0)
        assert segments[0]["constant"] == pytest.approx(570)

        # Zone 2: V >= 1200, slope=0.40
        assert segments[1]["volume"] == pytest.approx(1200)
        assert segments[1]["slope"] == pytest.approx(0.40)
        # constant = 570 - 0.40*1200 = 90
        assert segments[1]["constant"] == pytest.approx(90)

        # Zone 3: V >= 1370, slope=0.40
        assert segments[2]["volume"] == pytest.approx(1370)
        assert segments[2]["slope"] == pytest.approx(0.40)

        # Zone 4: V >= 1900, slope=0.25
        assert segments[3]["volume"] == pytest.approx(1900)
        assert segments[3]["slope"] == pytest.approx(0.25)

        # Verify evaluation at known points
        # V = 0: 570
        assert segments[0]["constant"] + segments[0]["slope"] * 0 == pytest.approx(570)
        # V = 1500 (zone 2): 90 + 0.40*1500 = 690
        assert segments[1]["constant"] + segments[1]["slope"] * 1500 == pytest.approx(
            690
        )
        # V = 3000 (zone 4): 375 + 0.25*3000 = 1125
        assert segments[3]["constant"] + segments[3]["slope"] * 3000 == pytest.approx(
            1125
        )

    def test_laja_electric_segments(self):
        """Verify Laja electric volume zone conversion.

        Rights = 0 + 0.05*V1 + 0.05*V2 + 0.40*V3 + 0.65*V4
        """
        segments = _zones_to_bound_rule_segments(
            base=0,
            factors=[0.05, 0.05, 0.40, 0.65],
            widths=[1200, 170, 530, 3682],
            vol_muerto=0,
        )
        assert len(segments) == 4

        # Zone 1: slope=0.05, constant=0
        assert segments[0]["slope"] == pytest.approx(0.05)
        assert segments[0]["constant"] == pytest.approx(0)

        # V = 1000 (zone 1): 0 + 0.05*1000 = 50
        assert segments[0]["constant"] + segments[0]["slope"] * 1000 == pytest.approx(
            50
        )

        # V = 5582 (max, zone 4):
        # 0 + 0.05*1200 + 0.05*170 + 0.40*530 + 0.65*(5582-1900)
        # = 60 + 8.5 + 212 + 2393.3 = 2673.8
        expected = 60 + 8.5 + 212 + 0.65 * (5582 - 1900)
        actual = segments[3]["constant"] + segments[3]["slope"] * 5582
        assert actual == pytest.approx(expected)

    def test_dead_volume_offset(self):
        """Verify dead volume shifts all breakpoints."""
        segments = _zones_to_bound_rule_segments(
            base=100,
            factors=[0.5, 0.3],
            widths=[200, 300],
            vol_muerto=50,
        )
        assert segments[0]["volume"] == pytest.approx(50)
        assert segments[1]["volume"] == pytest.approx(250)  # 50 + 200

    def test_continuity_at_breakpoints(self):
        """Adjacent segments must agree at every breakpoint."""
        segments = _zones_to_bound_rule_segments(
            base=570,
            factors=[0.00, 0.40, 0.40, 0.25],
            widths=[1200, 170, 530, 3682],
            vol_muerto=0,
        )
        for i in range(1, len(segments)):
            v = segments[i]["volume"]
            val_prev = segments[i - 1]["constant"] + segments[i - 1]["slope"] * v
            val_curr = segments[i]["constant"] + segments[i]["slope"] * v
            assert val_prev == pytest.approx(val_curr), f"Discontinuity at V={v}"

    def test_empty_factors_flat_segment(self):
        """No zones should produce a single flat segment at dead volume."""
        segments = _zones_to_bound_rule_segments(
            base=42, factors=[], widths=[], vol_muerto=10
        )
        assert len(segments) == 1
        assert segments[0]["volume"] == pytest.approx(10)
        assert segments[0]["slope"] == pytest.approx(0)
        assert segments[0]["constant"] == pytest.approx(42)

    def test_single_zone(self):
        """Single zone produces one segment."""
        segments = _zones_to_bound_rule_segments(
            base=100, factors=[0.5], widths=[500], vol_muerto=0
        )
        assert len(segments) == 1
        assert segments[0]["slope"] == pytest.approx(0.5)
        assert segments[0]["constant"] == pytest.approx(100)
        # V=500: 100 + 0.5*500 = 350
        assert segments[0]["constant"] + segments[0]["slope"] * 500 == pytest.approx(
            350
        )

    def test_dead_volume_constant_preserved(self):
        """Evaluation at vol_muerto must equal base."""
        segments = _zones_to_bound_rule_segments(
            base=200, factors=[0.3, 0.7], widths=[100, 400], vol_muerto=50
        )
        v0 = segments[0]["volume"]
        val = segments[0]["constant"] + segments[0]["slope"] * v0
        assert val == pytest.approx(200)


class TestLajaParser:
    """Test LajaParser with actual PLP data."""

    def test_central_name(self, laja_config):
        assert laja_config["central_laja"] == "ELTORO"

    def test_intermediate_basins(self, laja_config):
        basins = laja_config["intermediate_basins"]
        assert len(basins) == 4
        assert basins[0] == "ABANICO"
        assert basins[-1] == "TUCAPEL"

    def test_vol_max(self, laja_config):
        assert laja_config["vol_max"] == pytest.approx(5582.0)

    def test_volume_zones(self, laja_config):
        widths = laja_config["zone_widths"]
        assert len(widths) == 4
        assert widths == pytest.approx([1200, 170, 530, 3682])

    def test_irr_factors(self, laja_config):
        assert laja_config["irr_base"] == pytest.approx(570)
        assert laja_config["irr_factors"] == pytest.approx([0, 0.40, 0.40, 0.25])

    def test_elec_factors(self, laja_config):
        assert laja_config["elec_base"] == pytest.approx(0)
        assert laja_config["elec_factors"] == pytest.approx([0.05, 0.05, 0.40, 0.65])

    def test_mixed_factors(self, laja_config):
        assert laja_config["mixed_base"] == pytest.approx(30)
        assert laja_config["mixed_factors"] == pytest.approx([1.0, 0, 0, 0])

    def test_max_rights(self, laja_config):
        assert laja_config["max_irr"] == pytest.approx(5000)
        assert laja_config["max_elec"] == pytest.approx(1200)
        assert laja_config["max_mixed"] == pytest.approx(30)
        assert laja_config["max_anticipated"] == pytest.approx(5000)

    def test_flow_limits(self, laja_config):
        assert laja_config["qmax_irr"] == pytest.approx(1000)
        assert laja_config["qmax_elec"] == pytest.approx(1000)
        assert laja_config["qmax_mixed"] == pytest.approx(1000)
        assert laja_config["qmax_anticipated"] == pytest.approx(0)

    def test_costs(self, laja_config):
        assert laja_config["cost_irr_ns"] == pytest.approx(1100)
        # 1150 is CQVar(IQDE) — the electric USAGE cost
        # (leelajam.f:202-207); PLP has no electric non-served cost.
        assert "cost_elec_ns" not in laja_config
        assert laja_config["cost_elec_uso"] == pytest.approx(1150)

    def test_monthly_usage_irr(self, laja_config):
        usage = laja_config["monthly_usage_irr"]
        assert len(usage) == 12
        # Apr=1.0, May-Aug=0, Sep-Mar=1.0
        assert usage[0] == pytest.approx(1.0)  # Apr
        assert usage[1] == pytest.approx(0.0)  # May
        assert usage[8] == pytest.approx(1.0)  # Dec

    def test_monthly_usage_anticipated(self, laja_config):
        usage = laja_config["monthly_usage_anticipated"]
        assert len(usage) == 12
        # Anticipated active Sep-Nov only (hydro months 5-7)
        assert usage[5] == pytest.approx(1.0)  # Sep
        assert usage[0] == pytest.approx(0.0)  # Apr

    def test_initial_rights(self, laja_config):
        # Fixture support/plp/2_years/plplajam.dat.xz updated in 4a3264cc6
        # (PLP 2-year case). The "Derechos iniciales" line in that file is
        # `234 145 0 0`, so ini_irr=234 / ini_elec=145.
        assert laja_config["ini_irr"] == pytest.approx(234)
        assert laja_config["ini_elec"] == pytest.approx(145)

    def test_districts(self, laja_config):
        districts = laja_config["districts"]
        assert len(districts) == 3

        assert districts[0]["name"] == "RIEGZACO"
        assert districts[0]["cost_factor"] == pytest.approx(1.5)
        assert districts[0]["pct_1o_reg"] == pytest.approx(0.372)

        assert districts[1]["name"] == "RieTucapel"
        assert districts[1]["pct_2o_reg"] == pytest.approx(1.0)

        assert districts[2]["name"] == "RieSaltos"
        assert districts[2]["injection"] == "LAJA_I"
        assert districts[2]["pct_saltos"] == pytest.approx(1.0)

    def test_filtracion_laja(self, laja_config):
        assert laja_config["filtracion_laja"] == pytest.approx(47.0)

    def test_default_demands(self, laja_config):
        assert laja_config["demand_1o_reg"] == pytest.approx(90)
        assert laja_config["demand_2o_reg"] == pytest.approx(53)
        assert laja_config["demand_emergencia"] == pytest.approx(0)
        assert laja_config["demand_saltos"] == pytest.approx(7)

    def test_seasonal_curves(self, laja_config):
        s1 = laja_config["seasonal_1o_reg"]
        assert len(s1) == 12
        # Apr=1.0, May=0, Dec=1.0
        assert s1[0] == pytest.approx(1.0)
        assert s1[1] == pytest.approx(0.0)

    def test_vol_muerto(self, laja_config):
        assert laja_config["vol_muerto"] == pytest.approx(0.0)

    def test_season_start_months(self, laja_config):
        assert laja_config["mes_inicio_riego"] == 9
        assert laja_config["mes_inicio_anticipos"] == 6

    def test_cost_uso_fields(self, laja_config):
        # Read order per leelajam.f:202-207 / CEN informe annex 13.2:
        # CRiegoNS, uso riego, uso electrico, uso mixto, uso anticipado.
        assert laja_config["cost_irr_uso"] == pytest.approx(0.0)
        assert laja_config["cost_elec_uso"] == pytest.approx(1150)
        assert laja_config["cost_mixed_uso"] == pytest.approx(0.1)
        assert laja_config["cost_antic_uso"] == pytest.approx(1.0)

    def test_monthly_cost_arrays_length(self, laja_config):
        for key in [
            "monthly_cost_irr_ns",
            "monthly_cost_irr",
            "monthly_cost_elec",
            "monthly_cost_mixed",
            "monthly_cost_anticipated",
        ]:
            assert len(laja_config[key]) == 12, f"{key} should have 12 elements"

    def test_monthly_cost_irr_ns_values(self, laja_config):
        costs = laja_config["monthly_cost_irr_ns"]
        assert costs[0] == pytest.approx(1.0)  # Apr
        assert costs[1] == pytest.approx(0.0)  # May
        assert costs[5] == pytest.approx(0.1)  # Sep
        assert costs[8] == pytest.approx(1.5)  # Dec

    def test_monthly_usage_elec(self, laja_config):
        usage = laja_config["monthly_usage_elec"]
        assert len(usage) == 12
        assert all(u == pytest.approx(1.0) for u in usage)

    def test_monthly_usage_mixed(self, laja_config):
        usage = laja_config["monthly_usage_mixed"]
        assert len(usage) == 12
        assert all(u == pytest.approx(1.0) for u in usage)

    def test_seasonal_2o_reg(self, laja_config):
        s2 = laja_config["seasonal_2o_reg"]
        assert len(s2) == 12
        assert s2[0] == pytest.approx(0.20)  # Apr
        assert s2[1] == pytest.approx(0.0)  # May
        assert s2[8] == pytest.approx(1.0)  # Dec

    def test_seasonal_emergencia(self, laja_config):
        se = laja_config["seasonal_emergencia"]
        assert len(se) == 12
        assert se[0] == pytest.approx(0.20)  # Apr
        assert se[1] == pytest.approx(0.0)  # May

    def test_seasonal_saltos(self, laja_config):
        ss = laja_config["seasonal_saltos"]
        assert len(ss) == 12
        assert ss[0] == pytest.approx(0.0)  # Apr
        assert ss[8] == pytest.approx(0.50)  # Dec
        assert ss[9] == pytest.approx(1.0)  # Jan

    def test_district_injection_empty_becomes_none(self, laja_config):
        """Districts with '' injection name should parse as None."""
        assert laja_config["districts"][0]["injection"] is None  # RIEGZACO
        assert laja_config["districts"][1]["injection"] is None  # RieTucapel

    def test_district_injection_non_empty(self, laja_config):
        """RieSaltos has injection='LAJA_I'."""
        assert laja_config["districts"][2]["injection"] == "LAJA_I"

    def test_district_count(self, laja_config):
        assert len(laja_config["districts"]) == 3

    def test_district_tucapel_percentages(self, laja_config):
        d = laja_config["districts"][1]
        assert d["pct_1o_reg"] == pytest.approx(0.628)
        assert d["pct_2o_reg"] == pytest.approx(1.0)
        assert d["pct_emergencia"] == pytest.approx(0.628)
        assert d["pct_saltos"] == pytest.approx(0.0)

    def test_district_saltos_percentages(self, laja_config):
        d = laja_config["districts"][2]
        assert d["cost_factor"] == pytest.approx(0.2)
        assert d["pct_saltos"] == pytest.approx(1.0)
        assert d["pct_1o_reg"] == pytest.approx(0.0)

    def test_manual_withdrawals_empty(self, laja_config):
        assert laja_config["manual_withdrawals"] == []

    def test_forced_flows_empty(self, laja_config):
        assert laja_config["forced_flows"] == []

    def test_all_config_keys_present(self, laja_config):
        """Verify all expected config keys are parsed."""
        expected_keys = {
            "central_laja",
            "intermediate_basins",
            "vol_max",
            "zone_widths",
            "irr_base",
            "irr_factors",
            "elec_base",
            "elec_factors",
            "mixed_base",
            "mixed_factors",
            "max_irr",
            "max_elec",
            "max_mixed",
            "max_anticipated",
            "mes_inicio_riego",
            "mes_inicio_anticipos",
            "qmax_irr",
            "qmax_elec",
            "qmax_mixed",
            "qmax_anticipated",
            "cost_irr_ns",
            "cost_irr_uso",
            "cost_elec_uso",
            "cost_mixed_uso",
            "cost_antic_uso",
            "monthly_cost_irr_ns",
            "monthly_cost_irr",
            "monthly_cost_elec",
            "monthly_cost_mixed",
            "monthly_cost_anticipated",
            "monthly_usage_irr",
            "monthly_usage_elec",
            "monthly_usage_mixed",
            "monthly_usage_anticipated",
            "ini_irr",
            "ini_elec",
            "ini_mixed",
            "ini_anticipated",
            "districts",
            "filtracion_laja",
            "demand_1o_reg",
            "demand_2o_reg",
            "demand_emergencia",
            "demand_saltos",
            "seasonal_1o_reg",
            "seasonal_2o_reg",
            "seasonal_emergencia",
            "seasonal_saltos",
            "vol_muerto",
            "manual_withdrawals",
            "forced_flows",
        }
        assert expected_keys.issubset(set(laja_config.keys()))


class TestLajaWriter:
    """Test LajaWriter generates correct entities."""

    def test_entities_generated(self, laja_config):
        writer = LajaWriter(laja_config)

        assert len(writer.user_constraints) == 1  # laja_partition balance
        # 5 partition + district withdrawal FlowRights
        assert len(writer.flow_rights) >= 5
        assert (
            len(writer.volume_rights) == 7
        )  # irr, elec, mixed, anticipated + 3 economy

    def test_partition_user_constraint(self, laja_config):
        writer = LajaWriter(laja_config)

        uc = writer.user_constraints[0]
        assert uc["name"] == "laja_particion_derechos"

    def test_total_gen_flow_right(self, laja_config):
        writer = LajaWriter(laja_config)

        total = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_q_turbinado"
        )
        assert total["direction"] == 1  # supply
        assert total["use_average"] is True

    def test_flow_rights_no_bound_rule(self, laja_config):
        """FlowRights should NOT have bound_rule — flow caps come from fmax.
        The volume-dependent annual quota is on VolumeRight instead."""
        writer = LajaWriter(laja_config)
        for fr in writer.flow_rights:
            assert "bound_rule" not in fr, (
                f"{fr['name']} should not have bound_rule on FlowRight"
            )

    def test_irr_vol_bound_rule(self, laja_config):
        writer = LajaWriter(laja_config)

        vol_irr = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_riego"
        )
        assert "bound_rule" in vol_irr
        rule = vol_irr["bound_rule"]
        assert rule["reservoir"] == "ELTORO"
        assert rule["cap"] == pytest.approx(5000)
        assert len(rule["segments"]) == 4

        # First segment: base=570, slope=0
        seg0 = rule["segments"][0]
        assert seg0["constant"] == pytest.approx(570)
        assert seg0["slope"] == pytest.approx(0)

    def test_elec_vol_bound_rule(self, laja_config):
        writer = LajaWriter(laja_config)

        vol_elec = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_electrico"
        )
        rule = vol_elec["bound_rule"]
        assert rule["cap"] == pytest.approx(1200)
        assert len(rule["segments"]) == 4

    def test_volume_rights_reset_months(self, laja_config):
        writer = LajaWriter(laja_config)

        # Rights buckets re-provision at the irrigation-season start
        # (INICIOTEMP, hydro month 9 = december); the anticipado counter
        # resets at the anticipos window start (INICIOANTIC, hydro
        # month 6 = september).  genpdlajam.f:624-661 / 2017 Acuerdo.
        vr_by_name = {vr["name"]: vr for vr in writer.volume_rights}
        for name in (
            "laja_vol_der_riego",
            "laja_vol_der_electrico",
            "laja_vol_der_mixto",
        ):
            assert vr_by_name[name]["reset_month"] == "december"
        assert vr_by_name["laja_vol_gasto_anticipado"]["reset_month"] == "september"

    def test_volume_rights_initial(self, laja_config):
        writer = LajaWriter(laja_config)

        # Initial values come from `Derechos iniciales` line in
        # support/plp/2_years/plplajam.dat (234 / 145 / 0 / 0).
        vol_irr = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_riego"
        )
        assert vol_irr["eini"] == pytest.approx(234)
        assert vol_irr["emax"] == pytest.approx(5000)

        vol_elec = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_electrico"
        )
        assert vol_elec["eini"] == pytest.approx(145)

    def test_district_flow_rights(self, laja_config):
        writer = LajaWriter(laja_config)

        # RIEGZACO has pct_1o_reg=0.372, demand=90
        zaco_1o = next(
            (fr for fr in writer.flow_rights if fr["name"] == "RIEGZACO_1o_reg"),
            None,
        )
        assert zaco_1o is not None
        # fail_cost = cost_irr_ns × cost_factor × FactMenCRiegoNS(mes)
        # = 1100 × 1.5 × [1.0 (Apr) .. 1.5 (Dec)] — a hydro-year
        # schedule, mirroring PLP CRiegoNSEta (leelajam.f:221-229).
        fcost = zaco_1o["fcost"]
        assert fcost[0][0] == pytest.approx(1650.0)  # April, factor 1.0
        assert fcost[8][0] == pytest.approx(2475.0)  # December, factor 1.5

    def test_to_json_dict_keys(self, laja_config):
        writer = LajaWriter(laja_config)
        result = writer.to_json_dict()

        assert "user_constraint_array" in result
        assert "flow_right_array" in result
        assert "volume_right_array" in result

    def test_mixed_vol_bound_rule(self, laja_config):
        """Mixed rights are a per-zone STEP (PLP selector semantics):
        DerMixto = DerMixtoBase × FactDerMixtoColchon(active zone)
        (genpdlajam.f:647-648) — 30 hm³ in the lower cushion, 0 above
        (the base transfers to irrigation)."""
        writer = LajaWriter(laja_config)

        vol_mixed = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_mixto"
        )
        rule = vol_mixed["bound_rule"]
        assert rule["reservoir"] == "ELTORO"
        assert rule["cap"] == pytest.approx(30)
        assert len(rule["segments"]) == 4
        for seg in rule["segments"]:
            assert seg["slope"] == pytest.approx(0.0)
        assert rule["segments"][0]["constant"] == pytest.approx(30)
        assert rule["segments"][1]["constant"] == pytest.approx(0.0)
        assert rule["segments"][2]["constant"] == pytest.approx(0.0)
        assert rule["segments"][3]["constant"] == pytest.approx(0.0)

    def test_anticipated_is_up_counter(self, laja_config):
        """The anticipado bucket mirrors PLP's IVGAF: an up-counter
        that resets to zero each september and fills via the saving
        inflow (linked to qga in laja.pampl); no bound_rule."""
        writer = LajaWriter(laja_config)

        vol_antic = next(
            vr
            for vr in writer.volume_rights
            if vr["name"] == "laja_vol_gasto_anticipado"
        )
        assert "bound_rule" not in vol_antic
        assert vol_antic["reset_value"] == 0
        assert vol_antic["reset_month"] == "september"
        assert "saving_rate" in vol_antic

    def test_riego_reset_debit(self, laja_config):
        """The december riego provision debits the anticipado counter
        (PLP: IVGAF enters the IVDRF row, genpdlajam.f:234-239) — and
        the counter must precede the riego bucket in the array."""
        writer = LajaWriter(laja_config)
        names = [vr["name"] for vr in writer.volume_rights]
        vol_irr = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_riego"
        )
        assert vol_irr["reset_debit_right"] == "laja_vol_gasto_anticipado"
        assert names.index("laja_vol_gasto_anticipado") < names.index(
            "laja_vol_der_riego"
        )

    def test_zone_formula_matches_cen_tabla1(self, laja_config):
        """Riego + mixed rights reproduce the 2017 Acuerdo's Tabla 1.

        CEN informe (Actualizacion Convenio Laja, Dec-2018), Tabla 1
        "Volumen disponible para riego" — which covers riego AND mixed
        combined (inferior: 570 exclusivos + 30 mixto = 600):

            inferior   (V<1200):    600
            transicion (1200-1370): 600 + 0.40 (V-1200)
            intermedio (1370-1900): 668 + 0.40 (V-1370)
            superior   (V>1900):    880 + 0.25 (V-1900)

        The +30 mixed transfer above the lower cushion
        (genpdlajam.f:647-649) is what makes these match; without it
        the totals were 30 hm³ short in every upper zone.
        """
        writer = LajaWriter(laja_config)
        vr = {v["name"]: v for v in writer.volume_rights}

        def evaluate(rule, volume):
            active = rule["segments"][0]
            for seg in rule["segments"]:
                if seg["volume"] <= volume:
                    active = seg
            value = active["constant"] + active["slope"] * volume
            return min(value, rule["cap"])

        irr = vr["laja_vol_der_riego"]["bound_rule"]
        mixed = vr["laja_vol_der_mixto"]["bound_rule"]
        elec = vr["laja_vol_der_electrico"]["bound_rule"]

        tabla1_riego = {
            600: 600.0,
            1200: 600.0,
            1500: 720.0,
            1900: 880.0,
            2000: 905.0,
            3000: 1155.0,
        }
        for volume, expected in tabla1_riego.items():
            total = evaluate(irr, volume) + evaluate(mixed, volume)
            assert total == pytest.approx(expected), f"V={volume}"

        # Generation column of Tabla 1 (no mixed complement in PLP).
        tabla1_gen = {600: 30.0, 1500: 120.5, 2000: 345.5}
        for volume, expected in tabla1_gen.items():
            assert evaluate(elec, volume) == pytest.approx(expected), f"V={volume}"

    def test_flow_rights_directions(self, laja_config):
        writer = LajaWriter(laja_config)

        total = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_q_turbinado"
        )
        assert total["direction"] == 1  # supply

        for name in [
            "laja_der_riego",
            "laja_der_electrico",
            "laja_der_mixto",
            "laja_gasto_anticipado",
        ]:
            fr = next(f for f in writer.flow_rights if f["name"] == name)
            assert fr["direction"] == -1, f"{name} should have direction=-1"

    def test_all_flow_rights_have_target(self, laja_config):
        writer = LajaWriter(laja_config)
        for fr in writer.flow_rights:
            assert "target" in fr, f"{fr['name']} missing target"

    def test_district_fail_cost_computation(self, laja_config):
        writer = LajaWriter(laja_config)

        # RieSaltos cost_factor=0.2, cost_irr_ns=1100 → base 220,
        # modulated by the monthly FactMenCRiegoNS schedule.
        saltos_frs = [
            fr for fr in writer.flow_rights if fr["name"].startswith("RieSaltos_")
        ]
        for fr in saltos_frs:
            fcost = fr["fcost"]
            assert fcost[0][0] == pytest.approx(220.0)  # April, factor 1.0
            assert fcost[8][0] == pytest.approx(330.0)  # December, factor 1.5

    def test_district_zero_pct_zero_demand_skipped(self):
        """Categories with pct<=0 AND demand<=0 should not generate FlowRights."""
        cfg = _minimal_laja_config()
        # Set all demands to 0, district has pct_1o_reg=1.0 → still 0*1.0=0
        cfg["demand_1o_reg"] = 0
        cfg["demand_2o_reg"] = 0
        cfg["demand_emergencia"] = 0
        cfg["demand_saltos"] = 0
        # Also set pct_1o_reg to 0 so both are <=0
        cfg["districts"][0]["pct_1o_reg"] = 0.0
        writer = LajaWriter(cfg)
        # With both pct=0 and demand=0, no district FlowRights should be generated
        district_frs = [fr for fr in writer.flow_rights if fr["name"].startswith("D1_")]
        assert len(district_frs) == 0

    def test_unique_uids(self, laja_config):
        writer = LajaWriter(laja_config)
        all_uids = []
        for entity_list in [
            writer.flow_rights,
            writer.volume_rights,
            writer.user_constraints,
        ]:
            for entity in entity_list:
                all_uids.append(entity["uid"])
        assert len(all_uids) == len(set(all_uids)), "UIDs must be unique"

    def test_volume_rights_no_reservoir_coupling(self, laja_config):
        """Ledgers are pure accounting (PLP-faithful): no `reservoir` key.

        Physical water leaves through the anchored turbine arc
        (laja_anclaje_turbinado); the buckets deplete via the ledger
        linkage constraints.  The bound_rule keeps its own reservoir
        reference for zone provisioning.
        """
        writer = LajaWriter(laja_config)
        for vr in writer.volume_rights:
            assert "reservoir" not in vr, vr["name"]
            if "bound_rule" in vr:
                assert vr["bound_rule"]["reservoir"] == "ELTORO"

    def test_qdefm_netted_cap(self, laja_config, tmp_path):
        """With hoya-intermedia inflow means and materialized stages,
        the attribution cap switches to the netted qdefm carrier
        (PLP GetQsLajaM, static-filtration approximation):

          qdefm = max(min(QP - QHoya - QFilt, 0) + QN + QS + QE, 0)
        """

        class _Stages:
            def get_all(self):
                # december (in-season) and june (off-season)
                return [
                    {"number": 1, "month": 12},
                    {"number": 2, "month": 6},
                ]

        cfg = dict(laja_config)
        cfg["q_hoya_inter"] = [60.0, 80.0]
        writer = LajaWriter(cfg, stage_parser=_Stages())

        carrier = next(
            (fr for fr in writer.flow_rights if fr["name"] == "laja_qdefm"),
            None,
        )
        assert carrier is not None
        # December (hydro idx 8): QP=90*1.0, QN=53*1.0, QE=0, QS=7*0.5,
        # QFilt=47, QHoya=60 -> min(90-60-47, 0) = -17; qdefm =
        # max(-17 + 53 + 3.5 + 0, 0) = 39.5.
        # June (hydro idx 2): all seasonals 0 -> qdefm = 0.
        fmax = carrier["fmax"]
        assert fmax[0][0] == pytest.approx(39.5)
        assert fmax[1][0] == pytest.approx(0.0)
        assert carrier["fmin"] == fmax  # forced column

        writer.generate_pampl(tmp_path)
        text = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        assert "<= flow_right('laja_qdefm').flow;" in text

    def test_pure_netting_functions(self):
        """The GetQsLajaM arithmetic is exposed as pure functions."""
        from gtopt_expand.laja_agreement import _netted_primary, _qdefm_value

        # Tributary surplus: min() clamps at 0, secondary categories
        # survive.
        assert _qdefm_value(
            qp=90.0, qn=53.0, qe=0.0, qs=3.5, q_hoya=60.0, qfilt=47.0
        ) == pytest.approx(39.5)
        # Positive tributary deficit: the static min(., 0) clamp
        # (QDefAbanico = 0 under QFiltLaja = QFiltHist) zeroes it —
        # only the secondary categories survive.
        assert _qdefm_value(
            qp=90.0, qn=0.0, qe=0.0, qs=0.0, q_hoya=10.0, qfilt=47.0
        ) == pytest.approx(0.0)
        # Fully negative interior: floor at 0.
        assert _qdefm_value(
            qp=0.0, qn=0.0, qe=0.0, qs=0.0, q_hoya=80.0, qfilt=47.0
        ) == pytest.approx(0.0)
        assert _netted_primary(90.0, 20.0, 47.0) == pytest.approx(67.0)
        assert _netted_primary(90.0, 80.0, 47.0) == pytest.approx(90.0)

    def test_district_junction_withdrawal(self, laja_config):
        """A district with `anchor_junction` (stamped by plp2gtopt when
        no diversion central exists but the district's junction does —
        pure irrigation withdrawal, e.g. RieTucapel) gets consumptive
        junction refs on every category."""
        cfg = dict(laja_config)
        cfg["districts"] = [dict(d) for d in cfg["districts"]]
        tucapel = next(d for d in cfg["districts"] if d["name"] == "RieTucapel")
        tucapel["anchor_junction"] = "RieTucapel"
        writer = LajaWriter(cfg)
        cats = [fr for fr in writer.flow_rights if fr["name"].startswith("RieTucapel_")]
        assert cats
        for fr in cats:
            assert fr["junction_a"] == "RieTucapel"

        # Disabled anchoring reverts to the legacy (junction-free) shape.
        cfg2 = dict(cfg)
        cfg2["enable_physical_anchoring"] = False
        writer2 = LajaWriter(cfg2)
        for fr in writer2.flow_rights:
            if fr["name"].startswith("RieTucapel_"):
                assert "junction_a" not in fr

    def test_seepage_aware_cap(self, laja_config, tmp_path):
        """With a ReservoirSeepage reference the cap becomes the exact
        linearization `rights + seepage.flow <= K(t)`: the carrier
        holds K(t) = min(QP − QHoya, QFiltHist) + R floored at the
        element's max flow, and the row references the seepage element
        directly."""
        from gtopt_expand.laja_agreement import _qdefm_k_value

        # Pure arithmetic: December (QP=90, hoya=60, R=56.5):
        # K = min(30, 47) + 56.5 = 86.5 (> qf_ub floor 50).
        assert _qdefm_k_value(
            qp=90.0,
            qn=53.0,
            qe=0.0,
            qs=3.5,
            q_hoya=60.0,
            qfilt_hist=47.0,
            qf_ub=50.0,
        ) == pytest.approx(86.5)
        # Off-season (QP=0, R=0): K negative -> floored at qf_ub so
        # the row stays feasible (rights forced towards 0 once the
        # seepage flow approaches its maximum).
        assert _qdefm_k_value(
            qp=0.0,
            qn=0.0,
            qe=0.0,
            qs=0.0,
            q_hoya=60.0,
            qfilt_hist=47.0,
            qf_ub=50.0,
        ) == pytest.approx(50.0)

        class _Stages:
            def get_all(self):
                return [{"number": 1, "month": 12}]

        cfg = dict(laja_config)
        cfg["q_hoya_inter"] = [60.0]
        cfg["seepage_ref"] = "ELTORO_seepage_2"
        cfg["seepage_flow_ub"] = 50.0
        writer = LajaWriter(cfg, stage_parser=_Stages())
        carrier = next(fr for fr in writer.flow_rights if fr["name"] == "laja_qdefm")
        fmax = carrier["fmax"]
        first = fmax[0][0] if isinstance(fmax, list) else fmax
        assert first == pytest.approx(86.5)

        writer.generate_pampl(tmp_path)
        text = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        assert (
            "+ seepage('ELTORO_seepage_2').flow"
            " <= flow_right('laja_qdefm').flow;" in text
        )

    def test_feature_toggles_disable_couplings(self, laja_config, tmp_path):
        """The enable_* keys revert each coupling to the legacy shape."""

        class _Stages:
            def get_all(self):
                return [{"number": 1, "month": 12}]

        cfg = dict(laja_config)
        cfg["q_hoya_inter"] = [60.0]
        cfg["anchor_turbinado_ref"] = "turbine('ELTORO').flow"
        cfg["enable_physical_anchoring"] = False
        cfg["enable_ledger_linkage"] = False
        cfg["enable_attribution_cap"] = False
        cfg["enable_netted_targets"] = False
        writer = LajaWriter(cfg, stage_parser=_Stages())

        # Attribution cap off: no qdefm carrier entity.
        assert not any(fr["name"] == "laja_qdefm" for fr in writer.flow_rights)
        # Netted targets off: gross December primary target.
        zaco = next(fr for fr in writer.flow_rights if fr["name"] == "RIEGZACO_1o_reg")
        target = zaco["target"]
        first = target[0][0] if isinstance(target, list) else target
        assert first == pytest.approx(0.372 * 90.0)

        writer.generate_pampl(tmp_path)
        text = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        assert "laja_anclaje_turbinado" not in text
        assert "laja_ledger_riego" not in text
        assert "laja_retiro_maximo" not in text
        # The partition identity itself is always emitted.
        assert "constraint laja_particion_derechos" in text

    def test_qdefm_scenario_mean(self, laja_config):
        """With per-scenario hoya inflows the carrier stays a 2D
        [stage][block] schedule built from the per-stage MEAN inflow —
        gtopt's FlowRight fmin/fmax is sampled per (stage, block) only
        (no scenario axis), so a 3D schedule cannot be emitted."""

        class _Stages:
            def get_all(self):
                return [{"number": 1, "month": 12}]

        cfg = dict(laja_config)
        cfg["q_hoya_inter"] = [60.0]
        cfg["q_hoya_inter_by_scenario"] = [[60.0], [10.0]]
        writer = LajaWriter(cfg, stage_parser=_Stages())
        carrier = next(fr for fr in writer.flow_rights if fr["name"] == "laja_qdefm")
        fmax = carrier["fmax"]

        def _depth(x):
            return 1 + _depth(x[0]) if isinstance(x, list) and x else 0

        # At most 2D (stage[/block]) — NEVER 3D scenario-dimensioned
        # (gtopt's FlowRight fmin/fmax has no scenario axis).
        assert _depth(fmax) <= 2, f"carrier must not be scenario-dimensioned: {fmax}"
        # mean hoya = (60+10)/2 = 35 -> min(90-35-47,0)+53+3.5 = 56.5
        flat = fmax
        while isinstance(flat, list):
            flat = flat[0]
        assert flat == pytest.approx(56.5)

    def test_district_anchoring(self, laja_config, tmp_path):
        """A district stamped with `anchor_flow_right` (by plp2gtopt when
        its physical diversion exists) gets a dist_anclaje constraint
        equating the offtake to the sum of its categories, and its
        categories drop the direct junction coupling."""
        cfg = dict(laja_config)
        cfg["districts"] = [dict(d) for d in cfg["districts"]]
        saltos = next(d for d in cfg["districts"] if d["name"] == "RieSaltos")
        saltos["anchor_flow_right"] = "RieSaltos_irrigation_right"
        writer = LajaWriter(cfg)

        saltos_frs = [
            fr
            for fr in writer.flow_rights
            if fr["name"].startswith("RieSaltos_")
            and not fr["name"].endswith("_irrigation_right")
        ]
        assert saltos_frs
        for fr in saltos_frs:
            assert "junction_a" not in fr  # anchor supersedes the legacy ref

        writer.generate_pampl(tmp_path)
        text = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        assert "constraint dist_anclaje_RieSaltos" in text
        assert "flow_right('RieSaltos_irrigation_right').flow = " in text
        for fr in saltos_frs:
            assert f"flow_right('{fr['name']}').flow" in text
        # Un-anchored districts don't get the constraint.
        assert "dist_anclaje_RIEGZACO" not in text

    def test_netted_primary_targets(self, laja_config):
        """With hoya-intermedia inflows, the 1o_reg targets are netted:
        pct x min(gross primary, hoya + filtration) — PLP GetQsLajaM's
        QPRiego re-set under the static-filtration approximation."""

        class _Stages:
            def get_all(self):
                return [
                    {"number": 1, "month": 12},  # december, in-season
                    {"number": 2, "month": 6},  # june, off-season
                ]

        cfg = dict(laja_config)
        cfg["q_hoya_inter"] = [20.0, 80.0]
        writer = LajaWriter(cfg, stage_parser=_Stages())
        zaco_1o = next(
            fr for fr in writer.flow_rights if fr["name"] == "RIEGZACO_1o_reg"
        )
        # December: gross = 90*1.0; qh+qfilt = 20+47 = 67 < 90 ->
        # netted primary = 67; target = 0.372 * 67 = 24.924.
        t = zaco_1o["target"]
        assert t[0][0] == pytest.approx(0.372 * 67.0)
        # June: seasonal 0 -> min(0, ...) = 0.
        assert t[1][0] == pytest.approx(0.0)

    def test_pampl_ledger_and_anchor_constraints(self, laja_config, tmp_path):
        """laja.pampl carries the ledger linkages; the anchor constraint
        is emitted only when plp2gtopt resolved the gen waterway."""
        writer = LajaWriter(laja_config)
        writer.generate_pampl(tmp_path)
        text = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        for name in (
            "laja_ledger_riego",
            "laja_ledger_electrico",
            "laja_ledger_mixto",
            "laja_ledger_anticipado",
        ):
            assert f"constraint {name}" in text
        assert "laja_anclaje_turbinado" not in text  # no anchor key set
        # Attribution cap sums exactly the emitted district FlowRights.
        assert "constraint laja_retiro_maximo" in text
        for fr in writer.flow_rights:
            if fr["name"].startswith(("RIEGZACO_", "RieTucapel_", "RieSaltos_")):
                assert f"flow_right('{fr['name']}').flow" in text

        cfg = dict(laja_config)
        cfg["anchor_turbinado_ref"] = "turbine('ELTORO').flow"
        writer2 = LajaWriter(cfg)
        writer2.generate_pampl(tmp_path)
        text2 = (tmp_path / "laja.pampl").read_text(encoding="utf-8")
        assert "constraint laja_anclaje_turbinado" in text2
        assert "= turbine('ELTORO').flow;" in text2

    def test_volume_rights_purposes(self, laja_config):
        writer = LajaWriter(laja_config)
        vr_by_name = {vr["name"]: vr for vr in writer.volume_rights}
        assert vr_by_name["laja_vol_der_riego"]["purpose"] == "irrigation"
        assert vr_by_name["laja_vol_der_electrico"]["purpose"] == "generation"
        assert vr_by_name["laja_vol_der_mixto"]["purpose"] == "mixed"
        assert vr_by_name["laja_vol_gasto_anticipado"]["purpose"] == "anticipated"

    def test_volume_rights_use_state_variable(self, laja_config):
        writer = LajaWriter(laja_config)
        for vr in writer.volume_rights:
            assert vr["use_state_variable"] is True

    def test_volume_rights_mixed_initial(self, laja_config):
        writer = LajaWriter(laja_config)
        vol_mixed = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_mixto"
        )
        assert vol_mixed["eini"] == pytest.approx(0)
        assert vol_mixed["emax"] == pytest.approx(30)

    def test_volume_rights_anticipated_initial(self, laja_config):
        writer = LajaWriter(laja_config)
        vol_antic = next(
            vr
            for vr in writer.volume_rights
            if vr["name"] == "laja_vol_gasto_anticipado"
        )
        assert vol_antic["eini"] == pytest.approx(0)
        assert vol_antic["emax"] == pytest.approx(5000)

    def test_total_gen_fmax(self, laja_config):
        writer = LajaWriter(laja_config)
        total = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_q_turbinado"
        )
        assert total["fmax"] == pytest.approx(5582.0)

    def test_economy_accumulators(self, laja_config):
        """Economy VolumeRights (IVESF, IVERF, IVAPF) are emitted."""
        writer = LajaWriter(laja_config)
        econ_names = {
            "laja_vol_econ_endesa",
            "laja_vol_econ_reserva",
            "laja_vol_econ_polcura",
        }
        econ_vrs = [vr for vr in writer.volume_rights if vr["name"] in econ_names]
        assert len(econ_vrs) == 3
        for vr in econ_vrs:
            assert vr["purpose"] == "economy"
            assert vr["eini"] == 0
            assert vr["use_state_variable"] is True
            assert "reset_month" not in vr  # economies carry forward

    def test_usage_cost_elec(self, laja_config):
        """Electric usage cost is a NEGATIVE uvalue (PLP charges qde flow)."""
        writer = LajaWriter(laja_config)
        elec = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_der_electrico"
        )
        # uvalue = -cost_elec_uso × monthly_cost_elec; the 2-year
        # fixture has cost_elec_uso=1150, FactMen(Apr)=1.1 → -1265.
        assert "uvalue" in elec
        assert elec["uvalue"][0][0] == pytest.approx(-1265.0)
        # No fail cost on the rights flows — PLP's NS penalties live on
        # the retiro deficits only.
        assert "fcost" not in elec

    def test_usage_cost_mixed(self, laja_config):
        """Mixed usage cost is a NEGATIVE uvalue (PLP charges qdm flow)."""
        writer = LajaWriter(laja_config)
        mixed = next(fr for fr in writer.flow_rights if fr["name"] == "laja_der_mixto")
        # uvalue = -cost_mixed_uso × monthly_cost_mixed = -0.1 (flat).
        assert mixed["uvalue"] == pytest.approx(-0.1)

    def test_usage_cost_anticipated(self, laja_config):
        """Anticipado usage cost is a NEGATIVE uvalue (PLP charges qga)."""
        writer = LajaWriter(laja_config)
        antic = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_gasto_anticipado"
        )
        # uvalue = -cost_antic_uso × monthly_cost_anticipated = -1.0 (flat)
        assert antic["uvalue"] == pytest.approx(-1.0)
        assert "fcost" not in antic

    def test_riego_has_no_fail_cost(self, laja_config):
        """laja_der_riego carries no NS cost (CQVar(IQDR)=0 in fixture)."""
        writer = LajaWriter(laja_config)
        riego = next(fr for fr in writer.flow_rights if fr["name"] == "laja_der_riego")
        assert "fcost" not in riego
        assert "uvalue" not in riego  # cost_irr_uso = 0.0

    def test_usage_cost_zero_omitted(self):
        """When usage cost is 0, use_value should not be emitted."""
        cfg = _minimal_laja_config()
        cfg["cost_elec_uso"] = 0.0
        cfg["cost_mixed_uso"] = 0.0
        writer = LajaWriter(cfg)
        elec = next(
            fr for fr in writer.flow_rights if fr["name"] == "laja_der_electrico"
        )
        mixed = next(fr for fr in writer.flow_rights if fr["name"] == "laja_der_mixto")
        assert "uvalue" not in elec
        assert "uvalue" not in mixed

    def test_user_constraints_partition(self, laja_config):
        """Laja writer emits 1 user constraint for the partition balance."""
        writer = LajaWriter(laja_config)
        assert len(writer.user_constraints) == 1
        assert writer.user_constraints[0]["name"] == "laja_particion_derechos"

    def test_json_serializable(self, laja_config):
        writer = LajaWriter(laja_config)
        result = writer.to_json_dict()
        json_str = json.dumps(result)
        assert len(json_str) > 0

    def test_json_roundtrip(self, laja_config):
        """JSON serialize and deserialize should preserve structure."""
        writer = LajaWriter(laja_config)
        result = writer.to_json_dict()
        roundtrip = json.loads(json.dumps(result))
        assert len(roundtrip["flow_right_array"]) == len(writer.flow_rights)
        assert len(roundtrip["volume_right_array"]) == len(writer.volume_rights)
        assert len(roundtrip["user_constraint_array"]) == len(writer.user_constraints)


class TestLajaScheduleHelpers:
    """Test schedule conversion helpers without stage_parser."""

    def _make_writer(self, blocks_per_stage=1):
        cfg = _minimal_laja_config()
        return LajaWriter(cfg, options={"blocks_per_stage": blocks_per_stage})

    def test_to_stb_sched_uniform(self):
        writer = self._make_writer()
        result = writer._to_stb_sched([5.0, 5.0, 5.0])
        assert result == 5.0

    def test_to_stb_sched_varying(self):
        writer = self._make_writer()
        result = writer._to_stb_sched([1.0, 2.0, 3.0])
        assert result == [[[1.0], [2.0], [3.0]]]

    def test_to_tb_sched_uniform(self):
        writer = self._make_writer()
        result = writer._to_tb_sched([10.0, 10.0])
        assert result == 10.0

    def test_to_tb_sched_varying(self):
        writer = self._make_writer()
        result = writer._to_tb_sched([10.0, 20.0])
        assert result == [[10.0], [20.0]]

    def test_to_stb_sched_multi_block(self):
        writer = self._make_writer(blocks_per_stage=3)
        result = writer._to_stb_sched([1.0, 2.0])
        assert result == [[[1.0, 1.0, 1.0], [2.0, 2.0, 2.0]]]

    def test_to_tb_sched_multi_block(self):
        writer = self._make_writer(blocks_per_stage=3)
        result = writer._to_tb_sched([10.0, 20.0])
        assert result == [[10.0, 10.0, 10.0], [20.0, 20.0, 20.0]]

    def test_hydro_to_stage_no_parser(self):
        """Without stage_parser, returns the input array unchanged."""
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        monthly = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0]
        result = writer._hydro_to_stage_schedule(monthly)
        assert result == monthly


def _minimal_laja_config():
    """Return a minimal valid Laja config for unit tests."""
    return {
        "central_laja": "TEST_CENTRAL",
        "vol_max": 1000.0,
        "vol_muerto": 0.0,
        "zone_widths": [500, 500],
        "irr_base": 100,
        "irr_factors": [0.0, 0.5],
        "elec_base": 0,
        "elec_factors": [0.1, 0.2],
        "mixed_base": 10,
        "mixed_factors": [0.0, 0.0],
        "max_irr": 500,
        "max_elec": 200,
        "max_mixed": 10,
        "max_anticipated": 500,
        "qmax_irr": 100,
        "qmax_elec": 100,
        "qmax_mixed": 50,
        "qmax_anticipated": 0,
        "cost_irr_ns": 1000,
        "cost_irr_uso": 0.0,
        "cost_elec_uso": 0.1,
        "cost_mixed_uso": 1.0,
        "cost_antic_uso": 0.0,
        "monthly_usage_irr": [1] * 12,
        "monthly_usage_elec": [1] * 12,
        "monthly_usage_mixed": [1] * 12,
        "monthly_usage_anticipated": [0] * 12,
        "monthly_cost_irr_ns": [1.0] * 12,
        "monthly_cost_irr": [1.0] * 12,
        "monthly_cost_elec": [1.0] * 12,
        "monthly_cost_mixed": [1.0] * 12,
        "monthly_cost_anticipated": [0.0] * 12,
        "ini_irr": 100,
        "ini_elec": 50,
        "ini_mixed": 0,
        "ini_anticipated": 0,
        "filtracion_laja": 10.0,
        "districts": [
            {
                "name": "D1",
                "injection": None,
                "cost_factor": 1.0,
                "pct_1o_reg": 1.0,
                "pct_2o_reg": 0.0,
                "pct_emergencia": 0.0,
                "pct_saltos": 0.0,
            },
        ],
        "demand_1o_reg": 50,
        "demand_2o_reg": 0,
        "demand_emergencia": 0,
        "demand_saltos": 0,
        "seasonal_1o_reg": [1] * 12,
        "seasonal_2o_reg": [0] * 12,
        "seasonal_emergencia": [0] * 12,
        "seasonal_saltos": [0] * 12,
    }


class TestLajaWriterMinimalConfig:
    """Test LajaWriter with a minimal synthetic config."""

    def test_minimal_config_builds(self):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        assert len(writer.flow_rights) >= 5
        assert len(writer.volume_rights) == 7  # 4 rights + 3 economy
        assert len(writer.user_constraints) == 1

    def test_minimal_district_target(self):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        d1 = next(fr for fr in writer.flow_rights if fr["name"] == "D1_1o_reg")
        # target = demand_1o_reg * pct_1o_reg * seasonal = 50 * 1.0 * 1.0 = 50
        assert d1["target"] == pytest.approx(50.0)

    def test_minimal_two_segments(self):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        vr_irr = next(
            vr for vr in writer.volume_rights if vr["name"] == "laja_vol_der_riego"
        )
        assert len(vr_irr["bound_rule"]["segments"]) == 2


class TestLajaPamplGeneration:
    """Test PAMPL template rendering for Laja agreement."""

    def test_generate_pampl_creates_file(self, tmp_path):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        pampl_name = writer.generate_pampl(tmp_path)

        assert pampl_name == "laja.pampl"
        pampl_file = tmp_path / "laja.pampl"
        assert pampl_file.exists()

    def test_generate_pampl_contains_params(self, tmp_path):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        writer.generate_pampl(tmp_path)

        content = (tmp_path / "laja.pampl").read_text()
        assert "param irr_base = 100" in content
        assert "param elec_base = 0" in content
        assert "param vol_muerto = 0.0" in content
        assert "param vol_max = 1000.0" in content
        assert "param filtracion_laja = 10.0" in content

    def test_generate_pampl_contains_monthly_arrays(self, tmp_path):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        writer.generate_pampl(tmp_path)

        content = (tmp_path / "laja.pampl").read_text()
        assert "param irr_usage[month]" in content
        assert "param elec_usage[month]" in content
        assert "param seasonal_1o_reg[month]" in content

    def test_generate_pampl_contains_header_comment(self, tmp_path):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        writer.generate_pampl(tmp_path)

        content = (tmp_path / "laja.pampl").read_text()
        assert "Laja Irrigation Agreement" in content
        assert "Convenio del Laja" in content
        assert "TEST_CENTRAL" in content

    def test_generate_pampl_contains_districts(self, tmp_path):
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        writer.generate_pampl(tmp_path)

        content = (tmp_path / "laja.pampl").read_text()
        assert "District: D1" in content

    def test_to_json_dict_with_output_dir(self, tmp_path):
        """When output_dir is provided, PAMPL file is generated."""
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        result = writer.to_json_dict(output_dir=tmp_path)
        assert "user_constraint_file" in result
        assert result["user_constraint_file"] == "laja.pampl"
        assert "user_constraint_array" not in result

    def test_to_json_dict_without_output_dir(self):
        """Without output_dir, constraints go inline."""
        cfg = _minimal_laja_config()
        writer = LajaWriter(cfg)
        result = writer.to_json_dict()
        assert "user_constraint_array" in result
        assert "user_constraint_file" not in result
