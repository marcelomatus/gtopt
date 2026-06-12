# -*- coding: utf-8 -*-

"""Parser for plpvrebemb.dat — per-embalse spill (vertimiento) cost.

PLP file format (one block per embalse):

    # Archivo de Volumenes de vertimiento de Embalses (plpvrebemb.dat)
    # Numero Embalses con volumenes espe
    10
    # Nombre del Embalse
    'COLBUN'
    # Volumen de Rebalse [ 10^3 m3 ]
    1553245
    # Costo de Rebalse
    0.1

Provides ``cost_by_name`` mapping ``EmbCReb`` per reservoir.  The volume
threshold (``EmbVReb`` = ``Volumen de Rebalse``) is captured but currently
unused on the gtopt side: gtopt's ``Reservoir.emax`` already enforces the
upper-storage box hard, so the PLP-style soft-overflow penalty maps cleanly
to ``Reservoir.spillway_cost`` on the existing drain column.
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class VrebembParser(BaseParser):
    """Parser for plpvrebemb.dat — per-embalse spill cost & threshold."""

    @property
    def vrebs(self) -> List[Dict[str, Any]]:
        return self.get_all()

    @property
    def num_vrebs(self) -> int:
        return len(self.vrebs)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse plpvrebemb.dat and populate the data structure."""
        self.validate_file()
        lines: List[str] = []
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("plpvrebemb.dat is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num = self._parse_int(lines[idx])

            for _ in range(num):
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip().strip("'")
                idx = self._next_idx(idx, lines)
                volume = self._parse_float(lines[idx])
                idx = self._next_idx(idx, lines)
                cost = self._parse_float(lines[idx])
                self._append({"name": name, "volume": volume, "cost": cost})
        finally:
            lines.clear()

    def get_cost(self, name: str) -> float | None:
        """Return per-reservoir spill cost (Costo de Rebalse) or None."""
        item = self.get_item_by_name(name)
        return None if item is None else float(item["cost"])
