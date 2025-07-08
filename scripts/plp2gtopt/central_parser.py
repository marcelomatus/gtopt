#!/usr/bin/env python3
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
from typing import Any, Dict, List, Optional, Tuple, Union
from .base_parser import BaseParser


class CentralParser(BaseParser):
    """Parser for plpcnfce.dat format files containing central data.

    Attributes:
        file_path: Path to the central file
        centrals: List of parsed central entries
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with central file path.

        Args:
            file_path: Path to plpcnfce.dat format file (str or Path)

        Raises:
            TypeError: If file_path is not str or Path
            ValueError: If file_path is empty
        """
        super().__init__(file_path)
        self.centrals: List[Dict[str, Union[str, float, bool]]] = self._data
        # Initialize type counts to 0
        self.num_centrales = 0
        self.num_embalses = 0
        self.num_series = 0
        self.num_fallas = 0
        self.num_pasadas = 0
        self.num_baterias = 0
        self.num_termicas = 0

    def parse(self) -> None:  # pylint: disable
        """Parse the central file and populate the centrals structure.

        The file format expected is:
        - Each central starts with an ID line
        - Followed by power limits (PotMin/PotMax)
        - Then cost and bus information (CosVar/Rendi/Barra)

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid or empty
            IOError: If file cannot be read
        """
        self.validate_file()
        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("File is empty")
        current_gen: Dict[str, Any] = {}

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("File is empty")

        try:
            idx = 0
            gen_idx = 0
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
                        gen_idx += 1
                        current_gen = self._parse_central_header(parts, gen_idx)
                elif line.startswith("Start"):
                    idx += 1  # Skip to next line
                elif line.startswith("PotMin"):
                    current_gen, idx = self._parse_power_limits(lines, idx, current_gen)
                elif line.startswith("CosVar"):
                    current_gen, idx = self._parse_cost_and_bus(lines, idx, current_gen)
                    # Finalize and add the central
                    self._finalize_central(current_gen)
                    current_gen = {}  # Reset for next central
        finally:
            lines.clear()
            del lines

    def get_centrals(self) -> List[Dict[str, Union[str, float, bool]]]:
        """Return the parsed centrals structure.

        Returns:
            List of central dictionaries.
        """
        return self.centrals

    def get_num_centrals(self) -> int:
        """Return the number of centrals in the file."""
        return len(self.centrals)

    @property
    def num_centrals(self) -> int:
        """Return the number of centrals (property version)."""
        return len(self.centrals)

    def get_centrals_by_bus(self, bus_id: Union[str, int]) -> List[Dict[str, Any]]:
        """Get all centrals connected to a specific bus.

        Args:
            bus_id: The bus ID to filter centrals by

        Returns:
            List of unique central dictionaries matching the bus ID
            (unique by id and bus combination)

        Example:
            >>> parser.get_centrals_by_bus("1")
            [{'id': '1', 'name': 'GEN1', ...}]
        """
        seen = set()
        unique_gens = []
        for g in self.centrals:
            if str(g["bus"]) == str(bus_id):  # Handle both string and int bus IDs
                key = (g["number"], g["bus"])
                if key not in seen:
                    seen.add(key)
                    unique_gens.append(g)
        return unique_gens

    def _central_type(self, gen_idx: int) -> str:
        """Determine central type based on its index and type counts."""
        # Ordered list of (type_name, count) tuples
        type_counts = [
            ("embalse", self.num_embalses),
            ("serie", self.num_series),
            ("termica", self.num_termicas),
            ("pasada", self.num_pasadas),
            ("bateria", self.num_baterias),
            ("falla", self.num_fallas),
        ]

        remaining_idx = gen_idx - 1  # Convert to 0-based index

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
                "number": int(parts[0]),
                "name": parts[1].strip("'"),
                "type": self._central_type(gen_idx),
            }
        except (ValueError, IndexError) as e:
            raise ValueError(f"Invalid central header: {str(e)}") from e

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
        current_gen["p_min"] = self._parse_float(parts[0])
        current_gen["p_max"] = self._parse_float(parts[1])
        if len(parts) > 2:
            current_gen["v_max"] = self._parse_float(parts[2])
            current_gen["v_min"] = self._parse_float(parts[3])

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
            current_gen["variable_cost"] = self._parse_float(parts[0])
            current_gen["efficiency"] = self._parse_float(parts[1])
            current_gen["bus"] = int(parts[2])
            current_gen["ser_hid"] = int(parts[3])
            current_gen["ser_ver"] = int(parts[4])
            current_gen["pot_tm0"] = self._parse_float(parts[5])
            current_gen["afluent"] = self._parse_float(parts[6])

            # Optional fields for embalses
            if len(parts) > 7:
                current_gen["vol_ini"] = self._parse_float(parts[7])
                current_gen["vol_fin"] = self._parse_float(parts[8])
                current_gen["vol_min"] = self._parse_float(parts[9])
                current_gen["vol_max"] = self._parse_float(parts[10])
                current_gen["fact_esc"] = self._parse_float(parts[11])

        except (ValueError, IndexError) as e:
            raise ValueError(
                f"Invalid central data format at line {idx}: {str(e)}"
            ) from e

        return current_gen, idx

    def _finalize_central(self, gen: Dict[str, Any]) -> None:
        """Validate and add a completed central to the list."""
        # Only check for truly required fields
        required = {"number", "name", "type"}
        missing = required - gen.keys()
        if missing:
            raise ValueError(
                f"Central {gen.get('id', 'unknown')}"
                "missing required fields: {missing}"
            )

        self.centrals.append(gen)


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for central file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        0 on success, 1 on failure

    Example:
        $ python central_parser.py input.dat
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpgen.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Central file not found: {input_path}")

        parser = CentralParser(str(input_path))
        parser.parse()

        print(f"\nCentral File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total centrals: {parser.get_num_centrals()}")

        centrals = parser.get_centrals()
        for gen in centrals:
            print(f"\nCentral: {gen['id']}")
            print(f"  Bus: {gen['bus']}")
            print(f"  Pmin: {gen['p_min']}")
            print(f"  Pmax: {gen['p_max']}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
