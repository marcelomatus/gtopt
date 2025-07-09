#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpcnfli.dat format files containing transmission line data.

Handles:
- File parsing and validation
- Line data structure creation
- Line lookup by name or buses
"""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union


from .base_parser import BaseParser


class LineParser(BaseParser):
    """Parser for plpcnfli.dat format files containing line data.

    Attributes:
        file_path: Path to the line file
        lines: List of parsed line entries
        num_lines: Number of lines in the file
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with line file path.

        Args:
            file_path: Path to plpcnfli.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.num_lines: int = 0

    def parse(self) -> None:
        """Parse the line file and populate the lines structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        if not lines:
            raise ValueError("File is empty")

        idx = 0
        # First line contains number of lines and other config
        config_parts = lines[idx].split()
        self.num_lines = self._parse_int(config_parts[0])
        idx += 1

        line_num = 1
        for _ in range(self.num_lines):
            # Line format is:
            # 'Name' F.Max.A-B F.Max.B-A BusA BusB Voltage R(Ohm) X(ohm) Mod.Perd.
            # Num.Tramos Operativa
            line_parts = lines[idx].split()
            if len(line_parts) < 11:
                raise ValueError(f"Invalid line entry at line {idx+1}")

            # Parse line name (removing quotes)
            self._data.append(
                {
                    "number": line_num,
                    "name": line_parts[0].strip("'"),
                    "active": line_parts[10] == "T",  # Operational status
                    "bus_a": int(line_parts[3]),  # Bus A number
                    "bus_b": int(line_parts[4]),  # Bus B number
                    "voltage": float(line_parts[5]),
                    "r": float(line_parts[6]),  # Resistance (Ohm)
                    "x": float(line_parts[7]),  # Reactance (Ohm)
                    "fmax_ab": float(line_parts[1]),  # Forward rating (MW)
                    "fmax_ba": float(line_parts[2]),  # Reverse rating (MW)
                    "mod_perdidas": line_parts[8] == "T",  # Loss modeling flag
                    "num_sections": int(line_parts[9]),  # Number of sections
                    **(
                        {"hvdc": line_parts[11] == "T"} if len(line_parts) > 11 else {}
                    ),  # HVDC line if more than 11 parts
                }
            )
            line_num += 1
            idx += 1

    def get_lines(self) -> List[Dict[str, Any]]:
        """Return the parsed lines structure."""
        return self._data

    def get_num_lines(self) -> int:
        """Return the number of lines in the file."""
        return self.num_lines

    def get_line_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get line data for a specific line name."""
        for line in self._data:
            if line["name"] == name:
                return line
        return None

    def get_lines_by_bus(self, bus_name: str) -> List[Dict[str, Any]]:
        """Get all lines connected to a specific bus."""
        return [
            line for line in self._data if bus_name in (line["bus_a"], line["bus_b"])
        ]

    def get_lines_by_bus_num(self, bus_num: int) -> List[Dict[str, Any]]:
        """Get all lines connected to a specific bus number."""
        return [
            line
            for line in self._data
            if bus_num in (line["bus_a_num"], line["bus_b_num"])
        ]


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for line file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpcnfli.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Line file not found: {input_path}")

        parser = LineParser(str(input_path))
        parser.parse()

        print(f"\nLine File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total lines: {parser.get_num_lines()}")

        lines = parser.get_lines()
        for line in lines[:5]:  # Print first 5 lines as sample
            print(f"\nLine: {line['name']}")
            print(f"  Bus A: {line['bus_a']} (#{line['bus_a_num']})")
            print(f"  Bus B: {line['bus_b']} (#{line['bus_b_num']})")
            print(f"  Voltage: {line['voltage']} kV")
            print(f"  R: {line['r']:.4f} ohm")
            print(f"  X: {line['x']:.4f} ohm")
            print(f"  Fwd Rating: {line['f_max_ab']} MW")
            print(f"  Rev Rating: {line['f_max_ba']} MW")
            print(f"  Losses Modeled: {'Yes' if line['has_losses'] else 'No'}")
            print(f"  Sections: {line['num_sections']}")
            print(f"  Operational: {'Yes' if line['is_operational'] else 'No'}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
