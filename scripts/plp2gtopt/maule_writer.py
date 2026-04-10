# -*- coding: utf-8 -*-

"""Writer for Maule irrigation agreement entities.

Converts parsed MauleParser data into gtopt JSON entities:
FlowRight, VolumeRight, and UserConstraint.

The Maule convention divides the Laguna del Maule / Colbun system into
three operational zones (normal, ordinary reserve, extraordinary reserve)
with volume-dependent rights allocation between ENDESA/Enel and irrigators.

The JSON entity structure is defined in the ``maule.tson`` template file,
which uses @param@ m4-style syntax for parameter substitution.  Computed
values (schedules, zone thresholds, costs) are pre-computed here and
passed as template context.

See also:
  gtopt_vs_plp_comparison.md -- detailed variable mapping
  plp_implementation.md -- PLP Fortran source reference
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

import jinja2

from plp2gtopt.template_engine import render_tson


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

    The JSON entity structure is defined in ``templates/maule.tson``.
    This class pre-computes all dynamic values (schedules, costs,
    zone thresholds) and passes them as template context parameters.

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

    def _prepare_context(self) -> Dict[str, Any]:
        """Prepare the template context with all pre-computed values.

        Returns a dict of parameters that the maule.tson template uses
        via @param@ substitution.
        """
        cfg = self._cfg

        res_colbun = cfg["central_colbun"]
        v_extraord = cfg["v_reserva_extraord"]
        v_ordinaria = cfg["v_reserva_ordinaria"]
        v_zone_extraord = v_extraord
        v_zone_normal = v_extraord + v_ordinaria

        elec_day_max = cfg["gasto_elec_dia_max"]
        riego_max = cfg["gasto_riego_max"]

        # --- Schedules ---
        pct_riego = cfg["pct_riego_mensual"]
        irr_fmax_schedule = self._monthly_fmax_schedule(pct_riego, riego_max)

        mod_elec = cfg["mod_elec_reserva"]
        elec_ord_fmax = self._monthly_fmax_schedule(mod_elec, elec_day_max)

        caudal_res105 = cfg["caudal_res105"]
        res105_values = self._monthly_schedule(caudal_res105)
        res105_discharge = self._to_stb_sched(res105_values)

        valor_riego = cfg.get("valor_riego_maule", 0)
        valor_riego_res105 = cfg.get("valor_riego_res105", 0)

        costo_embalsar = cfg.get("costo_embalsar", 1500.0)
        costo_no_embalsar = cfg.get("costo_no_embalsar", 1000.0)
        costo_canelon = cfg.get("costo_canelon", 0.0)

        # --- User constraint expressions ---
        pct_elec = cfg["pct_elec_reserva"]
        pct_riego_r = cfg["pct_riego_reserva"]

        expression_invernada = (
            "flow_right('invernada_deficit').flow "
            "+ flow_right('invernada_sin_deficit').flow "
            "+ flow_right('invernada_caudal_natural').flow "
            "= flow_right('invernada_embalsar').flow "
            "+ flow_right('invernada_no_embalsar').flow"
        )
        description_invernada = "La Invernada winter balance: inflows equal outflows"

        expression_pct_elec = (
            f"flow_right('maule_gasto_ordinario_elec').flow <= "
            f"{pct_elec / 100.0} * flow_right('maule_gasto_ordinario_elec').flow "
            f"+ {pct_elec / 100.0} * flow_right('maule_gasto_ordinario_riego').flow"
        )
        description_pct_elec = (
            f"Electric capped at {pct_elec}% of total ordinary reserve flow"
        )

        expression_pct_riego = (
            f"flow_right('maule_gasto_ordinario_riego').flow <= "
            f"{pct_riego_r / 100.0} * flow_right('maule_gasto_ordinario_elec').flow "
            f"+ {pct_riego_r / 100.0} * flow_right('maule_gasto_ordinario_riego').flow"
        )
        description_pct_riego = (
            f"Irrigation capped at {pct_riego_r}% of total ordinary reserve flow"
        )

        # --- District entities (pre-computed) ---
        district_flow_rights, district_constraints = self._compute_district_entities(
            irr_fmax_schedule
        )

        return {
            # Reservoir / zone thresholds
            "res_colbun": res_colbun,
            "v_zone_extraord": v_zone_extraord,
            "v_zone_normal": v_zone_normal,
            "v_reserva_extraord": v_extraord,
            # Flow limits
            "elec_day_max": elec_day_max,
            "riego_max": riego_max,
            # Schedules
            "irr_fmax_schedule": irr_fmax_schedule,
            "elec_ord_fmax": elec_ord_fmax,
            "res105_discharge": res105_discharge,
            # Costs / penalties
            "penalizador_1": cfg.get("penalizador_1", 1500),
            "costo_riego_ns_maule": cfg.get("costo_riego_ns_maule", 1000),
            "valor_riego": valor_riego,
            "costo_riego_ns_res105": cfg.get("costo_riego_ns_res105", 1000),
            "valor_riego_res105": valor_riego_res105,
            "costo_embalsar": costo_embalsar,
            "costo_no_embalsar": costo_no_embalsar,
            "costo_canelon": costo_canelon,
            # VolumeRight initial values and caps
            "v_gasto_elec_men_ini": cfg["v_gasto_elec_men_ini"],
            "gasto_elec_men_max": cfg["gasto_elec_men_max"],
            "v_gasto_elec_anu_ini": cfg["v_gasto_elec_anu_ini"],
            "v_der_elect_anu_max": cfg["v_der_elect_anu_max"],
            "v_gasto_riego_ini": cfg["v_gasto_riego_ini"],
            "v_der_riego_temp_max": cfg["v_der_riego_temp_max"],
            "v_comp_elec_ini": cfg["v_comp_elec_ini"],
            "v_comp_elec_max": cfg["v_comp_elec_max"],
            "v_gasto_rext_elec_ini": cfg.get("v_gasto_rext_elec_ini", 0.0),
            "v_gasto_rext_riego_ini": cfg.get("v_gasto_rext_riego_ini", 0.0),
            # La Invernada
            "central_invernada": cfg["central_invernada"],
            "v_econ_inver_ini": cfg["v_econ_inver_ini"],
            "qmax_invernada": cfg.get("qmax_invernada", 200),
            # UserConstraint expressions
            "expression_invernada": expression_invernada,
            "description_invernada": description_invernada,
            "expression_pct_elec": expression_pct_elec,
            "description_pct_elec": description_pct_elec,
            "expression_pct_riego": expression_pct_riego,
            "description_pct_riego": description_pct_riego,
            # District entities
            "district_flow_rights": district_flow_rights,
            "district_constraints": district_constraints,
        }

    def _compute_district_entities(
        self,
        irr_fmax_schedule: float | List[List[float]],
    ) -> tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
        """Pre-compute district FlowRight and UserConstraint entities.

        Returns:
            Tuple of (district_flow_rights, district_constraints).
        """
        cfg = self._cfg
        district_flow_rights: List[Dict[str, Any]] = []
        district_constraints: List[Dict[str, Any]] = []

        for district in cfg["districts"]:
            pct = district["percentage"]
            constraint_op = "<=" if district["has_slack"] else "="

            # Transform district name: Rie prefix → retiro_ prefix
            raw_name = district["name"]
            if raw_name.startswith("Rie"):
                fr_name = "retiro_" + raw_name[3:]
            else:
                fr_name = raw_name

            district_flow_rights.append(
                {
                    "name": fr_name,
                    "purpose": "irrigation",
                    "direction": -1,
                    "discharge": 0,
                    "fmax": irr_fmax_schedule,
                }
            )

            district_constraints.append(
                {
                    "name": f"dist_{fr_name}",
                    "expression": (
                        f"flow_right('{fr_name}').flow "
                        f"{constraint_op} "
                        f"{pct / 100.0} * flow_right('maule_gasto_normal_riego').flow"
                    ),
                    "description": f"{fr_name}: {pct}% of total irrigation",
                }
            )

        return district_flow_rights, district_constraints

    def _assign_uids(self) -> None:
        """Assign unique UIDs to all entities after template rendering."""
        for entity_list in [
            self.flow_rights,
            self.volume_rights,
            self.user_constraints,
        ]:
            for entity in entity_list:
                entity["uid"] = self._next_uid()

    def _build(self) -> None:
        """Build all rights entities from the parsed configuration.

        Renders the maule.tson template with pre-computed context values,
        then assigns unique UIDs to all entities.
        """
        context = self._prepare_context()
        entities = render_tson("maule.tson", context)

        self.flow_rights = entities.get("flow_right_array", [])
        self.volume_rights = entities.get("volume_right_array", [])
        self.user_constraints = entities.get("user_constraint_array", [])

        self._assign_uids()

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
        pampl_file = output_path / "maule.pampl"
        pampl_file.write_text(rendered, encoding="utf-8")

        return "maule.pampl"

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
