#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict


class DemandParser:
    """Parser for plpdem.dat format files containing bus demand data.

    Attributes:
        file_path: Path to the demand file
        demands: List of parsed demand entries
        num_bars: Number of bars in the file
    """

    def __init__(self, file_path: str) -> None:
        """Initialize parser with demand file path.
        
        Args:
            file_path: Path to plpdem.dat format file
        """
        self.file_path = Path(file_path)
        self.demands: List[Dict[str, Any]] = []
        self.num_bars: int = 0

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure.
        
        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"Demand file not found: {self.file_path}")

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
                demands.append({"mes": month, "block": stage, "demand": demand})
                idx += 1

            self.demands.append({"name": name, "demands": demands})

    def get_demands(self) -> list[dict[str, Any]]:
        """Return the parsed demands structure."""
        return self.demands

    def get_num_bars(self) -> int:
        """Return the number of bars in the file."""
        return self.num_bars

    def get_demand_by_name(self, name: str) -> dict[str, Any] | None:
        """Get demand data for a specific bus name."""
        for demand in self.demands:
            if demand["name"] == name:
                return demand
        return None


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for demand file analysis.
    
    Args:
        args: Command line arguments (uses sys.argv if None)
    
    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpdem.dat file>", file=sys.stderr)
        return 1

    try:
        parser = DemandParser(args[0])
        parser.parse()

        print(f"\nDemand File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total bars: {parser.get_num_bars()}")
        
        demands = parser.get_demands()
        total_entries = sum(len(d["demands"]) for d in demands)
        print(f"Total demand entries: {total_entries}")

        self._print_demand_stats(demands)
        return 0
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1

def _print_demand_stats(self, demands: List[Dict[str, Any]]) -> None:
    """Print formatted demand statistics."""
    bar_stats = []
    for demand in demands:
        demand_list = demand["demands"]
        count = len(demand_list)
        avg = sum(d["demand"] for d in demand_list) / count if count > 0 else 0
        bar_stats.append({
            "name": demand["name"],
            "count": count,
            "avg": avg
        })

    bar_stats.sort(key=lambda x: x["avg"], reverse=True)
    self._print_stats_table(bar_stats)

def _print_stats_table(self, stats: List[Dict[str, Any]], limit: int = 10) -> None:
    """Print formatted statistics table."""
    print("\nBar Demand Statistics:")
    print("=" * 40)
    
    if len(stats) <= 2 * limit:
        print("All bars (sorted by average demand):")
        for stat in stats:
            self._print_stat_row(stat)
    else:
        print(f"Top {limit} bars by average demand:")
        for stat in stats[:limit]:
            self._print_stat_row(stat)
        
        print(f"\nBottom {limit} bars by average demand:")
        for stat in stats[-limit:]:
            self._print_stat_row(stat)

def _print_stat_row(self, stat: Dict[str, Any]) -> None:
    """Print single statistic row."""
    print(f"\nBar: {stat['name']}")
    print(f"  Demand entries: {stat['count']}")
    print(f"  Average demand: {stat['avg']:.2f}")

if __name__ == "__main__":
    sys.exit(main())
