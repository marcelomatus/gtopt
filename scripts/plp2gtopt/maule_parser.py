# -*- coding: utf-8 -*-

"""Parser for plpmaulen.dat format files containing Maule irrigation agreement data.

This parser reads the PLP Maule irrigation convention file which defines
the multi-zone water rights allocation between ENDESA/Enel and irrigators
for the Laguna del Maule / Colbun reservoir system.

File format (plpmaulen.dat)::

  # comments are allowed (lines starting with #)
  # Nombre Central Embalse Maule
  'LMAULE'
  # Nombre Central Laguna Invernada
  'CIPRESES'
  # Nombre Central Embalse Melado
  'PEHUENCHE'
  # Nombre Central Embalse Colbun
  'COLBUN'
  # Numero Caudales Hoya Intermedia
  N
  # Nombre Caudales Hoya Intermedia
  'name1' ... 'nameN'
  # VEmbalseUtilMin (en 10^3 m3)
  ...
  (see inline comments for remaining fields)

See also:
  parmaule.f  in PLP CEN65/src/ -- Fortran data structure
  genpdmaule.f  in PLP CEN65/src/ -- LP assembly
  plp_implementation.md -- detailed field documentation
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


# Month names used by gtopt Stage (1-indexed: january=1 .. december=12)
_MONTH_NAMES = [
    "",
    "january",
    "february",
    "march",
    "april",
    "may",
    "june",
    "july",
    "august",
    "september",
    "october",
    "november",
    "december",
]


class MauleParser(BaseParser):
    """Parser for plpmaulen.dat files containing Maule convention data.

    Extracts reservoir zone thresholds, water rights categories, monthly
    schedules, irrigation withdrawal districts, and cost parameters needed
    to emit FlowRight, VolumeRight, and RightJunction entities.
    """

    def __init__(self, file_path):
        super().__init__(file_path)
        self._config: Dict[str, Any] = {}

    @property
    def config(self) -> Dict[str, Any]:
        """Return the parsed Maule convention configuration."""
        return self._config

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpmaulen.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpmaulen.dat file is empty or malformed.")

        idx = 0

        # --- Central names ---
        # Embalse Maule (typically LMAULE)
        self._config["central_maule"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        # Laguna Invernada (typically CIPRESES)
        self._config["central_invernada"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        # Embalse Melado (typically PEHUENCHE)
        self._config["central_melado"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        # Embalse Colbun (typically COLBUN)
        self._config["central_colbun"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        # --- Intermediate basin flows ---
        num_intermediate = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        intermediate_names: List[str] = []
        for _ in range(num_intermediate):
            intermediate_names.append(self._parse_name(lines[idx]))
            idx = self._next_idx(idx, lines)
        self._config["intermediate_basins"] = intermediate_names

        # --- Volume thresholds (all in 10^3 m3 = dam3) ---
        # Convert to hm3 (÷ 1000) for gtopt
        self._config["v_util_min"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_reserva_extraord"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_reserva_ordinaria"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_der_riego_temp_max"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_der_elect_anu_max"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_comp_elec_max"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        # --- Initial volumes (10^3 m3 -> hm3) ---
        self._config["v_gasto_elec_men_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_gasto_elec_anu_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_gasto_riego_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_gasto_rext_elec_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_gasto_rext_riego_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_der_rext_elec_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_der_rext_riego_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_comp_elec_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        self._config["v_econ_inver_ini"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        # --- Flow limits (m3/s) ---
        self._config["gasto_elec_men_max"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        self._config["gasto_elec_dia_max"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Monthly modulation of daily max electric in reserve (%)
        parts = lines[idx].split()
        self._config["mod_elec_reserva"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Descuenta caudales derechos electricos del balance en Armerillo
        self._config["descuenta_elec_armerillo"] = lines[idx].strip().upper() in (
            "T",
            "TRUE",
            ".TRUE.",
        )
        idx = self._next_idx(idx, lines)

        # Gasto Riego Maximo [m3/seg]
        self._config["gasto_riego_max"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Caudal Maule Minimo para embalsar: value + two flags
        parts = lines[idx].split()
        self._config["caudal_min_embalsa"] = float(parts[0])
        self._config["flag_embalsa_1"] = parts[1].upper() in ("T", "TRUE")
        self._config["flag_embalsa_2"] = (
            parts[2].upper() in ("T", "TRUE") if len(parts) > 2 else False
        )
        idx = self._next_idx(idx, lines)

        # Valor Riego servido Conv Maule & Res 105
        parts = lines[idx].split()
        self._config["valor_riego_maule"] = float(parts[0])
        self._config["valor_riego_res105"] = float(parts[1])
        idx = self._next_idx(idx, lines)

        # Costo Riego no servido Conv Maule & Res 105
        parts = lines[idx].split()
        self._config["costo_riego_ns_maule"] = float(parts[0])
        self._config["costo_riego_ns_res105"] = float(parts[1])
        idx = self._next_idx(idx, lines)

        # Porcentaje de riego mensuales Maule (12 values)
        parts = lines[idx].split()
        self._config["pct_riego_mensual"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Agnos iniciales de porcentajes manuales
        self._config["n_agnos_pct_manual"] = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        # Porcentajes manuales (12 values)
        parts = lines[idx].split()
        self._config["pct_riego_manual"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Ano modulacion automatica (0 = desactivado)
        self._config["ano_mod_auto"] = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        # Factor caudales futuros
        self._config["factor_caudales_futuros"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Modulacion automatica afecta Res 105
        self._config["mod_auto_res105"] = lines[idx].strip().upper() in (
            "T",
            "TRUE",
            ".TRUE.",
        )
        idx = self._next_idx(idx, lines)

        # Volumen acumulado derechos riego para terminar temporada (12 values, 10^3 m3 -> hm3)
        parts = lines[idx].split()
        self._config["vol_acum_riego_temp"] = [float(v) / 1000.0 for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Dias acumulados para terminar temporada (12 values)
        parts = lines[idx].split()
        self._config["dias_acum_temp"] = [int(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Porcentaje derechos electricos en reserva
        self._config["pct_elec_reserva"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Porcentaje derechos riego en reserva
        self._config["pct_riego_reserva"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Caudales riego Res 105 mensuales (12 values, m3/s)
        parts = lines[idx].split()
        self._config["caudal_res105"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # Agnos iniciales Res 105 manuales
        self._config["n_agnos_res105_manual"] = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        # Caud Res 105 manuales (12 values)
        parts = lines[idx].split()
        self._config["caudal_res105_manual"] = [float(v) for v in parts[:12]]
        idx = self._next_idx(idx, lines)

        # --- Withdrawal districts ---
        num_retiros = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        district_names: List[str] = []
        for _ in range(num_retiros):
            district_names.append(self._parse_name(lines[idx]))
            idx = self._next_idx(idx, lines)

        # Porcentajes de retiro
        parts = lines[idx].split()
        district_pcts = [float(v) for v in parts[:num_retiros]]
        idx = self._next_idx(idx, lines)

        # Holgura en retiro de riego (T/F per district)
        parts = lines[idx].split()
        district_slack = [
            p.upper() in ("T", "TRUE", ".TRUE.") for p in parts[:num_retiros]
        ]
        idx = self._next_idx(idx, lines)

        districts: List[Dict[str, Any]] = []
        for i in range(num_retiros):
            districts.append(
                {
                    "name": district_names[i],
                    "percentage": district_pcts[i],
                    "has_slack": district_slack[i],
                }
            )
        self._config["districts"] = districts

        # --- Bocatoma Canelon ---
        self._config["bocatoma_canelon"] = self._parse_name(lines[idx])
        idx = self._next_idx(idx, lines)

        self._config["costo_canelon"] = self._parse_float(lines[idx])
        idx = self._next_idx(idx, lines)

        # Indice extraccion Colbun con cota 425
        self._config["idx_extrac_colbun_425"] = self._parse_int(lines[idx])
        idx = self._next_idx(idx, lines)

        # Volumen Cota Disponibilidad 425 (10^3 m3 -> hm3)
        self._config["v_cota_425"] = self._parse_float(lines[idx]) / 1000.0
        idx = self._next_idx(idx, lines)

        # Economias Invernada: uso en Reserva, acumulables, Costo
        parts = lines[idx].split()
        self._config["econ_inver_uso_reserva"] = parts[0].upper() in (
            "T",
            "TRUE",
        )
        self._config["econ_inver_acumulables"] = parts[1].upper() in (
            "T",
            "TRUE",
        )
        self._config["econ_inver_costo"] = float(parts[2])
        idx = self._next_idx(idx, lines)

        # Costo de embalsar / "Penalizadores Convenio" (optional)
        # PLP (genpdmaule.f:1797): CostoEmbalsar applied to IQHINV
        # (invernada_storage flow), CostoNoEmbalsar applied to IQHNEIN
        # (invernada_bypass flow).  Defaults: 1500/1000.
        # In the .dat file this line is labeled "Penalizadores Convenio".
        try:
            idx = self._next_idx(idx, lines)
            parts = lines[idx].split()
            self._config["costo_embalsar"] = float(parts[0])
            self._config["costo_no_embalsar"] = (
                float(parts[1]) if len(parts) > 1 else 1000.0
            )
        except (IndexError, ValueError):
            self._config["costo_embalsar"] = 1500.0
            self._config["costo_no_embalsar"] = 1000.0

        # Backward compatibility: keep penalizador_1/2 as aliases
        self._config["penalizador_1"] = self._config["costo_embalsar"]
        self._config["penalizador_2"] = self._config["costo_no_embalsar"]

        # Store as single item for BaseParser compatibility
        self._append({"name": "maule_convention", **self._config})
