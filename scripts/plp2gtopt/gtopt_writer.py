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

    def process_options(self, options):
        """Process options data to include input and output paths."""
        self.planning["options"] = {
            "input_directory": str(options.get("output_dir", "")),
            "input_format": "parquet",
            "output_directory": "results",
            "output_format": "parquet",
            "use_lp_names": True,
            "use_single_bus": False,
            "demand_fail_cost": 1000,
            "scale_objective": 1000,
            "use_kirchhoff": True,
            "annual_discount_rate": 0.0,
            "write_lp": True,
        }

    def process_stage_blocks(self):
        """Calculate first_block and count_block for stages."""
        stages = self.parser.parsed_data.get("stage_array", []).stages
        blocks = self.parser.parsed_data.get("block_array", []).blocks
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(blocks)
                if block["stage"] == stage["number"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        self.planning["simulation"]["block_array"] = BlockWriter().to_json_array(blocks)
        self.planning["simulation"]["stage_array"] = StageWriter().to_json_array(stages)

    def process_scenarios(self, options):
        """Process scenario data to include block and stage information."""
        hydrologies = [int(h) for h in options.get("hydrologies", "0").split(",")]
        probability_factors = options.get("probability_factors", None)

        if probability_factors is None or len(probability_factors) < len(hydrologies):
            probability_factors = [1.0 / len(hydrologies)] * len(hydrologies)
        else:
            probability_factors = [
                float(factor) for factor in probability_factors.split(",")
            ]

        scenarios = []
        for hydro_idx, factor in zip(hydrologies, probability_factors):
            scenarios.append(
                {
                    "uid": 1,
                    "probability_factor": factor,
                    "hydrology": hydro_idx,
                }
            )
        self.planning["simulation"]["scenario_array"] = scenarios

    def process_generator_profiles(self, options):
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_array", [])
        blocks = self.parser.parsed_data.get("block_array", None)
        buses = self.parser.parsed_data.get("bus_array", None)
        aflces = self.parser.parsed_data.get("aflce_array", [])
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
        centrals = self.parser.parsed_data.get("central_array", [])
        blocks = self.parser.parsed_data.get("block_array", None)
        aflces = self.parser.parsed_data.get("aflce_array", [])
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
        centrals = self.parser.parsed_data.get("central_array", None)
        aflces = self.parser.parsed_data.get("aflce_array", None)
        extracts = self.parser.parsed_data.get("extract_array", None)
        json_junctions = JunctionWriter(
            centrals,
            aflces,
            extracts,
            options,
        ).to_json_array()

        if not json_junctions:
            return

        for j in json_junctions:
            for key, val in j.items():
                self.planning["system"][key] = val

    def process_centrals(self, options):
        """Process central data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_array", [])
        stages = self.parser.parsed_data.get("stage_array", None)
        blocks = self.parser.parsed_data.get("block_array", None)
        costs = self.parser.parsed_data.get("cost_array", None)
        buses = self.parser.parsed_data.get("bus_array", None)
        mances = self.parser.parsed_data.get("mance_array", None)
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
        demands = self.parser.parsed_data.get("demand_array", [])
        if not demands:
            return

        buses = self.parser.parsed_data.get("bus_array", [])
        if not buses:
            return

        dems = demands.get_all()
        for demand in dems:
            demand["bus"] = buses.get_bus_by_name(demand["name"])["number"]

        blocks = self.parser.parsed_data.get("block_array", [])
        self.planning["system"]["demand_array"] = DemandWriter(
            demands, blocks, options
        ).to_json_array()

    def process_buses(self):
        """Process bus data to include block and stage information."""
        buses = self.parser.parsed_data.get("bus_array", [])
        if not buses:
            return

        self.planning["system"]["bus_array"] = BusWriter(buses).to_json_array()

    def process_lines(self, options):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_array", [])
        blocks = self.parser.parsed_data.get("block_array", None)
        manlis = self.parser.parsed_data.get("manli_array", None)

        self.planning["system"]["line_array"] = LineWriter(
            lines, blocks, manlis, options
        ).to_json_array()

    def to_json(self, options=None) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        if options is None:
            options = {}

        self.process_options(options)
        self.process_stage_blocks()
        self.process_scenarios(options)
        self.process_buses()
        self.process_lines(options)
        self.process_centrals(options)
        self.process_demands(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)

        # Organize into planning structure
        self.planning["system"]["name"] = "plp2gtopt"
        # self.planning["system"]["version"] = "1.0"

        return self.planning

    def write(self, options=None):
        """Write JSON output to file."""
        if options is None:
            options = {}

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = Path(options["output_file"]) if options else Path("gtopt.json")

        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(options), f, indent=4)
