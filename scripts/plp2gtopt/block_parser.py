# -*- coding: utf-8 -*-

"""Parser for plpblo.dat format files containing block data.

Handles:
- File parsing and validation
- Block data structure creation
- Block lookup by number
"""

from pathlib import Path
from typing import Any, Dict, List
from .base_parser import BaseParser


class BlockParser(BaseParser):
    """Parser for plpblo.dat format files containing block data.

    Attributes:
        file_path: Path to the block file
        blocks: List of parsed block entries
        num_blocks: Number of blocks in the file
    """

    def __init__(self, file_path: str | Path) -> None:
        """Initialize parser with block file path.

        Args:
            file_path: Path to plpblo.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.stage_number_map: Dict[int, int] = {}

    def parse(self) -> None:
        """Parse the block file and populate the blocks structure."""
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        num_blocks = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_blocks):
            parts = lines[idx].split()
            if len(parts) < 3:
                raise ValueError(f"Invalid block entry at line {idx+1}")

            block: dict[str, Any] = {
                "number": self._parse_int(parts[0]),
                "stage": self._parse_int(parts[1]),
                "duration": self._parse_float(parts[2]),
            }
            self._append(block)
            self.stage_number_map[int(block["number"])] = int(block["stage"])
            idx += 1

    @property
    def blocks(self) -> List[Dict[str, Any]]:
        """Return the parsed blocks structure."""
        return self.get_all()

    @property
    def num_blocks(self) -> int:
        """Return the number of blocks in the file."""
        return len(self.blocks)

    def get_stage_number(self, block_num: int) -> int:
        """Return the stage num for the block."""
        return self.stage_number_map.get(int(block_num), -1)

    def get_stage_numbers(self, block_nums) -> List[int]:
        """Return the stage num for the block."""
        return [self.stage_number_map.get(block_num, -1) for block_num in block_nums]
