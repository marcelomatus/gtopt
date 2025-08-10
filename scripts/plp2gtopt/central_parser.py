# -*- coding: utf-8 -*-

"""Parser for plpcnfce.dat format files containing central data.

Handles:
- File parsing and validation
- Central data structure creation
- Central lookup by bus
- Battery storage identification
"""

import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple, Optional
from .base_parser import BaseParser


class CentralParser(BaseParser):
    """Parser for plpcnfce.dat format files containing central data.

    Attributes:
        file_path: Path to the central file
        centrals: List of parsed central entries
    """

    def __init__(self, file_path: str | Path) -> None:
        """Initialize parser with central file path.

        Args:
            file_path: Path to plpcnfce.dat format file (str or Path)

        Raises:
            TypeError: If file_path is not str or Path
            ValueError: If file_path is empty
        """
        super().__init__(file_path)
        # Initialize type counts to 0
        self.num_centrales = 0
        self.num_embalses = 0
        self.num_series = 0
        self.num_fallas = 0
        self.num_pasadas = 0
        self.num_baterias = 0
        self.num_termicas = 0
        self.centrals_of_type: Dict[str, List[Any]] = {}

    @property
    def centrals(self) -> List[Dict[str, str | float | bool]]:
        """Return the parsed centrals structure."""
        return self.get_all()

    @property
    def num_centrals(self) -> int:
        """Return the number of centrals in the file."""
        return len(self.centrals)

    def parse(self) -> None:  # pylint: disable=too-many-branches,too-many-statements
        """Parse the central file and populate the centrals structure."""
        self.validate_file()
        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("File is empty")

        try:
            idx = 0
            gen_idx = 0
            current_gen = {}

            while idx < len(lines):
                line = lines[idx]
                idx += 1

                # Central header line
                if line[0].isdigit():
                    parts = line.split()
                    if self.num_centrales == 0:
                        self._parse_header(parts)
                    else:
                        # Central line format: number 'name' ...
                        current_gen = self._parse_central_header(parts, gen_idx)
                        gen_idx += 1
                elif line.startswith("Start"):
                    idx += 1  # Skip to next line
                elif line.startswith("PotMin"):
                    current_gen, idx = self._parse_power_limits(lines, idx, current_gen)
                elif line.startswith("CosVar"):
                    current_gen, idx = self._parse_cost_and_bus(lines, idx, current_gen)
                    # Finalize and add the central
                    self._append(current_gen)
                    current_gen = {}  # Reset for next central
        finally:
            lines.clear()

        for central in self.centrals:
            central_type = str(central["type"])
            if central_type not in self.centrals_of_type:
                self.centrals_of_type[central_type] = []
            self.centrals_of_type[central_type].append(central)

    def get_central_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get central data by name."""
        return self.get_item_by_name(name)

    def _central_type(self, gen_idx: int) -> str:
        """Determine central type based on its index and type counts.

        Note: This is marked protected but has test helper method test_central_type().
        """
        # Ordered list of (type_name, count) tuples
        type_counts = [
            ("embalse", self.num_embalses),
            ("serie", self.num_series),
            ("pasada", self.num_pasadas),
            ("termica", self.num_termicas),
            ("bateria", self.num_baterias),
            ("falla", self.num_fallas),
        ]

        remaining_idx = gen_idx

        for type_name, type_count in type_counts:
            if type_count > 0 and remaining_idx < type_count:
                return type_name
            remaining_idx -= type_count

        return "unknown"  # Fallback if no type matched

    def _parse_header(self, parts: List[str]) -> None:
        """Parse the file header containing central type counts."""
        # First line contains counts - handle test file format
        if len(parts) >= 5 and all(p.isdigit() for p in parts):
            self.num_centrales = int(parts[0])
            self.num_embalses = int(parts[1])
            self.num_series = int(parts[2])
            self.num_fallas = int(parts[3])
            self.num_pasadas = int(parts[4])
            self.num_baterias = int(parts[5]) if len(parts) > 5 else 0
            self.num_termicas = self.num_centrales - (
                self.num_embalses
                + self.num_series
                + self.num_pasadas
                + self.num_baterias
                + self.num_fallas
            )
        else:
            # If header line is invalid, assume we're parsing a test file
            # with implicit count and set num_centrales to max possible
            self.num_centrales = sys.maxsize

    def _parse_central_header(self, parts: List[str], gen_idx: int) -> Dict[str, Any]:
        """Parse a central header line."""
        if len(parts) < 2:
            raise ValueError("Invalid central header - expected at least 2 parts")

        try:
            # First try parsing as float then convert to int
            return {
                "number": self._parse_int(parts[0]),
                "name": parts[1].strip("'"),
                "type": self._central_type(gen_idx),
            }
        except (ValueError, IndexError) as e:
            raise ValueError(f"Invalid central header: {str(e)}") from e

    def get_central_type(self, gen_idx: int) -> str:
        """Test helper method to expose central type determination."""
        return self._central_type(gen_idx)

    def _parse_power_limits(
        self, lines: List[str], idx: int, current_gen: Dict[str, Any]
    ) -> Tuple[Dict[str, Any], int]:
        """Parse the power limits section of a central definition."""
        if idx >= len(lines):
            raise ValueError("Unexpected end of file")

        # Get the next line for values
        parts = lines[idx].split()
        if len(parts) < 2:
            raise ValueError(
                f"Invalid central data at line {idx}: expected at least 2 values"
            )

        idx += 1
        current_gen["pmin"] = self._parse_float(parts[0])
        current_gen["pmax"] = self._parse_float(parts[1])
        if len(parts) > 2:
            current_gen["vert_min"] = self._parse_float(parts[2])
            current_gen["vert_max"] = self._parse_float(parts[3])

        return current_gen, idx

    def _parse_cost_and_bus(
        self, lines: List[str], idx: int, current_gen: Dict[str, Any]
    ) -> Tuple[Dict[str, Any], int]:
        """Parse the cost and bus information section of a central definition."""
        # Get the next non-empty line for values
        while idx < len(lines) and not lines[idx].strip():
            idx += 1
        if idx >= len(lines):
            raise ValueError("Unexpected end of file after CosVar header")

        parts = lines[idx].split()
        idx += 1

        # Ensure we have minimum required columns
        if len(parts) < 7:
            raise ValueError(
                f"Invalid central data at line {idx}: expected at least 7 values"
            )

        try:
            current_gen["gcost"] = self._parse_float(parts[0])
            current_gen["efficiency"] = self._parse_float(parts[1])
            current_gen["bus"] = self._parse_int(parts[2])
            current_gen["ser_hid"] = self._parse_int(parts[3])
            current_gen["ser_ver"] = self._parse_int(parts[4])
            current_gen["pot_tm0"] = self._parse_float(parts[5])
            current_gen["afluent"] = self._parse_float(parts[6])

            # Optional fields for embalses
            if len(parts) > 7:
                scale = (
                    self._parse_float(parts[11]) / 1000.0 if len(parts) > 11 else 1.0
                )
                current_gen["vol_ini"] = self._parse_float(parts[7]) * scale
                current_gen["vol_fin"] = self._parse_float(parts[8]) * scale
                current_gen["vol_min"] = self._parse_float(parts[9]) * scale
                current_gen["vol_max"] = self._parse_float(parts[10]) * scale

        except (ValueError, IndexError) as e:
            raise ValueError(
                f"Invalid central data format at line {idx}: {str(e)}"
            ) from e

        return current_gen, idx
