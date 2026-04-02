# -*- coding: utf-8 -*-

"""Parser for plplajam.dat format files containing Laja irrigation agreement data.

This parser reads the PLP Laja irrigation convention file which defines
the volume-zone water rights allocation between ENDESA/Enel and irrigators
for the Laguna del Laja reservoir system.

File format (plplajam.dat)::

  # comments are allowed (lines starting with #)
  # Nombre Central Lago Laja
  'ELTORO'
  # Numero Caudales Hoya Intermedia
  N
  # Nombre Caudales Hoya Intermedia
  'ABANICO' ... 'TUCAPEL'
  # Volumen maximo Lago Laja
  5582.0
  ...
  (see inline comments for remaining fields)

The convention divides Laguna del Laja volume into volume zones, each
with different allocation factors for irrigation, electric, and mixed
rights.  Monthly factors modulate costs and maximum usage.

See also:
  parlajam.f  in PLP CEN65/src/ -- Fortran data structure
  genpdlajam.f  in PLP CEN65/src/ -- LP assembly
  plp_implementation.md -- detailed field documentation
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class LajaParser(BaseParser):
    """Parser for plplajam.dat files containing Laja convention data.

    Extracts reservoir identification, volume zone parameters, monthly
    factors, rights limits, cost parameters, and irrigation withdrawal
    districts needed to emit FlowRight, VolumeRight, and RightJunction
    entities.
    """

    def __init__(self, file_path):
        super().__init__(file_path)
        self._config: Dict[str, Any] = {}

    @property
    def config(self) -> Dict[str, Any]:
        """Return the parsed Laja convention configuration."""
        return self._config

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plplajam.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plplajam.dat file is empty or malformed.")

        idx = 0

        # --- Central name (reservoir operator) ---
        self._config["central_laja"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        # --- Intermediate basin flows ---
        num_intermediate = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        intermediate_names: List[str] = []
        for _ in range(num_intermediate):
            intermediate_names.append(self._parse_name(lines[idx]))
            idx = self._next_idx(idx, lines)
        self._config["intermediate_basins"] = intermediate_names

        # --- Reservoir volume max (hm3, already in physical units) ---
        self._config["vol_max"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # --- Volume zones ---
        num_zones = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        # Volume zone widths (hm3)
        parts = lines[idx].split()
        zone_widths = [float(v) for v in parts[:num_zones]]
        self._config["zone_widths"] = zone_widths
        idx = self._next_idx(idx, lines)

        # Rights base and incremental factors per volume zone:
        # base  f1  f2  f3  f4  (base + num_zones values)

        # Irrigation rights
        parts = lines[idx].split()
        self._config["irr_base"] = float(parts[0])
        self._config["irr_factors"] = [float(v) for v in parts[1 : num_zones + 1]]
        idx = self._next_idx(idx, lines)

        # Electric rights
        parts = lines[idx].split()
        self._config["elec_base"] = float(parts[0])
        self._config["elec_factors"] = [float(v) for v in parts[1 : num_zones + 1]]
        idx = self._next_idx(idx, lines)

        # Mixed rights
        parts = lines[idx].split()
        self._config["mixed_base"] = float(parts[0])
        self._config["mixed_factors"] = [float(v) for v in parts[1 : num_zones + 1]]
        idx = self._next_idx(idx, lines)

        # Maximum rights (riego, electrico, mixto, anticipos)
        parts = lines[idx].split()
        self._config["max_irr"] = float(parts[0])
        self._config["max_elec"] = float(parts[1])
        self._config["max_mixed"] = float(parts[2])
        self._config["max_anticipated"] = float(parts[3])
        idx = self._next_idx(idx, lines)

        # Irrigation season start months (hydro months: 1=Apr, 9=Dec, etc.)
        # Two values: start of irrigation season, start of anticipated
        parts = lines[idx].split()
        self._config["mes_inicio_riego"] = int(parts[0])
        self._config["mes_inicio_anticipos"] = int(parts[1])
        idx = self._next_idx(idx, lines)

        # Max flow per category [m3/s]: riego, electrico, mixtos, anticipados
        parts = lines[idx].split()
        self._config["qmax_irr"] = float(parts[0])
        self._config["qmax_elec"] = float(parts[1])
        self._config["qmax_mixed"] = float(parts[2])
        self._config["qmax_anticipated"] = float(parts[3])
        idx = self._next_idx(idx, lines)

        # Costs: CRiegoNS, CUsoRiego, CElectNS, CUsoElect, CMixto
        parts = lines[idx].split()
        self._config["cost_irr_ns"] = float(parts[0])
        self._config["cost_irr_uso"] = float(parts[1])
        self._config["cost_elec_ns"] = float(parts[2])
        self._config["cost_elec_uso"] = float(parts[3])
        self._config["cost_mixed"] = float(parts[4])
        idx = self._next_idx(idx, lines)

        # --- Monthly factor arrays (12 values, hydro year Apr-Mar) ---
        # Factor mensual costo riego no servido
        parts = lines[idx].split()
        self._config["monthly_cost_irr_ns"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor mensual costo derechos de riego
        parts = lines[idx].split()
        self._config["monthly_cost_irr"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor mensual costo derechos electricos
        parts = lines[idx].split()
        self._config["monthly_cost_elec"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor mensual costo derechos mixto
        parts = lines[idx].split()
        self._config["monthly_cost_mixed"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor mensual costo gasto anticipado
        parts = lines[idx].split()
        self._config["monthly_cost_anticipated"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor maximo uso mensual derechos riego
        parts = lines[idx].split()
        self._config["monthly_usage_irr"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor maximo uso mensual derechos electricos
        parts = lines[idx].split()
        self._config["monthly_usage_elec"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor maximo uso mensual derechos mixtos
        parts = lines[idx].split()
        self._config["monthly_usage_mixed"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Factor maximo uso mensual derechos anticipados
        parts = lines[idx].split()
        self._config["monthly_usage_anticipated"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Initial available rights (riego, elect, mixto, anticipos)
        parts = lines[idx].split()
        self._config["ini_irr"] = float(parts[0])
        self._config["ini_elec"] = float(parts[1])
        self._config["ini_mixed"] = float(parts[2])
        self._config["ini_anticipated"] = float(parts[3])
        idx = self._next_idx(idx, lines)

        # --- Withdrawal districts ---
        num_retiros = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        districts: List[Dict[str, Any]] = []
        for _ in range(num_retiros):
            district: Dict[str, Any] = {}

            # District name
            district["name"] = self._parse_name(lines[idx])
            idx = self._next_idx(idx, lines)

            # Injection name (may be empty '' meaning no injection point)
            line = lines[idx].strip()
            if line == "''":
                inj_name = ""
            elif "'" in line:
                inj_name = self._parse_name(line)
            else:
                inj_name = ""
            district["injection"] = inj_name if inj_name else None
            idx = self._next_idx(idx, lines)

            # Cost factor + 4 withdrawal percentages (1oReg, 2oReg, Emer, Saltos)
            parts = lines[idx].split()
            district["cost_factor"] = float(parts[0])
            district["pct_1o_reg"] = float(parts[1])
            district["pct_2o_reg"] = float(parts[2])
            district["pct_emergencia"] = float(parts[3])
            district["pct_saltos"] = float(parts[4])
            idx = self._next_idx(idx, lines)

            districts.append(district)

        self._config["districts"] = districts

        # --- Historical filtration ---
        self._config["filtration"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Default irrigation demands per regante category [m3/s]
        # 1oReg, 2oReg, Emergencia, Saltos
        parts = lines[idx].split()
        self._config["demand_1o_reg"] = float(parts[0])
        self._config["demand_2o_reg"] = float(parts[1])
        self._config["demand_emergencia"] = float(parts[2])
        self._config["demand_saltos"] = float(parts[3])
        idx = self._next_idx(idx, lines)

        # Seasonal variation curves (4 × 12 values, hydro year Apr-Mar)
        seasonal_curves: List[List[float]] = []
        for _ in range(4):
            parts = lines[idx].split()
            seasonal_curves.append([float(v) for v in parts[:12]])
            idx = self._next_idx(idx, lines)
        self._config["seasonal_1o_reg"] = seasonal_curves[0]
        self._config["seasonal_2o_reg"] = seasonal_curves[1]
        self._config["seasonal_emergencia"] = seasonal_curves[2]
        self._config["seasonal_saltos"] = seasonal_curves[3]

        # Dead volume
        self._config["vol_muerto"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Manual withdrawal overrides per stage (optional)
        num_manual_stages = self._parse_int(lines[idx])
        manual_withdrawals: List[Dict[str, Any]] = []
        for _ in range(num_manual_stages):
            idx = self._next_idx(idx, lines)
            parts = lines[idx].split()
            manual_withdrawals.append(
                {
                    "stage": int(parts[0]),
                    "q_1o_reg": float(parts[1]),
                    "q_2o_reg": float(parts[2]),
                    "q_emergencia": float(parts[3]),
                    "q_saltos": float(parts[4]),
                }
            )
        self._config["manual_withdrawals"] = manual_withdrawals

        # Forced El Toro flows per stage (optional)
        idx = self._next_idx(idx, lines)
        num_forced_stages = self._parse_int(lines[idx])
        forced_flows: List[Dict[str, float]] = []
        for _ in range(num_forced_stages):
            idx = self._next_idx(idx, lines)
            parts = lines[idx].split()
            forced_flows.append(
                {
                    "stage": int(parts[0]),
                    "flow": float(parts[1]),
                }
            )
        self._config["forced_flows"] = forced_flows

        # Store as single item for BaseParser compatibility
        self._append({"name": "laja_convention", **self._config})
