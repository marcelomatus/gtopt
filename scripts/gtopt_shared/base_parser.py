"""Shared base class for fixed-format text parsers.

Extracted from ``plp2gtopt.base_parser`` so every converter that reads a
fixed-format text dialect (PLP ``.dat``, PSR SDDP/NCP ``.dat``, …) shares
one set of primitives: file handling, name/number indexing, scalar
parsing, and a comment-aware line reader.

The dependency-heavy PLP extras (numpy vectorised numeric blocks,
``compressed_open`` transparent decompression) stay in
``plp2gtopt.base_parser.BaseParser``, which now subclasses this base.
PSR-style readers use :meth:`BaseTextParser.read_data_lines` (plain
Python, configurable comment prefixes — PSR uses ``!``).
"""

from __future__ import annotations

import re
from abc import ABC
from pathlib import Path
from typing import Any


class BaseTextParser(ABC):
    """Abstract base for fixed-format text-file parsers.

    Provides:

    * file-path handling + :meth:`validate_file`,
    * an ordered ``_data`` list with name/number → index maps
      (:meth:`_append`, :meth:`get_item_by_name`,
      :meth:`get_item_by_number`),
    * scalar parsing helpers (:meth:`_parse_int`, :meth:`_parse_float`,
      :meth:`_parse_name`),
    * a comment-aware line reader (:meth:`read_data_lines`).
    """

    def __init__(self, file_path: str | Path) -> None:
        """Initialise the parser with a file path."""
        self.file_path = Path(file_path)
        self._data: list[dict[str, Any]] = []
        self._name_index_map: dict[str, int] = {}
        self._number_index_map: dict[int, int] = {}

    # ── item store ───────────────────────────────────────────────────────
    def _append(self, item: dict[str, Any]) -> int:
        """Append a parsed item and index it by ``name`` / ``number``."""
        idx = len(self._data)
        self._data.append(item)
        if "name" in item:
            self._name_index_map[item["name"]] = idx
        if "number" in item:
            self._number_index_map[item["number"]] = idx
        return idx

    def get_all(self) -> list[dict[str, Any]]:
        """Return all parsed items."""
        return self._data

    @property
    def items(self) -> list[dict[str, Any]]:
        """Return the parsed items."""
        return self.get_all()

    @property
    def num_items(self) -> int:
        """Return the number of parsed items."""
        return len(self._data)

    def get_item_by_name(self, name: str) -> dict[str, Any] | None:
        """Return the item with the given ``name`` (or ``None``)."""
        idx = self._name_index_map.get(name)
        return self._data[idx] if idx is not None else None

    def get_item_by_number(self, number: int) -> dict[str, Any] | None:
        """Return the item with the given ``number`` (or ``None``)."""
        idx = self._number_index_map.get(number)
        return self._data[idx] if idx is not None else None

    # ── validation ───────────────────────────────────────────────────────
    def validate_file(self) -> None:
        """Validate that the input file exists and is a regular file."""
        if not self.file_path.exists():
            raise FileNotFoundError(f"File not found: {self.file_path}")
        if not self.file_path.is_file():
            raise ValueError(f"Path is not a file: {self.file_path}")

    # ── scalar parsing ───────────────────────────────────────────────────
    def _parse_int(self, value: str) -> int:
        """Parse an integer, tolerating zero-padded strings."""
        return int(value.lstrip("0") or 0)

    def _parse_float(self, value: str) -> float:
        """Parse a float."""
        return float(value)

    def _parse_name(self, line: str) -> str:
        """Parse a single-quoted name from the start of a line."""
        match = re.match(r"'([^']+)'", line)
        if not match:
            raise ValueError(f"Invalid name format in line {line}")
        return match.group(1)

    def _parse_array_line(self, line: str, expected_fields: int) -> list[str]:
        """Split a whitespace-delimited line, requiring ``expected_fields``."""
        parts = line.split()
        if len(parts) < expected_fields:
            raise ValueError(
                f"Expected {expected_fields} fields, got {len(parts)} in line: {line}"
            )
        return parts

    def _parse_float_array(self, values: list[str]) -> list[float]:
        """Convert a list of strings to floats."""
        return [self._parse_float(v) for v in values]

    def _parse_int_array(self, values: list[str]) -> list[int]:
        """Convert a list of strings to ints."""
        return [self._parse_int(v) for v in values]

    def _next_idx(self, idx: int, lines: list[str] | None = None) -> int:
        """Advance to the next line index, bounds-checking against ``lines``."""
        idx += 1
        if lines is None or idx < len(lines):
            return idx
        raise IndexError("No more non-empty lines available")

    # ── comment-aware reader (PSR `.dat` etc.) ───────────────────────────
    def read_data_lines(
        self,
        comment_prefixes: tuple[str, ...] = ("!", "#"),
        *,
        encoding: str = "latin-1",
    ) -> list[str]:
        """Return non-blank, non-comment lines (stripped).

        Lines whose first non-whitespace character is one of
        ``comment_prefixes`` are dropped.  PSR SDDP/NCP ``.dat`` files use
        ``!`` for comments; ``#`` is included for convenience.  Read as
        Latin-1 by default (PSR writes Windows-1252 / Latin-1 text).
        """
        out: list[str] = []
        with self.file_path.open("r", encoding=encoding, errors="replace") as fh:
            for raw in fh:
                line = raw.rstrip("\n").rstrip()
                stripped = line.lstrip()
                if not stripped:
                    continue
                if stripped[0] in comment_prefixes:
                    continue
                out.append(line)
        return out
