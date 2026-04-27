# -*- coding: utf-8 -*-

"""Generation domain mixin for :class:`plp2gtopt.gtopt_writer.GTOptWriter`.

Holds central / generator-profile / falla logic plus the pasada
classification step that routes pasada centrals between the hydro
flow+turbine path and the renewable generator-profile path.
"""

from __future__ import annotations

import logging
from typing import Any, Dict

from .central_writer import CentralWriter
from .generator_profile_writer import GeneratorProfileWriter
from .tech_detect import detect_technology, load_centipo_csv

_logger = logging.getLogger(__name__)


class GenerationMixin:
    """Generator-profile, central, falla and pasada-classification."""

    parser: Any
    planning: Dict[str, Dict[str, Any]]

    def process_generator_profiles(self, options):
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", [])
        scenarios = self.planning["simulation"]["scenario_array"]

        self.planning["system"]["generator_profile_array"] = GeneratorProfileWriter(
            centrals,
            blocks,
            buses,
            aflces,
            scenarios,
            options,
        ).to_json_array()

    def classify_pasada_centrals(self, options):
        """Classify pasada centrals by detected technology.

        Populates ``options["_pasada_hydro_names"]`` (set of names for
        hydro run-of-river centrals → flow+turbine mode) and
        ``options["_pasada_profile_names"]`` (set of names for
        solar/wind/renewable centrals → generator profile mode).

        Modes:
        - ``auto`` (default): per-central routing based on detected
          technology.  Solar/wind → profile, hydro → flow+turbine.
        - ``profile``: ALL pasada go to generator profile mode.
        - ``flow-turbine``: ALL pasada go to flow+turbine mode.
        - ``hydro``: ALL pasada go to full hydro topology (junctions).
        """
        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return

        centrals_of_type = getattr(central_parser, "centrals_of_type", None)
        if not centrals_of_type:
            return
        pasada_centrals = centrals_of_type.get("pasada", [])
        if not pasada_centrals:
            return

        pasada_mode = options.get("pasada_mode", "auto")
        active_names = {c["name"] for c in pasada_centrals if c.get("bus", 0) > 0}

        # Global modes: all pasada go to the same path
        if pasada_mode == "profile":
            options["_pasada_hydro_names"] = set()
            options["_pasada_profile_names"] = active_names
            return
        if pasada_mode == "hydro":
            options["_pasada_hydro_names"] = active_names
            options["_pasada_profile_names"] = set()
            return
        if pasada_mode == "flow-turbine":
            options["_pasada_hydro_names"] = active_names
            options["_pasada_profile_names"] = set()
            return

        # Auto mode: per-central routing based on detected technology
        hydro_names: set[str] = set()
        profile_names: set[str] = set()

        user_overrides = options.get("tech_overrides")
        centipo_overrides = (
            load_centipo_csv(options.get("input_dir", ""))
            if options.get("input_dir")
            else {}
        )
        effective_overrides = {**centipo_overrides}
        if user_overrides:
            effective_overrides.update(user_overrides)

        auto_detect = options.get("auto_detect_tech", False)

        # Renewable types that should use generator profile mode
        _profile_types = {"solar", "wind", "csp", "renewable"}

        for central in pasada_centrals:
            if central.get("bus", 0) <= 0:
                continue
            name = central["name"]

            tech = detect_technology(
                "pasada",
                name,
                overrides=effective_overrides,
                auto_detect=auto_detect,
            )

            if tech in _profile_types:
                profile_names.add(name)
                _logger.info("  pasada '%s' → profile mode (tech=%s)", name, tech)
            else:
                hydro_names.add(name)

        options["_pasada_hydro_names"] = hydro_names
        options["_pasada_profile_names"] = profile_names

        if profile_names:
            _logger.info(
                "  pasada routing: %d hydro (flow+turbine), %d renewable (profile)",
                len(hydro_names),
                len(profile_names),
            )

    def process_centrals(self, options):
        """Process central data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", None)
        stages = self.parser.parsed_data.get("stage_parser", None)
        blocks = self.parser.parsed_data.get("block_parser", None)
        costs = self.parser.parsed_data.get("cost_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        mances = self.parser.parsed_data.get("mance_parser", None)
        self.planning["system"]["generator_array"] = CentralWriter(
            centrals,
            stages,
            blocks,
            costs,
            buses,
            mances,
            options,
        ).to_json_array()

    def _falla_by_bus(self) -> Dict[int, Dict[str, Any]]:
        """Build bus → falla central mapping from central_parser.

        PLP "falla" centrals represent the cost of unserved energy at
        each bus.  When multiple falla centrals sit on the same bus,
        the one with the smallest gcost is chosen (most conservative
        curtailment cost).

        Returns a dict mapping bus number to the chosen falla central dict.
        """
        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return {}

        falla_by_bus: Dict[int, Dict[str, Any]] = {}
        for central in central_parser.centrals:
            if central.get("type") != "falla":
                continue
            bus = central.get("bus", 0)
            if bus <= 0:
                continue
            gcost = central.get("gcost", 0.0)
            prev = falla_by_bus.get(bus)
            if prev is None or gcost < prev.get("gcost", 0.0):
                falla_by_bus[bus] = central

        if falla_by_bus:
            costs = [c.get("gcost", 0.0) for c in falla_by_bus.values()]
            _logger.debug(
                "  falla centrals: %d bus(es) with fcost (range %.2f–%.2f $/MWh)",
                len(falla_by_bus),
                min(costs),
                max(costs),
            )

        return falla_by_bus
