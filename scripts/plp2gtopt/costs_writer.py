#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .costs_parser import CostsParser


class CostWriter(BaseWriter):
    """Converts cost parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_costs()

    def __init__(self, cost_parser: CostParser):
        """Initialize with a CostsParser instance."""
        super().__init__(costs_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert cost data to JSON array format.

        Returns:
            List of cost dictionaries with:
            - name (str): Generator name
            - stages (list[int]): Stage numbers
            - costs (list[float]): Cost values

        Note:
            Converts numpy arrays to lists for JSON serialization
        """
        return [
            {
                "name": cost["name"],
                "stages": cost["stages"].tolist(),
                "costs": cost["costs"].tolist(),
            }
            for cost in self.items
        ]


if __name__ == "__main__":
    BaseWriter.main(CostsWriter, CostsParser)
