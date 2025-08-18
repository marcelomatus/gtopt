# -*- coding: utf-8 -*-

"""Writer for converting extraction data to JSON format."""

from typing import Any, Dict, List, Optional, TypedDict, cast

from .base_writer import BaseWriter
from .extrac_parser import ExtracParser


class Extraction(TypedDict):
    """Represents an extraction entry."""

    name: str
    max_extrac: float
    downstream: str


class ExtracWriter(BaseWriter):
    """Converts extraction parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        extrac_parser: Optional[ExtracParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with an ExtracParser instance.

        Args:
            extrac_parser: Parser for extraction data
            options: Dictionary of writer options
        """
        super().__init__(extrac_parser, options)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert extraction data to JSON array format."""
        if items is None:
            items = self.items or []
        json_extractions: List[Extraction] = [
            {
                "name": extrac["name"],
                "max_extrac": extrac["max_extrac"],
                "downstream": extrac["downstream"],
            }
            for extrac in items
        ]
        return cast(List[Dict[str, Any]], json_extractions)
