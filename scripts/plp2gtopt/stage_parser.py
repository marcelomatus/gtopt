#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpeta.dat format files containing stage data."""

from typing import Any, List, Dict
from pathlib import Path


class StageParser:
    """Parser for plpeta.dat format files containing stage data.

    Attributes:
        file_path: Path to the stage file
        stages: List of parsed stage entries
        num_stages: Number of stages in the file
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with stage file path.
        
        Args:
            file_path: Path to plpeta.dat format file (str or Path)
        """
        self.file_path = Path(file_path) if isinstance(file_path, str) else file_path
        self.stages: List[Dict[str, Any]] = []
        self.num_stages = 0

    def parse(self) -> None:
        """Parse the stage file and populate the stages structure.
        
        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        with open(self.file_path, "r", encoding="utf-8") as f:
            # Skip initial comments and empty lines
            lines = []
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    lines.append(line)

        idx = 0
        # Extract just the number part from first line (may have trailing metadata)
        first_line_parts = lines[idx].split()
        self.num_stages = int(first_line_parts[0])
        idx += 1

        for _ in range(self.num_stages):
            # Get stage number and duration (may have trailing metadata)
            parts = lines[idx].split()
            if len(parts) < 2:
                raise ValueError(f"Invalid stage entry at line {idx+1}")

            stage_num = int(parts[2])
            duration = float(parts[4])
            # Calculate discount factor from FactTasa if present, default to 1.0
            discount_factor = 1.0 / float(parts[5]) if len(parts) > 5 else 1.0
            idx += 1

            self.stages.append(
                {
                    "number": stage_num,
                    "duration": duration,
                    "discount_factor": discount_factor,
                }
            )

    def get_stages(self) -> List[Dict[str, Any]]:
        """Return the parsed stages structure."""
        return self.stages

    def get_num_stages(self) -> int:
        """Return the number of stages in the file."""
        return self.num_stages

    def get_stage_by_number(self, stage_num: int) -> Dict[str, Any] | None:
        """Get stage data for a specific stage number."""
        for stage in self.stages:
            if stage["number"] == stage_num:
                return stage
        return None


if __name__ == "__main__":
    import sys

    def main() -> None:
        """Main function to run stage file analysis."""
        if len(sys.argv) != 2:
            print(f"Usage: {sys.argv[0]} <plpeta.dat file>")
            sys.exit(1)

        file_path = Path(sys.argv[1])
        if not file_path.exists():
            print(f"Error: File '{file_path}' not found")
            sys.exit(1)

        parser = StageParser(file_path)
        parser.parse()

        print(f"\nStage File Analysis: {file_path.name}")
        print("=" * 40)
        print(f"Total stages: {parser.get_num_stages()}")

        # Print all stages
        print("\nStage Details:")
        print("=" * 40)
        for stage in parser.get_stages():
            print(f"\nStage: {stage['number']}")
            print(f"  Duration: {stage['duration']}")
            print(f"  Discount Factor: {stage['discount_factor']}")

    main()
