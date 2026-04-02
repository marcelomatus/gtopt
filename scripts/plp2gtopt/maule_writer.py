# -*- coding: utf-8 -*-

"""Writer for Maule irrigation agreement entities.

Converts parsed MauleParser data into gtopt JSON entities:
FlowRight, VolumeRight, and UserConstraint.

The Maule convention divides the Laguna del Maule / Colbun system into
three operational zones (normal, ordinary reserve, extraordinary reserve)
with volume-dependent rights allocation between ENDESA/Enel and irrigators.

See also:
  gtopt_vs_plp_comparison.md -- detailed variable mapping
  plp_implementation.md -- PLP Fortran source reference
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

import jinja2


# Month names for gtopt Stage (1-indexed: january=1 .. december=12)
_MONTH_NAMES = [
    "",
    "january",
    "february",
    "march",
    "april",
    "may",
    "june",
    "july",
    "august",
    "september",
    "october",
    "november",
    "december",
]


class MauleWriter:
    """Emits FlowRight, VolumeRight, and UserConstraint
    entities for the Maule irrigation agreement.

    Args:
        maule_config: Parsed configuration from MauleParser.config
        stage_parser: Parsed stage data (for month mapping)
        options: Conversion options dict
    """

    def __init__(
        self,
        maule_config: Dict[str, Any],
        stage_parser: Any = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        self._cfg = maule_config
        self._stage_parser = stage_parser
        self._options = options or {}
        self._uid_counter = 1000  # Start UIDs at 1000 to avoid collisions

        self.flow_rights: List[Dict[str, Any]] = []
        self.volume_rights: List[Dict[str, Any]] = []
        self.user_constraints: List[Dict[str, Any]] = []

        self._build()

    def _next_uid(self) -> int:
        """Generate a unique UID for rights entities."""
        uid = self._uid_counter
        self._uid_counter += 1
        return uid

    def _get_stages(self) -> List[Dict[str, Any]]:
        """Return the effective list of stages, truncated to match options."""
        if self._stage_parser is None:
            return []
        stages = self._stage_parser.get_all()
        last_stage = self._options.get("last_stage", -1)
        try:
            last_stage = int(last_stage)
        except (ValueError, TypeError):
            last_stage = -1
        if last_stage > 0:
            stages = [s for s in stages if s["number"] <= last_stage]
        return stages

    def _monthly_schedule(self, monthly_values: List[float]) -> List[float]:
        """Convert 12-element monthly array to per-stage values.

        Maps each stage's month (from stage_parser) to the corresponding
        monthly value. If no stage_parser is available, returns the raw
        12-element array (the user must ensure stage-month alignment).
        """
        if self._stage_parser is None:
            return monthly_values

        stages = self._get_stages()
        schedule: List[float] = []
        for stage in stages:
            month = stage.get("month", 1)
            # PLP months are 1-indexed, array is 0-indexed
            idx = (month - 1) % 12
            schedule.append(monthly_values[idx])
        return schedule

    def _to_stb_sched(
        self,
        values: List[float],
    ) -> float | List[List[List[float]]]:
        """Convert per-stage values to STBRealFieldSched format.

        STBRealFieldSched = FieldSched<Real, vector<vector<vector<Real>>>>
        so a per-stage schedule needs to be [[[v1]*nblocks, [v2]*nblocks, ...]]
        (3D).  Each stage value is replicated across all blocks.
        Returns scalar if all values are the same.
        """
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[[v] * nblocks for v in values]]

    def _to_tb_sched(
        self,
        values: List[float],
    ) -> float | List[List[float]]:
        """Convert per-stage values to TBRealFieldSched format.

        TBRealFieldSched = FieldSched<Real, vector<vector<Real>>>
        so a per-stage schedule needs to be [[v1]*nblocks, [v2]*nblocks, ...]
        (2D).  Each stage value is replicated across all blocks.
        Returns scalar if all values are the same.
        """
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[v] * nblocks for v in values]

    def _monthly_fmax_schedule(
        self,
        monthly_pcts: List[float],
        base_flow: float,
    ) -> float | List[List[float]]:
        """Build per-stage fmax from monthly percentage × base flow.

        Returns a scalar if all stages have the same value, otherwise
        a 2D array compatible with OptTBRealFieldSched (stage × block).
        """
        monthly_flows = [pct / 100.0 * base_flow for pct in monthly_pcts]
        schedule = self._monthly_schedule(monthly_flows)
        return self._to_tb_sched(schedule)

    def _build(self) -> None:
        """Build all rights entities from the parsed configuration."""
        cfg = self._cfg

        # Reservoir references (used by bound_rule)
        res_colbun = cfg["central_colbun"]
        v_extraord = cfg["v_reserva_extraord"]  # hm3
        v_ordinaria = cfg["v_reserva_ordinaria"]  # hm3 (above extraord)

        # Total volume thresholds (cumulative)
        v_zone_extraord = v_extraord  # 129 hm3
        v_zone_normal = v_extraord + v_ordinaria  # 129 + 452 = 581 hm3

        # --- FlowRight: Normal electric rights (IQMNE) ---
        # Active in normal zone, fmax = gasto_elec_dia_max
        fr_elec_normal_uid = self._next_uid()
        elec_day_max = cfg["gasto_elec_dia_max"]
        self.flow_rights.append(
            {
                "uid": fr_elec_normal_uid,
                "name": "maule_elec_normal",
                "purpose": "generation",
                "direction": -1,
                "discharge": 0,
                "fmax": elec_day_max,
                "use_average": True,
                "fail_cost": cfg.get("penalizador_1", 1500),
                "bound_rule": {
                    "reservoir": res_colbun,
                    "segments": [
                        {"volume": 0, "slope": 0, "constant": 0},
                        {
                            "volume": v_zone_normal,
                            "slope": 0,
                            "constant": elec_day_max,
                        },
                    ],
                },
            }
        )

        # --- FlowRight: Normal irrigation rights (IQMNR) ---
        fr_irr_normal_uid = self._next_uid()
        riego_max = cfg["gasto_riego_max"]
        # Monthly percentage schedule for irrigation
        pct_riego = cfg["pct_riego_mensual"]
        irr_fmax_schedule = self._monthly_fmax_schedule(pct_riego, riego_max)
        fr_irr_normal: Dict[str, Any] = {
            "uid": fr_irr_normal_uid,
            "name": "maule_irr_normal",
            "purpose": "irrigation",
            "direction": -1,
            "discharge": 0,
            "fmax": irr_fmax_schedule,
            "use_average": True,
            "fail_cost": cfg.get("costo_riego_ns_maule", 1000),
            "bound_rule": {
                "reservoir": res_colbun,
                "segments": [
                    {"volume": 0, "slope": 0, "constant": 0},
                    {
                        "volume": v_zone_normal,
                        "slope": 0,
                        "constant": riego_max,
                    },
                ],
            },
        }
        # Irrigation delivery benefit (negative cost = benefit in objective)
        valor_riego = cfg.get("valor_riego_maule", 0)
        if valor_riego > 0:
            fr_irr_normal["use_value"] = valor_riego
        self.flow_rights.append(fr_irr_normal)

        # --- FlowRight: Ordinary reserve electric (IQMOE) ---
        fr_elec_ord_uid = self._next_uid()
        # In ordinary reserve zone, electric gets pct_elec_reserva % of flow
        # Monthly modulation via mod_elec_reserva
        mod_elec = cfg["mod_elec_reserva"]
        elec_ord_fmax = self._monthly_fmax_schedule(mod_elec, elec_day_max)
        self.flow_rights.append(
            {
                "uid": fr_elec_ord_uid,
                "name": "maule_elec_ordinary",
                "purpose": "generation",
                "direction": -1,
                "discharge": 0,
                "fmax": elec_ord_fmax,
                "use_average": True,
                "bound_rule": {
                    "reservoir": res_colbun,
                    "segments": [
                        {"volume": 0, "slope": 0, "constant": 0},
                        {
                            "volume": v_zone_extraord,
                            "slope": 0,
                            "constant": elec_day_max,
                        },
                        {
                            "volume": v_zone_normal,
                            "slope": 0,
                            "constant": 0,
                        },
                    ],
                },
            }
        )

        # --- FlowRight: Ordinary reserve irrigation (IQMOR) ---
        fr_irr_ord_uid = self._next_uid()
        fr_irr_ord: Dict[str, Any] = {
            "uid": fr_irr_ord_uid,
            "name": "maule_irr_ordinary",
            "purpose": "irrigation",
            "direction": -1,
            "discharge": 0,
            "fmax": irr_fmax_schedule,
            "use_average": True,
            "bound_rule": {
                "reservoir": res_colbun,
                "segments": [
                    {"volume": 0, "slope": 0, "constant": 0},
                    {
                        "volume": v_zone_extraord,
                        "slope": 0,
                        "constant": riego_max,
                    },
                    {
                        "volume": v_zone_normal,
                        "slope": 0,
                        "constant": 0,
                    },
                ],
            },
        }
        if valor_riego > 0:
            fr_irr_ord["use_value"] = valor_riego
        self.flow_rights.append(fr_irr_ord)

        # --- FlowRight: ENDESA compensation (IQMCE) ---
        fr_comp_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_comp_uid,
                "name": "maule_compensation",
                "purpose": "generation",
                "direction": -1,
                "discharge": 0,
                "fmax": elec_day_max,
                "use_average": True,
            }
        )

        # --- FlowRight: Resolution 105 minimum flows ---
        fr_res105_uid = self._next_uid()
        caudal_res105 = cfg["caudal_res105"]
        res105_values = self._monthly_schedule(caudal_res105)
        res105_discharge = self._to_stb_sched(res105_values)
        fr_res105: Dict[str, Any] = {
            "uid": fr_res105_uid,
            "name": "maule_res105",
            "purpose": "environmental",
            "direction": -1,
            "discharge": res105_discharge,
            "fail_cost": cfg.get("costo_riego_ns_res105", 1000),
        }
        # Res105 delivery benefit (negative cost = benefit in objective)
        valor_res105 = cfg.get("valor_riego_res105", 0)
        if valor_res105 > 0:
            fr_res105["use_value"] = valor_res105
        self.flow_rights.append(fr_res105)

        # --- VolumeRight: Monthly electric accumulator (IVMGEMF) ---
        # Coupled to maule_elec_normal: FlowRight flow decrements volume.
        vr_elec_men_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_elec_men_uid,
                "name": "maule_vol_elec_monthly",
                "purpose": "generation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_elec_normal",
                "eini": cfg["v_gasto_elec_men_ini"],
                "emax": cfg["gasto_elec_men_max"],
                "use_state_variable": True,
                "reset_month": "january",
            }
        )

        # --- VolumeRight: Annual electric accumulator (IVMGEAF) ---
        # Also coupled to maule_elec_normal (same flow, different reset).
        vr_elec_anu_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_elec_anu_uid,
                "name": "maule_vol_elec_annual",
                "purpose": "generation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_elec_normal",
                "eini": cfg["v_gasto_elec_anu_ini"],
                "emax": cfg["v_der_elect_anu_max"],
                "use_state_variable": True,
                "reset_month": "june",
            }
        )

        # --- VolumeRight: Seasonal irrigation accumulator (IVMGRTF) ---
        vr_irr_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_irr_uid,
                "name": "maule_vol_irr_seasonal",
                "purpose": "irrigation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_irr_normal",
                "eini": cfg["v_gasto_riego_ini"],
                "emax": cfg["v_der_riego_temp_max"],
                "use_state_variable": True,
                "reset_month": "june",
            }
        )

        # --- VolumeRight: ENDESA compensation accumulator ---
        vr_comp_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_comp_uid,
                "name": "maule_vol_compensation",
                "purpose": "generation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_compensation",
                "eini": cfg["v_comp_elec_ini"],
                "emax": cfg["v_comp_elec_max"],
                "use_state_variable": True,
            }
        )

        # --- VolumeRight: Extraordinary reserve electric accumulator ---
        # Tracks accumulated electric extraction from the extraordinary
        # reserve zone (bottom zone, 0..v_reserva_extraord hm3).
        # PLP variable: v_gasto_rext_elec / v_der_rext_elec
        vr_rext_elec_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_rext_elec_uid,
                "name": "maule_vol_rext_elec",
                "purpose": "generation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_elec_ordinary",
                "eini": cfg.get("v_gasto_rext_elec_ini", 0.0),
                "emax": cfg["v_reserva_extraord"],
                "use_state_variable": True,
            }
        )

        # --- VolumeRight: Extraordinary reserve irrigation accumulator ---
        # Tracks accumulated irrigation extraction from the extraordinary
        # reserve zone.
        # PLP variable: v_gasto_rext_riego / v_der_rext_riego
        vr_rext_riego_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_rext_riego_uid,
                "name": "maule_vol_rext_riego",
                "purpose": "irrigation",
                "reservoir": res_colbun,
                "source_flow_right": "maule_irr_ordinary",
                "eini": cfg.get("v_gasto_rext_riego_ini", 0.0),
                "emax": cfg["v_reserva_extraord"],
                "use_state_variable": True,
            }
        )

        # --- VolumeRight: La Invernada winter economy ---
        vr_econ_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_econ_uid,
                "name": "maule_vol_econ_invernada",
                "purpose": "economy",
                "reservoir": cfg["central_invernada"],
                "eini": cfg["v_econ_inver_ini"],
                "use_state_variable": True,
            }
        )

        # --- FlowRight: La Invernada deficit discharge (IQIDN) ---
        fr_idn_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_idn_uid,
                "name": "invernada_deficit",
                "purpose": "irrigation",
                "direction": 1,
                "discharge": 0,
                "use_average": True,
            }
        )

        # --- FlowRight: La Invernada no-deficit storage (IQISD) ---
        fr_isd_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_isd_uid,
                "name": "invernada_no_deficit",
                "purpose": "irrigation",
                "direction": 1,
                "discharge": 0,
                "use_average": True,
            }
        )

        # --- FlowRight: La Invernada natural inflow (IQNINV) ---
        fr_ninv_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_ninv_uid,
                "name": "invernada_natural_inflow",
                "purpose": "irrigation",
                "direction": 1,
                "discharge": 0,
                "use_average": True,
            }
        )

        # --- FlowRight: La Invernada storage to reservoir (IQHEIN) ---
        fr_hein_uid = self._next_uid()
        fr_hein: Dict[str, Any] = {
            "uid": fr_hein_uid,
            "name": "invernada_storage",
            "purpose": "economy",
            "direction": -1,
            "discharge": 0,
            "use_average": True,
        }
        econ_costo = cfg.get("econ_inver_costo", 0.0)
        if econ_costo > 0:
            fr_hein["use_value"] = econ_costo
        self.flow_rights.append(fr_hein)

        # --- FlowRight: La Invernada bypass (IQHNEIN) ---
        fr_hnein_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_hnein_uid,
                "name": "invernada_bypass",
                "purpose": "economy",
                "direction": -1,
                "discharge": 0,
                "use_average": True,
            }
        )

        # --- FlowRight: Bocatoma Canelon ---
        costo_canelon = cfg.get("costo_canelon", 0.0)
        if costo_canelon > 0:
            fr_canelon_uid = self._next_uid()
            self.flow_rights.append(
                {
                    "uid": fr_canelon_uid,
                    "name": cfg.get("bocatoma_canelon", "bocatoma_canelon"),
                    "purpose": "irrigation",
                    "direction": -1,
                    "discharge": 0,
                    "use_value": costo_canelon,
                }
            )

        # --- UserConstraint: La Invernada flow balance ---
        # deficit + no_deficit + natural_inflow = storage + bypass
        uc_invernada_uid = self._next_uid()
        self.user_constraints.append(
            {
                "uid": uc_invernada_uid,
                "name": "invernada_balance",
                "expression": (
                    "flow_right('invernada_deficit').flow "
                    "+ flow_right('invernada_no_deficit').flow "
                    "+ flow_right('invernada_natural_inflow').flow "
                    "= flow_right('invernada_storage').flow "
                    "+ flow_right('invernada_bypass').flow"
                ),
                "description": ("La Invernada winter balance: inflows equal outflows"),
            }
        )

        # --- UserConstraint: Percentage allocation in ordinary reserve ---
        # Electric gets pct_elec_reserva% of total flow in ordinary zone
        uc_elec_pct_uid = self._next_uid()
        pct_elec = cfg["pct_elec_reserva"]
        self.user_constraints.append(
            {
                "uid": uc_elec_pct_uid,
                "name": "maule_ord_elec_pct",
                "expression": (
                    f"flow_right('maule_elec_ordinary').flow <= "
                    f"{pct_elec / 100.0} * flow_right('maule_elec_ordinary').flow "
                    f"+ {pct_elec / 100.0} * flow_right('maule_irr_ordinary').flow"
                ),
                "description": (
                    f"Electric capped at {pct_elec}% of total ordinary reserve flow"
                ),
            }
        )

        # Irrigation gets pct_riego_reserva% of total flow in ordinary zone
        uc_irr_pct_uid = self._next_uid()
        pct_riego_r = cfg["pct_riego_reserva"]
        self.user_constraints.append(
            {
                "uid": uc_irr_pct_uid,
                "name": "maule_ord_irr_pct",
                "expression": (
                    f"flow_right('maule_irr_ordinary').flow <= "
                    f"{pct_riego_r / 100.0} * flow_right('maule_elec_ordinary').flow "
                    f"+ {pct_riego_r / 100.0} * flow_right('maule_irr_ordinary').flow"
                ),
                "description": (
                    f"Irrigation capped at {pct_riego_r}% of total ordinary reserve flow"
                ),
            }
        )

        # --- FlowRight: Withdrawal districts ---
        for district in cfg["districts"]:
            d_uid = self._next_uid()
            pct = district["percentage"]
            constraint_op = "<=" if district["has_slack"] else "="

            self.flow_rights.append(
                {
                    "uid": d_uid,
                    "name": district["name"],
                    "purpose": "irrigation",
                    "direction": -1,
                    "discharge": 0,
                    "fmax": irr_fmax_schedule,
                }
            )

            # UserConstraint for proportional allocation
            uc_uid = self._next_uid()
            self.user_constraints.append(
                {
                    "uid": uc_uid,
                    "name": f"dist_{district['name']}",
                    "expression": (
                        f"flow_right('{district['name']}').flow "
                        f"{constraint_op} "
                        f"{pct / 100.0} * flow_right('maule_irr_normal').flow"
                    ),
                    "description": (f"{district['name']}: {pct}% of total irrigation"),
                }
            )

    def generate_pampl(self, output_path: Path) -> str:
        """Render the Maule agreement PAMPL file from the Jinja2 template.

        Args:
            output_path: Directory where the .pampl file will be written.

        Returns:
            Filename of the generated .pampl file (relative name only).
        """
        template_dir = Path(__file__).parent / "templates"
        env = jinja2.Environment(
            loader=jinja2.FileSystemLoader(str(template_dir)),
            keep_trailing_newline=True,
            undefined=jinja2.StrictUndefined,
        )
        template = env.get_template("maule_agreement.tampl")

        rendered = template.render(self._cfg)

        output_path = Path(output_path)
        output_path.mkdir(parents=True, exist_ok=True)
        pampl_file = output_path / "maule_agreement.pampl"
        pampl_file.write_text(rendered, encoding="utf-8")

        return "maule_agreement.pampl"

    def to_json_dict(
        self,
        output_dir: Optional[Path] = None,
    ) -> Dict[str, List[Dict[str, Any]]]:
        """Return all entities as a dict of arrays for system JSON.

        Args:
            output_dir: If provided, generates a .pampl file in this
                directory and sets ``user_constraint_file`` instead of
                embedding constraints in ``user_constraint_array``.
        """
        result: Dict[str, List[Dict[str, Any]]] = {}
        if self.flow_rights:
            result["flow_right_array"] = self.flow_rights
        if self.volume_rights:
            result["volume_right_array"] = self.volume_rights
        if self.user_constraints:
            if output_dir is not None:
                pampl_name = self.generate_pampl(output_dir)
                result["user_constraint_file"] = pampl_name  # type: ignore[assignment]
            else:
                result["user_constraint_array"] = self.user_constraints
        return result
