# -*- coding: utf-8 -*-

"""Writer for converting maintenance data to JSON format."""

from typing import Union, Any, Dict, List
from pathlib import Path
from .base_writer import BaseWriter
from .mance_parser import ManceParser
import pandas as pd


class ManceWriter(BaseWriter):
    """Converts maintenance parser data to JSON format used by GTOPT."""

    def __init__(self, mance_parser: ManceParser = None):
        """Initialize with a ManceParser instance."""
        super().__init__(mance_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format.

        Returns:
            List of maintenance dictionaries with:
            - name (str): Generator name
            - months (list[int]): Month numbers
            - blocks (list[int]): Block numbers
            - p_min (list[float]): Minimum power values
            - p_max (list[float]): Maximum power values
        """
        if items is None:
            items = self.items
        return [
            {
                "name": mance["name"],
                "blocks": mance["blocks"].tolist(),
                "p_min": mance["p_min"].tolist(),
                "p_max": mance["p_max"].tolist(),
            }
            for mance in items
        ]

    def to_dataframe(self, items=None) -> pd.DataFrame:
        """Convert maintenance data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers
            - Columns: Maintenance data for each central
        """
        if items is None:
            items = self.items

        df_pmin = pd.DataFrame()
        df_pmax = pd.DataFrame()

        default_pmin = {}
        default_pmax = {}
        for mance in items:
            if len(mance["blocks"]) == 0:
                continue

            # check if mance has a valid central name
            cname = mance.get("name", "")
            central = self.central_parser.get_central_by_name(cname)
            if central is None:
                continue

            # Create Series for the pmax and pmin
            id = central.get("number", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            default_pmin[name] = central.get("p_min", 0.0)
            s_pmin = pd.Series(data=mance["p_min"], index=mance["blocks"], name=name)
            default_pmax[name] = central.get("p_max", 0.0)
            s_pmax = pd.Series(data=mance["p_max"], index=mance["blocks"], name=name)

            # Add to DataFrames
            df_pmin = pd.concat([df_pmin, s_pmin], ignore_index=True)
            df_pmax = pd.concat([df_pmax, s_pmax], ignore_index=True)

        # Ensure blocks are sorted and unique
        df_pmin = df_pmin.sort_index().drop_duplicates()
        df_pmax = df_pmax.sort_index().drop_duplicates()
        # Fill missing values with column-specific defaults
        df_pmin = df_pmin.fillna(default_pmin)
        df_pmax = df_pmax.fillna(default_pmax)
        # Convert to efficient dtypes
        df_pmin = df_pmin.reset_index().rename(columns={"index": "block"})
        df_pmin["block"] = df_pmin["block"].astype("int16")
        df_pmax = df_pmax.reset_index().rename(columns={"index": "block"})
        df_pmax["block"] = df_pmax["block"].astype("int16")

        return df_pmin, df_pmax

    def to_parquet(self, output_files, items=None) -> None:
        """Write maintenance data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        output_file_pmin = output_dir / output_files["pmin"]
        output_file_pmax = output_dir / output_files["pmax"]

        compression = self.options.get("compression", "gzip")

        df_pmin, df_pmax = self.to_dataframe(items)
        df_pmin.to_parquet(
            output_file_pmin, index=False, engine="pyarrow", compression=compression
        )
        df_pmax.to_parquet(
            output_file_pmax, index=False, engine="pyarrow", compression=compression
        )
