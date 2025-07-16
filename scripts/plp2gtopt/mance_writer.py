# -*- coding: utf-8 -*-

"""Writer for converting maintenance data to JSON format."""

import pandas as pd
from pathlib import Path
from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .mance_parser import ManceParser
from .central_parser import CentralParser


class ManceWriter(BaseWriter):
    """Converts maintenance parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        mance_parser: Optional[ManceParser] = None,
        central_parser: CentralParser = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a ManceParser instance."""
        super().__init__(mance_parser)
        self.central_parser = central_parser
        self.options = options or {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
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

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (pmin/pmax)."""
        df = pd.DataFrame()
        defaults = {}

        for mance in items:
            if len(mance["blocks"]) == 0:
                continue

            cname = mance.get("name", "")
            central = (
                self.central_parser.get_central_by_name(cname)
                if self.central_parser
                else None
            )

            id = central.get("number", cname) if central else mance.get("uid", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            defaults[name] = central.get(field, 0.0) if central else 0.0
            s = pd.Series(data=mance[field], index=mance["blocks"], name=name)
            df = pd.concat([df, s], ignore_index=True)

        # Post-processing
        df = df.sort_index().drop_duplicates()
        df = df.fillna(defaults)
        df.index = df.index.astype("int16")
        df = df.reset_index().rename(columns={"index": "block"})

        return df

    def to_dataframe(self, items=None) -> tuple[pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames for pmin and pmax."""
        if items is None:
            items = self.items

        df_pmin = self._create_dataframe_for_field("p_min", items)
        df_pmax = self._create_dataframe_for_field("p_max", items)

        return df_pmin, df_pmax

    def _write_parquet_for_field(self, df: pd.DataFrame, output_path: Path) -> None:
        """Write a single DataFrame to parquet format."""
        compression = self.options.get("compression", "gzip")
        df.to_parquet(
            output_path, index=False, engine="pyarrow", compression=compression
        )

    def to_parquet(self, output_files: dict, items=None) -> None:
        """Write maintenance data to Parquet files for pmin and pmax."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        df_pmin, df_pmax = self.to_dataframe(items)
        self._write_parquet_for_field(df_pmin, output_dir / output_files["pmin"])
        self._write_parquet_for_field(df_pmax, output_dir / output_files["pmax"])
