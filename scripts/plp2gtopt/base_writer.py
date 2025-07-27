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
from typing import Any, Dict, List, Optional, TypeVar, Callable

import numpy as np
import pandas as pd

from .base_parser import BaseParser
from .block_parser import BlockParser
from .stage_parser import StageParser

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
        unit_parser: ParserVar | None,
        index_parser: BlockParser | StageParser | None,
        value_field: str,
        index_field: str,
        index_name: Optional[str] = None,
        fill_field: Optional[str] = None,
        item_key: str = "number",
        skip_types=(),
        value_oper: Callable = lambda x: x,
    ) -> pd.DataFrame:
        """Create a DataFrame from items with common processing."""
        df = pd.DataFrame()
        if not items:
            return df

        fill_values = {}
        for item in items:
            name = item.get("name", "")
            unit = unit_parser.get_item_by_name(name) if unit_parser else None
            if not unit or ("type" in unit and unit["type"] in skip_types):
                continue

            uid = int(unit[item_key]) if item_key in unit else name
            col_name = f"uid:{uid}" if isinstance(uid, int) else uid

            if fill_field and fill_field in unit:
                fill_values[col_name] = unit[fill_field]

            # Skip if all values match the fill value
            values = [value_oper(v) for v in item.get(value_field, [])]
            index = item.get(index_field, [])
            if len(values) * len(index) == 0:
                continue

            if col_name in fill_values and np.allclose(
                values, fill_values[col_name], rtol=1e-8, atol=1e-11
            ):
                continue

            s = pd.Series(data=values, index=index, name=col_name)
            s = s.loc[~s.index.duplicated(keep="last")]
            df = pd.concat([df, s], axis=1)

        if df.empty:
            return df

        # Convert index to column
        index_name = index_name or index_field
        if index_parser and index_parser.items:
            index_values = np.array(
                [item[item_key] for item in index_parser.items], dtype=np.int16
            )
            s = pd.Series(data=index_values, index=index_values, name=index_name)
            df = pd.concat([s, df], axis=1)

        # Fill missing values with column-specific defaults
        df = df.fillna(fill_values)

        return df
