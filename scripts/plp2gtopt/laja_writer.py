# -*- coding: utf-8 -*-

"""Writer for Laja irrigation agreement entities.

Converts parsed LajaParser data into gtopt JSON entities:
FlowRight, VolumeRight, RightJunction, and UserConstraint.

The Laja convention (1958) divides Laguna del Laja volume into volume
zones, each with different allocation factors for irrigation, electric,
and mixed rights categories.

Key conversion: PLP volume zone formula
  Rights = Base + Sum_i(Factor_i * Zone_Volume_i)
is a piecewise-linear function of total volume, which maps directly to
the gtopt ``bound_rule`` segment format.

See also:
  gtopt_vs_plp_comparison.md -- detailed variable mapping
  plp_implementation.md -- PLP Fortran source reference
"""

from typing import Any, Dict, List, Optional


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
    """Emits FlowRight, VolumeRight, RightJunction, and UserConstraint
    entities for the Laja irrigation agreement.

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
        self.right_junctions: List[Dict[str, Any]] = []
        self.user_constraints: List[Dict[str, Any]] = []

        self._build()

    def _next_uid(self) -> int:
        """Generate a unique UID for rights entities."""
        uid = self._uid_counter
        self._uid_counter += 1
        return uid

    def _hydro_to_stage_schedule(self, hydro_monthly: List[float]) -> List[float]:
        """Convert 12-element hydrological-year array to per-stage schedule.

        PLP Laja uses hydrological year (Apr=index 0 .. Mar=index 11).
        Maps each stage's calendar month to the corresponding hydro index.
        """
        if self._stage_parser is None:
            return hydro_monthly

        stages = self._stage_parser.get_all()
        schedule: List[float] = []
        for stage in stages:
            cal_month = stage.get("month", 1)  # calendar: 1=Jan..12=Dec
            # Calendar month → hydro index: Apr=0, May=1, ..., Mar=11
            hydro_idx = (cal_month - 4) % 12
            schedule.append(hydro_monthly[hydro_idx])
        return schedule

    @staticmethod
    def _to_stb_sched(
        values: List[float],
    ) -> float | List[List[List[float]]]:
        """Convert per-stage values to STBRealFieldSched format (3D)."""
        if len(set(values)) == 1:
            return values[0]
        return [[[v] for v in values]]

    @staticmethod
    def _to_tb_sched(
        values: List[float],
    ) -> float | List[List[float]]:
        """Convert per-stage values to TBRealFieldSched format (2D)."""
        if len(set(values)) == 1:
            return values[0]
        return [[v] for v in values]

    def _build(self) -> None:
        """Build all rights entities from the parsed configuration."""
        cfg = self._cfg

        central = cfg["central_laja"]  # e.g. "ELTORO"
        vol_muerto = cfg["vol_muerto"]

        # --- Build bound_rule segments for each rights category ---
        irr_segments = _zones_to_bound_rule_segments(
            cfg["irr_base"], cfg["irr_factors"], cfg["zone_widths"], vol_muerto
        )
        elec_segments = _zones_to_bound_rule_segments(
            cfg["elec_base"], cfg["elec_factors"], cfg["zone_widths"], vol_muerto
        )
        mixed_segments = _zones_to_bound_rule_segments(
            cfg["mixed_base"], cfg["mixed_factors"], cfg["zone_widths"], vol_muerto
        )

        # --- RightJunction: El Toro flow partition ---
        # -qgt + qdr + qde + qdm + qga = 0
        rj_partition_uid = self._next_uid()
        self.right_junctions.append(
            {
                "uid": rj_partition_uid,
                "name": "laja_partition",
                "drain": False,
            }
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

        # --- FlowRight: Total generation (qgt) ---
        # direction=+1 (supply into the partition balance)
        fr_total_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_total_uid,
                "name": "laja_total_gen",
                "purpose": "generation",
                "right_junction": "laja_partition",
                "direction": 1,
                "discharge": 0,
                "fmax": cfg["vol_max"],  # effectively unbounded
                "use_average": True,
            }
        )

        # --- FlowRight: Irrigation rights (qdr) ---
        # Flow cap is qmax × monthly_usage (m³/s).  The volume-dependent
        # annual rights quota (hm³) is on the VolumeRight emax, not here.
        fr_irr_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_irr_uid,
                "name": "laja_irr_rights",
                "purpose": "irrigation",
                "right_junction": "laja_partition",
                "direction": -1,
                "discharge": 0,
                "fmax": fmax_irr,
                "use_average": True,
                "fail_cost": cfg["cost_irr_ns"],
            }
        )

        # --- FlowRight: Electrical rights (qde) ---
        fr_elec_uid = self._next_uid()
        fr_elec: Dict[str, Any] = {
            "uid": fr_elec_uid,
            "name": "laja_elec_rights",
            "purpose": "generation",
            "right_junction": "laja_partition",
            "direction": -1,
            "discharge": 0,
            "fmax": fmax_elec,
            "use_average": True,
            "fail_cost": cfg["cost_elec_ns"],
        }
        if cfg["cost_elec_uso"] > 0:
            fr_elec["use_cost"] = cfg["cost_elec_uso"]
        self.flow_rights.append(fr_elec)

        # --- FlowRight: Mixed rights (qdm) ---
        fr_mixed_uid = self._next_uid()
        fr_mixed: Dict[str, Any] = {
            "uid": fr_mixed_uid,
            "name": "laja_mixed_rights",
            "purpose": "mixed",
            "right_junction": "laja_partition",
            "direction": -1,
            "discharge": 0,
            "fmax": fmax_mixed,
            "use_average": True,
        }
        if cfg["cost_mixed"] > 0:
            fr_mixed["use_cost"] = cfg["cost_mixed"]
        self.flow_rights.append(fr_mixed)

        # --- FlowRight: Anticipated discharge (qga) ---
        fr_antic_uid = self._next_uid()
        self.flow_rights.append(
            {
                "uid": fr_antic_uid,
                "name": "laja_anticipated",
                "purpose": "anticipated",
                "right_junction": "laja_partition",
                "direction": -1,
                "discharge": 0,
                "fmax": fmax_antic,
                "use_average": True,
            }
        )

        # --- VolumeRight: Irrigation volume accumulator (IVDRF) ---
        # bound_rule dynamically caps extraction rate based on reservoir
        # volume (PLP DerRiego formula: base + Σ factor_i × zone_volume_i).
        vr_irr_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_irr_uid,
                "name": "laja_vol_irr",
                "purpose": "irrigation",
                "reservoir": central,
                "eini": cfg["ini_irr"],
                "emax": cfg["max_irr"],
                "use_state_variable": True,
                "reset_month": "april",
                "bound_rule": {
                    "reservoir": central,
                    "segments": irr_segments,
                    "cap": cfg["max_irr"],
                },
            }
        )

        # --- VolumeRight: Electrical volume accumulator (IVDEF) ---
        vr_elec_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_elec_uid,
                "name": "laja_vol_elec",
                "purpose": "generation",
                "reservoir": central,
                "eini": cfg["ini_elec"],
                "emax": cfg["max_elec"],
                "use_state_variable": True,
                "reset_month": "april",
                "bound_rule": {
                    "reservoir": central,
                    "segments": elec_segments,
                    "cap": cfg["max_elec"],
                },
            }
        )

        # --- VolumeRight: Mixed volume accumulator (IVDMF) ---
        vr_mixed_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_mixed_uid,
                "name": "laja_vol_mixed",
                "purpose": "mixed",
                "reservoir": central,
                "eini": cfg["ini_mixed"],
                "emax": cfg["max_mixed"],
                "use_state_variable": True,
                "reset_month": "april",
                "bound_rule": {
                    "reservoir": central,
                    "segments": mixed_segments,
                    "cap": cfg["max_mixed"],
                },
            }
        )

        # --- VolumeRight: Anticipated volume accumulator (IVGAF) ---
        vr_antic_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_antic_uid,
                "name": "laja_vol_anticipated",
                "purpose": "anticipated",
                "reservoir": central,
                "eini": cfg["ini_anticipated"],
                "emax": cfg["max_anticipated"],
                "use_state_variable": True,
                "reset_month": "april",
                "bound_rule": {
                    "reservoir": central,
                    "segments": irr_segments,  # same zones as irrigation
                    "cap": cfg["max_anticipated"],
                },
            }
        )

        # --- VolumeRight: ENDESA economy accumulator (IVESF) ---
        # Tracks unused extraction rights carried forward (no annual reset)
        vr_econ_endesa_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_econ_endesa_uid,
                "name": "laja_vol_econ_endesa",
                "purpose": "economy",
                "reservoir": central,
                "eini": 0,
                "use_state_variable": True,
            }
        )

        # --- VolumeRight: Reserve economy accumulator (IVERF) ---
        vr_econ_reserve_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_econ_reserve_uid,
                "name": "laja_vol_econ_reserve",
                "purpose": "economy",
                "reservoir": central,
                "eini": 0,
                "use_state_variable": True,
            }
        )

        # --- VolumeRight: Alto Polcura economy accumulator (IVAPF) ---
        vr_econ_polcura_uid = self._next_uid()
        self.volume_rights.append(
            {
                "uid": vr_econ_polcura_uid,
                "name": "laja_vol_econ_polcura",
                "purpose": "economy",
                "reservoir": central,
                "eini": 0,
                "use_state_variable": True,
            }
        )

        # --- FlowRight: Withdrawal districts ---
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

        for district in cfg["districts"]:
            for category, demand_base in demands.items():
                pct = district[pct_keys[category]]
                if pct <= 0 and demand_base <= 0:
                    continue  # Skip zero-allocation categories

                seasonal = self._hydro_to_stage_schedule(cfg[seasonal_keys[category]])
                # discharge = base_demand * percentage * seasonal_factor
                discharge_values = [demand_base * pct * s for s in seasonal]
                discharge_sched = self._to_stb_sched(discharge_values)

                d_uid = self._next_uid()
                fr_name = f"{district['name']}_{category}"
                self.flow_rights.append(
                    {
                        "uid": d_uid,
                        "name": fr_name,
                        "purpose": "irrigation",
                        "direction": -1,
                        "discharge": discharge_sched,
                        "fail_cost": cfg["cost_irr_ns"] * district["cost_factor"],
                    }
                )

    def to_json_dict(self) -> Dict[str, List[Dict[str, Any]]]:
        """Return all entities as a dict of arrays for system JSON."""
        result: Dict[str, List[Dict[str, Any]]] = {}
        if self.right_junctions:
            result["right_junction_array"] = self.right_junctions
        if self.flow_rights:
            result["flow_right_array"] = self.flow_rights
        if self.volume_rights:
            result["volume_right_array"] = self.volume_rights
        if self.user_constraints:
            result["user_constraint_array"] = self.user_constraints
        return result
