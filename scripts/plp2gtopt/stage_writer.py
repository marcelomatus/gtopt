#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting stage data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .stage_parser import StageParser


class StageWriter(BaseWriter):
    """Converts stage parser data to JSON format used by GTOPT."""

    def __init__(self, stage_parser: StageParser):
        """Initialize with a StageParser instance."""
        super().__init__(stage_parser)
        self.stages = stage_parser.get_stages()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert stage data to JSON array format."""
        return [
            {
                "uid": stage["number"],
                "first_block": stage["number"] - 1,  # Convert to 0-based index
                "count_block": 1,  # Each stage has exactly 1 block
                "active": 1,  # All stages are active by default
            }
            for stage in self.stages
        ]


if __name__ == "__main__":
    BaseWriter.main(StageWriter, StageParser)
