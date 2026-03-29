"""Base parser class for PLP file parsers."""

import re
from pathlib import Path
from typing import Any, Dict, List, Optional
from abc import ABC, abstractmethod

import numpy as np

from .compressed_open import compressed_open

# Compiled regex for _read_non_empty_lines: matches non-empty, non-comment
# lines and captures the stripped content.  The pattern skips leading
# whitespace, rejects lines starting with '#', and strips trailing whitespace.
# Uses C-level regex engine for ~2x speedup over Python per-line strip+startswith.
_RE_DATA_LINE = re.compile(r"^[ \t]*([^#\s][^\n]*\S|[^#\s])[ \t]*$", re.MULTILINE)


class BaseParser(ABC):
    """Abstract base class for PLP file parsers.

    Provides common functionality for all parsers:
    - File path handling
    - Basic validation
    - Common interface
    """

    def __init__(self, file_path: str | Path) -> None:
        """Initialize parser with file path."""
        self.file_path = Path(file_path)
        self._data: List[Dict[str, Any]] = []
        self._name_index_map: Dict[str, int] = {}  # Maps names to indices
        self._number_index_map: Dict[int, int] = {}  # Maps number to indices

    def _append(self, item: Dict[str, Any]) -> int:
        """Validate and add a completed item to the list."""
        idx = len(self._data)
        self._data.append(item)
        if "name" in item:
            self._name_index_map[item["name"]] = idx
        if "number" in item:
            self._number_index_map[item["number"]] = idx
        return idx

    def _parse_array_line(self, line: str, expected_fields: int) -> List[Any]:
        """Parse a line containing array data with field count validation."""
        parts = line.split()
        if len(parts) < expected_fields:
            raise ValueError(
                f"Expected {expected_fields} fields, got {len(parts)} in line: {line}"
            )
        return parts

    def _parse_float_array(self, values: List[str]) -> List[float]:
        """Convert string array to float array."""
        return [self._parse_float(v) for v in values]

    def _parse_int_array(self, values: List[str]) -> List[int]:
        """Convert string array to int array."""
        return [self._parse_int(v) for v in values]

    @abstractmethod
    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the input file."""

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all parsed items."""
        return self._data

    @property
    def items(self) -> List[Dict[str, Any]]:
        """Return the parsed blocks structure."""
        return self.get_all()

    @property
    def num_items(self) -> int:
        """Return the number of blocks in the file."""
        return len(self.items)

    def get_item_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get item by name.

        Args:
            name: Name of item to retrieve

        Returns:
            The item dictionary if found, None otherwise
        """
        return (
            self._data[self._name_index_map[name]]
            if name in self._name_index_map
            else None
        )

    def get_item_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get itemg by name."""
        return (
            self._data[self._number_index_map[number]]
            if number in self._number_index_map
            else None
        )

    def validate_file(self) -> None:
        """Validate input file exists and is readable."""
        if not self.file_path.exists():
            raise FileNotFoundError(f"File not found: {self.file_path}")
        if not self.file_path.is_file():
            raise ValueError(f"Path is not a file: {self.file_path}")

    def _read_non_empty_lines(self) -> List[str]:
        """Read file and return non-empty, non-comment lines.

        Uses ASCII encoding with errors='ignore' to handle PLP files that may
        contain non-ASCII characters (e.g., accented letters in comments).
        Reads the entire file at once and uses a compiled regex to filter
        blank and comment lines in a single C-level pass.
        """
        with compressed_open(self.file_path) as f:
            content = f.read()
        # Match lines that have at least one non-whitespace character
        # and do not start with '#' (after optional leading whitespace).
        return _RE_DATA_LINE.findall(content)

    def _parse_int(self, value: str) -> int:
        """Parse integer handling zero-padded strings."""
        return int(value.lstrip("0") or 0)

    def _parse_float(self, value: str) -> float:
        """Parse float handling zero-padded strings."""
        return float(value)

    def _parse_name(self, line: str) -> str:
        """Parse a name from a line, removing quotes."""
        match = re.match(r"'([^']+)'", line)
        if not match:
            raise ValueError(f"Invalid name format in line {line}")
        return match.group(1)

    def _next_idx(self, idx: int, lines=None) -> int:
        """Advance to the next non-empty line."""
        idx += 1
        if lines is None or idx < len(lines):
            return idx
        raise IndexError("No more non-empty lines available")

    def _parse_numeric_block(
        self,
        lines: List[str],
        start_idx: int,
        num_rows: int,
        int_cols: tuple[int, ...] = (),
        float_cols: tuple[int, ...] = (),
    ) -> tuple[int, dict[int, np.ndarray]]:
        """Parse a block of numeric lines into numpy arrays (vectorized).

        Args:
            lines: All non-empty lines from the file.
            start_idx: Index of the first data line in *lines*.
            num_rows: Number of data lines to parse.
            int_cols: 0-based column indices to extract as int32 arrays.
            float_cols: 0-based column indices to extract as float64 arrays.

        Returns:
            (next_idx, columns) where *columns* maps column index → ndarray.
        """
        if num_rows <= 0:
            columns_empty: dict[int, np.ndarray] = {}
            for ci in int_cols:
                columns_empty[ci] = np.empty(0, dtype=np.int32)
            for ci in float_cols:
                columns_empty[ci] = np.empty(0, dtype=np.float64)
            return start_idx, columns_empty

        end_idx = start_idx + num_rows
        chunk = lines[start_idx:end_idx]
        try:
            all_cols = sorted(set(int_cols) | set(float_cols))
            raw = np.loadtxt(
                chunk,
                usecols=all_cols,
                dtype=np.float64,
                ndmin=2,
            )
        except ValueError:
            # Fallback to per-line parsing when numpy cannot handle the data
            columns: dict[int, np.ndarray] = {}
            for ci in int_cols:
                columns[ci] = np.empty(num_rows, dtype=np.int32)
            for ci in float_cols:
                columns[ci] = np.empty(num_rows, dtype=np.float64)
            all_needed = sorted(set(int_cols) | set(float_cols))
            min_fields = (max(all_needed) + 1) if all_needed else 0
            idx = start_idx
            for row in range(num_rows):
                parts = lines[idx].split()
                if len(parts) < min_fields:
                    msg = (
                        f"Expected at least {min_fields} fields, "
                        f"got {len(parts)} at line {idx + 1}"
                    )
                    raise ValueError(msg) from None
                for ci in int_cols:
                    columns[ci][row] = int(parts[ci].lstrip("0") or 0)
                for ci in float_cols:
                    columns[ci][row] = float(parts[ci])
                idx += 1
            return end_idx, columns

        col_pos = {c: i for i, c in enumerate(all_cols)}
        columns = {}
        for ci in int_cols:
            columns[ci] = raw[:, col_pos[ci]].astype(np.int32)
        for ci in float_cols:
            columns[ci] = raw[:, col_pos[ci]]
        return end_idx, columns

    def _parse_numeric_block_wide(
        self,
        lines: List[str],
        start_idx: int,
        num_rows: int,
        skip_cols: int = 0,
    ) -> tuple[int, np.ndarray]:
        """Parse a block of lines into a 2D float64 array (variable width).

        Skips the first *skip_cols* whitespace-delimited fields on each line,
        then converts the remaining fields to float64.

        Args:
            lines: All non-empty lines from the file.
            start_idx: Index of the first data line.
            num_rows: Number of rows to parse.
            skip_cols: Number of leading columns to skip.

        Returns:
            (next_idx, array) where *array* has shape ``(num_rows, ncols)``.
        """
        if num_rows <= 0:
            return start_idx, np.empty((0, 0), dtype=np.float64)

        end_idx = start_idx + num_rows
        chunk = lines[start_idx:end_idx]
        if skip_cols == 0:
            try:
                arr = np.loadtxt(chunk, dtype=np.float64, ndmin=2)
                return end_idx, arr
            except ValueError:
                pass

        rows = []
        for line in chunk:
            parts = line.split()
            rows.append([float(v) for v in parts[skip_cols:]])
        return end_idx, np.array(rows, dtype=np.float64)
