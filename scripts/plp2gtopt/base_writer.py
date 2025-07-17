# -*- coding: utf-8 -*-

"""Base class for all GTOPT JSON writers.

This module defines the BaseWriter class, which serves as a foundation for creating
GTOPT JSON writers. It provides methods for converting data to JSON format,
and writing it to files. Specific writers should inherit from this class and implement
the required methods for their specific data formats.
"""

import json
from abc import ABC
from pathlib import Path
from typing import Any, Dict, List, Optional, TypeVar

import numpy as np
import pandas as pd

from .base_parser import BaseParser

WriterVar = TypeVar("WriterVar", bound="BaseWriter")
ParserVar = TypeVar("ParserVar", bound="BaseParser")  # Used in type hints


class BaseWriter(ABC):
    """Base class for all GTOPT JSON writers."""

    def get_items(self) -> List[Dict[str, Any]] | None:
        """Get items from the parser."""
        return self.items

    def __init__(self, parser: Optional[ParserVar] = None) -> None:
        """Initialize with a parser instance.

        Args:
            parser: Parser containing parsed data to be written
        """
        self.parser = parser
        self.items = self.parser.get_all() if self.parser else None

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert data to JSON array format."""
        if items is None:
            items = self.items or []
        return items  # Default implementation, override in subclasses

    def write_to_file(self, output_path: Path) -> None:
        """Write data to JSON file."""
        json_data = self.to_json_array()
        output_path.parent.mkdir(parents=True, exist_ok=True)

        try:
            with open(output_path, "w", encoding="utf-8") as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except (IOError, ValueError) as e:
            raise IOError(f"Failed to write JSON to {output_path}: {str(e)}") from e

    def _build_json_item(self, **fields) -> Dict[str, Any]:
        """Help to consistently build JSON output items."""
        return {k: v for k, v in fields.items() if v is not None}

    def _create_dataframe(
        self,
        items: List[Dict[str, Any]],
        value_field: str,
        index_field: str,
        parser: Optional[Any] = None,
        item_key: str = "number",
        index_name: Optional[str] = None,
        filter_fn: Optional[callable] = None,
        fill_values: Optional[Dict[str, float]] = None,
    ) -> pd.DataFrame:
        """Create a DataFrame from items with common processing.

        Args:
            items: List of item dictionaries
            value_field: Field name containing the values
            index_field: Field name containing the index values
            parser: Optional parser for index conversion
            item_key: Key to extract from parser items
            index_name: Name for the index column (defaults to index_field)
            filter_fn: Optional filter function for items
            fill_values: Optional dict of default fill values per column

        Returns:
            Processed DataFrame
        """
        df = pd.DataFrame()
        if not items:
            return df

        index_name = index_name or index_field
        fill_values = fill_values or {}

        for item in items:
            name = item.get("name", "")
            if filter_fn and filter_fn(item):
                continue

            uid = item.get("number", name)
            col_name = f"uid:{uid}" if not isinstance(uid, str) else uid
            if col_name not in fill_values:
                fill_values[col_name] = 0.0

            # Skip if all values match the fill value
            if np.all(item[value_field] == fill_values[col_name]):
                continue

            s = pd.Series(
                data=item[value_field],
                index=item[index_field],
                name=col_name
            )
            s = s.loc[~s.index.duplicated(keep="last")]
            df = pd.concat([df, s], axis=1)

        # Post-processing
        df = df.sort_index().drop_duplicates()

        # Convert index to column
        df = self._convert_index_to_column(
            df,
            index_name=index_name,
            parser=parser,
            item_key=item_key
        )

        # Fill missing values with column-specific defaults
        return df.fillna(fill_values)

    def _convert_index_to_column(
        self,
        df: pd.DataFrame,
        index_name: str,
        parser: Optional[Any] = None,
        item_key: str = "number",
    ) -> pd.DataFrame:
        """Convert DataFrame index to a named column using parser data if available.

        Args:
            df: Input DataFrame
            index_name: Name for the new column (e.g. "block" or "stage")
            parser: Optional parser object containing items data
            item_key: Key to extract from item dictionaries

        Returns:
            DataFrame with index converted to column
        """
        if parser and hasattr(parser, f"num_{index_name}s"):
            items = getattr(parser, f"{index_name}s")
            if items:
                index_values = np.array(
                    [int(item[item_key]) for item in items], dtype=np.int16
                )
                s = pd.Series(data=index_values, index=index_values, name=index_name)
                return pd.concat([s, df], axis=1)

        return df.reset_index().rename(columns={"index": index_name})
