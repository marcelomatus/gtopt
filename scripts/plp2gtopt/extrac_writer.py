# -*- coding: utf-8 -*-

"""Writer for converting extraction data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd
from .base_writer import BaseWriter
from .extrac_parser import ExtracParser


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
        super().__init__(extrac_parser)
        self.options = options or {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert extraction data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "name": extrac["name"],
                "max_extrac": extrac["max_extrac"],
                "downstream": extrac["downstream"],
            }
            for extrac in items
        ]

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> pd.DataFrame:
        """Convert extraction data to pandas DataFrame."""
        if items is None:
            items = self.items or []

        if not items:
            return pd.DataFrame()

        try:
            df = pd.DataFrame(items)
            return df
        except Exception as e:
            raise ValueError(f"Failed to create DataFrame: {str(e)}") from e

    def to_parquet(self, output_path: Path, items=None) -> None:
        """Write extraction data to Parquet file."""
        df = self.to_dataframe(items)

        if df.empty:
            return

        try:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            df.to_parquet(
                output_path,
                index=False,
                compression=self.get_compression(),
            )
        except Exception as e:
            raise IOError(f"Failed to write Parquet file: {str(e)}") from e
