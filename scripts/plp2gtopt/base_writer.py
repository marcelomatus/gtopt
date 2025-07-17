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
from .base_parser import BaseParser

WriterVar = TypeVar("WriterVar", bound="BaseWriter")
ParserVar = TypeVar("ParserVar", bound="BaseParser")


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
