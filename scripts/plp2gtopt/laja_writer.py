# -*- coding: utf-8 -*-

"""Writer for Laja irrigation agreement entities.

Converts parsed LajaParser data into gtopt JSON entities:
FlowRight, VolumeRight, and UserConstraint.

The Laja convention (1958) divides Laguna del Laja volume into volume
zones, each with different allocation factors for irrigation, electric,
and mixed rights categories.

Key conversion: PLP volume zone formula
  Rights = Base + Sum_i(Factor_i * Zone_Volume_i)
is a piecewise-linear function of total volume, which maps directly to
the gtopt ``bound_rule`` segment format.

The JSON entity structure is defined in the ``laja.tson`` template file,
which uses @param@ m4-style syntax for parameter substitution.  Computed
values (bound_rule segments, schedules, costs) are pre-computed here and
passed as template context.

See also:
  gtopt_vs_plp_comparison.md -- detailed variable mapping
  plp_implementation.md -- PLP Fortran source reference
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

import jinja2

from plp2gtopt.template_engine import render_tson


# Hydrological year mapping: PLP uses Apr=1..Mar=12
# gtopt uses calendar months: january=1..december=12
# So PLP hydro month H maps to calendar month (H + 3) % 12, with 0 → 12
_HYDRO_TO_CALENDAR = [4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3]


def _zones_to_bound_rule_segments(
    base: float,
    factors: List[float],
    widths: List[float],
    vol_muerto: float = 0.0,
) -> List[Dict[str, float]]:
    """Convert PLP volume zone factors to bound_rule segments.

    PLP formula: Rights = base + Sum_i(factor_i * min(Vi, width_i))
    where Vi is the volume within zone i.

    This is piecewise-linear in total volume V:
      Zone 0: V in [vol_muerto, vol_muerto + width[0])
        rights(V) = base + factor[0] * (V - vol_muerto)
      Zone 1: V in [vol_muerto + width[0], vol_muerto + width[0] + width[1])
        rights(V) = base + factor[0]*width[0] + factor[1]*(V - vol_muerto - width[0])
      etc.

    Each bound_rule segment stores {volume, slope, constant} where
    rights(V) = constant + slope * V for V >= volume.

    Args:
        base: Base rights level (rights at vol_muerto)
        factors: Incremental factor per volume zone
        widths: Width of each volume zone [hm3]
        vol_muerto: Dead volume below which no extraction

    Returns:
        List of bound_rule segments sorted by volume breakpoint
    """
    segments: List[Dict[str, float]] = []
    cumulative_vol = vol_muerto
    cumulative_rights = base

    for factor, width in zip(factors, widths):
        # At volume = cumulative_vol, rights = cumulative_rights
        # and slope = factor
        # So: rights(V) = cumulative_rights + factor * (V - cumulative_vol)
        #                = (cumulative_rights - factor * cumulative_vol) + factor * V
        constant = cumulative_rights - factor * cumulative_vol
        segments.append(
            {
                "volume": cumulative_vol,
                "slope": factor,
                "constant": constant,
            }
        )
        # Advance to next zone boundary
        cumulative_rights += factor * width
        cumulative_vol += width

    # If no zones, just emit a flat segment
    if not segments:
        segments.append({"volume": vol_muerto, "slope": 0, "constant": base})

    return segments


class LajaWriter:
    """Emits FlowRight, VolumeRight, ReservoirSeepage, and UserConstraint
    entities for the Laja irrigation agreement.

    The JSON entity structure is defined in ``templates/laja.tson``.
    This class pre-computes all dynamic values (segments, schedules,
    costs) and passes them as template context parameters.

    Args:
        laja_config: Parsed configuration from LajaParser.config
        stage_parser: Parsed stage data (for month mapping)
        options: Conversion options dict
    """

    def __init__(
        self,
        laja_config: Dict[str, Any],
        stage_parser: Any = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        self._cfg = laja_config
        self._stage_parser = stage_parser
        self._options = options or {}
        self._uid_counter = 2000  # Start UIDs at 2000 to avoid collisions

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

    def _hydro_to_stage_schedule(self, hydro_monthly: List[float]) -> List[float]:
        """Convert 12-element hydrological-year array to per-stage schedule.

        PLP Laja uses hydrological year (Apr=index 0 .. Mar=index 11).
        Maps each stage's calendar month to the corresponding hydro index.
        """
        if self._stage_parser is None:
            return hydro_monthly

        stages = self._get_stages()
        schedule: List[float] = []
        for stage in stages:
            cal_month = stage.get("month", 1)  # calendar: 1=Jan..12=Dec
            # Calendar month → hydro index: Apr=0, May=1, ..., Mar=11
            hydro_idx = (cal_month - 4) % 12
            schedule.append(hydro_monthly[hydro_idx])
        return schedule

    def _to_stb_sched(
        self,
        values: List[float],
    ) -> float | List[List[List[float]]]:
        """Convert per-stage values to STBRealFieldSched format (3D).

        The 3D format is [scenario][stage][block].  Each stage value is
        replicated for every block in that stage (constant across blocks).
        """
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[[v] * nblocks for v in values]]

    def _to_tb_sched(
        self,
        values: List[float],
    ) -> float | List[List[float]]:
        """Convert per-stage values to TBRealFieldSched format (2D).

        The 2D format is [stage][block].  Each stage value is
        replicated across all blocks in that stage.
        """
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[v] * nblocks for v in values]

    def _prepare_context(self) -> Dict[str, Any]:
        """Prepare the template context with all pre-computed values.

        Returns a dict of parameters that the laja.tson template uses
        via @param@ substitution.
        """
        cfg = self._cfg

        central = cfg["central_laja"]
        vol_muerto = cfg["vol_muerto"]

        # --- Bound_rule segments for each rights category ---
        irr_segments = _zones_to_bound_rule_segments(
            cfg["irr_base"], cfg["irr_factors"], cfg["zone_widths"], vol_muerto
        )
        elec_segments = _zones_to_bound_rule_segments(
            cfg["elec_base"], cfg["elec_factors"], cfg["zone_widths"], vol_muerto
        )
        mixed_segments = _zones_to_bound_rule_segments(
            cfg["mixed_base"], cfg["mixed_factors"], cfg["zone_widths"], vol_muerto
        )

        # --- Monthly usage schedules (hydro year → stage) ---
        usage_irr = self._hydro_to_stage_schedule(cfg["monthly_usage_irr"])
        usage_elec = self._hydro_to_stage_schedule(cfg["monthly_usage_elec"])
        usage_mixed = self._hydro_to_stage_schedule(cfg["monthly_usage_mixed"])
        usage_antic = self._hydro_to_stage_schedule(cfg["monthly_usage_anticipated"])

        # fmax = qmax * monthly_usage_factor
        def _fmax(qmax: float, usage: List[float]) -> float | List[List[float]]:
            return self._to_tb_sched([qmax * u for u in usage])

        fmax_irr = _fmax(cfg["qmax_irr"], usage_irr)
        fmax_elec = _fmax(cfg["qmax_elec"], usage_elec)
        fmax_mixed = _fmax(cfg["qmax_mixed"], usage_mixed)
        fmax_antic = _fmax(cfg["qmax_anticipated"], usage_antic)

        # --- Monthly cost modulation ---
        def _monthly_cost(
            base_cost: float, monthly_factors: List[float]
        ) -> float | List[List[float]]:
            modulated = self._hydro_to_stage_schedule(monthly_factors)
            return self._to_tb_sched([base_cost * f for f in modulated])

        # Irrigation costs
        fail_cost_irr = _monthly_cost(cfg["cost_irr_ns"], cfg["monthly_cost_irr_ns"])
        use_value_irr = (
            _monthly_cost(cfg["cost_irr_uso"], cfg["monthly_cost_irr"])
            if cfg.get("cost_irr_uso", 0) > 0
            else None
        )

        # Electric costs
        fail_cost_elec = _monthly_cost(cfg["cost_elec_ns"], cfg["monthly_cost_elec"])
        use_value_elec = (
            _monthly_cost(cfg["cost_elec_uso"], cfg["monthly_cost_elec"])
            if cfg["cost_elec_uso"] > 0
            else None
        )

        # Mixed costs
        use_value_mixed = (
            _monthly_cost(cfg["cost_mixed"], cfg["monthly_cost_mixed"])
            if cfg["cost_mixed"] > 0
            else None
        )

        # Anticipated costs
        fail_cost_antic = _monthly_cost(
            cfg.get("cost_irr_ns", 0), cfg["monthly_cost_anticipated"]
        )

        # --- District flow rights (pre-computed) ---
        district_flow_rights = self._compute_district_flow_rights()

        # --- User constraint expressions ---
        expression_partition = (
            "flow_right('laja_q_turbinado').flow = "
            "flow_right('laja_der_riego').flow "
            "+ flow_right('laja_der_electrico').flow "
            "+ flow_right('laja_der_mixto').flow "
            "+ flow_right('laja_gasto_anticipado').flow"
        )
        description_partition = (
            "Flow partition: total generation equals sum of extractions"
        )

        return {
            # FlowRight: laja_q_turbinado
            "vol_max": cfg["vol_max"],
            # FlowRight: laja_der_riego
            "fmax_irr": fmax_irr,
            "fail_cost_irr": fail_cost_irr,
            "use_value_irr": use_value_irr,
            # FlowRight: laja_der_electrico
            "fmax_elec": fmax_elec,
            "fail_cost_elec": fail_cost_elec,
            "use_value_elec": use_value_elec,
            # FlowRight: laja_der_mixto
            "fmax_mixed": fmax_mixed,
            "use_value_mixed": use_value_mixed,
            # FlowRight: laja_gasto_anticipado
            "fmax_antic": fmax_antic,
            "fail_cost_antic": fail_cost_antic,
            # FlowRight: districts
            "district_flow_rights": district_flow_rights,
            # VolumeRight common
            "central": central,
            # VolumeRight: irrigation
            "ini_irr": cfg["ini_irr"],
            "max_irr": cfg["max_irr"],
            "irr_segments": irr_segments,
            # VolumeRight: electric
            "ini_elec": cfg["ini_elec"],
            "max_elec": cfg["max_elec"],
            "elec_segments": elec_segments,
            # VolumeRight: mixed
            "ini_mixed": cfg["ini_mixed"],
            "max_mixed": cfg["max_mixed"],
            "mixed_segments": mixed_segments,
            # VolumeRight: anticipated
            "ini_anticipated": cfg["ini_anticipated"],
            "max_anticipated": cfg["max_anticipated"],
            # VolumeRight: economy
            "ini_econ_endesa": cfg.get("ini_econ_endesa", 0),
            "ini_econ_reserve": cfg.get("ini_econ_reserve", 0),
            "ini_econ_polcura": cfg.get("ini_econ_polcura", 0),
            "saving_rate_econ": cfg.get("qmax_elec", 200),
            # UserConstraint: partition balance
            "expression_partition": expression_partition,
            "description_partition": description_partition,
        }

    def _compute_district_flow_rights(self) -> List[Dict[str, Any]]:
        """Pre-compute district withdrawal FlowRight entities.

        Returns a list of dicts ready for JSON serialization.
        Districts × categories, skipping zero-allocation entries.
        """
        cfg = self._cfg
        demands = {
            "1o_reg": cfg["demand_1o_reg"],
            "2o_reg": cfg["demand_2o_reg"],
            "emergencia": cfg["demand_emergencia"],
            "saltos": cfg["demand_saltos"],
        }
        seasonal_keys = {
            "1o_reg": "seasonal_1o_reg",
            "2o_reg": "seasonal_2o_reg",
            "emergencia": "seasonal_emergencia",
            "saltos": "seasonal_saltos",
        }
        pct_keys = {
            "1o_reg": "pct_1o_reg",
            "2o_reg": "pct_2o_reg",
            "emergencia": "pct_emergencia",
            "saltos": "pct_saltos",
        }

        result: List[Dict[str, Any]] = []
        for district in cfg["districts"]:
            for category, demand_base in demands.items():
                pct = district[pct_keys[category]]
                if pct <= 0 and demand_base <= 0:
                    continue

                seasonal = self._hydro_to_stage_schedule(cfg[seasonal_keys[category]])
                discharge_values = [demand_base * pct * s for s in seasonal]
                discharge_sched = self._to_stb_sched(discharge_values)

                fr_name = f"{district['name']}_{category}"
                fr_district: Dict[str, Any] = {
                    "name": fr_name,
                    "purpose": "irrigation",
                    "direction": -1,
                    "discharge": discharge_sched,
                    "fail_cost": cfg["cost_irr_ns"] * district["cost_factor"],
                }
                injection = district.get("injection")
                if injection:
                    fr_district["junction"] = injection
                result.append(fr_district)

        return result

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

        Renders the laja.tson template with pre-computed context values,
        then assigns unique UIDs to all entities.
        """
        context = self._prepare_context()
        entities = render_tson("laja.tson", context)

        self.flow_rights = entities.get("flow_right_array", [])
        self.volume_rights = entities.get("volume_right_array", [])
        self.user_constraints = entities.get("user_constraint_array", [])

        self._assign_uids()

    def generate_pampl(self, output_path: Path) -> str:
        """Render the Laja agreement PAMPL file from the Jinja2 template.

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
        template = env.get_template("laja_agreement.tampl")

        rendered = template.render(self._cfg)

        output_path = Path(output_path)
        output_path.mkdir(parents=True, exist_ok=True)
        pampl_file = output_path / "laja_agreement.pampl"
        pampl_file.write_text(rendered, encoding="utf-8")

        return "laja_agreement.pampl"

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
