# -*- coding: utf-8 -*-

"""Hydro domain mixin for :class:`plp2gtopt.gtopt_writer.GTOptWriter`.

Holds RoR / afluent / junction / water-rights / LNG / pumped-storage /
flow-turbine / pmin-flowright handling, plus the helper methods used
to merge ``gtopt_expand`` entity arrays back into ``planning["system"]``.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Dict, Mapping

from gtopt_shared.water_values import (
    WaterValueResolver,
    default_water_fail_value,
)

from .aflce_writer import AflceWriter
from .junction_writer import JunctionWriter

_logger = logging.getLogger(__name__)


class HydroMixin:
    """Hydro / water-rights / LNG / pumped-storage processing."""

    parser: Any
    planning: Dict[str, Dict[str, Any]]

    def _build_cut_water_values(self) -> Dict[str, float]:
        """Return the per-reservoir boundary-cut water-value dict.

        Derived from PLP's boundary cuts (plpplem2.dat) as the cut
        **lower-bound** water value across all parsed cuts (see
        :meth:`planos_parser.PlanosParser.lower_bound_water_value_by_reservoir`
        and :func:`gtopt_shared.water_values.cut_lower_bound`).  Returns
        an empty dict when no boundary cuts are available, in which case
        the resolver keeps its auto ``ANCHOR × lost_pf`` estimate for
        every reservoir.

        The value lives in raw ``$/hm³`` (after FEscala scaling).  It is
        the per-reservoir **OVERWRITE** for ``Reservoir.efin_cost`` (the
        2026-06-16 decision replaced the legacy ``min(auto, cap)`` cap
        with a direct overwrite — see
        :meth:`WaterValueResolver.efin_cost_for`).

        Present-value adjustment
        ------------------------
        PLP's boundary cuts encode ``GradX_i = ∂E[Z*]/∂v_i`` where
        ``E[Z*]`` is the **discounted** future cost (PLP's LP objective
        applies the per-stage ``discount_factor = 1/FactTasa`` directly
        to every coefficient before the dual is taken).  The auto
        lost-production-factor surface is in **un-discounted** $/hm³ (its
        ``gcost`` inputs are stage-invariant marginal $/MWh, with no
        boundary-stage discount baked in).  To keep the overwrite in the
        same accounting frame as reservoirs that fall back to the auto
        estimate, un-discount the cuts back to the "boundary face-value"
        frame by **dividing** every per-reservoir value by the last
        stage's ``discount_factor``.  Since ``discount_factor < 1`` at
        the boundary on multi-year horizons, this raises the value —
        which is the correct direction: the LP's stored cut coefficients
        are smaller than their boundary face value because PLP already
        multiplied them by the discount on disk.
        """
        planos = self.parser.parsed_data.get("planos_parser")
        if planos is None:
            return {}
        # Per-reservoir cut lower-bound water value, in raw `$/hm³`
        # (after FEscala scaling — that conversion happens inside
        # `lower_bound_water_value_by_reservoir` when
        # `apply_fescala=True`).  We DO NOT divide by N here because
        # `efin_cost` is the per-hm³ marginal cost added to the LP
        # objective for the END-of-horizon volume — it represents the
        # *aggregate* marginal water value, not the per-scene share.
        try:
            raw = planos.lower_bound_water_value_by_reservoir(num_scenarios=None)
        except (AttributeError, TypeError):
            # Defensive — older parser fixtures may not implement the
            # helper.  Treat as "no cuts available" instead of crashing
            # mid-build.
            return {}
        # Un-discount the cut-derived caps to the auto's face-value
        # frame using the LAST stage's `discount_factor` from the
        # emitted simulation — this is the discount factor the LP
        # actually applies at solve time.  Previously we read from
        # `stage_parser.stages[-1]` (the full PLP view); that diverged
        # from the simulation's view whenever the writer truncated
        # tail stages (e.g. `-t 1y` on a 2y PLP), leaving the caps
        # off by exactly the missing-tail cumulative discount.  Read
        # the planning JSON's stage_array — which IS what gtopt will
        # see — so writer and LP agree.  Fall back to the parser when
        # no simulation has been built yet (legacy / unit-test paths).
        stages = (
            self.planning.get("simulation", {}).get("stage_array", [])
            if hasattr(self, "planning")
            else []
        )
        if not stages:
            stage_parser = self.parser.parsed_data.get("stage_parser")
            if stage_parser is None:
                return raw
            stages = getattr(stage_parser, "stages", None) or []
        if not stages:
            return raw
        try:
            last_df = float(stages[-1].get("discount_factor", 1.0))
        except (TypeError, ValueError):
            return raw
        if last_df <= 0.0 or last_df == 1.0:
            return raw
        return {name: value / last_df for name, value in raw.items()}

    def apply_default_water_fail(self) -> None:
        """Stamp a default water-fail value on un-priced reservoirs.

        Computes the global default water-fail value as the **maximum**
        of every per-reservoir ``efin_cost`` already assigned (see
        :func:`gtopt_shared.water_values.default_water_fail_value`), then
        applies it as a fallback to reservoirs that carry a non-trivial
        terminal volume (``efin``) but were left **without** an
        ``efin_cost`` — so no reservoir's terminal volume slack is ever
        priced at zero (free fictitious water).

        Placement rationale: ``efin_cost`` is in ``$/hm³``, but no
        ``model_options`` field carries a global hydro water-fail default
        in that unit — ``hydro_spill_cost`` is ``$/m³`` (1e6× off) and
        ``state_violation_cost`` is ``$/MWh`` (SDDP elastic-filter
        fallback, converted via production_factor, a different surface).
        Rather than mis-unit a model option, the default is pushed onto
        the un-priced reservoirs directly, where the unit matches exactly.
        A no-op when no reservoir carries a positive ``efin_cost``.
        """
        reservoirs = self.planning.get("system", {}).get("reservoir_array", [])
        if not reservoirs:
            return

        assigned = [
            float(r["efin_cost"])
            for r in reservoirs
            if isinstance(r.get("efin_cost"), (int, float)) and r["efin_cost"] > 0
        ]
        if not assigned:
            return

        default_wf = default_water_fail_value(assigned)
        if default_wf <= 0.0:
            return

        # Fallback only: reservoirs with a real terminal volume but no
        # explicit per-reservoir price inherit the global default so the
        # ``vol_end + slack >= efin`` row is never free.
        patched = 0
        for r in reservoirs:
            efin = r.get("efin")
            has_cost = (
                isinstance(r.get("efin_cost"), (int, float)) and r["efin_cost"] > 0
            )
            if isinstance(efin, (int, float)) and efin > 0 and not has_cost:
                r["efin_cost"] = default_wf
                patched += 1

        if patched:
            _logger.info(
                "water-value: default water-fail %.2f $/hm³ applied to %d "
                "un-priced reservoir(s)",
                default_wf,
                patched,
            )

    def process_ror_spec(self, options):
        """Resolve ``--ror-as-reservoirs`` once so downstream writers share it.

        Runs before ``process_afluents`` and ``process_junctions`` so that
        the discharge parquet can un-scale promoted pasada inflows and the
        junction writer can re-use the same resolved spec without re-parsing
        the CSV.  Stores two keys on ``options``:

        * ``_ror_spec_resolved``: ``{name: RorSpec}`` from the resolver.
        * ``_pasada_unscale_map``: ``{name: 1.0/production_factor}`` for
          every promoted **pasada** central (serie centrals keep their
          physical flow and are not included).
        """
        from .ror_equivalence_parser import (  # noqa: PLC0415
            pasada_unscale_map,
            resolve_ror_reservoir_spec,
        )

        centrals = self.parser.parsed_data.get("central_parser", None)
        if not centrals:
            return

        cot = getattr(centrals, "centrals_of_type", None)
        if not cot:
            return

        # Match JunctionWriter's item filter: eligible centrals are
        # pasada+serie with bus>0 (and pasada must also be routed to the
        # hydro path — not profile/solar).
        pasada_hydro_names = options.get("_pasada_hydro_names", set())
        items: list[dict[str, Any]] = []
        for c in cot.get("serie", []):
            if c.get("bus", 0) > 0:
                items.append(c)
        for c in cot.get("pasada", []):
            if c.get("bus", 0) > 0 and c["name"] in pasada_hydro_names:
                items.append(c)
        for c in cot.get("embalse", []):
            items.append(c)

        resolved = resolve_ror_reservoir_spec(options, items)
        options["_ror_spec_resolved"] = resolved
        options["_pasada_unscale_map"] = pasada_unscale_map(resolved, items)

        if resolved and options.get("expand_ror", True):
            self._dump_ror_promoted(resolved, options)

    @staticmethod
    def _dump_ror_promoted(
        resolved: Dict[str, Any],
        options: Mapping[str, Any],
    ) -> None:
        """Emit ``ror_promoted.json`` — the ``gtopt_expand ror`` audit artifact.

        Mirrors the schema produced by ``gtopt_expand.ror_expand.
        expand_ror_from_file``: ``{"promoted": [{name, vmax_hm3,
        production_factor, pmax_mw?}, ...]}``.  Skipped when
        ``--no-expand-ror`` is set or no output directory is configured.
        """
        output_dir = options.get("output_dir")
        if not output_dir:
            return

        promoted: list[dict[str, Any]] = []
        for name in sorted(resolved):
            spec = resolved[name]
            entry: dict[str, Any] = {
                "name": name,
                "vmax_hm3": spec.vmax_hm3,
                "production_factor": spec.production_factor,
            }
            if getattr(spec, "pmax_mw", None) is not None:
                entry["pmax_mw"] = spec.pmax_mw
            promoted.append(entry)

        target = Path(output_dir) / "ror_promoted.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, "w", encoding="utf-8") as fh:
            json.dump({"promoted": promoted}, fh, indent=2, sort_keys=False)
            fh.write("\n")
        _logger.info(
            "ror: audit artifact → %s (%d promoted central(s))",
            target.name,
            len(promoted),
        )

    def process_afluents(self, options):
        """Write affluent/discharge Parquet files for Flow elements.

        Excludes:
        - Pasada centrals with bus<=0 (isolated, no turbine).
        - Pasada centrals routed to profile mode (solar/wind) — their
          data is written to GeneratorProfile/ by the profile writer.
        """
        centrals = self.parser.parsed_data.get("central_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", [])
        scenarios = self.planning["simulation"]["scenario_array"]

        # Build set of names to exclude from Flow parquet
        excluded: set[str] = set()
        cot = getattr(centrals, "centrals_of_type", None) if centrals else None
        if cot:
            for c in cot.get("pasada", []):
                if c.get("bus", 0) <= 0:
                    excluded.add(c["name"])
        # Also exclude profile-mode centrals (their data goes to GeneratorProfile/)
        profile_names = options.get("_pasada_profile_names", set())
        excluded.update(profile_names)

        if excluded and aflces:
            aflces_items = [f for f in aflces.flows if f.get("name") not in excluded]
        else:
            aflces_items = None  # use default (all)

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir = output_dir / "Flow"
        output_dir.mkdir(parents=True, exist_ok=True)

        aflce_writer = AflceWriter(
            aflces,
            centrals,
            blocks,
            scenarios,
            options,
            pasada_unscale_map=options.get("_pasada_unscale_map") or None,
        )

        cols = aflce_writer.to_parquet(output_dir, items=aflces_items)
        # AflceWriter applies a sparsity filter that drops central columns
        # whose flow is constant-equal-to-afluent across all active
        # scenarios.  ``process_junctions`` needs to know which uids
        # actually survived that filter — otherwise it would emit
        # ``Flow.discharge = "discharge"`` (parquet reference) for
        # filtered-out centrals and gtopt would abort with
        # ``Can't find element 'NAME:<uid>' in table 'discharge'``.
        emitted: set[int] = set()
        for c in (cols or {}).get("discharge", []):
            if isinstance(c, str) and c.startswith("uid:"):
                try:
                    emitted.add(int(c.split(":", 1)[1]))
                except ValueError:
                    pass
        # Store on the options dict so ``process_junctions`` (next stage
        # in the orchestration) can consult it from ``_get_central_flow``.
        if options is not None:
            options["_aflce_emitted_uids"] = emitted

    def process_junctions(self, options):
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", None)
        stages = self.parser.parsed_data.get("stage_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", None)
        extracs = self.parser.parsed_data.get("extrac_parser", None)
        manems = self.parser.parsed_data.get("manem_parser", None)
        cenre = self.parser.parsed_data.get("cenre_parser", None)
        cenfi = self.parser.parsed_data.get("cenfi_parser", None)
        filemb = self.parser.parsed_data.get("filemb_parser", None)
        ralco = self.parser.parsed_data.get("ralco_parser", None)
        minembh = self.parser.parsed_data.get("minembh_parser", None)
        vrebemb = self.parser.parsed_data.get("vrebemb_parser", None)
        plpmat = self.parser.parsed_data.get("plpmat_parser", None)
        cenpmax = self.parser.parsed_data.get("cenpmax_parser", None)
        mance = self.parser.parsed_data.get("mance_parser", None)
        block = self.parser.parsed_data.get("block_parser", None)
        # Build the water-shortfall pricing resolver once and share it
        # with downstream writers (JunctionWriter, PminFlowRightWriter).
        # Construction is cheap (just builds lookup tables); whether it
        # actually drives prices depends on ``resolver.is_active``.
        #
        # ``cut_water_values`` carries the per-reservoir SDDP-revealed
        # marginal water value (cut LOWER BOUND from the boundary cuts).
        # When a reservoir is present there, the resolver OVERWRITES its
        # auto ``ANCHOR × lost_pf`` estimate with that value so the LP
        # uses the cut-revealed water value directly.
        water_value_resolver = WaterValueResolver(
            central_parser=centrals,
            cenre_parser=cenre,
            options=options,
            cut_water_values=self._build_cut_water_values(),
        )
        # Stash on self so other hydro phases (pmin_as_flowright) can
        # pick up the same instance instead of rebuilding.
        self._water_value_resolver = water_value_resolver
        jw = JunctionWriter(
            central_parser=centrals,
            stage_parser=stages,
            aflce_parser=aflces,
            extrac_parser=extracs,
            manem_parser=manems,
            cenre_parser=cenre,
            cenfi_parser=cenfi,
            filemb_parser=filemb,
            ralco_parser=ralco,
            minembh_parser=minembh,
            vrebemb_parser=vrebemb,
            plpmat_parser=plpmat,
            cenpmax_parser=cenpmax,
            mance_parser=mance,
            block_parser=block,
            options=options,
            water_value_resolver=water_value_resolver,
        )
        json_junctions = jw.to_json_array()
        # Store names of isolated centrals that were skipped (for reporting)
        skipped = getattr(jw, "_skipped_isolated", [])
        if skipped:
            self.planning["_skipped_isolated"] = skipped

        if not json_junctions:
            return

        for j in json_junctions:
            for key, val in j.items():
                # EXTEND ``flow_right_array`` so the irrigation-diversion
                # FlowRights from JunctionWriter coexist with those appended
                # by later steps (``process_pmin_flowright`` /
                # ``process_water_rights``).  Every other entity array is
                # produced solely by JunctionWriter, so a plain assignment is
                # correct there and preserves the existing behavior.
                if key == "flow_right_array" and isinstance(val, list):
                    self.planning["system"].setdefault(key, []).extend(val)
                else:
                    self.planning["system"][key] = val

    def process_water_rights(self, options):
        """Emit Laja / Maule Stage-2 artifacts.

        Controls Laja/Maule irrigation-agreement expansion only.  LNG
        is fully independent and handled by ``process_lng``; RoR
        promotion is orthogonal (see ``process_ror_spec``) but
        complementary — when ``--ror-as-reservoirs`` promotes
        MACHICURA, the Maule agreement gains its ``embalse`` template
        variant (see auto-detection note below).

        Gated by ``expand_water_rights`` (opt-in, default False).  When
        set, dispatches ``gtopt_expand laja|maule`` in-process against
        the already-parsed config (no ``*_dat.json`` intermediate is
        written to disk — those parser dumps would never be shipped
        anyway).  The Stage-2 entities are merged into
        ``planning["system"]``, companion ``laja.pampl`` /
        ``maule.pampl`` files are written, and per-agreement system
        fragments ``laja_water_rights.json`` / ``maule_water_rights.json``
        are emitted (these DO go into the manifest so gtopt can merge
        them alongside the main planning JSON).

        Machicura auto-detection consults both
        ``planning["system"]["reservoir_array"]`` (populated by
        ``process_junctions`` and any ``--ror-as-reservoirs`` promotion)
        AND, as a belt-and-suspenders check, the ``ror_promoted.json``
        audit file written by ``process_ror_spec``.  When ``MACHICURA``
        appears in either set, ``MauleAgreement`` picks the ``embalse``
        template variant; otherwise the ``pasada`` default.  Hand-
        authored fixtures can still pin the variant by setting
        ``cfg["machicura_model"]`` explicitly.
        """
        if not options.get("expand_water_rights", False):
            return

        output_dir = Path(options["output_dir"]) if options.get("output_dir") else None
        if output_dir is None:
            return

        stage_parser = self.parser.parsed_data.get("stage_parser")

        # When ``process_junctions`` was skipped (e.g. no hydro topology
        # but Laja/Maule agreements still requested) the
        # ``_water_value_resolver`` attribute is unset.  Build a fresh
        # one here so the auto-water-fail-cost path still works.  Same
        # pattern as ``process_pmin_flowright``.
        if getattr(self, "_water_value_resolver", None) is None:
            self._water_value_resolver = WaterValueResolver(
                central_parser=self.parser.parsed_data.get("central_parser"),
                cenre_parser=self.parser.parsed_data.get("cenre_parser"),
                options=options,
                cut_water_values=self._build_cut_water_values(),
            )

        laja_parser = self.parser.parsed_data.get("laja_parser")
        if laja_parser is not None:
            self._expand_laja(laja_parser.config, stage_parser, output_dir)

        maule_parser = self.parser.parsed_data.get("maule_parser")
        if maule_parser is not None:
            cfg = dict(maule_parser.config)
            extrac_parser = self.parser.parsed_data.get("extrac_parser")
            if extrac_parser is not None:
                cfg["extrac_entries"] = list(extrac_parser.get_all())
            self._expand_maule(cfg, stage_parser, output_dir)

    def process_lng(self, options):
        """Emit LNG Stage-2 expansion.

        Fully independent of ``process_water_rights``.  When
        ``expand_lng`` is True (the default), dispatches
        ``gtopt_expand lng`` against the already-parsed
        ``plpcnfgnl.dat`` config and merges the resulting
        ``lng_terminal_array`` into ``planning["system"]``.  A no-op
        when the PLP case has no ``plpcnfgnl.dat`` (no ``gnl_parser``).
        No intermediate ``lng_dat.json`` is written — parser dumps are
        never shipped.
        """
        if not options.get("expand_lng", True):
            return

        gnl_parser = self.parser.parsed_data.get("gnl_parser")
        if gnl_parser is None:
            return

        self._expand_lng(gnl_parser.config)

    def process_pumped_storage(self, options):
        """Emit pumped-storage expansions from ``--pumped-storage FILE[s]``.

        For each config file in ``options["pumped_storage_files"]``,
        runs the ``gtopt_expand.pumped_storage_expand`` transform and
        merges the resulting entities into the planning JSON.  The
        per-unit artifact ``{name}.json`` is written to ``output_dir``
        wrapped as ``{"system": {...}}``.  The ``name`` comes from the
        file's ``"name"`` field (or the filename stem as fallback) and
        drives all emitted element names
        (``hydro_{name}``, ``tur_{name}``, …).

        ``vmin`` / ``vmax`` at ``0`` (or absent) fall back to the upper
        reservoir's ``emin`` / ``emax`` in plpcnfce.dat.  Requires each
        unit's ``lower_reservoir`` to be a reservoir — real embalse or
        RoR-promoted via --ror-as-reservoirs.  Raises on missing
        prerequisites.
        """
        ps_files = options.get("pumped_storage_files") or []
        if not ps_files:
            return

        output_dir = Path(options["output_dir"]) if options.get("output_dir") else None
        if output_dir is None:
            return

        from gtopt_expand.pumped_storage_expand import (  # noqa: PLC0415
            expand_pumped_storage,
        )

        central_parser = self.parser.parsed_data.get("central_parser")
        embalses = (
            central_parser.centrals_of_type.get("embalse", [])
            if central_parser is not None
            else []
        )

        def _plpcnfce_vmin_vmax(
            upper_name: str,
        ) -> tuple[float | None, float | None]:
            for c in embalses:
                if c.get("name") == upper_name:
                    vmin = float(c["emin"]) if "emin" in c else None
                    vmax = float(c["emax"]) if "emax" in c else None
                    return vmin, vmax
            return None, None

        reservoir_names = self._reservoir_names(output_dir)
        reservoirs = self.planning["system"].get("reservoir_array", [])
        reservoirs_list = list(reservoirs) if isinstance(reservoirs, list) else []

        def _resolve(
            c: Dict[str, Any], key: str, fallback: float | None
        ) -> float | None:
            val = c.get(key)
            if val is None or float(val) == 0.0:
                return fallback
            return float(val)

        for idx, params_path in enumerate(ps_files):
            path = Path(params_path)
            with open(path, "r", encoding="utf-8") as fh:
                loaded = json.load(fh)
            if not isinstance(loaded, dict):
                raise ValueError(
                    f"--pumped-storage {path}: expected a JSON object, "
                    f"got {type(loaded).__name__}"
                )
            cfg: Dict[str, Any] = dict(loaded)

            # Unit name: config wins, then filename stem.
            unit_name = str(cfg.get("name") or path.stem).strip()
            if not unit_name:
                raise ValueError(
                    f"--pumped-storage {path}: unit 'name' cannot be empty"
                )
            cfg["name"] = unit_name

            # Backfill vmin/vmax from plpcnfce.dat when the user left
            # them at 0 (or absent).  The upper reservoir drives the
            # PF curve; default to COLBUN for backwards compatibility
            # with the HB Maule workflow.
            upper_name = str(cfg.get("upper_reservoir") or "COLBUN")
            plp_vmin, plp_vmax = _plpcnfce_vmin_vmax(upper_name)

            resolved_vmin = _resolve(cfg, "vmin", plp_vmin)
            resolved_vmax = _resolve(cfg, "vmax", plp_vmax)
            if resolved_vmin is None or resolved_vmax is None:
                raise ValueError(
                    f"pumped-storage '{unit_name}' needs upper reservoir "
                    f"'{upper_name}' vmin/vmax: not provided in "
                    f"{path} and no '{upper_name}' embalse found in "
                    f"plpcnfce.dat"
                )
            cfg["vmin"] = resolved_vmin
            cfg["vmax"] = resolved_vmax

            entities = expand_pumped_storage(
                config=cfg,
                name=unit_name,
                reservoirs=reservoirs_list,
                reservoir_names=reservoir_names,
                uid_start=900_000 + idx * 16,
            )

            target = output_dir / f"{unit_name}.json"
            with open(target, "w", encoding="utf-8") as fh:
                json.dump({"system": entities}, fh, indent=2, sort_keys=False)
                fh.write("\n")

            self._merge_entities(entities)
            _logger.info(
                "pumped_storage: emitted '%s' + %s.json "
                "(2 waterways, 1 turbine, 1 pump, 1 RPF)",
                unit_name,
                unit_name,
            )

    def _merge_entities(self, entities: Mapping[str, Any]) -> None:
        """Merge gtopt_expand entity arrays into ``planning["system"]``.

        ``*_array`` keys are appended (so Laja and Maule can contribute
        to the same ``flow_right_array`` / ``volume_right_array`` /
        ``user_constraint_array``).  Singular ``user_constraint_file``
        strings are aggregated into the plural ``user_constraint_files``
        list because each agreement emits its own ``.pampl`` and gtopt
        accepts multiple files via that plural field.
        """
        system = self.planning["system"]
        for key, val in entities.items():
            if key == "user_constraint_file":
                system.setdefault("user_constraint_files", []).append(val)
            elif isinstance(val, list) and key.endswith("_array"):
                system.setdefault(key, []).extend(val)
            else:
                system[key] = val

    def _reservoir_names(self, output_dir: Path | None = None) -> set[str]:
        """Return reservoir names currently known to the writer.

        Includes:

        * Names in ``planning["system"]["reservoir_array"]`` (populated
          by ``process_junctions`` and any ``--ror-as-reservoirs``
          promotion).
        * When ``output_dir`` is given and ``ror_promoted.json`` exists
          in it, the promoted names from that audit file.  This covers
          Stage-2-only runs where ``process_junctions`` has not yet
          mutated ``planning["system"]``.
        """
        names = {
            r.get("name", "")
            for r in self.planning["system"].get("reservoir_array", [])
            if r.get("name")
        }
        if output_dir is not None:
            audit = Path(output_dir) / "ror_promoted.json"
            if audit.exists():
                try:
                    data = json.loads(audit.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    data = {}
                for entry in data.get("promoted", []):
                    name = entry.get("name") if isinstance(entry, dict) else None
                    if name:
                        names.add(name)
        return names

    @staticmethod
    def _dump_water_rights_fragment(
        tag: str, entities: Mapping[str, Any], output_dir: Path
    ) -> Path | None:
        """Write a per-agreement ``<tag>_water_rights.json`` system fragment.

        The fragment mirrors the manifest-mergeable structure
        ``{"system": {...entity arrays..., "user_constraint_files": [...]}}``
        so gtopt can load it directly via the planning-file merge path.
        Returns the path written, or None if ``entities`` is empty.
        """
        if not entities:
            return None
        system: Dict[str, Any] = {}
        for key, val in entities.items():
            if key == "user_constraint_file":
                system.setdefault("user_constraint_files", []).append(val)
            elif isinstance(val, list) and key.endswith("_array"):
                system.setdefault(key, []).extend(val)
            else:
                system[key] = val
        target = output_dir / f"{tag}_water_rights.json"
        with open(target, "w", encoding="utf-8") as fh:
            json.dump({"system": system}, fh, indent=2, sort_keys=False)
            fh.write("\n")
        return target

    def _expand_laja(
        self,
        cfg: Mapping[str, Any],
        stage_parser: Any,
        output_dir: Path,
    ) -> Dict[str, Any]:
        """Run the Stage-2 Laja transform, merge entities, return them."""
        from gtopt_expand.laja_agreement import LajaAgreement  # noqa: PLC0415

        agreement = LajaAgreement(
            dict(cfg),
            stage_parser=stage_parser,
            water_value_resolver=getattr(self, "_water_value_resolver", None),
        )
        entities = agreement.to_json_dict(output_dir=output_dir)
        self._merge_entities(entities)
        self._dump_water_rights_fragment("laja", entities, output_dir)
        _logger.info(
            "laja: expanded to %d flow_right(s), %d volume_right(s)%s"
            " + laja_water_rights.json",
            len(entities.get("flow_right_array", [])),
            len(entities.get("volume_right_array", [])),
            " + laja.pampl" if "user_constraint_file" in entities else "",
        )
        return entities

    def _expand_maule(
        self,
        cfg: Mapping[str, Any],
        stage_parser: Any,
        output_dir: Path,
    ) -> Dict[str, Any]:
        """Run the Stage-2 Maule transform, merge entities, return them."""
        from gtopt_expand.maule_agreement import MauleAgreement  # noqa: PLC0415

        agreement = MauleAgreement(
            dict(cfg),
            stage_parser=stage_parser,
            options={"reservoir_names": self._reservoir_names(output_dir)},
            water_value_resolver=getattr(self, "_water_value_resolver", None),
        )
        entities = agreement.to_json_dict(output_dir=output_dir)
        self._merge_entities(entities)
        self._dump_water_rights_fragment("maule", entities, output_dir)
        _logger.info(
            "maule: expanded to %d flow_right(s), %d volume_right(s)%s"
            " + maule_water_rights.json",
            len(entities.get("flow_right_array", [])),
            len(entities.get("volume_right_array", [])),
            " + maule.pampl" if "user_constraint_file" in entities else "",
        )
        return entities

    def _expand_lng(self, cfg: Mapping[str, Any]) -> None:
        """Run the Stage-2 LNG transform and merge ``lng_terminal_array``."""
        from gtopt_expand.lng_expand import expand_lng  # noqa: PLC0415

        num_stages = len(self.planning["simulation"].get("stage_array", []))
        entities = expand_lng(dict(cfg), num_stages=num_stages)
        self._merge_entities(entities)
        _logger.info(
            "lng: expanded to %d terminal(s)",
            len(entities.get("lng_terminal_array", [])),
        )

    def process_pmin_flowright(self, options):
        """Convert whitelisted generators' ``pmin`` into FlowRight obligations.

        Companion to :mod:`gtopt_expand.pmin_flowright_expand`.  Runs
        only when ``options["pmin_as_flowright"]`` is set (string;
        empty string means "use bundled default CSV").  For each
        whitelisted central:

        * Looks up the gen waterway and its ``junction_b`` in
          ``planning["system"]``.
        * Computes ``flow_min[block] = pmin[block] / Rendi`` from
          ``mance_parser`` (per-stage-per-block pmin) — falls back to
          the central's static ``pmin`` when there is no mance entry.
        * Writes ``FlowRight/<central>_pmin_as_flow_right.parquet``
          (one column per FlowRight, schema mirrors
          ``Generator/pmin.parquet``).
        * Appends a FlowRight JSON entry to
          ``planning["system"]["flow_right_array"]`` with
          ``discharge = "<central>_pmin_as_flow_right"`` (which the
          gtopt JSON binding aliases to ``target``), ``junction =
          junction_b`` and a ``fcost`` calibrated from plpvrebemb (2×
          max rebalse_cost) or plpmat CVert (2× CVert) — fallback
          $10 000/m³/s·h when neither is set.
        * Zeros the matching generator's ``pmin`` so the LP no longer
          enforces the must-run obligation directly.
        """
        # Default ON when the key is absent — matches the CLI default
        # (see ``--pmin-as-flowright`` / ``--no-pmin-as-flowright`` in
        # ``_parsers.py``).  Programmatic callers
        # (``convert_plp_case``) opt out by setting
        # ``pmin_as_flowright=False`` or ``None``.
        spec = options.get("pmin_as_flowright", "")
        # ``False`` / ``None`` → opt out (no transform).
        # ``True`` / empty string → use the bundled default whitelist.
        # Otherwise: pass through to ``resolve_whitelist`` (path or names).
        if spec is None or spec is False:
            return
        if spec is True:
            spec = ""

        from ._parsers import DEFAULT_PMIN_FLOWRIGHT_FILE  # noqa: PLC0415
        from .pmin_flowright_writer import (  # noqa: PLC0415
            PminFlowRightWriter,
            resolve_whitelist,
        )

        try:
            whitelist = resolve_whitelist(
                str(spec), default_csv=DEFAULT_PMIN_FLOWRIGHT_FILE
            )
        except (FileNotFoundError, ValueError) as exc:
            _logger.warning("pmin_as_flowright: %s; skipping conversion.", exc)
            return
        if not whitelist:
            return

        # Reuse the water-shortfall resolver constructed during junction
        # processing when available; otherwise build a fresh one (e.g.
        # when ``process_junctions`` was skipped because the case has no
        # hydro topology but pmin_as_flowright is still requested).
        wvr = getattr(self, "_water_value_resolver", None)
        if wvr is None:
            wvr = WaterValueResolver(
                central_parser=self.parser.parsed_data.get("central_parser"),
                cenre_parser=self.parser.parsed_data.get("cenre_parser"),
                options=options,
                cut_water_values=self._build_cut_water_values(),
            )
        writer = PminFlowRightWriter(
            whitelist=whitelist,
            central_parser=self.parser.parsed_data.get("central_parser"),
            mance_parser=self.parser.parsed_data.get("mance_parser"),
            block_parser=self.parser.parsed_data.get("block_parser"),
            stage_parser=self.parser.parsed_data.get("stage_parser"),
            vrebemb_parser=self.parser.parsed_data.get("vrebemb_parser"),
            plpmat_parser=self.parser.parsed_data.get("plpmat_parser"),
            cenre_parser=self.parser.parsed_data.get("cenre_parser"),
            options=options,
            water_value_resolver=wvr,
        )
        writer.process(self.planning["system"])

        # Wire each soft FlowRight to an inline bypass column via
        # ``FlowRight.bypass_junction``.  Replaces the legacy synthetic
        # parallel ``_spill`` waterway with the inline mechanism added
        # to the LP in 35d3bdb8a — see
        # ``gtopt_expand.pmin_flowright_expand.ensure_bypass_for_flowrights``.
        from gtopt_expand.pmin_flowright_expand import (  # noqa: PLC0415
            ensure_bypass_for_flowrights,
        )

        ensure_bypass_for_flowrights(self.planning["system"])

    def process_flow_turbines(self, options):
        """Create Flow + Turbine(flow=ref) for hydro pasada centrals.

        Only pasada centrals classified as hydro (in
        ``_pasada_hydro_names``) get flow+turbine elements.  Solar/wind
        pasada centrals are handled by ``process_generator_profiles``.
        """
        hydro_names = options.get("_pasada_hydro_names", set())
        if not hydro_names:
            return

        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return

        centrals_of_type = getattr(central_parser, "centrals_of_type", None)
        if not centrals_of_type:
            return
        pasada_centrals = centrals_of_type.get("pasada", [])
        if not pasada_centrals:
            return

        flows = self.planning["system"].setdefault("flow_array", [])
        turbines = self.planning["system"].setdefault("turbine_array", [])

        aflce_parser = self.parser.parsed_data.get("aflce_parser")

        for central in pasada_centrals:
            central_name = central["name"]
            if central_name not in hydro_names:
                continue

            central_id = central["number"]

            # Determine discharge: parquet reference if aflce data exists AND
            # the column survived AflceWriter's sparsity filter; else fall
            # back to the scalar ``afluent`` (which is the same constant the
            # filter detected, so the LP sees an identical value).  Skipping
            # this check produced the
            #   ``Can't find element 'NAME:<uid>' in table 'discharge'``
            # crash for centrals like FLORIDA_1 whose flow is constant
            # across the active hydrologies.
            afluent: float | str = central.get("afluent", 0.0)
            if aflce_parser and aflce_parser.get_item_by_name(central_name):
                emitted = options.get("_aflce_emitted_uids")
                if emitted is None or int(central_id) in emitted:
                    afluent = "discharge"

            if isinstance(afluent, (int, float)) and afluent == 0.0:
                continue

            # Create Flow element
            flows.append(
                {
                    "uid": central_id,
                    "name": central_name,
                    "discharge": afluent,
                }
            )

            # Create Turbine with flow reference (not waterway)
            turbines.append(
                {
                    "uid": central_id,
                    "name": central_name,
                    "flow": central_name,
                    "generator": central_name,
                    "production_factor": central.get("efficiency", 1.0),
                }
            )
