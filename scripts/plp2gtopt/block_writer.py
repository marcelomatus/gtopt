# -*- coding: utf-8 -*-

"""Writer for converting block data to JSON format."""

from typing import Any, Dict, List, Optional, TypedDict, cast
from .base_writer import BaseWriter
from .block_parser import BlockParser


class Block(TypedDict):
    """Represents a block in the system."""

    uid: int
    duration: float
    stage: int
    accumulated_time: float


class BlockWriter(BaseWriter):
    """Converts block parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a BlockParser instance."""
        super().__init__(block_parser, options)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert block data to JSON array format."""
        if items is None:
            items = self.items or []

        blocks = self.parser.items if self.parser else []
        last_stage = self._get_last_stage(blocks)

        json_blocks: List[Block] = []
        for block in items:
            stage_number = block["stage"]
            if stage_number > last_stage:
                continue

            jblock: Block = {
                "uid": block["number"],
                "duration": block["duration"],
                "stage": stage_number,
                "accumulated_time": block["accumulated_time"],
            }
            json_blocks.append(jblock)

        return cast(List[Dict[str, Any]], json_blocks)
