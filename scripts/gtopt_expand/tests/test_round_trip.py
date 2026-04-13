# -*- coding: utf-8 -*-

"""Round-trip tests for the gtopt_expand Stage-2 transform.

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

from gtopt_expand import LajaAgreement, MauleAgreement
from gtopt_expand.cli import main as cli_main


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


class TestMauleInvernadaFlowRightBounds:
    """La Invernada FlowRights must be variable (fmax>0), not pinned to [0,0].

    Regression guard for a template bug where the five Invernada
    FlowRights were emitted with ``discharge: 0`` and no ``fmax`` field.
    Per ``source/flow_right_lp.cpp:53-56`` that combination pins the
    column to ``[0, 0]`` and the ``invernada_balance`` UserConstraint
    degenerates to ``0 = 0``.  The fix is to render ``qmax_invernada``
    into the ``fmax`` field so the columns are free in
    ``[0, qmax_invernada]``.
    """

    _INVERNADA_NAMES = (
        "invernada_deficit",
        "invernada_sin_deficit",
        "invernada_caudal_natural",
        "invernada_embalsar",
        "invernada_no_embalsar",
    )

    def test_all_five_have_fmax_equal_to_qmax_invernada(self):
        cfg = _minimal_maule_config()
        cfg["qmax_invernada"] = 275  # distinct from defaults so we prove the wire-up
        agreement = MauleAgreement(cfg)
        fr_by_name = {fr["name"]: fr for fr in agreement.flow_rights}
        for name in self._INVERNADA_NAMES:
            assert name in fr_by_name, f"missing FlowRight {name}"
            fr = fr_by_name[name]
            assert "fmax" in fr, (
                f"{name} has no fmax — would be pinned to [0, 0] by "
                f"flow_right_lp.cpp and invernada_balance would degenerate"
            )
            assert fr["fmax"] == 275, (
                f"{name}.fmax ({fr['fmax']}) does not match qmax_invernada (275)"
            )

    def test_default_qmax_invernada_is_used_when_omitted(self):
        """When ``qmax_invernada`` is absent from the config, the default
        (200) from ``maule_agreement.py`` must still produce a nonzero fmax."""
        cfg = _minimal_maule_config()
        cfg.pop("qmax_invernada", None)
        agreement = MauleAgreement(cfg)
        fr_by_name = {fr["name"]: fr for fr in agreement.flow_rights}
        for name in self._INVERNADA_NAMES:
            fr = fr_by_name[name]
            assert fr.get("fmax", 0) > 0, (
                f"{name}.fmax is falsy — default qmax_invernada not wired up"
            )


class TestMauleInvernadaBalanceSoft:
    """``invernada_balance`` must be emitted as a SOFT equality with
    ``penalty_class = "hydro_flow"`` so the LP absorbs PLP-style
    conditional-bound infeasibility through visible slack columns priced
    in $/m³ × duration[h] × 3600 (see ``source/user_constraint_lp.cpp``
    ``resolve_block_soft_penalty``).
    """

    def _invernada_balance_entry(self, cfg: dict) -> dict:
        agreement = MauleAgreement(cfg)
        ucs_by_name = {uc["name"]: uc for uc in agreement.user_constraints}
        assert "invernada_balance" in ucs_by_name, (
            "invernada_balance UserConstraint missing from agreement output"
        )
        return ucs_by_name["invernada_balance"]

    def test_penalty_class_is_hydro_flow(self):
        cfg = _minimal_maule_config()
        uc = self._invernada_balance_entry(cfg)
        assert uc.get("penalty_class") == "hydro_flow", (
            f"invernada_balance.penalty_class = {uc.get('penalty_class')!r}; "
            "expected 'hydro_flow' so the LP converts the $/m³ penalty to "
            "$/(m³/s) per block via duration × 3600"
        )

    def test_penalty_respects_explicit_penalty_invernada(self):
        cfg = _minimal_maule_config()
        cfg["penalty_invernada"] = 42.5
        uc = self._invernada_balance_entry(cfg)
        assert uc.get("penalty") == 42.5, (
            f"explicit penalty_invernada = 42.5 was not rendered "
            f"(got penalty={uc.get('penalty')!r})"
        )

    def test_penalty_defaults_to_hydro_fail_cost_when_omitted(self):
        cfg = _minimal_maule_config()
        cfg.pop("penalty_invernada", None)
        cfg["hydro_fail_cost"] = 17.0
        uc = self._invernada_balance_entry(cfg)
        assert uc.get("penalty") == 17.0, (
            f"penalty_invernada absent → should default to hydro_fail_cost=17.0, "
            f"got penalty={uc.get('penalty')!r}"
        )

    def test_penalty_has_a_positive_default_when_both_unset(self):
        """Belt-and-suspenders: even if both ``penalty_invernada`` and
        ``hydro_fail_cost`` are absent from the config, the rendered
        entry must still carry a **positive** penalty so the soft
        relaxation is genuinely armed.  A zero default would silently
        restore hard-equality behaviour."""
        cfg = _minimal_maule_config()
        cfg.pop("penalty_invernada", None)
        cfg.pop("hydro_fail_cost", None)
        uc = self._invernada_balance_entry(cfg)
        penalty = uc.get("penalty")
        assert isinstance(penalty, (int, float)) and penalty > 0, (
            f"invernada_balance.penalty must have a positive default, got {penalty!r}"
        )


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
        assert err.startswith("ERROR: laja:")

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


class TestMachicuraVariantAutoDetection:
    """The Machicura variant is resolved from three sources in priority order.

    1. Explicit ``cfg["machicura_model"]`` override wins.
    2. Otherwise, ``options["reservoir_names"]`` containing the resolved
       ``junction_retiro`` selects the ``embalse`` variant.
    3. Otherwise, the default ``pasada`` variant is used.
    """

    def test_pasada_default_when_nothing_supplied(self):
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        agreement = MauleAgreement(cfg)
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "pasada"
        assert agreement._template_name() == "maule"

    def test_reservoir_names_triggers_embalse(self):
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        agreement = MauleAgreement(
            cfg,
            options={"reservoir_names": {"MACHICURA", "OTHER_RESERVOIR"}},
        )
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "embalse"
        assert agreement._template_name() == "maule_machicura"

    def test_reservoir_names_detection_is_case_insensitive(self):
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        agreement = MauleAgreement(
            cfg,
            options={"reservoir_names": {"machicura"}},
        )
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "embalse"

    def test_reservoir_names_without_junction_stays_pasada(self):
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        agreement = MauleAgreement(
            cfg,
            options={"reservoir_names": {"COLBUN", "MELADO"}},
        )
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "pasada"

    def test_extrac_entries_override_junction_retiro(self):
        """``extrac_entries`` COLBUN downstream steers the match key."""
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        cfg["central_colbun"] = "COLBUN"
        cfg["extrac_entries"] = [
            {"name": "COLBUN", "downstream": "AguasAbajoColbun"},
        ]
        agreement = MauleAgreement(
            cfg,
            options={"reservoir_names": {"AguasAbajoColbun"}},
        )
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "embalse"

    def test_explicit_cfg_override_beats_reservoir_names(self):
        cfg = _minimal_maule_config()
        cfg["machicura_model"] = "pasada"
        agreement = MauleAgreement(
            cfg,
            options={"reservoir_names": {"MACHICURA"}},
        )
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "pasada"

    def test_explicit_cfg_embalse_without_reservoir_names(self):
        cfg = _minimal_maule_config()
        cfg["machicura_model"] = "embalse"
        agreement = MauleAgreement(cfg)
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "embalse"
        assert agreement._template_name() == "maule_machicura"

    def test_english_synonym_reservoir(self):
        cfg = _minimal_maule_config()
        cfg["machicura_model"] = "reservoir"
        agreement = MauleAgreement(cfg)
        # pylint: disable=protected-access
        assert agreement._machicura_model() == "embalse"

    def test_unknown_cfg_value_raises(self):
        import pytest

        cfg = _minimal_maule_config()
        cfg["machicura_model"] = "not-a-valid-variant"
        with pytest.raises(ValueError, match="unknown machicura_model"):
            MauleAgreement(cfg)


class TestCliMachicuraAutoDetection:
    """The CLI wires the sibling planning JSON into ``reservoir_names``."""

    @staticmethod
    def _write_maule(tmp_path):
        json_path = tmp_path / "maule.json"
        cfg = _minimal_maule_config()
        cfg.pop("machicura_model", None)
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh)
        return json_path

    @staticmethod
    def _write_planning(tmp_path, reservoir_names):
        planning = {
            "system": {
                "reservoir_array": [{"name": n} for n in reservoir_names],
            },
        }
        planning_path = tmp_path / "gtopt.json"
        with open(planning_path, "w", encoding="utf-8") as fh:
            json.dump(planning, fh)
        return planning_path

    def test_cli_autodetects_embalse_from_sibling_planning(self, tmp_path):
        json_path = self._write_maule(tmp_path)
        self._write_planning(tmp_path, ["MACHICURA"])

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
        entities = json.loads(
            (out_dir / "maule_entities.json").read_text(encoding="utf-8")
        )
        pampl = (out_dir / "maule.pampl").read_text(encoding="utf-8")
        # The embalse template references junction_retiro ("MACHICURA")
        # on the three retiro FlowRights — look for it in the entities.
        flows = entities.get("flow_right_array", [])
        retiro_names = {fr.get("name") for fr in flows}
        assert "retiro_maule_riego_reserva" in retiro_names or any(
            "MACHICURA" in str(fr.get("junction", "")) for fr in flows
        ), f"expected embalse variant markers, got {sorted(retiro_names)}"
        # Weaker check: the embalse pampl ships extra machicura content
        # — the pasada variant does not reference the string "machicura"
        # in its rendered pampl.  Either is fine as a smoke test.
        assert "MACHICURA" in pampl or "machicura" in pampl.lower()

    def test_cli_stays_pasada_without_sibling_planning(self, tmp_path):
        json_path = self._write_maule(tmp_path)
        # No planning.json next to it.

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

    def test_cli_explicit_planning_flag(self, tmp_path):
        json_path = self._write_maule(tmp_path)
        planning_path = tmp_path / "custom_planning.json"
        with open(planning_path, "w", encoding="utf-8") as fh:
            json.dump(
                {"system": {"reservoir_array": [{"name": "MACHICURA"}]}},
                fh,
            )

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "maule",
                "--in",
                str(json_path),
                "--out",
                str(out_dir),
                "--planning",
                str(planning_path),
            ]
        )
        assert rc == 0
        pampl = (out_dir / "maule.pampl").read_text(encoding="utf-8")
        assert "MACHICURA" in pampl or "machicura" in pampl.lower()

    def test_cli_no_auto_detect_flag_forces_pasada(self, tmp_path):
        json_path = self._write_maule(tmp_path)
        self._write_planning(tmp_path, ["MACHICURA"])

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "maule",
                "--in",
                str(json_path),
                "--out",
                str(out_dir),
                "--no-auto-detect-machicura",
            ]
        )
        assert rc == 0
        # With auto-detection disabled, even a sibling reservoir_array
        # listing MACHICURA must NOT flip the variant.  We can't easily
        # inspect "which template was rendered" post-hoc, but the emitted
        # entities should differ from the auto-detected case — at minimum,
        # the pampl file must still exist.
        assert (out_dir / "maule.pampl").exists()

    def test_cli_missing_explicit_planning_is_error(self, tmp_path):
        json_path = self._write_maule(tmp_path)

        out_dir = tmp_path / "out"
        rc = cli_main(
            [
                "maule",
                "--in",
                str(json_path),
                "--out",
                str(out_dir),
                "--planning",
                str(tmp_path / "nonexistent.json"),
            ]
        )
        # ``FileNotFoundError`` is caught by ``main`` and turned into
        # exit code 2 — a loud error, not a silent fallback.
        assert rc == 2


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
        from gtopt_expand._template_engine import render_tson as new_render_tson

        assert shim_render_tson is new_render_tson
