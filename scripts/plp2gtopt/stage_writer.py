# -*- coding: utf-8 -*-

"""Writer for converting stage data to JSON format."""

import sys

from typing import Any, Dict, List, TypedDict, Optional, cast
from .base_writer import BaseWriter
from .stage_parser import StageParser


class Stage(TypedDict):
    """Represents a stage in the system."""

    uid: int
    first_block: int
    count_block: int
    active: int


class StageWriter(BaseWriter):
    """Converts stage parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        block_parser: Optional[StageParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a BlockParser instance."""
        super().__init__(block_parser, options)

    def _get_last_stage(self) -> int:
        """Get the last stage number from options with validation.
        
        Returns:
            int: The last stage number, or sys.maxsize if invalid/not specified
        """
        DEFAULT_LAST_STAGE = sys.maxsize  # Largest possible integer on the platform
        if not self.options:
            return DEFAULT_LAST_STAGE
            
        try:
            last_stage = int(self.options.get("last_stage", DEFAULT_LAST_STAGE))
            return last_stage if last_stage > 0 else DEFAULT_LAST_STAGE
        except (ValueError, TypeError):
            return DEFAULT_LAST_STAGE

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert stage data to JSON array format."""
        if items is None:
            items = self.items or []

        last_stage = self._get_last_stage()

        json_stages: List[Stage] = []
        for stage in items:
            stage_number = stage["number"]
            if stage_number > last_stage:
                continue

            stage = {
                "uid": stage_number,
                "first_block": stage.get("first_block", 0),
                "count_block": stage.get("count_block", -1),
                "active": 1,
            }
            json_stages.append(stage)

        return cast(List[Dict[str, Any]], json_stages)
