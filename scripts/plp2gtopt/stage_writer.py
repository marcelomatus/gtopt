#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting stage data to JSON format."""

from typing import List, Dict, Any
from pathlib import Path
import json
from .stage_parser import StageParser


class StageWriter:
    """Converts stage parser data to JSON format used by GTOPT.

    Handles:
    - Converting stage data to JSON array format
    - Writing to output files
    - Maintaining format consistency with system_c0.json
    """

    def __init__(self, stage_parser: StageParser):
        """Initialize with a StageParser instance.

        Args:
            stage_parser: StageParser containing parsed stage data
        """
        self.stage_parser = stage_parser
        self.stages = stage_parser.get_stages()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert stage data to JSON array format.

        Returns:
            List of stage dictionaries in GTOPT JSON format
        """
        json_stages = []
        for stage in self.stages:
            json_stage = {
                "uid": stage["number"],
                "first_block": stage["number"] - 1,  # Convert to 0-based index
                "count_block": 1,  # Each stage has exactly 1 block
                "active": 1,  # All stages are active by default
            }
            json_stages.append(json_stage)
        return json_stages

    def write_to_file(self, output_path: Path) -> None:
        """Write stage data to JSON file.

        Args:
            output_path: Path to output JSON file

        Raises:
            IOError: If file writing fails
        """
        json_data = self.to_json_array()
        try:
            with open(output_path, "w", encoding="utf-8") as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except IOError as e:
            raise IOError(f"Failed to write stage JSON: {str(e)}") from e

    @staticmethod
    def from_stage_file(stage_file: Path) -> "StageWriter":
        """Create StageWriter directly from stage file.

        Args:
            stage_file: Path to plpeta.dat file

        Returns:
            StageWriter instance

        Raises:
            FileNotFoundError: If stage file doesn't exist
        """
        parser = StageParser(stage_file)
        parser.parse()
        return StageWriter(parser)


if __name__ == "__main__":
    import sys
    from pathlib import Path

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.plpeta.dat> <output.json>")
        sys.exit(1)

    input_file = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    try:
        writer = StageWriter.from_stage_file(input_file)
        writer.write_to_file(output_file)
        print(f"Successfully wrote stage data to {output_file}")
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        sys.exit(1)
