# -*- coding: utf-8 -*-

"""Base class for all GTOPT JSON writers.

This module defines the BaseWriter class, which serves as a foundation for creating
GTOPT JSON writers. It provides methods for converting data to JSON format,
and writing it to files. Specific writers should inherit from this class and implement
the required methods for their specific data formats.
"""

import json
import sys
from abc import ABC
from pathlib import Path
from typing import Any, Dict, List, Type, TypeVar
from .base_parser import BaseParser

WriterVar = TypeVar("WriterVar", bound="BaseWriter")
ParserVar = TypeVar("ParserVar", bound=BaseParser.__name__)


class BaseWriter(ABC):
    """Base class for all GTOPT JSON writers."""

    def get_items(self) -> List[Dict[str, Any]]:
        """Get items from the parser."""
        return self.items

    def __init__(self, parser: ParserVar = None) -> None:
        """Initialize with a parser instance.

        Args:
            parser: Parser containing parsed data to be written
        """
        self.parser = parser
        self.items = self.parser.get_all() if self.parser is not None else None

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert data to JSON array format (to be implemented by subclasses)."""
        raise NotImplementedError

    def write_to_file(self, output_path: Path) -> None:
        """Write data to JSON file.

        Args:
            output_path: Path to output JSON file

        Raises:
            IOError: If file writing fails
            ValueError: If JSON conversion fails
        """
        json_data = self.to_json_array()
        try:
            with open(output_path, "w", encoding="utf-8") as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except (IOError, ValueError) as e:
            raise IOError(f"Failed to write JSON: {str(e)}") from e

    @classmethod
    def from_file(
        cls: Type[WriterVar], input_file: Path, parser_class: Type[ParserVar]
    ) -> WriterVar:
        """Create writer directly from input file.

        Args:
            input_file: Path to input data file
            parser_class: Parser class to use

        Returns:
            Writer instance

        Raises:
            FileNotFoundError: If input file doesn't exist
        """
        parser = parser_class(input_file)
        parser.parse()
        return cls(parser)

    @staticmethod
    def main(writer_class: Type[WriterVar], parser_class: Type[ParserVar]) -> None:
        """Run main method for CLI execution.

        Args:
            writer_class: Writer class to use
            parser_class: Parser class to use
        """
        if len(sys.argv) != 3:
            print(f"Usage: {sys.argv[0]} <input.dat> <output.json>")
            sys.exit(1)

        input_file = Path(sys.argv[1])
        output_file = Path(sys.argv[2])

        try:
            writer = writer_class.from_file(input_file, parser_class)
            writer.write_to_file(output_file)
            print(f"Successfully wrote data to {output_file}")
        except (IOError, ValueError, FileNotFoundError) as e:
            print(f"Error: {str(e)}", file=sys.stderr)
            sys.exit(1)
