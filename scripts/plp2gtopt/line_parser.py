# -*- coding: utf-8 -*-

"""Parser for plpcnfli.dat format files containing transmission line data.

Handles:
- File parsing and validation
- Line data structure creation
- Line lookup by name or buses
"""

from typing import Any, Dict, List, Optional
from .base_parser import BaseParser


class LineParser(BaseParser):
    """Parser for plpcnfli.dat format files containing line data.

    Attributes:
        file_path: Path to the line file
        _data: List of parsed line entries
        num_lines: Number of lines in the file
        _name_index_map: Dict mapping names to indices
        _number_index_map: Dict mapping numbers to indices
    """

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
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

        try:
            idx = 0
            # First line contains number of lines and other config
            config_parts = lines[idx].split()
            num_lines = self._parse_int(config_parts[0])
            idx += 1

            line_num = 1
            for _ in range(num_lines):
                # Line format is:
                # 'Name' F.Max.A-B F.Max.B-A BusA BusB Voltage R(Ohm) X(ohm) Mod.Perd.
                # Num.Tramos Operativa
                line_parts = lines[idx].split()
                if len(line_parts) < 11:
                    raise ValueError(f"Invalid line entry at line {idx + 1}")

                # Parse line name (removing quotes)
                self._append(
                    {
                        "number": line_num,
                        "name": line_parts[0].strip("'"),
                        "operational": int(line_parts[10] == "T"),  # Operational status
                        "bus_a": int(line_parts[3]),  # Bus A number
                        "bus_b": int(line_parts[4]),  # Bus B number
                        "voltage": float(line_parts[5]),
                        "r": float(line_parts[6]),  # Resistance (Ohm)
                        "x": float(line_parts[7]),  # Reactance (Ohm)
                        "tmax_ab": float(line_parts[1]),  # Forward rating (MW)
                        "tmax_ba": float(line_parts[2]),  # Reverse rating (MW)
                        "mod_perdidas": line_parts[8] == "T",  # Loss modeling flag
                        "num_sections": int(line_parts[9]),  # Number of sections
                        **(
                            {"hvdc": line_parts[11] == "T"}
                            if len(line_parts) > 11
                            else {}
                        ),  # HVDC line if more than 11 parts
                    }
                )
                line_num += 1
                idx += 1
        finally:
            lines.clear()

    @property
    def lines(self) -> List[Dict[str, Any]]:
        """Return the parsed lines structure."""
        return self.get_all()

    @property
    def num_lines(self) -> int:
        """Return the number of lines in the file."""
        return len(self.lines)

    def get_line_by_name(self, name: str) -> Dict[str, Any] | None:
        """Get line data for a specific line name."""
        return self.get_item_by_name(name)
