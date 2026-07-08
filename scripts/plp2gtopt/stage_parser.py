# -*- coding: utf-8 -*-


"""Parser for plpeta.dat format files containing stage data."""

from typing import Any, List, Dict, Optional


from .base_parser import BaseParser


class StageParser(BaseParser):
    """Parser for plpeta.dat format files containing stage data.

    Handles:
    - File parsing and validation
    - Stage data structure creation
    - Duration and discount factor calculation
    """

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the stage file and populate the stages structure."""
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
                raise ValueError(f"Invalid stage entry at line {idx + 1}")

            # `Mes` in plpeta.dat is a HYDROLOGICAL month (1 = April ..
            # 12 = March; the calendar year rolls at month 10 = January
            # — see the PLP source assumption in genpdmaule.f:1860-1862
            # and leelajam.f, plus the February=672h discriminator in
            # the production files).  Convert to the calendar month
            # (1 = January .. 12 = December) that every downstream
            # consumer (Stage.month, the agreement schedules) expects.
            hydro_month = int(parts[1])
            month = ((hydro_month + 2) % 12) + 1
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
                "month": month,
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
