# -*- coding: utf-8 -*-

"""Parser for plpfilemb.dat format files containing reservoir seepage data.

This is the primary seepage model used by PLP (analogous to plpcenre.dat
for reservoir efficiencies).

Handles:
- File parsing and validation
- ReservoirSeepage data structure creation (piecewise-linear segments)
- Lookup by embalse (reservoir) name

File format (plpfilemb.dat - Archivo de Filtraciones de Embalses):

  # comments are allowed (lines starting with #)
  # Numero Embalses con filtraciones
  N
  # For each entry:
  # Nombre de embalse (source – filtered reservoir)
  'EMBALSE_NAME'
  # Filtraciones medias  [m³/s]
  mean_seepage
  # Numero de Tramos
  num_segments
  # Tramo  Vol[10e6 m3]  Pendiente  Constante
  idx      volume_Mm3    slope      constant
  ...
  # Nombre de la Central aguas abajo (receiving central)
  'CENTRAL_NAME'

Field definitions:
  EMBALSE_NAME   – Filtered reservoir name (source)
  mean_seepage – Mean/fallback seepage flow [m³/s]
  num_segments    – Number of piecewise-linear segments
  idx             – Segment index (1-based, informational)
  volume_Mm3      – Volume breakpoint [hm³ = Mm³ = hm³].  This is the
                    same unit used by gtopt reservoir volumes, so no
                    conversion is applied.
  slope           – Piecewise-linear slope [m³/s per hm³].  Already in
                    the correct per-gtopt-volume unit; no conversion needed.
  constant        – ReservoirSeepage rate at the volume breakpoint [m³/s] (no
                    unit conversion needed).
  CENTRAL_NAME    – Receiving central name (destination of seepage water)

No unit conversions are applied during parsing: plpfilemb.dat stores
volumes in hm³ (= Mm³ = hm³) which is the same physical unit used by
gtopt reservoir volumes.  Slopes are in m³/s per hm³ and constants in
m³/s — both are passed through unchanged.

The seepage flow per block is modelled as:
  seepage [m³/s] = slope(V) × avg_volume [hm³] + constant(V)

where avg_volume = (eini + efin) / 2 and the active segment is selected
from the piecewise-linear concave envelope at the current reservoir volume.

Equivalent gtopt JSON fields:
  ReservoirSeepage.slope     (scalar or schedule) ← segment slope for SDDP update
  ReservoirSeepage.constant  (scalar or schedule) ← segment constant for SDDP update
  ReservoirSeepage.segments  ← list of {volume, slope, constant} dicts

See also:
  plpcenre.dat / CenreParser  – analogous model for turbine efficiency
  leefilemb.f  in PLP CEN65/src/ – authoritative Fortran parser
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class FilembParser(BaseParser):
    """Parser for plpfilemb.dat files containing reservoir seepage data.

    This parser is the primary source for seepage data in PLP cases
    and is analogous to CenreParser for reservoir efficiencies.

    The piecewise-linear segments it provides are used by ReservoirSeepageLP to
    update the LP constraint coefficients (slope on eini/efin columns and
    the constant RHS term) at each phase based on the current reservoir
    volume.
    """

    @property
    def seepages(self) -> List[Dict[str, Any]]:
        """Return the parsed seepage entries."""
        return self.get_all()

    @property
    def num_seepages(self) -> int:
        """Return the number of seepage entries."""
        return len(self.seepages)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpfilemb.dat file and populate the data structure.

        Reads piecewise-linear seepage curves from the PLP file format.
        No unit conversions are applied — volumes are already in hm³
        (= Mm³ = hm³), the same physical unit as gtopt reservoir volumes.
        Slopes are in m³/s per hm³ and constants in m³/s.
        """
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpfilemb.dat file is empty or malformed.")

        idx = 0
        num_entries = self._parse_int(lines[idx])
        idx += 1

        if num_entries < 0:
            raise ValueError(
                f"Invalid number of seepage entries: {num_entries}."
                " Must be non-negative."
            )

        for _ in range(num_entries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of plpfilemb.dat file.")

            # Source reservoir/embalse name
            embalse_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpfilemb.dat file (mean seepage).")

            # Mean/average seepage flow [m³/s]
            mean_seepage = max(self._parse_float(lines[idx]), 0.0)
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpfilemb.dat file (num_segments).")

            # Number of piecewise-linear segments
            num_segments = self._parse_int(lines[idx])
            idx += 1

            segments: List[Dict[str, float]] = []
            for _ in range(num_segments):
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of plpfilemb.dat file (segment data)."
                    )
                parts = lines[idx].split()
                if len(parts) < 4:
                    raise ValueError(f"Segment line has too few fields: {lines[idx]}")
                # Format: idx  volume_Mm3  slope_per_Mm3  constant_m3s
                # No unit conversion: Mm³ = hm³ = gtopt physical unit.
                seg: Dict[str, float] = {
                    "volume": self._parse_float(parts[1]),
                    "slope": self._parse_float(parts[2]),
                    "constant": self._parse_float(parts[3]),
                }
                segments.append(seg)
                idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpfilemb.dat file (central name).")

            # Receiving central name (destination of seepage water)
            central_name = self._parse_name(lines[idx])
            idx += 1

            entry: Dict[str, Any] = {
                "name": embalse_name,  # source embalse name (BaseParser key)
                "embalse": embalse_name,
                "central": central_name,
                "mean_seepage": mean_seepage,
                "segments": segments,
            }
            self._append(entry)

        if self.num_seepages != num_entries:
            raise ValueError(
                f"Expected {num_entries} seepage entries but parsed"
                f" {self.num_seepages}."
            )

    def get_seepage_by_embalse(self, embalse_name: str) -> Optional[Dict[str, Any]]:
        """Get seepage data by embalse (source reservoir) name."""
        return self.get_item_by_name(embalse_name)
