# -*- coding: utf-8 -*-

"""Writer for converting stage data to JSON format."""

from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .stage_parser import StageParser


class StageWriter(BaseWriter):
    """Converts stage parser data to JSON format used by GTOPT."""

    def __init__(self, stage_parser: Optional[StageParser] = None):
        """Initialize with a StageParser instance."""
        super().__init__(stage_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert stage data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "uid": stage["number"],
                "first_block": stage.get("first_block", 0),
                "count_block": stage.get("count_block", -1),
                "active": 1,
            }
            for stage in items
        ]
