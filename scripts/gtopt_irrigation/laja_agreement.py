# -*- coding: utf-8 -*-

"""Laja irrigation agreement → gtopt entities.

Reads a canonical ``laja.json`` agreement description (the Stage-1 product
of ``plp2gtopt`` or hand-authored) and produces:

* ``FlowRight`` entries (irrigation, electric, mixed, anticipated, district
  withdrawals).
* ``VolumeRight`` entries (rights buckets + economy trackers).
* ``UserConstraint`` entries (flow partition balance).
* A companion ``laja.pampl`` file containing the named sets and parameters.

This is the canonical Stage-2 transform in the irrigation pipeline (see
``project_irrigation_pipeline.md``).  Stage 1 (``plp2gtopt``) converts
``plplajam.dat`` to ``laja.json``; Stage 3 (``gtopt``) consumes the
entities the same way it consumes any other JSON/PAMPL input.

``laja.json`` schema
--------------------

A flat dict mirroring the field set produced by ``LajaParser`` (see
``scripts/plp2gtopt/laja_parser.py``).  Required keys:

* ``central_laja`` (str), ``vol_max`` (float), ``vol_muerto`` (float).
* ``zone_widths`` (list[float]) — width of each volume zone.
* ``irr_base`` / ``elec_base`` / ``mixed_base`` (float) and matching
  ``*_factors`` (list[float]) — one factor per zone.
* ``max_irr`` / ``max_elec`` / ``max_mixed`` / ``max_anticipated`` (float).
* ``qmax_*`` flow caps (m³/s) for each rights category.
* ``cost_irr_ns`` / ``cost_irr_uso`` / ``cost_elec_ns`` /
  ``cost_elec_uso`` / ``cost_mixed`` (float).
* ``monthly_cost_*`` and ``monthly_usage_*`` arrays (12 floats, hydro
  year Apr-Mar) for each category.
* ``ini_*`` initial bucket levels for each category and economy tracker.
* ``districts`` (list[dict]) with ``name``, ``injection``, ``cost_factor``,
  and ``pct_*`` shares.
* ``demand_*`` and ``seasonal_*`` arrays for the four irrigation classes.

Stages can be supplied via a ``stage_parser``-shaped object whose
``get_all()`` returns dicts containing ``number`` and ``month``; if no
stage_parser is given the schedules stay in raw hydro-year form.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jinja2

from gtopt_irrigation._template_engine import _TEMPLATE_DIR, render_tson


# Hydrological year mapping: PLP uses Apr=1..Mar=12
# gtopt uses calendar months: january=1..december=12
# So PLP hydro month H maps to calendar month (H + 3) % 12, with 0 → 12
_HYDRO_TO_CALENDAR = [4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3]


def _zones_to_bound_rule_segments(
    base: float,
    factors: list[float],
    widths: list[float],
    vol_muerto: float = 0.0,
) -> list[dict[str, float]]:
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
    segments: list[dict[str, float]] = []
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


class LajaAgreement:
    """Stage-2 transform: laja.json → FlowRight/VolumeRight/UserConstraint.

    The JSON entity structure is defined in ``templates/laja.tson``.
    This class pre-computes all dynamic values (segments, schedules,
    costs) and passes them as template context parameters.

    Args:
        laja_config: Canonical Laja configuration dict (see module docstring
            for the schema).
        stage_parser: Optional stage-parser-shaped object whose ``get_all()``
            yields per-stage dicts with ``number`` and ``month``.  When
            omitted, monthly schedules stay in raw hydro-year form.
        options: Conversion options dict (e.g. ``last_stage``,
            ``blocks_per_stage``).
    """

    def __init__(
        self,
        laja_config: dict[str, Any],
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
    ):
        self._cfg = laja_config
        self._stage_parser = stage_parser
        self._options = options or {}
        self._uid_counter = 2000  # Start UIDs at 2000 to avoid collisions

        self.flow_rights: list[dict[str, Any]] = []
        self.volume_rights: list[dict[str, Any]] = []
        self.user_constraints: list[dict[str, Any]] = []

        self._build()

    @classmethod
    def from_json(
        cls,
        json_path: Path | str,
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
    ) -> LajaAgreement:
        """Load a canonical ``laja.json`` and construct an agreement.

        Args:
            json_path: Path to a ``laja.json`` file.
            stage_parser: Optional stage-parser-shaped object (see ``__init__``).
            options: Conversion options dict.
        """
        with open(json_path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
        return cls(cfg, stage_parser=stage_parser, options=options)

    def _next_uid(self) -> int:
        """Generate a unique UID for rights entities."""
        uid = self._uid_counter
        self._uid_counter += 1
        return uid

    def _get_stages(self) -> list[dict[str, Any]]:
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

    def _hydro_to_stage_schedule(self, hydro_monthly: list[float]) -> list[float]:
        """Convert 12-element hydrological-year array to per-stage schedule.

        PLP Laja uses hydrological year (Apr=index 0 .. Mar=index 11).
        Maps each stage's calendar month to the corresponding hydro index.
        """
        if self._stage_parser is None:
            return hydro_monthly

        stages = self._get_stages()
        schedule: list[float] = []
        for stage in stages:
            cal_month = stage.get("month", 1)  # calendar: 1=Jan..12=Dec
            # Calendar month → hydro index: Apr=0, May=1, ..., Mar=11
            hydro_idx = (cal_month - 4) % 12
            schedule.append(hydro_monthly[hydro_idx])
        return schedule

    def _to_stb_sched(
        self,
        values: list[float],
    ) -> float | list[list[list[float]]]:
        """Convert per-stage values to STBRealFieldSched format (3D).

        The 3D format is [scenario][stage][block].  Each stage value is
        replicated for every block in that stage (constant across blocks).
        Returns scalar 0.0 on empty input, or the single value if all
        stages match.
        """
        if not values:
            return 0.0
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[[v] * nblocks for v in values]]

    def _to_tb_sched(
        self,
        values: list[float],
    ) -> float | list[list[float]]:
        """Convert per-stage values to TBRealFieldSched format (2D).

        The 2D format is [stage][block].  Each stage value is
        replicated across all blocks in that stage.
        Returns scalar 0.0 on empty input, or the single value if all
        stages match.
        """
        if not values:
            return 0.0
        if len(set(values)) == 1:
            return values[0]
        nblocks = self._options.get("blocks_per_stage", 1)
        return [[v] * nblocks for v in values]

    def _fmax_schedule(
        self,
        qmax: float,
        usage: list[float],
    ) -> float | list[list[float]]:
        """Return a per-stage fmax schedule = ``qmax * usage[stage]``."""
        return self._to_tb_sched([qmax * u for u in usage])

    def _monthly_cost_schedule(
        self,
        base_cost: float,
        monthly_factors: list[float],
    ) -> float | list[list[float]]:
        """Return a per-stage cost schedule modulated by a hydro-year factor."""
        modulated = self._hydro_to_stage_schedule(monthly_factors)
        return self._to_tb_sched([base_cost * f for f in modulated])

    def _prepare_context(self) -> dict[str, Any]:
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
        fmax_irr = self._fmax_schedule(cfg["qmax_irr"], usage_irr)
        fmax_elec = self._fmax_schedule(cfg["qmax_elec"], usage_elec)
        fmax_mixed = self._fmax_schedule(cfg["qmax_mixed"], usage_mixed)
        fmax_antic = self._fmax_schedule(cfg["qmax_anticipated"], usage_antic)

        # --- Monthly cost modulation ---
        # Irrigation costs
        fail_cost_irr = self._monthly_cost_schedule(
            cfg["cost_irr_ns"], cfg["monthly_cost_irr_ns"]
        )
        use_value_irr = (
            self._monthly_cost_schedule(cfg["cost_irr_uso"], cfg["monthly_cost_irr"])
            if cfg.get("cost_irr_uso", 0) > 0
            else None
        )

        # Electric costs
        fail_cost_elec = self._monthly_cost_schedule(
            cfg["cost_elec_ns"], cfg["monthly_cost_elec"]
        )
        use_value_elec = (
            self._monthly_cost_schedule(cfg["cost_elec_uso"], cfg["monthly_cost_elec"])
            if cfg["cost_elec_uso"] > 0
            else None
        )

        # Mixed costs
        use_value_mixed = (
            self._monthly_cost_schedule(cfg["cost_mixed"], cfg["monthly_cost_mixed"])
            if cfg["cost_mixed"] > 0
            else None
        )

        # Anticipated costs
        fail_cost_antic = self._monthly_cost_schedule(
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

    def _compute_district_flow_rights(self) -> list[dict[str, Any]]:
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

        result: list[dict[str, Any]] = []
        for district in cfg["districts"]:
            for category, demand_base in demands.items():
                pct = district[pct_keys[category]]
                if pct <= 0 and demand_base <= 0:
                    continue

                seasonal = self._hydro_to_stage_schedule(cfg[seasonal_keys[category]])
                discharge_values = [demand_base * pct * s for s in seasonal]
                discharge_sched = self._to_stb_sched(discharge_values)

                fr_name = f"{district['name']}_{category}"
                fr_district: dict[str, Any] = {
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
        env = jinja2.Environment(
            loader=jinja2.FileSystemLoader(str(_TEMPLATE_DIR)),
            keep_trailing_newline=True,
            undefined=jinja2.StrictUndefined,
        )
        template = env.get_template("laja.tampl")

        rendered = template.render(self._cfg)

        output_path = Path(output_path)
        output_path.mkdir(parents=True, exist_ok=True)
        pampl_file = output_path / "laja.pampl"
        pampl_file.write_text(rendered, encoding="utf-8")

        return "laja.pampl"

    def to_json_dict(
        self,
        output_dir: Path | None = None,
    ) -> dict[str, Any]:
        """Return all entities as a dict of arrays for system JSON.

        Args:
            output_dir: If provided, generates a .pampl file in this
                directory and sets ``user_constraint_file`` instead of
                embedding constraints in ``user_constraint_array``.
        """
        result: dict[str, Any] = {}
        if self.flow_rights:
            result["flow_right_array"] = self.flow_rights
        if self.volume_rights:
            result["volume_right_array"] = self.volume_rights
        if self.user_constraints:
            if output_dir is not None:
                result["user_constraint_file"] = self.generate_pampl(output_dir)
            else:
                result["user_constraint_array"] = self.user_constraints
        return result
