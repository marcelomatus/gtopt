#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

from typing import Any, Dict, List
import pandas as pd
from .base_writer import BaseWriter
from .demand_parser import DemandParser


class DemandWriter(BaseWriter):
    """Converts demand parser data to JSON format used by GTOPT."""

    def __init__(self, demand_parser: DemandParser = None):
        """Initialize with a DemandParser instance."""
        super().__init__(demand_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert demand data to JSON array format.

        Returns:
            List of demand dictionaries with:
            - uid (int): Bus number
            - name (str): Bus name
            - bus (str): Bus name (same as name)
            - blocks (list[int]): Block numbers
            - values (list[float]): Demand values

        Note:
            Converts numpy arrays to lists for JSON serialization
        """
        if items is None:
            items = self.items

        json_demands = []
        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            dem = {
                "uid": demand["number"],
                "name": demand["name"],
                "bus": demand.get("bus", demand["name"]),
                "blocks": demand["blocks"].tolist(),
                "values": demand["values"].tolist(),
            }

            json_demands.append(dem)

        return json_demands

    def to_dataframe(self, items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format.
        
        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        if items is None:
            items = self.items

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            # Create Series for this demand
            s = pd.Series(
                data=demand["values"],
                index=demand["blocks"],
                name=demand["name"]
            )
            
            # Add to DataFrame
            df = pd.concat([df, s], axis=1)

        return df


if __name__ == "__main__":
    BaseWriter.main(DemandWriter, DemandParser)
