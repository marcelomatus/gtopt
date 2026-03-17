# -*- coding: utf-8 -*-

"""Base class for all GTOPT JSON writers.

This module defines the BaseWriter class, which serves as a foundation for creating
GTOPT JSON writers. It provides methods for converting data to JSON format,
and writing it to files. Specific writers should inherit from this class and implement
the required methods for their specific data formats.
"""

import functools
import json
import sys
from abc import ABC
from pathlib import Path
from typing import Any, Dict, List, Optional, TypeVar, Callable

import numpy as np
import pandas as pd

from .block_parser import BlockParser
from .stage_parser import StageParser
from .base_parser import BaseParser

ParserVar = TypeVar("ParserVar", bound=BaseParser)  # Used in type hints


@functools.lru_cache(maxsize=8)
def _probe_parquet_codec(requested: str) -> str:
    """Return the best available PyArrow Parquet codec for *requested*.

    Uses ``pyarrow.Codec`` to test whether the codec is compiled into the
    linked Arrow library.  Falls back to ``"gzip"`` when *requested* is
    unavailable, logging a warning to *stderr*.  Results are cached via
    ``lru_cache`` so the probe runs **at most once per unique codec name**
    across the entire program run.

    Parameters
    ----------
    requested:
        Codec name to probe (e.g. ``"zstd"``, ``"gzip"``).
        Empty string / ``"none"`` / ``"uncompressed"`` → returns as-is
        (uncompressed; no probe needed).
    """
    if not requested or requested in ("none", "uncompressed"):
        return requested
    try:
        import pyarrow as pa  # noqa: PLC0415 (local import OK inside cached fn)

        pa.Codec(requested)
        return requested
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        print(
            f"Warning: Parquet codec '{requested}' is not available in this "
            "Arrow build; falling back to gzip",
            file=sys.stderr,
        )
        return "gzip"


# Best available Parquet codec — probed once at module import time.
# "zstd" is preferred (matches the C++ default); falls back to "gzip"
# when the linked Arrow library was built without zstd support.
_DEFAULT_COMPRESSION: str = _probe_parquet_codec("zstd")


class BaseWriter(ABC):
    """Base class for all GTOPT JSON writers."""

    def get_items(self) -> Optional[List[Dict[str, Any]]]:
        """Get items from the parser."""
        return self.items

    def __init__(
        self,
        parser: Optional[ParserVar] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a parser instance.

        Args:
            parser: Parser containing parsed data to be written
        """
        self.parser = parser
        self.items = self.parser.get_all() if self.parser else None
        self.options = options if options is not None else {}

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
        """Create a DataFrame from items with common processing.

        Args:
            items: List of dictionaries containing the data
            unit_parser: Parser for unit information
            index_parser: Parser for index information
            value_field: Field name containing the values
            index_field: Field name containing the indices
            index_name: Optional name for the index column
            fill_field: Field name containing fill values
            item_key: Key to use for item identification
            skip_types: Types to skip during processing
            value_oper: Function to apply to values

        Returns:
            DataFrame with processed data
        """
        if not items:
            return pd.DataFrame()

        # Process items into a dictionary of {col_name: (index, values)}
        fill_values = {}
        series = []
        for item in items:
            name = item.get("name", "")
            unit = unit_parser.get_item_by_name(name) if unit_parser else None
            if not unit or ("type" in unit and unit["type"] in skip_types):
                continue

            uid = int(unit[item_key]) if item_key in unit else name
            col_name = self.pcol_name(name, uid)

            if fill_field and fill_field in unit:
                fill_values[col_name] = unit[fill_field]

            values = [value_oper(v) for v in item.get(value_field, [])]
            index = item.get(index_field, [])
            if len(values) * len(index) == 0:
                continue

            # Skip if all values match the fill value
            if col_name in fill_values and np.allclose(
                values, fill_values[col_name], rtol=1e-8, atol=1e-11
            ):
                continue

            s = pd.Series(data=values, index=index, name=col_name)
            s = s.loc[~s.index.duplicated(keep="last")]
            series.append(s)

        if not series:
            return pd.DataFrame()

        df = pd.concat(series, axis=1)

        # Convert index to column
        index_name = index_name or index_field
        if index_parser and index_parser.items:
            index_values = np.array(
                [item[item_key] for item in index_parser.items], dtype=np.int32
            )
            s = pd.Series(data=index_values, index=index_values, name=index_name)
            df = pd.concat([s, df], axis=1)

        # Fill missing values with column-specific defaults
        if fill_values:
            df = df.fillna(fill_values)

        return df

    # Supported compression formats for Parquet files
    VALID_COMPRESSION = ["zstd", "gzip", "snappy", "brotli", "none", "", "uncompressed"]

    # Codecs that map to uncompressed (no-compression) output
    UNCOMPRESSED_ALIASES = {"none", "", "uncompressed"}

    def get_compression(
        self, options: Optional[Dict[str, Any]] = None
    ) -> Optional[str]:
        """Return the best available Parquet codec for this writer.

        Reads the ``"compression"`` key from *options* (defaulting to
        ``_DEFAULT_COMPRESSION``), then passes the value through
        ``_probe_parquet_codec()`` to guarantee the codec is actually
        compiled into the linked Arrow library.  Returns ``None`` for
        uncompressed output.
        """
        if options is None:
            options = self.options

        requested = (
            options.get("compression", _DEFAULT_COMPRESSION)
            if options
            else _DEFAULT_COMPRESSION
        )
        codec = _probe_parquet_codec(requested)
        return None if codec in self.UNCOMPRESSED_ALIASES else codec

    def pcol_name(
        self,
        item_name: str,
        item_number: int | str,
        options: Optional[Dict[str, Any]] = None,
    ) -> str:
        """Get and validate compression option from writer options."""
        if options is None:
            options = self.options

        if isinstance(item_number, str):
            return item_number

        if options.get("use_uid_label", True):
            col_name = f"uid:{item_number}"
        else:
            col_name = f"{item_name}:{item_number}"

        return col_name

    def _get_last_stage(self, blocks=None) -> int:
        """Get the last stage number from options with validation.

        Returns:
            int: The last stage number, or sys.maxsize if invalid/not specified
        """
        default_last_stage = sys.maxsize  # Largest possible integer on the platform
        if not self.options:
            return default_last_stage

        try:
            last_stage = int(self.options.get("last_stage", default_last_stage))
            last_stage = last_stage if last_stage > 0 else default_last_stage
        except (ValueError, TypeError):
            last_stage = default_last_stage

        last_time = (
            float(self.options["last_time"]) if "last_time" in self.options else -1.0
        )

        if last_time > 0 and blocks:
            for block in blocks:
                if block["accumulated_time"] >= last_time:
                    return block["stage"]

        return last_stage
