# -*- coding: utf-8 -*-

"""Writer for converting block data to JSON format."""

from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .block_parser import BlockParser


class BlockWriter(BaseWriter):
    """Converts block parser data to JSON format used by GTOPT."""

    def __init__(self, block_parser: Optional[BlockParser] = None):
        """Initialize with a BlockParser instance."""
        super().__init__(block_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert block data to JSON array format."""
        if items is None:
            items = self.items

        return [
            {
                "uid": block["number"],
                "duration": block["duration"],
                "stage": block["stage"],
            }
            for block in items
        ]
