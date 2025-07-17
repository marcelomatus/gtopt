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
        """Helper to consistently build JSON output items."""
        return {k: v for k, v in fields.items() if v is not None}

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
