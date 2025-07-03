#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data."""

from typing import Any


class DemandParser:
    """Parser for plpdem.dat format files containing bus demand data.

    Attributes:
        file_path: Path to the demand file
        demands: List of parsed demand entries
        num_bars: Number of bars in the file
    """

    def __init__(self, file_path: str) -> None:
        self.file_path = file_path
        self.demands: list[dict[str, Any]] = []
        self.num_bars = 0

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure."""
        with open(self.file_path, "r", encoding="utf-8") as f:
            # Skip initial comments and empty lines
            lines = []
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    lines.append(line)

        idx = 0
        self.num_bars = int(lines[idx])
        idx += 1

        for _ in range(self.num_bars):
            # Get bus name (removing quotes and any remaining comments)
            name = lines[idx].strip("'").split("#")[0].strip()
            idx += 1

            # Get number of demand entries
            num_demands = int(lines[idx])
            idx += 1

            # Read demand entries
            demands = []
            for _ in range(num_demands):
                parts = lines[idx].split()
                if len(parts) < 3:
                    raise ValueError(f"Invalid demand entry at line {idx+1}")
                month = int(parts[0])
                stage = int(parts[1])
                demand = float(parts[2])
                demands.append({"mes": month, "etapa": stage, "demanda": demand})
                idx += 1

            self.demands.append({"nombre": name, "demandas": demands})

    def get_demands(self) -> list[dict[str, Any]]:
        """Return the parsed demands structure."""
        return self.demands

    def get_num_bars(self) -> int:
        """Return the number of bars in the file."""
        return self.num_bars

    def get_demand_by_name(self, name: str) -> dict[str, Any] | None:
        """Get demand data for a specific bus name."""
        for demand in self.demands:
            if demand["nombre"] == name:
                return demand
        return None


if __name__ == "__main__":
    import sys
    from pathlib import Path

    def main():
        if len(sys.argv) != 2:
            print(f"Usage: {sys.argv[0]} <plpdem.dat file>")
            sys.exit(1)

        file_path = Path(sys.argv[1])
        if not file_path.exists():
            print(f"Error: File '{file_path}' not found")
            sys.exit(1)

        parser = DemandParser(file_path)
        parser.parse()

        print(f"\nDemand File Analysis: {file_path.name}")
        print("=" * 40)
        print(f"Total bars: {parser.get_num_bars()}")
        print(f"Total demand entries: {sum(len(d['demandas']) for d in parser.get_demands())}")

        # Show first 3 bars as sample
        print("\nSample bars (first 3):")
        for i, demand in enumerate(parser.get_demands()[:3], 1):
            print(f"\nBar {i}: {demand['nombre']}")
            print(f"  Demand entries: {len(demand['demandas'])}")
            print("  First demand entry:")
            first = demand['demandas'][0]
            print(f"    Month: {first['mes']}, Stage: {first['etapa']}, Demand: {first['demanda']}")

    main()
