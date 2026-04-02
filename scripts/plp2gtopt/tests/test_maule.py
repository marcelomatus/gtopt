# -*- coding: utf-8 -*-

"""Tests for Maule irrigation agreement parser and writer."""

import json
from pathlib import Path

import pytest

from plp2gtopt.compressed_open import find_compressed_path
from plp2gtopt.maule_parser import MauleParser
from plp2gtopt.maule_writer import MauleWriter


# Path to actual PLP test data (compressed)
_SUPPORT_DIR = Path(__file__).parent.parent.parent.parent / "support"
_PLP_2Y = _SUPPORT_DIR / "plp_2_years"


@pytest.fixture()
def maule_parser():
    """Parse the 2-year PLP Maule convention file."""
    resolved = find_compressed_path(_PLP_2Y / "plpmaulen.dat")
    if resolved is None:
        pytest.skip("plpmaulen.dat not found in support/plp_2_years")
    parser = MauleParser(resolved)
    parser.parse()
    return parser


@pytest.fixture()
def maule_config(maule_parser):
    """Return parsed Maule convention config."""
    return maule_parser.config


class TestMauleParser:
    """Test MauleParser with actual PLP data."""

    def test_central_names(self, maule_config):
        assert maule_config["central_maule"] == "LMAULE"
        assert maule_config["central_invernada"] == "CIPRESES"
        assert maule_config["central_melado"] == "PEHUENCHE"
        assert maule_config["central_colbun"] == "COLBUN"

    def test_intermediate_basins(self, maule_config):
        basins = maule_config["intermediate_basins"]
        assert len(basins) == 6
        assert basins[0] == "B_LaMina"
        assert basins[-1] == "COLBUN"

    def test_volume_thresholds(self, maule_config):
        # Values in plpmaulen.dat are in 10^3 m3, converted to hm3
        assert maule_config["v_util_min"] == pytest.approx(0.0)
        assert maule_config["v_reserva_extraord"] == pytest.approx(129.0)
        assert maule_config["v_reserva_ordinaria"] == pytest.approx(452.0)
        assert maule_config["v_der_riego_temp_max"] == pytest.approx(800.0)
        assert maule_config["v_der_elect_anu_max"] == pytest.approx(250.0)
        assert maule_config["v_comp_elec_max"] == pytest.approx(350.0)

    def test_initial_volumes(self, maule_config):
        assert maule_config["v_gasto_elec_men_ini"] == pytest.approx(0.0)
        assert maule_config["v_gasto_elec_anu_ini"] == pytest.approx(73.406)
        assert maule_config["v_gasto_riego_ini"] == pytest.approx(420.488)
        assert maule_config["v_comp_elec_ini"] == pytest.approx(10.5893)

    def test_flow_limits(self, maule_config):
        assert maule_config["gasto_elec_men_max"] == pytest.approx(25.0)
        assert maule_config["gasto_elec_dia_max"] == pytest.approx(30.0)
        assert maule_config["gasto_riego_max"] == pytest.approx(200.0)

    def test_monthly_modulation(self, maule_config):
        mod = maule_config["mod_elec_reserva"]
        assert len(mod) == 12
        # First 6 months: 100%, last 6: 0%
        assert mod[:6] == [100, 100, 100, 100, 100, 100]
        assert mod[6:] == [0, 0, 0, 0, 0, 0]

    def test_armerillo_flag(self, maule_config):
        assert maule_config["descuenta_elec_armerillo"] is True

    def test_irrigation_percentages(self, maule_config):
        pct = maule_config["pct_riego_mensual"]
        assert len(pct) == 12
        # October (index 9) should be 100%
        assert pct[8] == 100.0

    def test_reserve_split(self, maule_config):
        assert maule_config["pct_elec_reserva"] == pytest.approx(20.0)
        assert maule_config["pct_riego_reserva"] == pytest.approx(80.0)

    def test_res105_flows(self, maule_config):
        res105 = maule_config["caudal_res105"]
        assert len(res105) == 12
        assert res105[0] == pytest.approx(80.0)  # Apr
        assert res105[1] == pytest.approx(40.0)  # May

    def test_districts(self, maule_config):
        districts = maule_config["districts"]
        assert len(districts) == 7

        # First district
        assert districts[0]["name"] == "RieCMNA"
        assert districts[0]["percentage"] == pytest.approx(12.66)
        assert districts[0]["has_slack"] is False

        # RieMelado has slack (index 4)
        assert districts[4]["name"] == "RieMelado"
        assert districts[4]["has_slack"] is True

        # Percentages should sum to ~100%
        total_pct = sum(d["percentage"] for d in districts)
        assert total_pct == pytest.approx(100.0, abs=0.01)

    def test_bocatoma(self, maule_config):
        assert maule_config["bocatoma_canelon"] == "BCanelon"
        assert maule_config["costo_canelon"] == pytest.approx(10.0)

    def test_penalties(self, maule_config):
        assert maule_config["penalizador_1"] == pytest.approx(1500.0)
        assert maule_config["penalizador_2"] == pytest.approx(1000.0)

    def test_initial_reserve_volumes(self, maule_config):
        assert maule_config["v_gasto_rext_elec_ini"] == pytest.approx(0.0)
        assert maule_config["v_gasto_rext_riego_ini"] == pytest.approx(0.0)
        assert maule_config["v_der_rext_elec_ini"] == pytest.approx(0.0)
        assert maule_config["v_der_rext_riego_ini"] == pytest.approx(0.0)

    def test_econ_invernada_ini(self, maule_config):
        assert "v_econ_inver_ini" in maule_config

    def test_caudal_min_embalsa(self, maule_config):
        assert maule_config["caudal_min_embalsa"] == pytest.approx(250.0)

    def test_valor_riego(self, maule_config):
        assert "valor_riego_maule" in maule_config
        assert "valor_riego_res105" in maule_config

    def test_costo_riego_ns(self, maule_config):
        assert "costo_riego_ns_maule" in maule_config
        assert "costo_riego_ns_res105" in maule_config

    def test_pct_riego_manual(self, maule_config):
        pct = maule_config["pct_riego_manual"]
        assert len(pct) == 12

    def test_auto_modulation(self, maule_config):
        assert "ano_mod_auto" in maule_config
        assert "factor_caudales_futuros" in maule_config
        assert "mod_auto_res105" in maule_config

    def test_vol_acum_riego_temp(self, maule_config):
        vol = maule_config["vol_acum_riego_temp"]
        assert len(vol) == 12

    def test_dias_acum_temp(self, maule_config):
        dias = maule_config["dias_acum_temp"]
        assert len(dias) == 12

    def test_caudal_res105_manual(self, maule_config):
        res105m = maule_config["caudal_res105_manual"]
        assert len(res105m) == 12

    def test_econ_invernada_flags(self, maule_config):
        assert isinstance(maule_config["econ_inver_uso_reserva"], bool)
        assert isinstance(maule_config["econ_inver_acumulables"], bool)
        assert "econ_inver_costo" in maule_config

    def test_idx_extrac_colbun_425(self, maule_config):
        assert "idx_extrac_colbun_425" in maule_config

    def test_v_cota_425(self, maule_config):
        assert "v_cota_425" in maule_config

    def test_all_config_keys_present(self, maule_config):
        """Verify all expected config keys are parsed."""
        expected_keys = {
            "central_maule",
            "central_invernada",
            "central_melado",
            "central_colbun",
            "intermediate_basins",
            "v_util_min",
            "v_reserva_extraord",
            "v_reserva_ordinaria",
            "v_der_riego_temp_max",
            "v_der_elect_anu_max",
            "v_comp_elec_max",
            "v_gasto_elec_men_ini",
            "v_gasto_elec_anu_ini",
            "v_gasto_riego_ini",
            "v_comp_elec_ini",
            "gasto_elec_men_max",
            "gasto_elec_dia_max",
            "gasto_riego_max",
            "mod_elec_reserva",
            "descuenta_elec_armerillo",
            "pct_riego_mensual",
            "pct_elec_reserva",
            "pct_riego_reserva",
            "caudal_res105",
            "districts",
            "bocatoma_canelon",
            "costo_canelon",
            "penalizador_1",
            "penalizador_2",
        }
        assert expected_keys.issubset(set(maule_config.keys()))


class TestMauleWriter:
    """Test MauleWriter generates correct entities."""

    def test_entities_generated(self, maule_config):
        writer = MauleWriter(maule_config)

        assert len(writer.right_junctions) >= 1  # at least armerillo
        assert len(writer.flow_rights) >= 5  # normal+ordinary+comp+res105+districts
        assert len(writer.volume_rights) >= 3  # monthly+annual+seasonal
        assert len(writer.user_constraints) >= 2  # reserve allocation splits

    def test_armerillo_junction(self, maule_config):
        writer = MauleWriter(maule_config)

        armerillo = next(
            rj for rj in writer.right_junctions if rj["name"] == "armerillo"
        )
        assert armerillo["drain"] is True

    def test_flow_rights_have_bound_rule(self, maule_config):
        writer = MauleWriter(maule_config)

        elec_normal = next(
            fr for fr in writer.flow_rights if fr["name"] == "maule_elec_normal"
        )
        assert "bound_rule" in elec_normal
        rule = elec_normal["bound_rule"]
        assert rule["reservoir"] == "COLBUN"
        assert len(rule["segments"]) == 2

    def test_volume_rights_have_reset_month(self, maule_config):
        writer = MauleWriter(maule_config)

        vol_elec_annual = next(
            vr for vr in writer.volume_rights if vr["name"] == "maule_vol_elec_annual"
        )
        assert vol_elec_annual["reset_month"] == "june"
        assert vol_elec_annual["emax"] == pytest.approx(250.0)

        vol_irr = next(
            vr for vr in writer.volume_rights if vr["name"] == "maule_vol_irr_seasonal"
        )
        assert vol_irr["reset_month"] == "june"

    def test_res105_fixed_mode(self, maule_config):
        writer = MauleWriter(maule_config)

        res105 = next(fr for fr in writer.flow_rights if fr["name"] == "maule_res105")
        assert "discharge" in res105
        assert res105["purpose"] == "environmental"

    def test_district_flow_rights(self, maule_config):
        writer = MauleWriter(maule_config)

        district_names = {d["name"] for d in maule_config["districts"]}
        fr_names = {fr["name"] for fr in writer.flow_rights}
        assert district_names.issubset(fr_names)

    def test_district_user_constraints(self, maule_config):
        writer = MauleWriter(maule_config)

        # Should have user constraints for each district
        district_ucs = [
            uc for uc in writer.user_constraints if uc["name"].startswith("dist_")
        ]
        assert len(district_ucs) == len(maule_config["districts"])

    def test_to_json_dict_keys(self, maule_config):
        writer = MauleWriter(maule_config)
        result = writer.to_json_dict()

        assert "right_junction_array" in result
        assert "flow_right_array" in result
        assert "volume_right_array" in result
        assert "user_constraint_array" in result

    def test_json_serializable(self, maule_config):
        writer = MauleWriter(maule_config)
        result = writer.to_json_dict()
        # Ensure all entities are JSON-serializable
        json_str = json.dumps(result)
        assert len(json_str) > 0

    def test_json_roundtrip(self, maule_config):
        writer = MauleWriter(maule_config)
        result = writer.to_json_dict()
        roundtrip = json.loads(json.dumps(result))
        assert len(roundtrip["flow_right_array"]) == len(writer.flow_rights)
        assert len(roundtrip["volume_right_array"]) == len(writer.volume_rights)
        assert len(roundtrip["right_junction_array"]) == len(writer.right_junctions)
        assert len(roundtrip["user_constraint_array"]) == len(writer.user_constraints)

    def test_bound_rule_zone_thresholds(self, maule_config):
        """Verify normal zone bound_rule uses correct cumulative thresholds."""
        writer = MauleWriter(maule_config)

        elec_normal = next(
            fr for fr in writer.flow_rights if fr["name"] == "maule_elec_normal"
        )
        segs = elec_normal["bound_rule"]["segments"]
        # Segment 0: inactive at volume=0
        assert segs[0]["volume"] == pytest.approx(0)
        assert segs[0]["constant"] == pytest.approx(0)
        # Segment 1: active at v_zone_normal = v_reserva_extraord + v_reserva_ordinaria
        v_zone_normal = (
            maule_config["v_reserva_extraord"] + maule_config["v_reserva_ordinaria"]
        )
        assert segs[1]["volume"] == pytest.approx(v_zone_normal)
        assert segs[1]["constant"] == pytest.approx(maule_config["gasto_elec_dia_max"])

    def test_ordinary_reserve_three_segments(self, maule_config):
        """Ordinary reserve FlowRights must have 3 bound_rule segments."""
        writer = MauleWriter(maule_config)

        for name in ["maule_elec_ordinary", "maule_irr_ordinary"]:
            fr = next(f for f in writer.flow_rights if f["name"] == name)
            segs = fr["bound_rule"]["segments"]
            assert len(segs) == 3, f"{name} should have 3 segments"
            # Segment 0: inactive below extraordinary zone
            assert segs[0]["constant"] == pytest.approx(0)
            # Segment 1: active between extraordinary and normal
            assert segs[1]["volume"] == pytest.approx(
                maule_config["v_reserva_extraord"]
            )
            # Segment 2: deactivated above normal zone
            v_zone_normal = (
                maule_config["v_reserva_extraord"] + maule_config["v_reserva_ordinaria"]
            )
            assert segs[2]["volume"] == pytest.approx(v_zone_normal)
            assert segs[2]["constant"] == pytest.approx(0)

    def test_compensation_flow_right(self, maule_config):
        writer = MauleWriter(maule_config)
        comp = next(
            fr for fr in writer.flow_rights if fr["name"] == "maule_compensation"
        )
        assert comp["purpose"] == "generation"
        assert comp["fmax"] == pytest.approx(maule_config["gasto_elec_dia_max"])
        assert "bound_rule" not in comp  # no volume-dependent bound

    def test_all_flow_rights_have_discharge(self, maule_config):
        writer = MauleWriter(maule_config)
        for fr in writer.flow_rights:
            assert "discharge" in fr, f"{fr['name']} missing discharge"

    def test_district_constraint_operator(self, maule_config):
        """Districts with has_slack=True use <=, others use =."""
        writer = MauleWriter(maule_config)

        districts_by_name = {d["name"]: d for d in maule_config["districts"]}
        for uc in writer.user_constraints:
            if not uc["name"].startswith("dist_"):
                continue
            district_name = uc["name"][len("dist_") :]
            d = districts_by_name[district_name]
            if d["has_slack"]:
                assert "<=" in uc["expression"], f"{district_name} should use <="
            else:
                # Exact equality: expression should contain "= " but not "<="
                assert "<=" not in uc["expression"], f"{district_name} should use ="

    def test_volume_rights_compensation_no_reset(self, maule_config):
        writer = MauleWriter(maule_config)
        vol_comp = next(
            vr for vr in writer.volume_rights if vr["name"] == "maule_vol_compensation"
        )
        assert "reset_month" not in vol_comp

    def test_volume_rights_econ_invernada(self, maule_config):
        writer = MauleWriter(maule_config)
        vol_econ = next(
            vr
            for vr in writer.volume_rights
            if vr["name"] == "maule_vol_econ_invernada"
        )
        assert vol_econ["reservoir"] == maule_config["central_invernada"]
        assert vol_econ["purpose"] == "economy"
        assert "reset_month" not in vol_econ

    def test_volume_rights_monthly_reset(self, maule_config):
        writer = MauleWriter(maule_config)
        vol_monthly = next(
            vr for vr in writer.volume_rights if vr["name"] == "maule_vol_elec_monthly"
        )
        assert vol_monthly["reset_month"] == "january"
        assert vol_monthly["emax"] == pytest.approx(maule_config["gasto_elec_men_max"])

    def test_unique_uids(self, maule_config):
        writer = MauleWriter(maule_config)
        all_uids = []
        for entity_list in [
            writer.flow_rights,
            writer.volume_rights,
            writer.right_junctions,
            writer.user_constraints,
        ]:
            for entity in entity_list:
                all_uids.append(entity["uid"])
        assert len(all_uids) == len(set(all_uids)), "UIDs must be unique"

    def test_right_junction_count(self, maule_config):
        """Should have armerillo + 4 central partitions + invernada_balance = 6."""
        writer = MauleWriter(maule_config)
        assert len(writer.right_junctions) == 6

    def test_central_partition_junctions(self, maule_config):
        writer = MauleWriter(maule_config)
        rj_names = {rj["name"] for rj in writer.right_junctions}
        for central in [
            maule_config["central_maule"],
            maule_config["central_invernada"],
            maule_config["central_melado"],
            maule_config["central_colbun"],
        ]:
            assert f"partition_{central}" in rj_names

    def test_user_constraint_expressions_reference_flow_rights(self, maule_config):
        """UserConstraint expressions should reference existing FlowRight names."""
        writer = MauleWriter(maule_config)
        fr_names = {fr["name"] for fr in writer.flow_rights}
        for uc in writer.user_constraints:
            # Extract flow_right('name') references from expression
            expr = uc["expression"]
            import re

            refs = re.findall(r"flow_right\('([^']+)'\)", expr)
            for ref in refs:
                assert ref in fr_names, (
                    f"UC '{uc['name']}' references '{ref}' which is not a FlowRight"
                )


class TestMauleScheduleHelpers:
    """Test schedule conversion helpers without stage_parser."""

    def test_to_stb_sched_uniform(self):
        result = MauleWriter._to_stb_sched([5.0, 5.0, 5.0])
        assert result == 5.0

    def test_to_stb_sched_varying(self):
        result = MauleWriter._to_stb_sched([1.0, 2.0, 3.0])
        assert result == [[[1.0], [2.0], [3.0]]]

    def test_to_tb_sched_uniform(self):
        result = MauleWriter._to_tb_sched([10.0, 10.0])
        assert result == 10.0

    def test_to_tb_sched_varying(self):
        result = MauleWriter._to_tb_sched([10.0, 20.0])
        assert result == [[10.0], [20.0]]


def _minimal_maule_config():
    """Return a minimal valid Maule config for unit tests."""
    return {
        "central_maule": "LMAULE",
        "central_invernada": "CIPRESES",
        "central_melado": "PEHUENCHE",
        "central_colbun": "COLBUN",
        "v_reserva_extraord": 100.0,
        "v_reserva_ordinaria": 400.0,
        "v_der_riego_temp_max": 800.0,
        "v_der_elect_anu_max": 250.0,
        "v_comp_elec_max": 350.0,
        "gasto_elec_men_max": 25.0,
        "gasto_elec_dia_max": 30.0,
        "gasto_riego_max": 200.0,
        "mod_elec_reserva": [100] * 6 + [0] * 6,
        "pct_riego_mensual": [100] * 12,
        "pct_elec_reserva": 20.0,
        "pct_riego_reserva": 80.0,
        "caudal_res105": [80, 40, 40, 40, 40, 40, 40, 40, 60, 80, 80, 80],
        "v_gasto_elec_men_ini": 0.0,
        "v_gasto_elec_anu_ini": 50.0,
        "v_gasto_riego_ini": 400.0,
        "v_comp_elec_ini": 10.0,
        "v_econ_inver_ini": 0.0,
        "penalizador_1": 1500.0,
        "costo_riego_ns_maule": 1000.0,
        "costo_riego_ns_res105": 1000.0,
        "econ_inver_costo": 0.5,
        "bocatoma_canelon": "BCanelon",
        "costo_canelon": 10.0,
        "districts": [
            {"name": "Dist1", "percentage": 60.0, "has_slack": False},
            {"name": "Dist2", "percentage": 40.0, "has_slack": True},
        ],
    }


class TestMauleWriterMinimalConfig:
    """Test MauleWriter with a minimal synthetic config."""

    def test_minimal_config_builds(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        assert len(writer.flow_rights) >= 6
        assert len(writer.volume_rights) == 5
        assert len(writer.right_junctions) == 6  # 5 original + invernada_balance

    def test_district_percentage_sum(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        district_ucs = [
            uc for uc in writer.user_constraints if uc["name"].startswith("dist_")
        ]
        assert len(district_ucs) == 2

    def test_slack_district_uses_le(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        uc_dist2 = next(
            uc for uc in writer.user_constraints if uc["name"] == "dist_Dist2"
        )
        assert "<=" in uc_dist2["expression"]

    def test_no_slack_district_uses_eq(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        uc_dist1 = next(
            uc for uc in writer.user_constraints if uc["name"] == "dist_Dist1"
        )
        assert "<=" not in uc_dist1["expression"]


class TestMauleInvernadaBalance:
    """Tests for La Invernada winter storage balance entities."""

    def test_invernada_right_junction(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        rj = next(
            rj for rj in writer.right_junctions if rj["name"] == "invernada_balance"
        )
        assert rj["drain"] is False

    def test_invernada_flow_rights_count(self):
        """Should emit 5 FlowRights for La Invernada balance."""
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        inv_names = {
            "invernada_deficit",
            "invernada_no_deficit",
            "invernada_natural_inflow",
            "invernada_storage",
            "invernada_bypass",
        }
        inv_frs = [fr for fr in writer.flow_rights if fr["name"] in inv_names]
        assert len(inv_frs) == 5

    def test_invernada_directions(self):
        """Deficit/no-deficit/inflow are supply (+1), storage/bypass are withdrawal (-1)."""
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        fr_by_name = {fr["name"]: fr for fr in writer.flow_rights}

        for name in [
            "invernada_deficit",
            "invernada_no_deficit",
            "invernada_natural_inflow",
        ]:
            assert fr_by_name[name]["direction"] == 1

        for name in ["invernada_storage", "invernada_bypass"]:
            assert fr_by_name[name]["direction"] == -1

    def test_invernada_junction_refs(self):
        """All Invernada FlowRights should reference invernada_balance."""
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        inv_names = {
            "invernada_deficit",
            "invernada_no_deficit",
            "invernada_natural_inflow",
            "invernada_storage",
            "invernada_bypass",
        }
        for fr in writer.flow_rights:
            if fr["name"] in inv_names:
                assert fr["right_junction"] == "invernada_balance"

    def test_invernada_storage_use_cost(self):
        """Storage FlowRight should have use_cost from econ_inver_costo."""
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        storage = next(
            fr for fr in writer.flow_rights if fr["name"] == "invernada_storage"
        )
        assert storage["use_cost"] == pytest.approx(0.5)

    def test_invernada_storage_zero_cost_omitted(self):
        """When econ_inver_costo is 0, use_cost should not be emitted."""
        cfg = _minimal_maule_config()
        cfg["econ_inver_costo"] = 0.0
        writer = MauleWriter(cfg)
        storage = next(
            fr for fr in writer.flow_rights if fr["name"] == "invernada_storage"
        )
        assert "use_cost" not in storage


class TestMauleBocatomaCanelon:
    """Tests for Bocatoma Canelon infrastructure cost."""

    def test_bocatoma_emitted(self):
        cfg = _minimal_maule_config()
        writer = MauleWriter(cfg)
        canelon = next(
            (fr for fr in writer.flow_rights if fr["name"] == "BCanelon"), None
        )
        assert canelon is not None
        assert canelon["use_cost"] == pytest.approx(10.0)
        assert canelon["purpose"] == "irrigation"

    def test_bocatoma_zero_cost_omitted(self):
        """When costo_canelon is 0, Bocatoma should not be emitted."""
        cfg = _minimal_maule_config()
        cfg["costo_canelon"] = 0.0
        writer = MauleWriter(cfg)
        canelon = next(
            (fr for fr in writer.flow_rights if fr["name"] == "BCanelon"), None
        )
        assert canelon is None
