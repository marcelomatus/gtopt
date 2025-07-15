# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
from typing import Dict

from pathlib import Path

from .plp_parser import PLPParser

from .block_writer import BlockWriter
from .stage_writer import StageWriter
from .bus_writer import BusWriter
from .central_writer import CentralWriter
from .demand_writer import DemandWriter
from .line_writer import LineWriter


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: PLPParser, options=None):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.options = options
        self.output_path = None

        self.planning = {"options": {}, "system": {}, "simulation": {}}

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
        self.planning["simulation"]["scenario_array"] = [
            {
                "uid": 1,
                "probability_factor": 1.0,
            }
        ]

    def process_central(self, options):
        """Process central data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_array", [])

        stages = self.parser.parsed_data.get("stage_array", None)
        costs = self.parser.parsed_data.get("cost_array", None)
        buses = self.parser.parsed_data.get("bus_array", None)
        self.planning["system"]["generator_array"] = CentralWriter(
            centrals, stages, costs, buses, options
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

    def process_lines(self):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_array", [])
        if not lines:
            return

        self.planning["system"]["line_array"] = LineWriter(lines).to_json_array()

    def to_json(self, options={}) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        self.process_options(options)
        self.process_stage_blocks()
        self.process_buses()
        self.process_lines()
        self.process_central(options)
        self.process_demands(options)

        # Organize into planning structure
        self.planning["system"]["name"] = "plp2gtopt"
        # self.planning["system"]["version"] = "1.0"

        return self.planning

    def write(self, options={}):
        """Write JSON output to file."""
        self.output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir = self.output_dir
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = Path(options["output_file"]) if options else Path("gtopt.json")

        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(options), f, indent=4)
