"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
from typing import Optional, List, Dict

from pathlib import Path
from typing import Dict, Union

from .plp_parser import PLPParser

from .block_writer import BlockWriter
from .stage_writer import StageWriter
from .bus_writer import BusWriter
from .central_writer import CentralWriter
from .demand_writer import DemandWriter
from .line_writer import LineWriter


find_bus = lambda bus_name: str, buses: List[Dict] -> Optional[Dict]: (
    """Find a bus by name in the buses list.
    
    Args:
        bus_name: Name of bus to find
        buses: List of bus dictionaries to search
        
    Returns:
        The matching bus dict or None if not found
    """
    next((bus for bus in buses if bus.get("name") == bus_name), None)
)


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: "PLPParser"):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.output_path = None

        self.options = {}
        self.system = {}
        self.simulation = {}

    def process_options(self):
        """Process options data to include input and output paths."""
        self.options = {
            "input_dir": str(self.parser.input_path),
            "output_dir": str(self.output_path),
        }

    def process_stage_blocks(self):
        """Calculate first_block and count_block for stages."""
        stages = self.parser.parsed_data.get("stage_array", []).get_stages()
        blocks = self.parser.parsed_data.get("block_array", []).get_blocks()
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(blocks)
                if block["stage"] == stage["number"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        self.simulation["block_array"] = BlockWriter().to_json_array(blocks)
        self.simulation["stage_array"] = StageWriter().to_json_array(stages)

    def process_central_embalses(self, embalses):
        """Process embalses to include block and stage information."""
        if not embalses:
            return
        pass

    def process_central_series(self, series):
        """Process series to include block and stage information."""
        if not series:
            return
        pass

    def process_central_pasadas(self, pasadas):
        """Process pasadas to include block and stage information."""
        if not pasadas:
            return
        pass

    def process_central_baterias(self, baterias):
        """Process baterias to include block and stage information."""
        if not baterias:
            return
        pass

    def process_central_termicas(self, termicas):
        """Process termicas to include block and stage information."""
        if not termicas:
            return

        self.system["generator_array"] = CentralWriter().to_json_array(termicas)
        pass

    def process_central_fallas(self, fallas):
        """Process fallas to include block and stage information."""
        if not fallas:
            return

        pass

    def process_central(self):
        """Process central data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_array", [])

        ceng = {
            "embalse": [],
            "serie": [],
            "pasada": [],
            "termica": [],
            "bateria": [],
            "falla": [],
        }

        for cen in centrals.get_all():
            ceng[cen["type"]].append(cen)

        self.process_central_embalses(ceng.get("embalse", []))
        self.process_central_series(ceng.get("serie", []))
        self.process_central_pasadas(ceng.get("pasada", []))
        self.process_central_baterias(ceng.get("bateria", []))
        self.process_central_termicas(ceng.get("termica", []))
        self.process_central_fallas(ceng.get("falla", []))

    def process_demands(self):
        """Process demand data to include block and stage information."""
        demands = self.parser.parsed_data.get("demand_array", [])
        if not demands:
            return

        buses = self.parser.parsed_data.get("bus_array", []).get_buses()
        for demand in demands.get_demands():
            bus = find(demand["bus"], buses)
            demand["bus"] = bus["number"] if bus else 0

        self.system["demand_array"] = DemandWriter(demands).to_json_array()

    def process_buses(self):
        """Process bus data to include block and stage information."""
        buses = self.parser.parsed_data.get("bus_array", [])
        if not buses:
            return

        self.system["bus_array"] = BusWriter(buses).to_json_array()

    def process_lines(self):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_array", [])
        if not lines:
            return

        self.system["line_array"] = LineWriter(lines).to_json_array()

    def to_json(self) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        self.process_options()
        self.process_stage_blocks()
        self.process_buses()
        self.process_lines()
        self.process_central()
        self.process_demands()

        # Organize into planning structure

        return {
            "options": self.options,
            "simulation": self.simulation,
            "system": self.system,
        }

    def write(self, output_path: Union[str, Path]):
        """Write JSON output to file."""
        self.output_path = Path(output_path)
        output_path = self.output_path
        output_path.mkdir(parents=True, exist_ok=True)
        output_file = output_path / "plp2gtopt.json"
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(), f, indent=4)
