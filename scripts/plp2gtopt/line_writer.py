#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting line data to JSON format."""

from typing import List, Dict, Any
from pathlib import Path
import json
from .line_parser import LineParser


class LineWriter:
    """Converts line parser data to JSON format used by GTOPT.

    Handles:
    - Converting line data to JSON array format
    - Writing to output files
    - Maintaining format consistency with system_c0.json
    """

    def __init__(self, line_parser: LineParser):
        """Initialize with a LineParser instance.

        Args:
            line_parser: LineParser containing parsed line data
        """
        self.line_parser = line_parser
        self.lines = line_parser.get_lines()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert line data to JSON array format.

        Returns:
            List of line dictionaries in GTOPT JSON format
        """
        json_lines = []
        for line in self.lines:
            json_line = {
                "uid": line["name"],  # Using name as UID
                "name": line["name"],
                "bus_a": line["bus_a"],
                "bus_b": line["bus_b"],
                "r": line["r"],
                "x": line["x"],
                "f_max_ab": line["f_max_ab"],
                "f_max_ba": line["f_max_ba"],
                "voltage": line["voltage"],
                "has_losses": line["has_losses"],
                "is_operational": line["is_operational"],
            }
            json_lines.append(json_line)
        return json_lines

    def write_to_file(self, output_path: Path) -> None:
        """Write line data to JSON file.

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
            raise IOError(f"Failed to write line JSON: {str(e)}") from e

    @staticmethod
    def from_line_file(line_file: Path) -> "LineWriter":
        """Create LineWriter directly from line file.

        Args:
            line_file: Path to plpcnfli.dat file

        Returns:
            LineWriter instance

        Raises:
            FileNotFoundError: If line file doesn't exist
        """
        parser = LineParser(line_file)
        parser.parse()
        return LineWriter(parser)


if __name__ == "__main__":
    import sys
    from pathlib import Path

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.plpcnfli.dat> <output.json>")
        sys.exit(1)

    input_file = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    try:
        writer = LineWriter.from_line_file(input_file)
        writer.write_to_file(output_file)
        print(f"Successfully wrote line data to {output_file}")
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        sys.exit(1)
