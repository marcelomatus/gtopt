"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
from pathlib import Path
from typing import Dict, Union

from .plp_parser import PLPParser


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: "PLPParser"):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.output_path = None

    def _process_stage_blocks(self):
        """Calculate first_block and count_block for stages."""
        stages = self.parser.parsed_data.get("stage_array", [])
        blocks = self.parser.parsed_data.get("block_array", [])
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(blocks)
                if block["stage"] == stage["uid"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

    def to_json(self) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        self._process_stage_blocks()
        # Convert parser data to JSON arrays
        json_data = {}
        for name, parser in self.parser.parsed_data.items():
            writer_class = globals()[f"{name.split('_')[0].capitalize()}Writer"]
            json_data[name] = writer_class(parser).to_json_array()
        # Organize into planning structure
        options = {
            "input_dir": str(self.parser.input_path),
            "output_dir": str(self.output_path),
        }
        simulation = {}
        for key in ["block_array", "stage_array"]:
            if key in json_data:
                simulation[key] = json_data[key]
                del json_data[key]
        return {"options": options, "simulation": simulation, "system": json_data}

    def write(self, output_path: Union[str, Path]):
        """Write JSON output to file."""
        self.output_path = Path(output_path)
        output_path = self.output_path
        output_path.mkdir(parents=True, exist_ok=True)
        output_file = output_path / "plp2gtopt.json"
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(), f, indent=4)
