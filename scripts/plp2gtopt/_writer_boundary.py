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
from typing import Any, Dict

from .planos_writer import write_boundary_cuts_csv, write_hot_start_cuts_csv

_logger = logging.getLogger(__name__)


class BoundaryMixin:
    """Boundary-cut + variable-scale processing for ``GTOptWriter``."""

    parser: Any
    planning: Dict[str, Dict[str, Any]]

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

        # ── Boundary cuts (last stage) ─────────────────────────────────────
        if planos.cuts:
            csv_path = output_dir / "boundary_cuts.csv"
            write_boundary_cuts_csv(
                planos.cuts,
                planos.reservoir_names,
                csv_path,
                name_alias=name_alias,
            )
            self.planning["_boundary_cuts_count"] = len(planos.cuts)
            self.planning["_boundary_state_variables"] = len(planos.reservoir_names)
            # Path relative to where gtopt runs (same dir as the JSON).
            # When JSON is inside output_dir, input_directory is "." and
            # the file is at "./boundary_cuts.csv".
            # When JSON is outside, input_directory is the output_dir path,
            # so use "{input_directory}/boundary_cuts.csv".
            input_dir_val = self.planning["options"].get("input_directory", ".")
            if input_dir_val == ".":
                sddp_opts["boundary_cuts_file"] = "boundary_cuts.csv"
            else:
                sddp_opts["boundary_cuts_file"] = str(
                    Path(input_dir_val) / "boundary_cuts.csv"
                )

        # Wire mode and max-iterations options through to the JSON
        bc_mode = options.get("boundary_cuts_mode")
        if bc_mode is not None:
            sddp_opts["boundary_cuts_mode"] = bc_mode

        bc_max_iter = options.get("boundary_max_iterations")
        if bc_max_iter is not None:
            sddp_opts["boundary_max_iterations"] = bc_max_iter

        # ── Hot-start cuts (intermediate stages) ───────────────────────────
        # Always export hot-start cuts when non-boundary cuts exist, so they
        # are available in the gtopt input directory.  Loading is disabled by
        # default; pass --hot-start-cuts to enable named_cuts_file.
        non_boundary = [
            c for c in planos.all_cuts if c["stage"] != planos.boundary_stage
        ]
        if non_boundary:
            hs_path = output_dir / "hot_start_cuts.csv"
            # Build stage→phase mapping from the planning structure
            stage_to_phase = self._build_stage_to_phase_map()
            write_hot_start_cuts_csv(
                non_boundary,
                planos.reservoir_names,
                hs_path,
                stage_to_phase=stage_to_phase,
                name_alias=name_alias,
            )
            # Only wire the file into the JSON options if explicitly requested
            if options.get("hot_start_cuts", False):
                if input_dir_val == ".":
                    sddp_opts["named_cuts_file"] = "hot_start_cuts.csv"
                else:
                    sddp_opts["named_cuts_file"] = str(
                        Path(input_dir_val) / "hot_start_cuts.csv"
                    )

    def _build_stage_to_phase_map(self) -> dict[int, int] | None:
        """Build a mapping from PLP stage (1-based) to gtopt phase UID.

        Uses the stage_array already set in the planning JSON.
        Returns ``None`` if no mapping can be built (which makes
        ``write_hot_start_cuts_csv`` use identity mapping).
        """
        raw_stages: Any = self.planning.get("stage_array", [])
        stage_array: list[dict[str, Any]] = (
            raw_stages if isinstance(raw_stages, list) else []
        )
        if not stage_array:
            return None

        stage_to_phase: dict[int, int] = {}
        for stage in stage_array:
            stage_uid: int = stage.get("uid", 0)
            phase_uid: int = stage.get("phase_uid", 0)
            stage_to_phase[stage_uid] = phase_uid

        return stage_to_phase or None

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
