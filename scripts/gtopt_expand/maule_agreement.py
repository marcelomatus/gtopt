# -*- coding: utf-8 -*-

"""Maule irrigation agreement → gtopt entities.

Reads a canonical ``maule.json`` agreement description (the Stage-1 product
of ``plp2gtopt`` or hand-authored) and produces:

* ``FlowRight`` entries (electric, irrigation, mixed, La Invernada
  branches, Resolution 105 ecological flow, district withdrawals).
* ``VolumeRight`` entries (rights buckets + economy trackers + La
  Invernada economy).
* ``UserConstraint`` entries (zone partition balances, district shares).
* A companion ``maule.pampl`` file containing the named sets and
  parameters.

Machicura-model variants
------------------------

The agreement supports two topology variants for Central Machicura.
Selection follows a three-level priority order so every downstream
consumer — tests, hand-authored JSON, and the full pipeline — gets a
deterministic answer without a CLI flag:

1. **Explicit override** — if ``cfg["machicura_model"]`` is set (either
   to ``"pasada"``/``"run-of-river"`` or ``"embalse"``/``"reservoir"``),
   that value wins.  Used by hand-authored fixtures and tests that want
   to pin the variant regardless of the surrounding gtopt case.

2. **Reservoir auto-detection** — otherwise, when
   ``options["reservoir_names"]`` contains the resolved
   ``junction_retiro`` (by default ``"MACHICURA"``, or the
   ``downstream`` field of the PLP COLBUN extraction entry when
   present in ``cfg["extrac_entries"]``), the ``embalse`` variant is
   selected.  This lets ``gtopt_expand`` detect a MACHICURA
   reservoir in the actual gtopt case — e.g. the planning JSON that
   plp2gtopt emits next to ``maule.json`` when ``--ror-as-reservoirs``
   promoted MACHICURA to a daily-cycle reservoir.  The CLI populates
   ``reservoir_names`` automatically from a sibling planning file.

3. **Default** — otherwise ``pasada`` (PLP-compatible, no downstream
   reservoir).  Safe fallback for cases that carry no planning JSON
   (synthetic fixtures, constraint-only tests, documentation snippets).

The two variants are:

* ``"pasada"`` (English synonym ``"run-of-river"``): historical
  PLP-compatible variant.  Machicura is a pass-through junction
  (pasada semantics, no volumetric balance of its own) and the
  retiros of riego and Res 105 are implicitly anchored at the Colbun
  reservoir.  Uses ``templates/maule.tson`` and ``templates/maule.tampl``.

* ``"embalse"`` (English synonym ``"reservoir"``): full physical variant.
  Machicura is modelled as a daily-cycle reservoir aguas abajo of Colbun
  and the three riego/Res 105 FlowRights carry an explicit ``junction``
  reference to it.  Uses ``templates/maule_machicura.tson`` and
  ``templates/maule_machicura.tampl``.

The ``embalse`` variant additionally requires:

* ``junction_retiro``: name of the downstream junction (auto-resolved from
  ``extrac_entries`` if present — matches the ``downstream`` field of the
  PLP COLBUN extraction entry — or defaults to ``"MACHICURA"``).
* ``machicura_vmax``: useful regulation volume [hm³] (default 12.0).
* ``machicura_capacity``: installed capacity [MW] (default 95.0).
* ``machicura_qmax``: turbine max flow [m³/s] (default 280.0).
* ``machicura_prod_factor``: production factor [MW/(m³/s)] (default 0.327).

This is the canonical Stage-2 transform in the irrigation pipeline (see
``project_irrigation_pipeline.md``).  Stage 1 (``plp2gtopt``) converts
``plpmaulen.dat`` to ``maule.json``; Stage 3 (``gtopt``) consumes the
entities the same way it consumes any other JSON/PAMPL input.

``maule.json`` schema
---------------------

A flat dict mirroring the field set produced by ``MauleParser`` (see
``scripts/plp2gtopt/maule_parser.py``).  Required keys include
``central_colbun``, ``central_invernada``, the three-zone volume
thresholds (``v_reserva_extraord``, ``v_reserva_ordinaria``), monthly
modulation arrays (``mod_elec_reserva``, ``pct_riego_mensual``,
``caudal_res105``), penalty/cost fields (``costo_embalsar``,
``costo_no_embalsar``, ``costo_canelon``, ``penalizador_1``,
``costo_riego_ns_*``), the initial bucket levels (``v_*_ini``) and the
``districts`` list.  See ``MauleParser`` for the authoritative definition.
"""

from __future__ import annotations

from typing import Any

from gtopt_expand._base import _RightsAgreementBase


class MauleAgreement(_RightsAgreementBase):
    """Stage-2 transform: maule.json → FlowRight/VolumeRight/UserConstraint.

    The JSON entity structure is defined in ``templates/maule.tson``.
    This class pre-computes all dynamic values (schedules, costs, zone
    thresholds) and passes them as template context parameters.

    Args:
        maule_config: Canonical Maule configuration dict (see module
            docstring for the schema).
        stage_parser: Optional stage-parser-shaped object whose ``get_all()``
            yields per-stage dicts with ``number`` and ``month``.  When
            omitted, monthly schedules stay in raw calendar form.
        options: Conversion options dict (e.g. ``last_stage``,
            ``blocks_per_stage``).
    """

    _ARTIFACT = "maule"
    _UID_START = 1000  # avoid collisions with Laja (2000..)

    #: supported ``machicura_model`` values → template stem
    _VARIANT_TEMPLATES: dict[str, str] = {
        "pasada": "maule",
        "embalse": "maule_machicura",
    }

    #: English synonyms accepted for ``machicura_model`` — normalized to the
    #: canonical Spanish keys of ``_VARIANT_TEMPLATES``.
    _VARIANT_SYNONYMS: dict[str, str] = {
        "pasada": "pasada",
        "run-of-river": "pasada",
        "run_of_river": "pasada",
        "embalse": "embalse",
        "reservoir": "embalse",
    }

    def __init__(
        self,
        maule_config: dict[str, Any],
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
    ):
        super().__init__(maule_config, stage_parser=stage_parser, options=options)

    # ----------------------------------------------------------- variant
    def _machicura_model(self) -> str:
        """Return the active Machicura topology variant.

        Resolution order (first match wins):

        1. ``cfg["machicura_model"]`` — explicit per-case override,
           accepted as either the canonical Spanish keys
           (``pasada``/``embalse``) or the English synonyms
           (``run-of-river``/``reservoir``) via ``_VARIANT_SYNONYMS``.
           Used mainly by hand-authored fixtures and tests.

        2. ``options["reservoir_names"]`` — set/iterable of reservoir
           names coming from the companion gtopt/planning case.  When
           the resolved ``junction_retiro`` (``_resolve_junction_retiro``)
           appears in that set, MACHICURA is physically a daily-cycle
           reservoir in the case, so the ``embalse`` variant is
           selected automatically.  This is how the CLI detects the
           MACHICURA promotion performed by
           ``plp2gtopt --ror-as-reservoirs`` without any explicit flag.

        3. Default ``pasada`` — safe fallback when no information is
           available (synthetic fixtures, constraint-only tests).
        """
        raw = self._cfg.get("machicura_model")
        if raw is not None:
            key = str(raw).strip().lower()
            if key not in self._VARIANT_SYNONYMS:
                raise ValueError(
                    f"unknown machicura_model {raw!r}: "
                    f"expected one of {sorted(self._VARIANT_SYNONYMS)}"
                )
            return self._VARIANT_SYNONYMS[key]

        reservoir_names = self._options.get("reservoir_names")
        if reservoir_names:
            junction_retiro = self._resolve_junction_retiro()
            # Case-insensitive membership check so callers can supply
            # names straight from the planning JSON without worrying
            # about the Spanish/English mixed casing used by PLP.
            lowered = {str(n).strip().lower() for n in reservoir_names}
            if junction_retiro.strip().lower() in lowered:
                return "embalse"

        return "pasada"

    def _template_name(self) -> str:  # noqa: D401
        return self._VARIANT_TEMPLATES[self._machicura_model()]

    def _resolve_junction_retiro(self) -> str:
        """Resolve the downstream junction name for the machicura variant.

        Looks up ``cfg["extrac_entries"]`` (Stage-1 output of
        ``plpextrac.dat``) for a COLBUN entry and returns its ``downstream``
        field.  Falls back to ``cfg["junction_retiro"]`` or ``"MACHICURA"``
        so hand-authored fixtures can override the PLP default.
        """
        cfg = self._cfg
        override = cfg.get("junction_retiro")
        if override:
            return str(override)

        extrac_entries = cfg.get("extrac_entries") or []
        central_colbun = str(cfg.get("central_colbun", "")).upper()
        for entry in extrac_entries:
            name = str(entry.get("name", "")).upper()
            if name in (central_colbun, "COLBUN"):
                downstream = entry.get("downstream")
                if downstream:
                    return str(downstream)

        return "MACHICURA"

    def _monthly_schedule(self, monthly_values: list[float]) -> list[float]:
        """Convert 12-element monthly array to per-stage values.

        Maps each stage's month (from stage_parser) to the corresponding
        monthly value. If no stage_parser is available, returns the raw
        12-element array (the user must ensure stage-month alignment).
        """
        if self._stage_parser is None:
            return monthly_values

        stages = self._get_stages()
        schedule: list[float] = []
        for stage in stages:
            month = stage.get("month", 1)
            # PLP months are 1-indexed, array is 0-indexed
            idx = (month - 1) % 12
            schedule.append(monthly_values[idx])
        return schedule

    def _monthly_fmax_schedule(
        self,
        monthly_pcts: list[float],
        base_flow: float,
    ) -> float | list[list[float]]:
        """Build per-stage fmax from monthly percentage × base flow.

        Returns a scalar if all stages have the same value, otherwise
        a 2D array compatible with OptTBRealFieldSched (stage × block).
        """
        monthly_flows = [pct / 100.0 * base_flow for pct in monthly_pcts]
        schedule = self._monthly_schedule(monthly_flows)
        return self._to_tb_sched(schedule)

    def _prepare_context(self) -> dict[str, Any]:
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

        # --- Machicura variant parameters ---
        # These are always computed (cheap) so tests and debug dumps can
        # inspect them; the machicura template uses `junction_retiro`
        # unconditionally, the plp template ignores them.
        junction_retiro = self._resolve_junction_retiro()
        machicura_vmax = cfg.get("machicura_vmax", 12.0)
        machicura_capacity = cfg.get("machicura_capacity", 95.0)
        machicura_qmax = cfg.get("machicura_qmax", 280.0)
        machicura_prod_factor = cfg.get("machicura_prod_factor", 0.327)

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
            # Soft-constraint penalty for the invernada_balance UserConstraint
            # ($/m³).  Rendered as `penalty` with `penalty_class = "hydro_flow"`
            # so the LP assembly converts it to $/(m³/s) per block via
            # `× duration[h] × 3600`, mirroring FlowRight.fail_cost.  Defaults
            # to the global `hydro_fail_cost` so the soft relaxation composes
            # with element-level pricing without a separate tuning knob.
            "penalty_invernada": cfg.get(
                "penalty_invernada", cfg.get("hydro_fail_cost", 10.0)
            ),
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
            # Machicura variant parameters (see class docstring)
            "junction_retiro": junction_retiro,
            "machicura_vmax": machicura_vmax,
            "machicura_capacity": machicura_capacity,
            "machicura_qmax": machicura_qmax,
            "machicura_prod_factor": machicura_prod_factor,
        }

    def _compute_district_entities(
        self,
        irr_fmax_schedule: float | list[list[float]],
    ) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
        """Pre-compute district FlowRight and UserConstraint entities.

        Returns:
            Tuple of (district_flow_rights, district_constraints).
        """
        cfg = self._cfg
        district_flow_rights: list[dict[str, Any]] = []
        district_constraints: list[dict[str, Any]] = []

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
