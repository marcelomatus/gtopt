#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting block data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .block_parser import BlockParser


class BlockWriter(BaseWriter):
    """Converts block parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_blocks()

    def __init__(self, block_parser: BlockParser):
        """Initialize with a BlockParser instance."""
        super().__init__(block_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert block data to JSON array format."""
        return [
            {"uid": block["number"], "duration": block["duration"]}
            for block in self.blocks
        ]


if __name__ == "__main__":
    BaseWriter.main(BlockWriter, BlockParser)
