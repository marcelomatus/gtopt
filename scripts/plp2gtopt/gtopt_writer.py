# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
from typing import Dict, Any

from pathlib import Path

from .plp_parser import PLPParser

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
        """Process options data to include input and output paths."""
        if not options:
            options = {}
        discount_rate = options.get("discount_rate", 0.0)
        output_format = options.get("output_format", "parquet")
        input_format = options.get("input_format", output_format)
        compression = options.get("compression", "gzip")
        solver_type = self._normalize_solver_type(options.get("solver_type", "sddp"))
        planning_opts = {
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
            "sddp_solver_type": solver_type,
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

        Hydrology indices in the ``hydrologies`` option follow the Fortran
        (1-based) convention: ``"1"`` means the first hydrology column in the
        PLP data file.  Internally, each hydrology is converted to a 0-based
        array index (used by :class:`~.aflce_writer.AflceWriter`).

        All PLP scenarios are considered equally probable unless explicit
        ``probability_factors`` are provided.  When no explicit weights are
        given, each exported scenario receives probability ``1/N`` where *N*
        is the number of exported hydrologies.  When ``probability_factors``
        is provided, those explicit weights are used instead.

        For ``solver_type='sddp'`` (default), each PLP hydrology becomes its
        own gtopt scenario **and** its own gtopt scene (one-to-one mapping),
        so the SDDP solver processes each scenario independently.

        For ``solver_type='monolithic'`` (or ``'mono'``), all PLP hydrologies
        map to individual gtopt scenarios but are grouped into a **single**
        gtopt scene so the monolithic solver processes them together.
        """
        # Support range syntax like "1,2,5-10,11"; default "1" = first hydrology
        hydro_spec = options.get("hydrologies", "1")
        hydrologies_1based = parse_index_range(hydro_spec)
        num_scenarios = len(hydrologies_1based)

        # Always use 1/N equal probability unless explicitly overridden.
        probability_factors = options.get("probability_factors", None)
        if probability_factors is None or len(probability_factors) == 0:
            probability_factors = [1.0 / num_scenarios] * num_scenarios
        else:
            probability_factors = [
                float(factor) for factor in probability_factors.split(",")
            ]

        scenarios = []
        for i, (hydro_1based, factor) in enumerate(
            zip(hydrologies_1based, probability_factors)
        ):
            uid = i + 1  # Unique 1-based UID per PLP scenario
            # Convert Fortran 1-based hydrology index to 0-based internal index
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
            # One scene with all scenarios grouped together
            scenes = [
                {
                    "uid": 1,
                    "first_scenario": 0,
                    "count_scenario": num_scenarios,
                }
            ]
        else:
            # SDDP: one scene per scenario (first_scenario = 0-based index)
            scenes = [
                {
                    "uid": i + 1,
                    "first_scenario": i,
                    "count_scenario": 1,
                }
                for i in range(num_scenarios)
            ]
        self.planning["simulation"]["scene_array"] = scenes

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
        json_junctions = JunctionWriter(
            central_parser=centrals,
            stage_parser=stages,
            aflce_parser=aflces,
            extrac_parser=extracs,
            manem_parser=manems,
            cenre_parser=cenre,
            cenfi_parser=cenfi,
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

    def to_json(self, options=None) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        if options is None:
            options = {}

        self.process_options(options)
        self.process_stage_blocks(options)
        self.process_scenarios(options)
        self.process_buses()
        self.process_lines(options)
        self.process_centrals(options)
        self.process_demands(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)
        self.process_battery(options)

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
