# -*- coding: utf-8 -*-

"""Parser for plpcenpmax.dat — volume-dependent turbine Pmax curves.

The ``plpcenpmax.dat`` file defines piecewise-linear curves of maximum
turbine power output (Pmax) as a function of reservoir volume.

**File format** (Fortran ``LeeCenPMax`` in ``plp-leecenpmax.f``)::

    # Archivo con curva pmax en funcion del volumen
    # Numero de embalses
    N
    # For each entry:
    # Nombre de Central
    'CENTRAL_NAME'
    # Nombre Embalse
    'EMBALSE_NAME'
    # Numero de Segmentos
    num_segments
    # Volumen [10e6 m3]   Pendiente   Coeficiente
    volume_Mm3            slope       constant
    ...

Field definitions:
  CENTRAL_NAME – Central/turbine name (must match a central in plpcnfce.dat)
  EMBALSE_NAME – Reservoir name (must match a reservoir in plpcnfce.dat)
  num_segments – Number of piecewise-linear segments
  volume_Mm3   – Volume breakpoint [Mm³]
  slope        – Piecewise slope [MW/Mm³]
  constant     – Pmax at the breakpoint [MW]

Optional unit conversions (matching PLP Fortran ``plp-leecenpmax.f``):
  volume_dam3 = volume_Mm3 × 1000    (1 Mm³ = 1000 dam³)
  slope_dam3  = slope_Mm3  / 1000    (MW/Mm³ → MW/dam³)
  constant is kept in MW (no conversion)

When ``convert_units=True`` is passed, these conversions are applied so that
volume breakpoints are in dam³ (matching gtopt reservoir volumes).  By default
(``convert_units=False``), values are stored as-is from the file.

Note: In PLP, the Pmax curve bounds are applied to the *flow* variable
(divided by efficiency), not directly to generator power.  This data may be
redundant with generator maintenance Pmax (plpmance.dat).

The turbine Pmax at reservoir volume V is:
  Pmax(V) = constant_i + slope_i × (V − volume_i)
where segment i is the active segment for volume V.
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class CenpmaxParser(BaseParser):
    """Parse ``plpcenpmax.dat`` — volume-dependent turbine Pmax curves.

    After parsing, each entry contains:

    - ``name`` — central/turbine name
    - ``reservoir`` — reservoir name
    - ``segments`` — list of ``{volume, slope, constant}`` dicts

    When ``convert_units=True``, segments use [dam³, MW/dam³, MW].
    When ``convert_units=False`` (default), segments use raw file units
    [Mm³, MW/Mm³, MW].
    """

    @property
    def pmax_curves(self) -> List[Dict[str, Any]]:
        """Return the parsed Pmax curve entries."""
        return self.get_all()

    @property
    def num_pmax_curves(self) -> int:
        """Return the number of Pmax curve entries."""
        return len(self.pmax_curves)

    def parse(
        self,
        parsers: Optional[Dict[str, Any]] = None,
        *,
        convert_units: bool = False,
    ) -> None:
        """Parse the plpcenpmax.dat file.

        Args:
            parsers: Upstream parsed data (unused, kept for API consistency).
            convert_units: If True, convert volume Mm³→dam³ (×1000) and
                slope MW/Mm³→MW/dam³ (÷1000).  Default False stores raw values.
        """
        self.validate_file()

        vol_factor = 1000.0 if convert_units else 1.0
        slope_divisor = 1000.0 if convert_units else 1.0

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpcenpmax.dat file is empty or malformed.")

        idx = 0
        num_entries = self._parse_int(lines[idx])
        idx += 1

        if num_entries < 0:
            raise ValueError(
                f"Invalid number of Pmax entries: {num_entries}. Must be non-negative."
            )

        for _ in range(num_entries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenpmax.dat file.")

            central_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(
                    "Unexpected end of plpcenpmax.dat file (reservoir name)."
                )

            reservoir_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(
                    "Unexpected end of plpcenpmax.dat file (num_segments)."
                )

            num_segments = self._parse_int(lines[idx])
            idx += 1

            segments: List[Dict[str, float]] = []
            for _ in range(num_segments):
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of plpcenpmax.dat file (segment data)."
                    )
                parts = lines[idx].split()
                if len(parts) < 3:
                    raise ValueError(f"Segment line has too few fields: {lines[idx]}")
                # Format: volume_Mm3  slope_Mm3  constant_MW
                seg: Dict[str, float] = {
                    "volume": self._parse_float(parts[0]) * vol_factor,
                    "slope": self._parse_float(parts[1]) / slope_divisor,
                    "constant": self._parse_float(parts[2]),
                }
                segments.append(seg)
                idx += 1

            entry: Dict[str, Any] = {
                "name": central_name,
                "reservoir": reservoir_name,
                "segments": segments,
            }
            self._append(entry)

        if self.num_pmax_curves != num_entries:
            raise ValueError(
                f"Expected {num_entries} Pmax entries but parsed"
                f" {self.num_pmax_curves}."
            )

    def get_pmax_by_central(self, central_name: str) -> Optional[Dict[str, Any]]:
        """Get Pmax curve data by central (turbine) name."""
        return self.get_item_by_name(central_name)
