# -*- coding: utf-8 -*-


"""Parser for plpeta.dat format files containing stage data."""


from typing import Any, List, Dict, Union
from pathlib import Path


from .base_parser import BaseParser


class StageParser(BaseParser):
    """Parser for plpeta.dat format files containing stage data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with stage file path.

        Args:
            file_path: Path to plpeta.dat format file (str or Path)
        """
        super().__init__(file_path)

    def parse(self) -> None:
        """Parse the stage file and populate the stages structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        # Extract just the number part from first line (may have trailing metadata)
        first_line_parts = lines[idx].split()
        num_stages = self._parse_int(first_line_parts[0])
        idx += 1

        for _ in range(num_stages):
            # Parse stage line w/format: Ano Mes Etapa FDesh NHoras FactTasa TipoEtapa
            parts = lines[idx].split()
            if len(parts) < 6:
                raise ValueError(f"Invalid stage entry at line {idx+1}")

            stage_num = int(parts[2])  # Etapa is the stage number
            duration = float(parts[4])  # NHoras is the duration
            # Calculate discount factor from FactTasa if present, default to 1.0
            discount_factor = (
                1.0 / float(parts[5])
                if len(parts) > 5 and float(parts[5]) != 0
                else 1.0
            )
            idx += 1
            stage = {
                "number": stage_num,
                "duration": duration,
                "discount_factor": discount_factor,
            }

            self._append(stage)

    @property
    def stages(self) -> List[Dict[str, Any]]:
        """Return the parsed stages structure."""
        return self.get_all()

    @property
    def num_stages(self) -> int:
        """Return the number of stages in the file."""
        return len(self.stages)
