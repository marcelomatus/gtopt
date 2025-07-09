#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .cost_parser import CostParser


class CostWriter(BaseWriter):
    """Converts cost parser data to JSON format used by GTOPT."""

    def __init__(self, cost_parser: CostParser = None):
        """Initialize with a CostParser instance."""
        super().__init__(cost_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert cost data to JSON array format.

        Returns:
            List of cost dictionaries with:
            - name (str): Generator name
            - stages (list[int]): Stage numbers
            - costs (list[float]): Cost values

        Note:
            Converts numpy arrays to lists for JSON serialization
        """
        if items is None:
            items = self.items
        return [
            {
                "name": cost["name"],
                "stages": cost["stages"].tolist(),
                "costs": cost["costs"].tolist(),
            }
            for cost in items
        ]


if __name__ == "__main__":
    BaseWriter.main(CostWriter, CostParser)
