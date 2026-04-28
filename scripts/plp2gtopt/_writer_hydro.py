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

from .aflce_writer import AflceWriter
from .junction_writer import JunctionWriter

_logger = logging.getLogger(__name__)


class HydroMixin:
    """Hydro / water-rights / LNG / pumped-storage processing."""

    parser: Any
    planning: Dict[str, Dict[str, Any]]

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

        aflce_writer.to_parquet(output_dir, items=aflces_items)

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

        agreement = LajaAgreement(dict(cfg), stage_parser=stage_parser)
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
          ``discharge = "<central>_pmin_as_flow_right"``,
          ``junction = junction_b`` and a ``fail_cost`` calibrated
          from plpvrebemb (2× max rebalse_cost) or plpmat CVert
          (2× CVert) — fallback $10 000/m³/s·h when neither is set.
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

        writer = PminFlowRightWriter(
            whitelist=whitelist,
            central_parser=self.parser.parsed_data.get("central_parser"),
            mance_parser=self.parser.parsed_data.get("mance_parser"),
            block_parser=self.parser.parsed_data.get("block_parser"),
            stage_parser=self.parser.parsed_data.get("stage_parser"),
            scenarios=self.planning["simulation"].get("scenario_array", []),
            vrebemb_parser=self.parser.parsed_data.get("vrebemb_parser"),
            plpmat_parser=self.parser.parsed_data.get("plpmat_parser"),
            options=options,
        )
        writer.process(self.planning["system"])

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

            # Determine discharge: file ref if aflce data exists, else scalar
            afluent: float | str = central.get("afluent", 0.0)
            if aflce_parser and aflce_parser.get_item_by_name(central_name):
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
