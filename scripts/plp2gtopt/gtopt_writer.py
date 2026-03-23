# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
import logging
from pathlib import Path
from typing import Any, Dict

from .aflce_writer import AflceWriter
from .aperture_writer import (
    build_aperture_array,
    build_phase_apertures,
    write_aperture_afluents,
)
from .battery_writer import BatteryWriter
from .block_writer import BlockWriter
from .bus_writer import BusWriter
from .central_writer import CentralWriter
from .demand_writer import DemandWriter
from .generator_profile_writer import GeneratorProfileWriter
from .index_utils import parse_index_range, parse_stages_phase
from .indhor_writer import IndhorWriter
from .junction_writer import JunctionWriter
from .line_writer import LineWriter
from .planos_writer import write_boundary_cuts_csv, write_hot_start_cuts_csv
from .plp_parser import PLPParser
from .stage_writer import StageWriter
from .tech_detect import detect_technology, load_centipo_csv

_logger = logging.getLogger(__name__)


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: PLPParser, options=None):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.options = options
        self.output_path = None

        self.planning: Dict[str, Dict[str, Any]] = {
            "options": {},
            "system": {},
            "simulation": {},
        }

    @staticmethod
    def _normalize_solver_type(solver_type: str) -> str:
        """Normalize solver type string.

        Accepts 'sddp', 'mono', or 'monolithic'; returns either 'sddp' or
        'monolithic' (the values understood by the gtopt C++ solver).
        """
        if solver_type in ("mono", "monolithic"):
            return "monolithic"
        return "sddp"

    def process_options(self, options):
        """Process options data to include input and output paths.

        The solver type is emitted at the top level as ``solver_type`` so that
        the gtopt C++ JSON parser maps it directly to ``Options::solver_type``.
        All other SDDP-specific settings are still grouped under the nested
        ``sddp_options`` key.
        """
        if not options:
            options = {}
        discount_rate = options.get("discount_rate", 0.0)
        output_format = options.get("output_format", "parquet")
        input_format = options.get("input_format", output_format)
        compression = options.get("compression", "zstd")
        solver_type = self._normalize_solver_type(options.get("solver_type", "sddp"))

        # Build the nested sddp_options block (all sddp_* fields except solver_type).
        # NOTE: num_apertures is NOT emitted here — the C++ SddpOptions JSON
        # contract has no "num_apertures" field (only "apertures", an array of
        # UIDs).  Aperture configuration is fully handled by aperture_array and
        # per-phase apertures in the simulation section, plus
        # aperture_directory in sddp_options (all set by process_apertures).
        sddp_opts: dict = {}

        cut_sharing_mode = options.get("cut_sharing_mode")
        if cut_sharing_mode is not None:
            sddp_opts["cut_sharing_mode"] = cut_sharing_mode

        max_iter = options.get("max_iterations")
        if max_iter is None:
            # Fall back to PDMaxIte from plpmat.dat if available.
            # PDMaxIte=1 means monolithic (single LP solve) in PLP, so only
            # use values > 1 as SDDP iteration limits.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "max_iterations", 0) > 1:
                    max_iter = plpmat.max_iterations
        if max_iter is not None:
            sddp_opts["max_iterations"] = max_iter

        convergence_tol = options.get("convergence_tol")
        if convergence_tol is None:
            # Fall back to PDError from plpmat.dat; use 0.1 if absent or zero.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "pd_error", 0.0) > 0.0:
                    convergence_tol = plpmat.pd_error
            if convergence_tol is None:
                convergence_tol = 0.1
        sddp_opts["convergence_tol"] = convergence_tol

        # When the JSON file lives inside the output directory (the default),
        # input_directory is "." so paths are relative to the JSON location.
        # When -f places the JSON elsewhere, use the full output_dir path.
        output_dir = Path(options.get("output_dir", ""))
        output_file = Path(options.get("output_file", ""))
        if output_file.parent == output_dir:
            input_dir_val = "."
        else:
            input_dir_val = str(output_dir)

        planning_opts = {
            "solver_type": solver_type,
            "input_directory": input_dir_val,
            "input_format": input_format,
            "output_directory": "results",
            "output_format": output_format,
            "output_compression": compression,
            "use_lp_names": 1,
            "use_single_bus": options.get("use_single_bus", False),
            "use_kirchhoff": options.get("use_kirchhoff", False),
            "demand_fail_cost": options.get("demand_fail_cost", 1000),
            "scale_objective": options.get("scale_objective", 1000),
            "annual_discount_rate": discount_rate,
            "sddp_options": sddp_opts,
        }
        if "reserve_fail_cost" in options:
            planning_opts["reserve_fail_cost"] = options["reserve_fail_cost"]
        if "use_line_losses" in options:
            planning_opts["use_line_losses"] = options["use_line_losses"]
        self.planning["options"] = planning_opts

    def process_stage_blocks(self, options):
        """Calculate first_block and count_block for stages, and build phase_array.

        Phase assignment priority (highest to lowest):

        1. **``stages_phase``** (explicit): A parsed ``--stages-phase`` spec
           (list-of-lists of 1-based PLP stage indices) fully controls the
           mapping regardless of ``solver_type``.
        2. **``solver_type='monolithic'``**: A single phase spanning all stages.
        3. **``solver_type='sddp'``** (default): One phase per PLP stage.

        The ``--stages-phase`` option accepts a string like
        ``"1:4,5,6,7,8,9,10,..."`` (see :func:`~.index_utils.parse_stages_phase`
        for the full syntax).
        """
        stage_parser = self.parser.parsed_data.get("stage_parser", [])
        block_parser = self.parser.parsed_data.get("block_parser", [])

        stages = stage_parser.items
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(block_parser.items)
                if block["stage"] == stage["number"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        self.planning["simulation"]["block_array"] = BlockWriter(
            block_parser=block_parser, options=options
        ).to_json_array()

        self.planning["simulation"]["stage_array"] = StageWriter(
            stage_parser=stage_parser, block_parser=block_parser, options=options
        ).to_json_array(stages)

        num_stages = len(self.planning["simulation"]["stage_array"])
        stages_phase_spec = options.get("stages_phase", None)

        if stages_phase_spec:
            # Explicit stages-phase mapping: parse the spec and build phases.
            # The spec uses 1-based PLP stage indices; convert to 0-based
            # first_stage for gtopt.
            phase_groups = (
                stages_phase_spec
                if isinstance(stages_phase_spec, list)
                else parse_stages_phase(stages_phase_spec, num_stages)
            )
            phase_array = []
            for uid, group in enumerate(phase_groups, start=1):
                # group is a list of 1-based stage indices; convert to
                # 0-based and find the first and count for the contiguous run.
                first_stage_0 = group[0] - 1
                phase_array.append(
                    {
                        "uid": uid,
                        "first_stage": first_stage_0,
                        "count_stage": len(group),
                    }
                )
            self.planning["simulation"]["phase_array"] = phase_array
        else:
            solver_type = self._normalize_solver_type(
                options.get("solver_type", "sddp")
            )
            if solver_type == "monolithic":
                # One phase covering all stages
                self.planning["simulation"]["phase_array"] = [
                    {
                        "uid": 1,
                        "first_stage": 0,
                        "count_stage": num_stages,
                    }
                ]
            else:
                # SDDP: one phase per PLP stage, enabling per-stage state
                # variables (Stochastic Dual Dynamic Programming).
                self.planning["simulation"]["phase_array"] = [
                    {
                        "uid": i + 1,
                        "first_stage": i,
                        "count_stage": 1,
                    }
                    for i in range(num_stages)
                ]

    def process_scenarios(self, options):
        """Process scenario data to include block and stage information.

        **Hydrology index convention (Fortran 1-based)**

        The ``-y`` / ``hydrologies`` option uses **raw 1-based hydrology class
        indices** — the column numbers in ``plpaflce.dat``.  For example,
        ``-y 51,52`` selects hydrology classes 51 and 52 directly from the
        plpaflce flow matrix, regardless of what ``plpidsim.dat`` maps to.

        The special value ``"all"`` selects the active hydrology classes:

        * When ``plpidsim.dat`` is present: the union of all hydrology classes
          referenced by any simulation (using the stage-1 mapping), preserving
          the order in which simulations are listed in the file.
        * When ``plpidsim.dat`` is absent: all hydrology columns (1..N_hydro)
          from ``plpaflce.dat``.

        Use ``plp2gtopt --info -i <input_dir>`` to see which hydrology classes
        are active and what the idsim mapping looks like.

        **Internal representation**

        The 0-based hydrology index (``hydro_1based - 1``) is stored in each
        scenario's ``"hydrology"`` field so that
        :class:`~.aflce_writer.AflceWriter` can look up the correct column
        from the flow matrix.  The C++ solver does not use this field.
        """
        # ---------------------------------------------------------------
        # Resolve the '-y' / 'hydrologies' spec to 1-based hydro indices
        # ---------------------------------------------------------------
        idsim_parser = self.parser.parsed_data.get("idsim_parser")
        hydro_spec = options.get("hydrologies", "all")
        spec = (hydro_spec or "all").strip().lower()

        # Helper: collect all available hydrology indices from PLP data
        def _all_hydro_indices() -> list:
            if idsim_parser is not None and idsim_parser.num_simulations > 0:
                indices: list = []
                seen_h: set = set()
                for sim_idx in range(idsim_parser.num_simulations):
                    h = idsim_parser.get_index(sim_idx, 1)
                    if h is not None and h not in seen_h:
                        seen_h.add(h)
                        indices.append(h)
                return indices
            aflce = self.parser.parsed_data.get("aflce_parser")
            if aflce and aflce.items:
                num_hydro = aflce.items[0].get("num_hydrologies", 1)
            else:
                num_hydro = 1
            return list(range(1, num_hydro + 1))

        if spec == "0":
            # List available scenarios and exit (informational query)
            import sys  # noqa: PLC0415

            all_hydros = _all_hydro_indices()
            source = (
                "plpidsim.dat"
                if (idsim_parser is not None and idsim_parser.num_simulations > 0)
                else "plpaflce.dat"
            )
            _logger.info(
                "Available scenarios (from %s): %d hydrologies",
                source,
                len(all_hydros),
            )
            _logger.info(
                "  Hydrology indices (Fortran 1-based): %s",
                ", ".join(str(h) for h in all_hydros),
            )
            _logger.info(
                "  Use -y <index> or -y <i1>,<i2>,... to select, "
                "or --first-scenario for the first one"
            )
            sys.exit(0)

        if spec in ("all", "first"):
            all_hydros = _all_hydro_indices()
            if spec == "first":
                hydro_indices_1based = all_hydros[:1]
            else:
                hydro_indices_1based = all_hydros
        else:
            # Explicit 1-based raw hydrology column indices (Fortran convention).
            # "-y 55,56" = hydrology classes 55 and 56 from plpaflce.dat.
            # No idsim remapping: the user specifies the hydrology numbers directly.
            hydro_indices_1based = parse_index_range(hydro_spec)

        num_scenarios = len(hydro_indices_1based)

        # Equal probability unless explicitly overridden
        probability_factors = options.get("probability_factors", None)
        if probability_factors is None or len(probability_factors) == 0:
            probability_factors = [1.0 / num_scenarios] * num_scenarios
        else:
            probability_factors = [
                float(factor) for factor in probability_factors.split(",")
            ]

        scenarios = []
        for hydro_1based, factor in zip(hydro_indices_1based, probability_factors):
            # Scenario UID = Fortran 1-based hydrology index (PLP convention).
            # This keeps -y values, scenario UIDs, and aperture source_scenario
            # references all using the same numbering.
            hydro_0based = hydro_1based - 1
            scenarios.append(
                {
                    "uid": hydro_1based,
                    "probability_factor": factor,
                    "hydrology": hydro_0based,
                }
            )
        self.planning["simulation"]["scenario_array"] = scenarios

        solver_type = self._normalize_solver_type(options.get("solver_type", "sddp"))

        if solver_type == "monolithic":
            scenes = [
                {
                    "uid": 1,
                    "first_scenario": 0,
                    "count_scenario": num_scenarios,
                }
            ]
        else:
            scenes = [
                {
                    "uid": i + 1,
                    "first_scenario": i,
                    "count_scenario": 1,
                }
                for i in range(num_scenarios)
            ]
        self.planning["simulation"]["scene_array"] = scenes

    def process_apertures(self, options):
        """Build aperture_array from parsed PLP aperture index files.

        When ``plpidap2.dat`` (or ``plpidape.dat``) is present, the aperture
        definitions are converted to a gtopt ``aperture_array`` where each
        aperture references a ``source_scenario`` by UID.

        If the aperture references hydrologies that are *not* in the
        forward-scenario set, an ``aperture_directory`` is created with the
        extra affluent Parquet files, and the ``sddp_aperture_directory``
        option is set accordingly.

        Aperture configuration is fully handled through aperture_array and
        per-phase apertures in the simulation section.
        """
        if not options:
            return

        idap2_parser = self.parser.parsed_data.get("idap2_parser", None)
        idape_parser = self.parser.parsed_data.get("idape_parser", None)

        if idap2_parser is None and idape_parser is None:
            return

        # Build map: 0-based hydrology index → gtopt scenario UID
        scenarios = self.planning["simulation"].get("scenario_array", [])
        scenario_hydro_map: dict = {}
        forward_hydros: set = set()
        for scen in scenarios:
            hydro_0based = scen.get("hydrology")
            if hydro_0based is not None:
                scenario_hydro_map[hydro_0based] = scen["uid"]
                forward_hydros.add(hydro_0based)

        num_stages = len(self.planning["simulation"].get("stage_array", []))
        max_scenario_uid = max((s["uid"] for s in scenarios), default=0)

        output_dir = Path(options.get("output_dir", ""))
        aperture_dir = output_dir / "apertures"

        result = build_aperture_array(
            idap2_parser=idap2_parser,
            scenario_hydro_map=scenario_hydro_map,
            num_stages=num_stages,
            max_scenario_uid=max_scenario_uid,
            aperture_directory=str(aperture_dir),
        )

        if not result.aperture_array:
            return

        self.planning["simulation"]["aperture_array"] = result.aperture_array

        # Do NOT add aperture-only scenarios to scenario_array — the C++
        # solver builds full LP and output for every scenario in the array.
        # Aperture-only scenarios are served from the aperture_directory;
        # the C++ aperture solver skips source_scenarios it can't find
        # (with an info log) and uses fallback Benders cuts instead.

        # Build hydro→uid map for the extra scenarios (used by parquet writer)
        hydro_uid_map: dict[int, int] = {}
        for es in result.extra_scenarios:
            hydro_uid_map[es["hydrology"]] = es["uid"]

        # Determine which aperture hydros are NOT in the forward set.
        extra_hydros = set(hydro_uid_map.keys())

        # Write aperture-specific affluent data if needed
        if extra_hydros:
            aperture_dir.mkdir(parents=True, exist_ok=True)

            aflce_parser = self.parser.parsed_data.get("aflce_parser", None)
            central_parser = self.parser.parsed_data.get("central_parser", None)
            block_parser = self.parser.parsed_data.get("block_parser", None)

            write_aperture_afluents(
                aflce_parser=aflce_parser,
                central_parser=central_parser,
                block_parser=block_parser,
                aperture_hydros=sorted(extra_hydros),
                forward_hydros=forward_hydros,
                output_dir=aperture_dir,
                options=options,
                hydro_uid_map=hydro_uid_map,
            )

            # Set aperture_directory in sddp_options
            sddp_opts = self.planning["options"].get("sddp_options", {})
            # Use a path relative to the JSON file location (same as
            # input_directory convention).  When the JSON is in output_dir,
            # the aperture directory is just "apertures".
            output_file = Path(options.get("output_file", ""))
            if output_file.parent == output_dir:
                sddp_opts["aperture_directory"] = "apertures"
            else:
                sddp_opts["aperture_directory"] = str(aperture_dir)
            self.planning["options"]["sddp_options"] = sddp_opts

        # NOTE: num_apertures is NOT emitted in sddp_options — the C++
        # SddpOptions JSON contract has no such field.  The aperture count is
        # fully determined by aperture_array and per-phase apertures.

        # Populate per-phase apertures from stage-indexed PLP data
        phase_array = self.planning["simulation"].get("phase_array", [])
        build_phase_apertures(
            idap2_parser=idap2_parser,
            aperture_array=result.aperture_array,
            phase_array=phase_array,
            num_stages=num_stages,
        )

    def process_indhor(self, options):
        """Write block-to-hour map from indhor.csv if present, and record in JSON.

        When the PLP input directory contains ``indhor.csv``, plp_parser will
        have created an ``IndhorParser`` in ``parsed_data["indhor_parser"]``.
        This method writes the data to ``BlockHourMap/block_hour_map.parquet``
        and adds ``"block_hour_map"`` to the simulation section so that
        post-processing tools can reconstruct hourly time-series from
        block-level solver output.

        The ``block_hour_map`` value is the stem path (without file extension)
        relative to the output directory; gtopt resolves the actual format
        (parquet or csv) from the ``input_format`` option.
        """
        indhor_parser = self.parser.parsed_data.get("indhor_parser", None)
        if indhor_parser is None or indhor_parser.is_empty:
            return

        output_dir = Path(options["output_dir"]) if options else Path("results")
        block_hour_dir = output_dir / IndhorWriter.SUBDIR

        writer = IndhorWriter(indhor_parser, options)
        rel_path = writer.to_parquet(block_hour_dir)
        if rel_path:
            self.planning["simulation"]["block_hour_map"] = rel_path

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
        minembh = self.parser.parsed_data.get("minembh_parser", None)
        jw = JunctionWriter(
            central_parser=centrals,
            stage_parser=stages,
            aflce_parser=aflces,
            extrac_parser=extracs,
            manem_parser=manems,
            cenre_parser=cenre,
            cenfi_parser=cenfi,
            filemb_parser=filemb,
            minembh_parser=minembh,
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
                    "conversion_rate": central.get("efficiency", 1.0),
                }
            )

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

        auto_detect = options.get("auto_detect_tech", True)

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

    def process_demands(self, options):
        """Process demand data to include block and stage information."""
        demands = self.parser.parsed_data.get("demand_parser", [])
        if not demands:
            return

        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        dems = demands.get_all()
        for demand in dems:
            bus = buses.get_bus_by_name(demand["name"])
            if bus is None:
                demand["bus"] = 0  # mark as unknown; DemandWriter skips bus==0
            else:
                demand["bus"] = bus["number"]

        blocks = self.parser.parsed_data.get("block_parser", [])
        self.planning["system"]["demand_array"] = DemandWriter(
            demands, blocks, options
        ).to_json_array()

    def process_buses(self):
        """Process bus data to include block and stage information."""
        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        self.planning["system"]["bus_array"] = BusWriter(buses).to_json_array()

    def process_lines(self, options):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        manlis = self.parser.parsed_data.get("manli_parser", None)

        self.planning["system"]["line_array"] = LineWriter(
            lines, blocks, manlis, options
        ).to_json_array()

    def process_battery(self, options):
        """Process battery/ESS data and append to existing arrays."""
        battery_parser = self.parser.parsed_data.get("battery_parser", None)
        ess_parser = self.parser.parsed_data.get("ess_parser", None)
        centrals = self.parser.parsed_data.get("central_parser", None)

        # Proceed if any storage source is available
        has_bat = centrals and any(
            c.get("type") == "bateria" for c in centrals.centrals
        )
        if battery_parser is None and ess_parser is None and not has_bat:
            return

        stages = self.parser.parsed_data.get("stage_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        manbat = self.parser.parsed_data.get("manbat_parser", None)
        maness = self.parser.parsed_data.get("maness_parser", None)

        output_dir = Path(options["output_dir"]) if options else Path("results")

        writer = BatteryWriter(
            battery_parser=battery_parser,
            ess_parser=ess_parser,
            central_parser=centrals,
            bus_parser=buses,
            stage_parser=stages,
            manbat_parser=manbat,
            maness_parser=maness,
            options=options,
        )

        existing_gen = self.planning["system"].get("generator_array", [])
        existing_dem = self.planning["system"].get("demand_array", [])

        result = writer.process(existing_gen, existing_dem, output_dir)

        self.planning["system"]["battery_array"] = result["battery_array"]
        self.planning["system"]["generator_array"] = result["generator_array"]
        self.planning["system"]["demand_array"] = result["demand_array"]
        if "converter_array" in result:
            self.planning["system"]["converter_array"] = result["converter_array"]

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

        # ── Boundary cuts (last stage) ─────────────────────────────────────
        if planos.cuts:
            csv_path = output_dir / "boundary_cuts.csv"
            write_boundary_cuts_csv(planos.cuts, planos.reservoir_names, csv_path)
            sddp_opts["boundary_cuts_file"] = str(csv_path)

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
            )
            # Only wire the file into the JSON options if explicitly requested
            if options.get("hot_start_cuts", False):
                sddp_opts["named_cuts_file"] = str(hs_path)

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
        in ``Options`` rather than per-element fields.

        Scale priority (highest to lowest):
        1. Explicit ``--rsv-energy-scale`` / ``--energy-scale`` name:value.
        2. ``--auto-rsv-energy-scale`` / ``--auto-energy-scale`` (ON default).
        3. ``--variable-scales-file`` entries (lowest priority).

        Auto-scaling is enabled by default.  Use ``--no-auto-rsv-energy-scale``
        and/or ``--no-auto-energy-scale`` to disable.
        """
        if not options:
            return

        has_rsv = "rsv_energy_scale" in options or options.get(
            "auto_rsv_energy_scale", False
        )
        has_bat = "bat_energy_scale" in options or options.get(
            "auto_bat_energy_scale", False
        )
        has_file = "variable_scales_file" in options

        if not has_rsv and not has_bat and not has_file:
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
        if has_rsv:
            explicit_rsv: dict = options.get("rsv_energy_scale", {})
            auto_rsv = options.get("auto_rsv_energy_scale", False)

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

                # Priority 1: explicit --rsv-energy-scale
                if name in explicit_rsv:
                    scale = explicit_rsv[name]
                # Priority 2: auto-rsv-energy-scale
                elif auto_rsv:
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
                    # Also scale flow (extraction) variables with the same
                    # factor so energy-balance coefficients stay O(1).
                    scales.append(
                        {
                            "class_name": "Reservoir",
                            "variable": "flow",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Reservoir", "flow", uid))

        # --- Battery energy scales ---
        if has_bat:
            explicit_energy: dict = options.get("bat_energy_scale", {})
            auto_energy = options.get("auto_bat_energy_scale", False)

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

        # --- Merge file-based scales (lowest priority) ---
        for entry in file_scales:
            key = (entry["class_name"], entry["variable"], entry["uid"])
            if key not in computed_keys and entry["scale"] != 1.0:
                scales.append(entry)

        if scales:
            self.planning["options"]["variable_scales"] = scales

    def to_json(self, options=None) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        if options is None:
            options = {}

        self.process_options(options)
        self.process_stage_blocks(options)
        self.process_indhor(options)
        self.process_scenarios(options)
        self.process_apertures(options)
        self.process_buses()
        self.process_lines(options)
        self.classify_pasada_centrals(options)
        self.process_centrals(options)
        self.process_demands(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)
        self.process_flow_turbines(options)
        self.process_battery(options)
        self.process_boundary_cuts(options)
        self.process_variable_scales(options)

        # Organize into planning structure
        name = options.get("name", "plp2gtopt") if options else "plp2gtopt"
        self.planning["system"]["name"] = name
        version = options.get("sys_version", "") if options else ""
        if version:
            self.planning["system"]["version"] = version

        return self.planning

    def write(self, options=None):
        """Write JSON output to file."""
        if options is None:
            options = {}

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = Path(options["output_file"]) if options else Path("gtopt.json")
        output_file.parent.mkdir(parents=True, exist_ok=True)

        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(options), f, indent=4)
