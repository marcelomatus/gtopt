#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpcnfce.dat format files containing generator data.

Handles:
- File parsing and validation
- Generator data structure creation
- Generator lookup by bus
- Battery storage identification
"""

import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Union
from .base_parser import BaseParser


class GeneratorParser(BaseParser):
    """Parser for plpcnfce.dat format files containing generator data.

    Attributes:
        file_path: Path to the generator file
        generators: List of parsed generator entries
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with generator file path.

        Args:
            file_path: Path to plpcnfce.dat format file (str or Path)

        Raises:
            TypeError: If file_path is not str or Path
            ValueError: If file_path is empty
        """
        super().__init__(file_path)
        self.generators: List[Dict[str, Union[str, float, bool]]] = self._data

    def parse(self) -> None:  # pylint: disable
        """Parse the generator file and populate the generators structure.

        The file format expected is:
        - Each generator starts with an ID line
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

        self.num_centrales = None
        lines = self._read_non_empty_lines()
        idx = 0
        gen_idx = 0
        while idx < len(lines):
            line = lines[idx]
            idx += 1

            # Skip comments and empty lines (shouldn't be needed
            # since _read_non_empty_lines() filters them)
            if not line or line.startswith("#"):
                continue

            # Generator header line
            if line[0].isdigit():
                if current_gen:
                    self._finalize_generator(current_gen)

                parts = line.split()
                if self.num_centrales is None:
                    # First line contains counts - handle test file format
                    if len(parts) >= 6 and all(p.isdigit() for p in parts[:6]):
                        self.num_centrales = int(parts[0])
                        self.num_embalses = int(parts[1])
                        self.num_series = int(parts[2])
                        self.num_fallas = int(parts[3])
                        self.num_pasadas = int(parts[4])
                        self.num_baterias = int(parts[5])
                        self.num_termicas = self.num_centrales - (
                            self.num_embalses
                            + self.num_series
                            + self.num_pasadas
                            + self.num_baterias
                            + self.num_fallas
                        )
                    continue  # Skip header line

                else:
                    # Generator line format: number 'name' ...
                    if len(parts) >= 2 and parts[0].isdigit():
                        gen_idx += 1
                        if gen_idx > self.num_centrales:
                            raise ValueError(
                                f"Generator index {gen_idx} exceeds declared number "
                                f"of generators {self.num_centrales}"
                            )

                        current_gen = {
                            "id": str(int(parts[0])),
                            "number": int(parts[0]),
                            "name": parts[1].strip("'"),
                            "type": self._determine_generator_type(gen_idx),
                            "is_battery": False,
                        }
                    else:
                        raise ValueError(f"Invalid generator header at line {idx+1}")

            # Power limits line
            elif line.startswith("PotMin"):
                if not current_gen:
                    continue
                # Get the next line for values
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file after generator header")
                parts = lines[idx].split()
                idx += 1
                current_gen["p_min"] = self._parse_float(parts[0])
                current_gen["p_max"] = self._parse_float(parts[1])
                current_gen["v_min"] = self._parse_float(parts[2])
                current_gen["v_max"] = self._parse_float(parts[3])

            # Cost and bus line
            elif line.startswith("CosVar"):
                if not current_gen:
                    continue
                # Get the next non-empty line for values
                while idx < len(lines) and not lines[idx].strip():
                    idx += 1
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file after CosVar header")
                parts = lines[idx].split()
                idx += 1

                # Ensure we have minimum required columns
                if len(parts) < 3:
                    raise ValueError(
                        f"Invalid generator data at line {idx}: expected at least 3 values"
                    )

                try:
                    current_gen["variable_cost"] = self._parse_float(parts[0])
                    current_gen["efficiency"] = self._parse_float(parts[1])
                    current_gen["bus"] = str(int(parts[2]))  # Bus ID is column 3

                    # Optional fields
                    if len(parts) > 3:
                        current_gen["ser_hid"] = int(parts[3])
                    if len(parts) > 4:
                        current_gen["ser_ver"] = int(parts[4])
                    if len(parts) > 5:
                        current_gen["pot_tm0"] = self._parse_float(parts[5])
                    if len(parts) > 6:
                        current_gen["afluent"] = self._parse_float(parts[6])

                except (ValueError, IndexError) as e:
                    raise ValueError(
                        f"Invalid generator data format at line {idx}: {str(e)}"
                    ) from e

                # Check for battery - based on type only since name isn't reliable
                current_gen["is_battery"] = current_gen.get("type") == "bateria"

                # Finalize and add the generator
                self._finalize_generator(current_gen)
                current_gen = {}  # Reset for next generator
        # Add last generator if exists
        if current_gen:
            self._finalize_generator(current_gen)

    def _finalize_generator(self, gen: Dict[str, Any]) -> None:
        """Validate and add a completed generator to the list.

        Args:
            gen: Generator dictionary to validate and add

        Raises:
            ValueError: If required generator fields are missing/invalid
        """
        required_fields = {
            "number",
            "name",
            "bus",
            "p_min",
            "p_max",
            "variable_cost",
            "efficiency",
            "type",
        }

        # Set default values for missing required fields
        defaults = {
            "bus": "0",
            "variable_cost": 0.0,
            "efficiency": 1.0,
            "p_min": 0.0,
            "p_max": 0.0,
        }

        for field, default in defaults.items():
            if field not in gen:
                gen[field] = default

        # Only check for truly required fields
        required = {"number", "name", "type"}
        missing = required - gen.keys()
        if missing:
            raise ValueError(
                f"Generator {gen.get('id', 'unknown')} missing required fields: {missing}"
            )

        self.generators.append(gen)

    def get_generators(self) -> List[Dict[str, Union[str, float, bool]]]:
        """Return the parsed generators structure.

        Returns:
            List of generator dictionaries with these guaranteed keys:
            - id (str): Generator identifier
            - name (str): Generator name
            - bus (str): Connected bus ID
            - p_min (float): Minimum power output
            - p_max (float): Maximum power output
            - variable_cost (float): Cost per unit power
            - efficiency (float): Conversion efficiency
            - is_battery (bool): True if battery storage
        """
        return self.generators

    def get_num_generators(self) -> int:
        """Return the number of generators in the file."""
        return len(self.generators)

    @property
    def num_generators(self) -> int:
        """Return the number of generators (property version)."""
        return len(self.generators)

    def _determine_generator_type(self, gen_idx: int) -> str:
        """Determine generator type based on its index and type counts.

        Args:
            gen_idx: 1-based generator index

        Returns:
            Generator type as string ("embalse", "serie", etc)

        Raises:
            ValueError: If generator index exceeds total declared generators
        """
        if gen_idx > self.num_centrales:
            raise ValueError(
                f"Generator index {gen_idx} exceeds declared count {self.num_centrales}"
            )

        # Ordered list of (type_name, count) tuples
        type_counts = [
            ("embalse", self.num_embalses),
            ("serie", self.num_series),
            ("pasada", self.num_pasadas),
            ("termica", self.num_termicas),
            ("bateria", self.num_baterias),
            ("fallas", self.num_fallas),
        ]

        remaining_idx = gen_idx - 1  # Convert to 0-based index

        for type_name, type_count in type_counts:
            if type_count > 0 and remaining_idx < type_count:
                return type_name
            remaining_idx -= type_count

        return "unknown"  # Fallback if no type matched

    def get_generators_by_bus(self, bus_id: Union[str, int]) -> List[Dict[str, Any]]:
        """Get all generators connected to a specific bus.

        Args:
            bus_id: The bus ID to filter generators by

        Returns:
            List of unique generator dictionaries matching the bus ID
            (unique by id and bus combination)

        Example:
            >>> parser.get_generators_by_bus("1")
            [{'id': '1', 'name': 'GEN1', ...}]
        """
        seen = set()
        unique_gens = []
        for g in self.generators:
            if str(g["bus"]) == str(bus_id):  # Handle both string and int bus IDs
                key = (g["number"], g["bus"])
                if key not in seen:
                    seen.add(key)
                    unique_gens.append(g)
        return unique_gens


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for generator file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        0 on success, 1 on failure

    Example:
        $ python generator_parser.py input.dat
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpgen.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Generator file not found: {input_path}")

        parser = GeneratorParser(str(input_path))
        parser.parse()

        print(f"\nGenerator File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total generators: {parser.get_num_generators()}")

        generators = parser.get_generators()
        for gen in generators:
            print(f"\nGenerator: {gen['id']}")
            print(f"  Bus: {gen['bus']}")
            print(f"  Pmin: {gen['p_min']}")
            print(f"  Pmax: {gen['p_max']}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
