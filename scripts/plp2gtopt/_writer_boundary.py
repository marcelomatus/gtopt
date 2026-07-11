# -*- coding: utf-8 -*-

"""Boundary-cut and variable-scale mixin for ``GTOptWriter``.

Holds:

* ``process_boundary_cuts`` (and its alias-file / stage-to-phase helpers)
* ``process_variable_scales`` (and its file-load helper)
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

from .planos_writer import write_boundary_cuts_csv

_logger = logging.getLogger(__name__)


class BoundaryMixin:
    """Boundary-cut + variable-scale processing for ``GTOptWriter``."""

    parser: Any
    planning: dict[str, dict[str, Any]]

    # Provided by the sibling ``HydroMixin`` (both mixed into
    # ``GTOptWriter``): per-reservoir cut lower-bound water values.
    _build_cut_water_values: Any

    @staticmethod
    def _load_alias_file(alias_file: Path | str | None) -> dict[str, str] | None:
        """Load a flat ``{old_name: new_name}`` alias map from JSON.

        Returns ``None`` when ``alias_file`` is ``None``.  Raises
        ``RuntimeError`` if the file is missing, unreadable, or does not
        contain a flat string→string mapping.
        """
        if alias_file is None:
            return None
        path = Path(alias_file)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"Cannot read alias file '{path}': {exc}") from exc
        if not isinstance(data, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in data.items()
        ):
            raise RuntimeError(
                f"Alias file '{path}' must be a flat JSON object of "
                "{string: string} pairs."
            )
        return data

    def process_boundary_cuts(self, options):
        """Write boundary-cut and hot-start-cut CSVs from parsed PLP planos data.

        If the PLP input contained plpplaem/plpplem files, the parsed boundary
        cuts are written to a CSV file in the output directory and the
        ``sddp_boundary_cuts_file`` option is set so that the SDDP solver
        loads them.  CLI options control mode, iteration filtering, and
        whether to export hot-start cuts for intermediate stages.

        Probability-factor scaling (``NVarPhi``): PLP cut values are divided
        by the scenario count so that each per-scene LP in gtopt loads its
        OWN share of the expected future cost (PLP's α-column carries
        ``1/NVarPhi`` internally; gtopt's per-scene α-column carries ``1.0``,
        so the scaling must happen at export).  See the docstring of
        :mod:`plp2gtopt.planos_writer` for the full derivation.  This
        function assumes equal scenario probabilities (the default
        ``probability_factor = 1/NVarPhi`` that
        :meth:`process_scenarios` sets); if the caller overrides
        ``--probability-factors`` with unequal values a warning is logged
        and the export is approximate.
        """
        planos = self.parser.parsed_data.get("planos_parser")
        if planos is None:
            return

        # Honour --no-boundary-cuts
        if options.get("no_boundary_cuts", False):
            return

        output_dir = Path(options.get("output_dir", ""))
        sddp_opts = self.planning["options"].setdefault("sddp_options", {})
        name_alias = self._load_alias_file(options.get("alias_file"))

        # NVarPhi divisor for the cut RHS and gradients.
        #
        # PLP applies its α-column with objective coefficient
        # `1/NVarPhi`, where `NVarPhi` is the **original PLP hydrology
        # count** (28 on juan/iplp).  gtopt's α-column instead uses
        # coefficient 1.0 and aggregates per-scene contributions via
        # `prob_s × α_s`.  For the gtopt expected α to match PLP, the
        # cut gradients/RHS must be pre-divided by NVarPhi, so that:
        #
        #     Σ_s prob_s × α_s        = (1/N) × N × (rhs/NVarPhi)
        #                             = rhs/NVarPhi                 ✓
        #
        # Up to 2026-05-13 this code divided by `len(scenario_array)`
        # (= the gtopt-side scene count, often a wet-subset of size 16)
        # which under-divided whenever the user filtered hydrologies
        # via `--hydrologies` — observed on juan/iplp_plain as a
        # +75 % LB overshoot vs UB (LB ≈ 4.6 G vs UB ≈ 2.9 G).
        #
        # `max(scene)` over the parsed cuts recovers PLP's NVarPhi
        # exactly (ISimul is 1-based and dense up to NVarPhi).  Pair
        # with `boundary_cuts_mode = combined` so gtopt's loader feeds
        # every cut into every scene (PLP behaviour), which is the
        # other half of the math being correct.
        scenario_array: list = (
            self.planning.get("simulation", {}).get("scenario_array") or []
        )
        # NVarPhi = max ISimul across the PLP cuts.  Fallback to the
        # gtopt scene count if the cut list is empty / scene field is
        # missing — preserves the pre-fix behaviour on degenerate input
        # without crashing.
        plp_nvarphi = 0
        if planos.cuts:
            plp_nvarphi = max(int(cut.get("scene", 0) or 0) for cut in planos.cuts)
        num_scenarios: int = plp_nvarphi if plp_nvarphi > 0 else len(scenario_array)
        self._warn_if_unequal_probabilities(scenario_array)

        # ── Boundary cuts (last stage) ─────────────────────────────────────
        if planos.cuts:
            csv_path = output_dir / "boundary_cuts.csv"
            # Per-reservoir FEscala for gradient-coefficient rescaling
            # (PLP stores GradX in `$/raw_volume_unit`; gtopt expects
            # `$/hm³`).  See planos_writer's _vol_scale block.
            write_boundary_cuts_csv(
                planos.cuts,
                planos.reservoir_names,
                csv_path,
                name_alias=name_alias,
                num_scenarios=num_scenarios,
                fescala_map=planos.reservoir_fescala,
            )
            self.planning["_boundary_cuts_count"] = len(planos.cuts)
            self.planning["_boundary_state_variables"] = len(planos.reservoir_names)
            # ALWAYS emit just the bare ``"boundary_cuts.csv"`` —
            # gtopt's `resolve_input` (`source/sddp_method.cpp::535`)
            # already prepends ``input_directory`` to every relative
            # path, so emitting ``"{input_directory}/boundary_cuts.csv"``
            # produced a double-prefixed path like
            # ``gtopt_iplp_plain/gtopt_iplp_plain/boundary_cuts.csv``
            # which then silently fell back to "no boundary cuts"
            # (α pinned at 0, terminal value treated as zero, UB
            # shifted down by the missing future-cost envelope).
            sddp_opts["boundary_cuts_file"] = "boundary_cuts.csv"

        # Wire mode and max-iterations options through to the JSON.
        # Default to `combined` so gtopt's loader feeds every cut into
        # every scene (mirrors PLP's single-master semantics — see
        # `plp_storage/CEN65/src/leeplaem.f::LeePlaEmb`, every cut at
        # the boundary stage installed into one master with `1/NSimul`
        # per-α weighting).  The cut RHS / gradients are pre-divided by
        # NVarPhi in the CSV, so combined + `prob_s × α_s` aggregation
        # reproduces PLP's expected α exactly.  Explicit user setting
        # via `--boundary-cuts-mode` still wins.
        #
        # NOTE: we deliberately do NOT switch boundary loading to the
        # newer `boundary_cut_sharing_mode=multicut` (N terminal varphi
        # columns).  multicut is only LB-correct when the gtopt scene
        # count equals PLP's NVarPhi; under `--hydrologies` filtering
        # (N_scenes < NVarPhi) it silently drops the cuts whose source
        # scenario is outside the gtopt scene set, biasing the terminal
        # value downward.  `combined` + NVarPhi pre-division keeps all
        # cuts as competing lower bounds and is robust to subsets — so it
        # stays the PLP-faithful default for boundary LOADING (the
        # intermediate-phase cut SHARING is multicut; see build_options).
        bc_mode = options.get("boundary_cuts_mode")
        if bc_mode is None and planos.cuts:
            bc_mode = "combined"
        if bc_mode is not None:
            sddp_opts["boundary_cuts_mode"] = bc_mode

        bc_max_iter = options.get("boundary_max_iterations")
        if bc_max_iter is not None:
            sddp_opts["boundary_max_iterations"] = bc_max_iter

        # CFUE terminal policy: when the boundary cuts are loaded
        # (combined mode) they ALREADY price the end-of-horizon volume
        # of every cut-covered reservoir — PLP's ``EmbCFUE`` (Costo
        # Futuro Uso Embalse) reservoirs are governed by the FCF cuts,
        # and ``VolFinEmb`` applies its hard ``vol_end >= EmbVFin``
        # bound ONLY to the non-CFUE ones (volfinem.f:23).  gtopt
        # instead leaves a SECOND terminal-value mechanism active on
        # the same reservoirs: the soft ``efin`` slack priced at
        # ``efin_cost``.  For a CFUE reservoir whose ``efin`` target
        # (PLP ``EmbVFin``) is unreachable within the horizon that soft
        # slack injects a large one-sided penalty that (a) double-counts
        # the terminal value the cuts already carry, (b) over-hoards the
        # reservoir toward the target, and (c) pollutes the objective by
        # ~efin_cost × (target − reachable).  ``--cuts-govern-terminal``
        # drops ``efin``/``efin_cost`` from every cut-covered reservoir
        # so the loaded cuts are the sole terminal-value mechanism —
        # matching PLP's CFUE handling.  ON by default: plp2gtopt's
        # purpose is to replicate PLP as closely as possible, and PLP
        # governs CFUE reservoirs by the FCF cuts (not a soft slack).
        # ``--no-cuts-govern-terminal`` restores the legacy behaviour.
        if options.get("cuts_govern_terminal", True):
            if bc_mode == "combined":
                self._apply_cuts_govern_terminal()
            # PLP's LeeManEmb (leemanem.f) overwrites the last-stage
            # EmbVMin from plpmanem.dat AFTER volfinem installs the vfin
            # pin, so a reservoir with a last-stage maintenance entry
            # loses its hard vol_end>=EmbVFin bound (its terminal floor is
            # the maintenance emin instead).  Drop efin for those.  This is
            # independent of the FCF-cut governance (LeeManEmb runs last),
            # so it applies regardless of the boundary-cut loading mode.
            self._drop_efin_for_last_stage_maintenance()

        # ── Hot-start cuts retired (2026-05) ───────────────────────────────
        # The "hot-start planos" CSV writer was removed; those cuts are
        # gtopt's own internal cut format and travel via the typed
        # Parquet path (driven by the gtopt-side ``cuts_input_file`` /
        # ``cuts_output_file`` options).  Only the PLP-compatible
        # boundary cuts (last-phase α floor) are still exported above.

    def _apply_cuts_govern_terminal(self) -> None:
        """Strip ``efin``/``efin_cost`` from cut-covered reservoirs.

        The loaded boundary cuts (combined mode) already price the
        terminal volume of every reservoir that appears in the planos
        (PLP ``EmbCFUE`` reservoirs).  Removing the soft-``efin`` slack
        for those reservoirs leaves the cuts as the sole terminal-value
        mechanism — PLP's CFUE behaviour (volfinem.f gates its hard
        ``vol_end >= EmbVFin`` bound to the NON-CFUE reservoirs).
        Reservoirs absent from the cuts keep their ``efin`` bound.
        """
        cut_names = set(self._build_cut_water_values())
        if not cut_names:
            return
        # Per-reservoir EmbCFUE flag (default True when the central
        # parser is absent — preserves the cut-governs behaviour).
        cfue: dict[str, bool] = {}
        centrals = self.parser.parsed_data.get("central_parser")
        if centrals is not None:
            for c in getattr(centrals, "centrals", []):
                cfue[str(c.get("name", ""))] = bool(c.get("cfue", True))
        govern = pinned = 0
        for r in self.planning["system"].get("reservoir_array", []):
            name = str(r.get("name", ""))
            if name not in cut_names:
                continue
            had_cost = r.pop("efin_cost", None) is not None
            if cfue.get(name, True):
                # CFUE=T: FCF cuts are the sole terminal mechanism —
                # drop the efin target entirely (PLP: no vol_fin bound).
                r.pop("efin", None)
                govern += int(had_cost)
            else:
                # CFUE=F: PLP pins the last stage with a HARD
                # vol_end>=EmbVFin bound (volfinem.f).  Keep ``efin``
                # (gtopt uses a hard ``>=`` when ``efin_cost`` is unset)
                # and only drop the soft-slack price.
                pinned += int(had_cost)
        if govern or pinned:
            _logger.info(
                "cuts_govern_terminal: %d CFUE reservoir(s) now "
                "cut-governed (efin dropped); %d non-CFUE reservoir(s) "
                "switched to a hard vol_end>=EmbVFin bound (efin_cost "
                "dropped) — PLP VolFinEmb behaviour.",
                govern,
                pinned,
            )

    def _drop_efin_for_last_stage_maintenance(self) -> int:
        """Drop ``efin`` from reservoirs whose plpmanem.dat profile covers
        the last stage — PLP's ``LeeManEmb`` overwrites the vfin bound.

        PLP fixes the terminal reservoir floor in three ordered steps
        (``plp-main.F``): ``LeeCnfCen`` sets ``EmbVMin = VolMin`` for every
        stage; ``VolFinEmb`` (``volfinem.f:23``) overwrites the LAST stage
        with ``EmbVFin`` for the non-CFUE reservoirs (the hard vfin pin);
        then ``LeeManEmb`` (``leemanem.f:152``, a plain assignment)
        overwrites ``EmbVMin`` per-stage from ``plpmanem.dat`` — INCLUDING
        the last stage — for every reservoir that carries a maintenance
        profile.  So a reservoir listed in ``plpmanem.dat`` with an entry
        at the last stage LOSES its ``vol_end >= EmbVFin`` pin; its terminal
        floor becomes the maintenance ``emin`` (already emitted per-stage
        into ``Reservoir/emin.parquet`` by :class:`ManemWriter`).  Keeping
        ``efin`` would over-constrain it.

        Empirically verified 2026-07-09 against the PLP-written last-stage
        LP (``PLP_SCALEVDI_MODE=no``): PEHUENCHE (CFUE=F, NO maintenance)
        keeps VFin (121615.93 = plpcnfce Final ×1e5); PANGUE (CFUE=F, HAS
        maintenance) reverts to the maintenance VolMin (57583.4 = plpmanem
        stage-52 VolMin ×1e5), NOT its EmbVFin (67868).

        Applies regardless of ``EmbCFUE`` (LeeManEmb runs after both
        volfinem and the cut machinery); CFUE reservoirs already had
        ``efin`` dropped by :meth:`_apply_cuts_govern_terminal`, so this is
        idempotent for them and only adds the non-CFUE + maintenance case.
        """
        manems = self.parser.parsed_data.get("manem_parser")
        stages = self.parser.parsed_data.get("stage_parser")
        if manems is None or stages is None:
            return 0
        last_stage = int(getattr(stages, "num_stages", 0))  # PLP 1-based
        if last_stage <= 0:
            return 0
        dropped = 0
        for r in self.planning["system"].get("reservoir_array", []):
            if "efin" not in r:
                continue
            entry = manems.get_manem_by_name(str(r.get("name", "")))
            if entry is None:
                continue
            stage_arr = entry.get("stage")
            if stage_arr is None:
                continue
            # LeeManEmb only overwrites the stages the profile lists; the
            # vfin is wiped only when the LAST stage is among them.
            if last_stage in {int(s) for s in stage_arr}:
                r.pop("efin", None)
                r.pop("efin_cost", None)
                dropped += 1
        if dropped:
            _logger.info(
                "cuts_govern_terminal: %d reservoir(s) with a last-stage "
                "plpmanem.dat maintenance override had efin dropped — PLP "
                "LeeManEmb overwrites EmbVFin at the last stage.",
                dropped,
            )
        return dropped

    @staticmethod
    def _warn_if_unequal_probabilities(scenario_array: list) -> None:
        """Log a warning when scenario probabilities are not equal.

        The 1/NVarPhi probability factor applied in :mod:`planos_writer`
        assumes uniform scenario probabilities (PLP's convention).  If the
        caller overrode ``--probability-factors`` with unequal values, the
        cut export is approximate — the warning flags this as a follow-up.
        """
        if not scenario_array:
            return
        probs = [s.get("probability_factor", 1.0) for s in scenario_array]
        if not probs:
            return
        ref = probs[0]
        # Allow a tiny relative tolerance for rounding noise in the JSON.
        tol = 1e-9
        if any(abs(p - ref) > tol * max(1.0, abs(ref)) for p in probs):
            _logger.warning(
                "Boundary-cut export: scenarios have non-uniform "
                "probability_factor (%s); the 1/NVarPhi cut scaling assumes "
                "equal probabilities and is approximate in this case.",
                ", ".join(f"{p:g}" for p in probs[:8])
                + ("..." if len(probs) > 8 else ""),
            )

    # ``_build_stage_to_phase_map`` was the sole helper for
    # ``write_hot_start_cuts_csv`` (retired 2026-05).  Removed alongside
    # the hot-start CSV writer.

    @staticmethod
    def _load_variable_scales_file(file_path: Path) -> list[dict]:
        """Load variable scales from a JSON file.

        The file must contain a JSON array of objects, each with keys:
        ``class_name``, ``variable``, ``uid``, ``scale``.

        Returns an empty list on any read/parse error (with a warning log).
        """
        logger = logging.getLogger(__name__)
        try:
            with open(file_path, encoding="utf-8") as fh:
                data = json.load(fh)
            if not isinstance(data, list):
                logger.warning(
                    "variable-scales-file %s: expected a JSON array, got %s",
                    file_path,
                    type(data).__name__,
                )
                return []
            required_keys = {"class_name", "variable", "uid", "scale"}
            result: list[dict] = []
            for entry in data:
                if not isinstance(entry, dict) or not required_keys <= entry.keys():
                    logger.warning(
                        "variable-scales-file %s: skipping invalid entry %r "
                        "(expected keys: %s)",
                        file_path,
                        entry,
                        ", ".join(sorted(required_keys)),
                    )
                    continue
                result.append(entry)
            return result
        except (OSError, json.JSONDecodeError) as exc:
            logger.warning(
                "variable-scales-file %s: failed to load: %s", file_path, exc
            )
            return []

    def process_variable_scales(self, options):
        """Build ``variable_scales`` entries in the options section.

        Generates VariableScale JSON entries for reservoir energy scaling
        and battery energy scaling, using the ``variable_scales`` mechanism
        in ``PlanningOptions`` rather than per-element fields.

        Scale priority (highest to lowest):
        1. Explicit ``--reservoir-energy-scale`` / ``--battery-energy-scale``.
        2. ``--auto-reservoir-energy-scale`` / ``--auto-battery-energy-scale``.
        3. ``--variable-scales-file`` entries (lowest priority).

        OFF by default — gtopt auto-scales reservoirs and batteries from emax.
        """
        if not options:
            return

        # Auto-scale is the default since per-element energy_scale fields have
        # been removed from the C++ structs. Use variable_scales exclusively.
        has_reservoir = "reservoir_energy_scale" in options or options.get(
            "auto_reservoir_energy_scale", False
        )
        has_battery = "battery_energy_scale" in options or options.get(
            "auto_battery_energy_scale", False
        )
        has_file = "variable_scales_file" in options

        if not has_reservoir and not has_battery and not has_file:
            return

        # --- Load file-based scales first (lowest priority) ---
        file_scales: list[dict] = []
        if has_file:
            file_path = options["variable_scales_file"]
            file_scales = self._load_variable_scales_file(Path(file_path))

        # Build a lookup of (class_name, variable, uid) → scale from the file
        # so we can skip file entries that are overridden by auto/explicit.
        file_scale_map: dict[tuple[str, str, int], float] = {}
        for entry in file_scales:
            key = (entry["class_name"], entry["variable"], entry["uid"])
            file_scale_map[key] = entry["scale"]

        # Track which (class_name, variable, uid) are set by auto/explicit
        computed_keys: set[tuple[str, str, int]] = set()

        scales: list[dict] = []

        # --- Reservoir energy scales ---
        if has_reservoir:
            explicit_reservoir: dict = options.get("reservoir_energy_scale", {})
            auto_reservoir = options.get("auto_reservoir_energy_scale", False)

            # Collect FEscala data from planos parser (plpplem1.dat)
            planos = self.parser.parsed_data.get("planos_parser")
            fescala_map: dict = {}
            if planos is not None:
                fescala_map = planos.reservoir_fescala

            # Collect central_parser energy_scale as fallback for auto mode
            central_parser = self.parser.parsed_data.get("central_parser")
            central_energy_scale: dict = {}
            if central_parser is not None:
                for central in central_parser.centrals:
                    if central.get("type") == "embalse" and "energy_scale" in central:
                        central_energy_scale[str(central["name"])] = central[
                            "energy_scale"
                        ]

            reservoirs = self.planning["system"].get("reservoir_array", [])
            for rsv in reservoirs:
                name = rsv["name"]
                uid = rsv["uid"]
                scale = None

                # Priority 1: explicit --reservoir-energy-scale
                if name in explicit_reservoir:
                    scale = explicit_reservoir[name]
                # Priority 2: auto-rsv-energy-scale
                elif auto_reservoir:
                    # Try FEscala from plpplem1.dat first
                    fescala = fescala_map.get(name)
                    if fescala is not None:
                        scale = 10.0 ** (fescala - 6)
                    else:
                        # Fallback: central_parser's energy_scale (Escala/1e6)
                        scale = central_energy_scale.get(name)

                if scale is not None and scale != 1.0:
                    scales.append(
                        {
                            "class_name": "Reservoir",
                            "variable": "energy",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Reservoir", "energy", uid))
                    # Scale flow (extraction) variables: energy is in GWh,
                    # flow is in hm³ ≈ energy/1000, so divide by 1000.
                    scales.append(
                        {
                            "class_name": "Reservoir",
                            "variable": "flow",
                            "uid": uid,
                            "scale": scale / 1000.0,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Reservoir", "flow", uid))

        # --- Battery energy scales ---
        if has_battery:
            explicit_energy: dict = options.get("battery_energy_scale", {})
            auto_energy = options.get("auto_battery_energy_scale", False)

            batteries = self.planning["system"].get("battery_array", [])
            for bat in batteries:
                name = bat["name"]
                uid = bat["uid"]
                scale = None

                # Priority 1: explicit --energy-scale
                if name in explicit_energy:
                    scale = explicit_energy[name]
                # Priority 2: auto-energy-scale → 0.01 for all PLP batteries
                elif auto_energy:
                    scale = 0.01

                if scale is not None and scale != 1.0:
                    scales.append(
                        {
                            "class_name": "Battery",
                            "variable": "energy",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Battery", "energy", uid))
                    # Scale flow (finp/fout) with the same factor so
                    # energy-balance coefficients stay O(1).
                    scales.append(
                        {
                            "class_name": "Battery",
                            "variable": "flow",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Battery", "flow", uid))

        # --- Merge file-based scales (lowest priority) ---
        for entry in file_scales:
            key = (entry["class_name"], entry["variable"], entry["uid"])
            if key not in computed_keys and entry["scale"] != 1.0:
                scales.append(entry)

        if scales:
            self.planning["options"]["variable_scales"] = scales
