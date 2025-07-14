"""Base parser class for PLP file parsers."""

from pathlib import Path
from typing import Any, Dict, List, Union
from abc import ABC, abstractmethod
from typing import Optional


class BaseParser(ABC):
    """Abstract base class for PLP file parsers.

    Provides common functionality for all parsers:
    - File path handling
    - Basic validation
    - Common interface
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with file path.

        Args:
            file_path: Path to input file
        """
        self.file_path = Path(file_path) if isinstance(file_path, str) else file_path
        self._data: List[Dict[str, Any]] = []
        self._index_map: Dict[str, int] = {}  # Maps central names to indices

    def _append(self, item: Dict[str, Any]) -> None:
        """Validate and add a completed central to the list."""
        idx = len(self._data)
        self._data.append(item)
        self._index_map[item["name"]] = idx

    @abstractmethod
    def parse(self) -> None:
        """Parse the input file."""

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all parsed items."""
        return self._data

    def get_item_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get itemg by name."""
        return self._data[self._index_map[name]] if name in self._index_map else None

    def validate_file(self) -> None:
        """Validate input file exists and is readable."""
        if not self.file_path.exists():
            raise FileNotFoundError(f"File not found: {self.file_path}")
        if not self.file_path.is_file():
            raise ValueError(f"Path is not a file: {self.file_path}")

    def _read_non_empty_lines(self) -> List[str]:
        """Read file and return non-empty, non-comment lines.

        Returns:
            List of stripped, non-empty lines that aren't comments
        """
        with open(self.file_path, "r", encoding="utf-8") as f:
            return [
                line.strip()
                for line in f
                if line.strip() and not line.strip().startswith("#")
            ]

    def _parse_int(self, value: str) -> int:
        """Parse integer handling zero-padded strings.

        Args:
            value: String to parse as int

        Returns:
            Parsed integer value
        """
        return int(value.lstrip("0") or 0)

    def _parse_float(self, value: str) -> float:
        """Parse float handling zero-padded strings.

        Args:
            value: String to parse as float

        Returns:
            Parsed float value
        """
        return float(value.lstrip("0") or "0.0")

    def _next_idx(self, idx: int, lines) -> int:
        """Advance to the next non-empty line."""
        idx += 1
        if idx < len(lines):
            return idx
        raise IndexError("No more non-empty lines available")
