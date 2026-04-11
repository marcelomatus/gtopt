# -*- coding: utf-8 -*-

"""Round-trip tests for the gtopt_irrigation Stage-2 transform.

These tests verify that:

1. Loading a canonical ``laja.json`` / ``maule.json`` via ``from_json``
   produces entities equivalent to constructing the agreement directly
   from the in-memory dict.
2. The CLI emits ``laja.pampl`` / ``maule.pampl`` and an entities JSON
   document at the requested output directory.
3. Backward-compatibility shims in ``plp2gtopt.laja_writer`` /
   ``plp2gtopt.maule_writer`` resolve to the new ``LajaAgreement`` /
   ``MauleAgreement`` classes.
"""

import json

from gtopt_irrigation import LajaAgreement, MauleAgreement
from gtopt_irrigation.cli import main as cli_main


def _minimal_laja_config():
    """Minimal valid Laja config matching plp2gtopt's test fixture."""
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
        "cost_elec_ns": 1100,
        "cost_elec_uso": 0.1,
        "cost_mixed": 1.0,
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


def _minimal_maule_config():
    """Minimal valid Maule config matching plp2gtopt's test fixture."""
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
        "costo_embalsar": 1500.0,
        "costo_no_embalsar": 1000.0,
        "bocatoma_canelon": "BCanelon",
        "costo_canelon": 10.0,
        "districts": [
            {"name": "Dist1", "percentage": 60.0, "has_slack": False},
            {"name": "Dist2", "percentage": 40.0, "has_slack": True},
        ],
    }


class TestLajaAgreementFromJson:
    """LajaAgreement.from_json reproduces direct-dict construction."""

    def test_from_json_matches_dict(self, tmp_path):
        cfg = _minimal_laja_config()
        json_path = tmp_path / "laja.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh)

        direct = LajaAgreement(cfg)
        loaded = LajaAgreement.from_json(json_path)

        assert direct.flow_rights == loaded.flow_rights
        assert direct.volume_rights == loaded.volume_rights
        assert direct.user_constraints == loaded.user_constraints

    def test_from_json_emits_pampl(self, tmp_path):
        cfg = _minimal_laja_config()
        json_path = tmp_path / "laja.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh)

        agreement = LajaAgreement.from_json(json_path)
        result = agreement.to_json_dict(output_dir=tmp_path)

        assert result.get("user_constraint_file") == "laja.pampl"
        assert (tmp_path / "laja.pampl").exists()


class TestMauleAgreementFromJson:
    """MauleAgreement.from_json reproduces direct-dict construction."""

    def test_from_json_matches_dict(self, tmp_path):
        cfg = _minimal_maule_config()
        json_path = tmp_path / "maule.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh)

        direct = MauleAgreement(cfg)
        loaded = MauleAgreement.from_json(json_path)

        assert direct.flow_rights == loaded.flow_rights
        assert direct.volume_rights == loaded.volume_rights
        assert direct.user_constraints == loaded.user_constraints

    def test_from_json_emits_pampl(self, tmp_path):
        cfg = _minimal_maule_config()
        json_path = tmp_path / "maule.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh)

        agreement = MauleAgreement.from_json(json_path)
        result = agreement.to_json_dict(output_dir=tmp_path)

        assert result.get("user_constraint_file") == "maule.pampl"
        assert (tmp_path / "maule.pampl").exists()


class TestCli:
    """End-to-end CLI invocation produces the expected output files."""

    def test_cli_laja(self, tmp_path):
        json_path = tmp_path / "laja.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(_minimal_laja_config(), fh)

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "laja",
                "--in",
                str(json_path),
                "--out",
                str(out_dir),
            ]
        )
        assert rc == 0
        assert (out_dir / "laja.pampl").exists()
        entities_path = out_dir / "laja_entities.json"
        assert entities_path.exists()
        entities = json.loads(entities_path.read_text(encoding="utf-8"))
        assert "flow_right_array" in entities
        assert "volume_right_array" in entities
        assert entities.get("user_constraint_file") == "laja.pampl"

    def test_cli_maule(self, tmp_path):
        json_path = tmp_path / "maule.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(_minimal_maule_config(), fh)

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "maule",
                "--in",
                str(json_path),
                "--out",
                str(out_dir),
            ]
        )
        assert rc == 0
        assert (out_dir / "maule.pampl").exists()
        entities_path = out_dir / "maule_entities.json"
        assert entities_path.exists()


class TestScheduleHelpersEmptyGuard:
    """_to_stb_sched / _to_tb_sched must not crash on empty input."""

    def test_laja_empty_stb_returns_scalar_zero(self):
        # Build a degenerate agreement just to grab a bound method instance.
        agreement = LajaAgreement(_minimal_laja_config())
        assert agreement._to_stb_sched([]) == 0.0  # pylint: disable=protected-access
        assert agreement._to_tb_sched([]) == 0.0  # pylint: disable=protected-access

    def test_maule_empty_stb_returns_scalar_zero(self):
        agreement = MauleAgreement(_minimal_maule_config())
        assert agreement._to_stb_sched([]) == 0.0  # pylint: disable=protected-access
        assert agreement._to_tb_sched([]) == 0.0  # pylint: disable=protected-access


class TestCliErrorExitCodes:
    """CLI returns exit code 2 with a one-line ``ERROR:`` on user errors."""

    def test_missing_input_file(self, tmp_path, capsys):
        rc = cli_main(
            [
                "laja",
                "--input",
                str(tmp_path / "does_not_exist.json"),
                "--output",
                str(tmp_path / "out"),
            ]
        )
        assert rc == 2
        err = capsys.readouterr().err
        assert err.startswith("ERROR: input file not found")

    def test_invalid_json_input(self, tmp_path, capsys):
        bad_json = tmp_path / "bad.json"
        bad_json.write_text("{not valid", encoding="utf-8")
        rc = cli_main(
            [
                "maule",
                "--input",
                str(bad_json),
                "--output",
                str(tmp_path / "out"),
            ]
        )
        assert rc == 2
        err = capsys.readouterr().err
        assert err.startswith("ERROR: invalid JSON in input")

    def test_schema_violation(self, tmp_path, capsys):
        empty = tmp_path / "empty.json"
        empty.write_text("{}", encoding="utf-8")
        rc = cli_main(
            [
                "laja",
                "--input",
                str(empty),
                "--output",
                str(tmp_path / "out"),
            ]
        )
        assert rc == 2
        err = capsys.readouterr().err
        assert err.startswith("ERROR: laja agreement:")

    def test_input_output_aliases(self, tmp_path):
        """The canonical ``--input``/``--output`` names work alongside ``--in``/``--out``."""
        json_path = tmp_path / "laja.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(_minimal_laja_config(), fh)

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "laja",
                "--input",
                str(json_path),
                "--output",
                str(out_dir),
            ]
        )
        assert rc == 0
        assert (out_dir / "laja.pampl").exists()
        assert (out_dir / "laja_entities.json").exists()


class TestBackwardCompatibilityShims:
    """plp2gtopt.laja_writer and .maule_writer still work via shims."""

    def test_laja_writer_alias(self):
        from plp2gtopt.laja_writer import LajaWriter

        assert LajaWriter is LajaAgreement

    def test_maule_writer_alias(self):
        from plp2gtopt.maule_writer import MauleWriter

        assert MauleWriter is MauleAgreement

    def test_template_engine_shim(self):
        from plp2gtopt.template_engine import render_tson as shim_render_tson
        from gtopt_irrigation._template_engine import render_tson as new_render_tson

        assert shim_render_tson is new_render_tson
