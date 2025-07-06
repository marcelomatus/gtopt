#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpblo.dat format files containing block data.

Handles:
- File parsing and validation
- Block data structure creation
- Block lookup by number
"""

import sys
from pathlib import Path
from typing import Any, List, Dict, Union, Optional
from .base_parser import BaseParser


class BlockParser(BaseParser):
    """Parser for plpblo.dat format files containing block data.

    Attributes:
        file_path: Path to the block file
        blocks: List of parsed block entries
        num_blocks: Number of blocks in the file
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with block file path.

        Args:
            file_path: Path to plpblo.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.num_blocks = 0

    def parse(self) -> None:
        """Parse the block file and populate the blocks structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        self.num_blocks = self._parse_int(lines[idx])
        idx += 1

        for _ in range(self.num_blocks):
            parts = lines[idx].split()
            if len(parts) < 3:
                raise ValueError(f"Invalid block entry at line {idx+1}")

            self._data.append(
                {
                    "number": self._parse_int(parts[0]),
                    "stage": self._parse_int(parts[1]),
                    "duration": self._parse_float(parts[2]),
                }
            )
            idx += 1

    def get_blocks(self) -> List[Dict[str, Any]]:
        """Return the parsed blocks structure."""
        return self.get_all()

    def get_num_blocks(self) -> int:
        """Return the number of blocks in the file."""
        return self.num_blocks

    def get_block_by_number(self, block_num: int) -> Optional[Dict[str, Any]]:
        """Get block data for a specific block number."""
        for block in self._data:
            if block["number"] == block_num:
                return block
        return None


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for block file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpblo.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Block file not found: {input_path}")

        parser = BlockParser(input_path)
        parser.parse()

        print(f"\nBlock File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total blocks: {parser.get_num_blocks()}")

        # Print all blocks
        print("\nBlock Details:")
        print("=" * 40)
        for block in parser.get_blocks():
            print(f"\nBlock: {block['number']}")
            print(f"  Stage: {block['stage']}")
            print(f"  Duration: {block['duration']}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
