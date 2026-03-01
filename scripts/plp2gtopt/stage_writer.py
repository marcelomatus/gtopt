# -*- coding: utf-8 -*-

"""Writer for converting stage data to JSON format."""

import logging

from typing import Any, Dict, List, TypedDict, Optional, cast
from .base_writer import BaseWriter
from .stage_parser import StageParser
from .block_parser import BlockParser

logger = logging.getLogger(__name__)


class Stage(TypedDict):
    """Represents a stage in the system."""

    uid: int
    first_block: int
    count_block: int
    active: int
    discount_factor: float


class StageWriter(BaseWriter):
    """Converts stage parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        stage_parser: Optional[StageParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a BlockParser instance."""
        super().__init__(stage_parser, options)
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert stage data to JSON array format."""
        if items is None:
            items = self.items or []

        blocks = self.block_parser.items if self.block_parser else []
        last_stage = self._get_last_stage(blocks)

        total_time = 0.0
        json_stages: List[Stage] = []
        for stage in items:
            stage_number = stage["number"]
            if stage_number > last_stage:
                continue

            jstage: Stage = {
                "uid": stage_number,
                "first_block": stage.get("first_block", 0),
                "count_block": stage.get("count_block", -1),
                "active": 1,
                "discount_factor": stage["discount_factor"],
            }
            total_time += stage["duration"]

            json_stages.append(jstage)
        logger.info(
            "Total period: %d [h] %.2f [m]",
            round(total_time, 2),
            round(total_time / (30.0 * 24.0), 2),
        )

        return cast(List[Dict[str, Any]], json_stages)
