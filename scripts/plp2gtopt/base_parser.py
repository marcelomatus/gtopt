"""Base parser class for PLP file parsers."""

from pathlib import Path
from typing import Any, Dict, List, Union
from abc import ABC, abstractmethod


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
        self._count: int = 0

    @property
    def count(self) -> int:
        """Return number of parsed items."""
        return self._count

    @abstractmethod
    def parse(self) -> None:
        """Parse the input file."""

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all parsed items."""
        return self._data

    def validate_file(self) -> None:
        """Validate input file exists and is readable."""
        if not self.file_path.exists():
            raise FileNotFoundError(f"File not found: {self.file_path}")
        if not self.file_path.is_file():
            raise ValueError(f"Path is not a file: {self.file_path}")
