"""Base parser class for PLP file parsers.

The dependency-light primitives (file handling, name/number indexing,
scalar parsing) live in :class:`gtopt_shared.base_parser.BaseTextParser`,
shared with the other converters.  This module keeps the PLP-specific
extras: ``compressed_open`` transparent decompression
(:meth:`_read_non_empty_lines`) and the numpy-vectorised numeric-block
readers.
"""

import re
from abc import abstractmethod
from typing import Any, List, Optional

import numpy as np

from gtopt_shared.base_parser import BaseTextParser

from .compressed_open import compressed_open

# Compiled regex for _read_non_empty_lines: matches non-empty, non-comment
# lines and captures the stripped content.  The pattern skips leading
# whitespace, rejects lines starting with '#', and strips trailing whitespace.
# Uses C-level regex engine for ~2x speedup over Python per-line strip+startswith.
_RE_DATA_LINE = re.compile(r"^[ \t]*([^#\s][^\n]*\S|[^#\s])[ \t]*$", re.MULTILINE)


class BaseParser(BaseTextParser):
    """Abstract base class for PLP file parsers.

    Inherits the shared item store, name/number indexing and scalar
    parsing from :class:`~gtopt_shared.base_parser.BaseTextParser`, and
    adds the PLP-specific compressed line reader plus the numpy numeric
    block helpers.
    """

    @abstractmethod
    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the input file."""

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
