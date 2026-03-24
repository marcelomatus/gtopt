# -*- coding: utf-8 -*-

"""Parser for plpcenfi.dat format files containing reservoir seepage data.

Handles:
- File parsing and validation
- ReservoirSeepage data structure creation
- Lookup by central name
- Piecewise-linear segment support (backward-compatible)

File format (plpcenfi.dat - Archivo de Centrales Filtración):

**Legacy format (single slope/constant pair)**::

  N
  'CENTRAL_NAME'
  'EMBALSE_NAME'
  slope  constant

**Extended format (piecewise-linear segments)**::

  N
  'CENTRAL_NAME'
  'EMBALSE_NAME'
  num_segments
  idx  volume  slope  constant
  idx  volume  slope  constant
  ...

Detection is automatic: if the line after the reservoir name contains a
single integer, it is the number of segments; if it contains two or more
numbers, it is the legacy slope/constant format.

Field definitions:
  CENTRAL_NAME  – Central/waterway name (must match a central in plpcnfce.dat)
  EMBALSE_NAME  – Receiving reservoir name (matches a reservoir in plpcnfce.dat)
  slope         – Seepage rate proportional to reservoir volume [m³/s / dam³]
  constant      – Constant seepage rate independent of volume [m³/s]

The seepage seepage flow per block is modelled as:
  seepage [m³/s] = slope × avg_reservoir_volume [dam³] + constant [m³/s]

where avg_reservoir_volume = (eini + efin) / 2.  When piecewise segments are
present, the active segment is selected based on the current reservoir volume.
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class CenfiParser(BaseParser):
    """Parser for plpcenfi.dat files containing reservoir seepage data."""

    @property
    def seepages(self) -> List[Dict[str, Any]]:
        """Return the parsed seepage entries."""
        return self.get_all()

    @property
    def num_seepages(self) -> int:
        """Return the number of seepage entries."""
        return len(self.seepages)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpcenfi.dat file and populate the data structure.

        Supports both the legacy format (slope constant) and the extended
        format with piecewise-linear segments.  Format detection is
        automatic based on the field count of the line after the reservoir
        name.
        """
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpcenfi.dat file is empty or malformed.")

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
                raise ValueError("Unexpected end of plpcenfi.dat file.")

            # Central name (waterway source)
            central_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenfi.dat file (reservoir).")

            # Reservoir name (seepage target)
            reservoir_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(
                    "Unexpected end of plpcenfi.dat file (slope/constant or segments)."
                )

            # Detect format: legacy (2+ fields) or extended (1 field = num_segments)
            parts = lines[idx].split()
            slope = 0.0
            constant = 0.0
            segments: List[Dict[str, float]] = []

            if len(parts) >= 2:
                # Legacy format: slope constant on a single line
                slope = self._parse_float(parts[0])
                constant = self._parse_float(parts[1])
                idx += 1
            elif len(parts) == 1:
                # Extended format: single integer = num_segments.
                # If it cannot be parsed as an integer, fall back to
                # the legacy error (too few fields).
                try:
                    num_segments = self._parse_int(parts[0])
                except ValueError as exc:
                    raise ValueError(
                        "ReservoirSeepage slope/constant line has too few fields:"
                        f" {lines[idx]}"
                    ) from exc
                idx += 1
                segments = []
                for _ in range(num_segments):
                    if idx >= len(lines):
                        raise ValueError(
                            "Unexpected end of plpcenfi.dat file (segment data)."
                        )
                    seg_parts = lines[idx].split()
                    if len(seg_parts) < 4:
                        raise ValueError(
                            f"Segment line has too few fields: {lines[idx]}"
                        )
                    # Format: idx volume slope constant
                    seg: Dict[str, float] = {
                        "volume": self._parse_float(seg_parts[1]),
                        "slope": self._parse_float(seg_parts[2]),
                        "constant": self._parse_float(seg_parts[3]),
                    }
                    segments.append(seg)
                    idx += 1
                # Use first segment's values as the default slope/constant
                if segments:
                    slope = segments[0]["slope"]
                    constant = segments[0]["constant"]
                else:
                    slope = 0.0
                    constant = 0.0

            entry: Dict[str, Any] = {
                "name": central_name,
                "reservoir": reservoir_name,
                "slope": slope,
                "constant": constant,
                "segments": segments,
            }
            self._append(entry)

        if self.num_seepages != num_entries:
            raise ValueError(
                f"Expected {num_entries} seepage entries but parsed"
                f" {self.num_seepages}."
            )

    def get_seepage_by_central(self, central_name: str) -> Optional[Dict[str, Any]]:
        """Get seepage data by central (waterway source) name."""
        return self.get_item_by_name(central_name)
