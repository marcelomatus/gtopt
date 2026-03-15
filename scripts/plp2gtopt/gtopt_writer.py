# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
from typing import Dict, Any

from pathlib import Path

from .plp_parser import PLPParser
from .planos_writer import write_boundary_cuts_csv

from .block_writer import BlockWriter
from .stage_writer import StageWriter
from .bus_writer import BusWriter
from .central_writer import CentralWriter

from .generator_profile_writer import GeneratorProfileWriter
from .demand_writer import DemandWriter
from .line_writer import LineWriter
from .junction_writer import JunctionWriter
from .aflce_writer import AflceWriter
from .battery_writer import BatteryWriter
from .index_utils import parse_index_range, parse_stages_phase
from .indhor_writer import IndhorWriter
from .aperture_writer import (
    build_aperture_array,
    build_phase_aperture_sets,
    write_aperture_afluents,
)


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
        compression = options.get("compression", "gzip")
        solver_type = self._normalize_solver_type(options.get("solver_type", "sddp"))

        # Build the nested sddp_options block (all sddp_* fields except solver_type).
        sddp_opts: dict = {}
        num_apertures = options.get("num_apertures")
        if num_apertures is not None:
            spec_str = str(num_apertures).strip().lower()
            # "all", empty, or "-1" → auto-detect in process_apertures; don't set here
            if spec_str not in ("all", "", "-1"):
                try:
                    # Try as a plain integer first (handles "0", "5", etc.)
                    n = int(spec_str)
                    if n >= 0:
                        sddp_opts["sddp_num_apertures"] = n
                    # negative (e.g. -2) treated as "all" → not set
                except ValueError:
                    # Not a plain integer → treat as range or comma list
                    try:
                        indices = parse_index_range(num_apertures)
                        sddp_opts["sddp_num_apertures"] = len(indices)
                    except (ValueError, TypeError):
                        pass  # Ignore invalid spec; process_apertures auto-detects

        cut_sharing_mode = options.get("cut_sharing_mode")
        if cut_sharing_mode is not None:
            sddp_opts["sddp_cut_sharing_mode"] = cut_sharing_mode

        planning_opts = {
            "solver_type": solver_type,
            "input_directory": str(options.get("output_dir", "")),
            "input_format": input_format,
            "output_directory": "results",
            "output_format": output_format,
            "output_compression": compression,
            "use_lp_names": True,
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

        if spec == "all":
            if idsim_parser is not None and idsim_parser.num_simulations > 0:
                # Collect the active hydrology classes from plpidsim.dat in
                # simulation order (stage 1).  get_index: 0-based sim, 1-based stage.
                hydro_indices_1based: list = []
                seen: set = set()
                for sim_idx in range(idsim_parser.num_simulations):
                    h = idsim_parser.get_index(sim_idx, 1)
                    if h is not None and h not in seen:
                        seen.add(h)
                        hydro_indices_1based.append(h)
            else:
                # No idsim → all raw hydrology columns in plpaflce.dat
                aflce = self.parser.parsed_data.get("aflce_parser")
                if aflce and aflce.items:
                    num_hydro = aflce.items[0].get("num_hydrologies", 1)
                else:
                    num_hydro = 1
                hydro_indices_1based = list(range(1, num_hydro + 1))
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
        for i, (hydro_1based, factor) in enumerate(
            zip(hydro_indices_1based, probability_factors)
        ):
            uid = i + 1  # 1-based UID
            # Store 0-based index for plpaflce.dat column lookup
            hydro_0based = hydro_1based - 1
            scenarios.append(
                {
                    "uid": uid,
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

        This method also sets ``sddp_num_apertures`` automatically when the
        PLP aperture files are present and the user didn't explicitly set it.
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

        aperture_array = build_aperture_array(
            idap2_parser=idap2_parser,
            scenario_hydro_map=scenario_hydro_map,
            num_stages=num_stages,
        )

        if not aperture_array:
            return

        self.planning["simulation"]["aperture_array"] = aperture_array

        # Determine which aperture hydros are NOT in the forward set.
        # Only consider stages that are actually included in the output
        # (num_stages); plpidap2/plpidape are stage-indexed and the late
        # stages may reference hydros outside the forward set.
        aperture_hydros_0based: list = []
        if idap2_parser is not None:
            for entry in idap2_parser.items:
                if 1 <= entry["stage"] <= num_stages:
                    for h in entry["indices"]:
                        aperture_hydros_0based.append(h - 1)
        if idape_parser is not None:
            for entry in idape_parser.items:
                if 1 <= entry["stage"] <= num_stages:
                    for h in entry["indices"]:
                        aperture_hydros_0based.append(h - 1)

        extra_hydros = set(aperture_hydros_0based) - forward_hydros

        # Write aperture-specific affluent data if needed
        if extra_hydros:
            output_dir = Path(options.get("output_dir", ""))
            aperture_dir = output_dir / "apertures"
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
            )

            # Set aperture_directory in sddp_options
            sddp_opts = self.planning["options"].get("sddp_options", {})
            sddp_opts["sddp_aperture_directory"] = str(aperture_dir)
            self.planning["options"]["sddp_options"] = sddp_opts

        # Auto-set num_apertures from PLP data when not explicitly configured
        sddp_opts = self.planning["options"].get("sddp_options", {})
        if "sddp_num_apertures" not in sddp_opts:
            sddp_opts["sddp_num_apertures"] = len(aperture_array)
            self.planning["options"]["sddp_options"] = sddp_opts

        # Populate per-phase aperture_set from stage-indexed PLP data
        phase_array = self.planning["simulation"].get("phase_array", [])
        build_phase_aperture_sets(
            idap2_parser=idap2_parser,
            aperture_array=aperture_array,
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
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", [])
        scenarios = self.planning["simulation"]["scenario_array"]

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir = output_dir / "Afluent"
        output_dir.mkdir(parents=True, exist_ok=True)

        aflce_writer = AflceWriter(
            aflces,
            centrals,
            blocks,
            scenarios,
            options,
        )

        aflce_writer.to_parquet(output_dir)

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
        json_junctions = JunctionWriter(
            central_parser=centrals,
            stage_parser=stages,
            aflce_parser=aflces,
            extrac_parser=extracs,
            manem_parser=manems,
            cenre_parser=cenre,
            cenfi_parser=cenfi,
            filemb_parser=filemb,
            options=options,
        ).to_json_array()

        if not json_junctions:
            return

        for j in json_junctions:
            for key, val in j.items():
                self.planning["system"][key] = val

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
        """Write boundary-cut CSV from parsed PLP planos data.

        If the PLP input contained ``plpplaem1.dat`` and ``plpplaem2.dat``,
        the parsed boundary cuts are written to a CSV file in the output
        directory and the ``sddp_boundary_cuts_file`` option is set so that
        the gtopt SDDP solver loads them as the future-cost approximation
        for the last planning stage (the ``varphi`` boundary condition).
        """
        planos = self.parser.parsed_data.get("planos_parser")
        if planos is None or not planos.cuts:
            return

        output_dir = Path(options.get("output_dir", ""))
        csv_path = output_dir / "boundary_cuts.csv"
        write_boundary_cuts_csv(planos.cuts, planos.reservoir_names, csv_path)

        # Set the option so the C++ solver picks up the file
        sddp_opts = self.planning["options"].setdefault("sddp_options", {})
        sddp_opts["sddp_boundary_cuts_file"] = str(csv_path)

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
        self.process_centrals(options)
        self.process_demands(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)
        self.process_battery(options)
        self.process_boundary_cuts(options)

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
