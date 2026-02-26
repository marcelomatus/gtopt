# -*- coding: utf-8 -*-

"""Writer for converting battery data (plpcenbat.dat) to GTOPT JSON format.

Data sources and their roles
-----------------------------
* ``plpcenbat.dat`` (via ``battery_parser``) – **primary** source for battery
  configuration: name, bus, injection centrals (FPC), FPD, emin, emax.
* ``plpcnfce.dat`` (BAT section via ``central_parser``) – source for battery
  UID (central number) and pmax_discharge (central pmax); also used to look up
  injection central pmax for pmax_charge.
* ``plpmanbat.dat`` (via ``manbat_parser``) – per-stage pmax_charge / pmax_discharge
  overrides (maintenance schedules).

Matching rule: plpcenbat.dat battery name == BAT central name in plpcnfce.dat.

When *no* plpcenbat.dat file is present but BAT centrals exist in plpcnfce.dat,
default battery parameters are applied (FPC=FPD=0.95, emax derived from pmax,
emin=0.0).

UID allocation
--------------
  Battery   uid = bat_central_number    (BAT central number in plpcnfce.dat)
  Generator uid = BATTERY_UID_OFFSET + bat_central_number   (discharge path)
  Demand    uid = BATTERY_UID_OFFSET + bat_central_number   (charge path)
  Converter uid = bat_central_number
"""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict

import pandas as pd

from .base_writer import BaseWriter
from .battery_parser import BatteryParser
from .central_parser import CentralParser
from .bus_parser import BusParser
from .stage_parser import StageParser
from .manbat_parser import ManbatParser

BATTERY_UID_OFFSET = 10000

# Default battery parameters used when only plpcnfce.dat BAT centrals are present
_DEFAULT_FPC = 0.95
_DEFAULT_FPD = 0.95
_DEFAULT_HRS_REG = 4.0
_DEFAULT_VINI = 0.5


class BatteryEntry(TypedDict, total=False):
    """Represents a battery in the GTOPT system."""

    uid: int
    name: str
    active: List[int]
    input_efficiency: float
    output_efficiency: float
    vmin: float
    vmax: float
    vini: float
    capacity: float


class ConverterEntry(TypedDict):
    """Represents a converter linking battery, generator and demand."""

    uid: int
    name: str
    battery: int
    generator: int
    demand: int
    capacity: float


class BatteryWriter(BaseWriter):
    """Converts battery data (plpcenbat.dat) to GTOPT JSON arrays.

    Combines ``plpcenbat.dat`` battery entries with ``plpcnfce.dat`` BAT
    centrals (for UID and pmax_discharge). When no plpcenbat.dat is present
    but BAT centrals exist, defaults are applied.
    """

    def __init__(
        self,
        battery_parser: Optional[BatteryParser] = None,
        central_parser: Optional[CentralParser] = None,
        bus_parser: Optional[BusParser] = None,
        stage_parser: Optional[StageParser] = None,
        manbat_parser: Optional[ManbatParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        super().__init__(None, options)
        self.battery_parser = battery_parser
        self.central_parser = central_parser
        self.bus_parser = bus_parser
        self.stage_parser = stage_parser
        self._man_parser = manbat_parser

    def _bat_centrals(self) -> Dict[str, Dict[str, Any]]:
        """Return BAT-type centrals from plpcnfce.dat keyed by name."""
        if not self.central_parser:
            return {}
        return {
            str(c["name"]): c
            for c in self.central_parser.centrals
            if c.get("type") == "bateria"
        }

    def _all_centrals_by_name(self) -> Dict[str, Dict[str, Any]]:
        """Return ALL centrals from plpcnfce.dat keyed by name (for injection lookup)."""
        if not self.central_parser:
            return {}
        return {str(c["name"]): c for c in self.central_parser.centrals}

    def _all_entries(self) -> List[Dict[str, Any]]:
        """Build the unified list of battery entries.

        Priority:
        1. plpcenbat.dat entries matched to BAT centrals (new battery model)
        2. BAT centrals with no matching file -> defaults applied
        """
        bat = self._bat_centrals()
        all_centrals = self._all_centrals_by_name()
        entries: List[Dict[str, Any]] = []

        if self.battery_parser and self.battery_parser.batteries:
            for item in self.battery_parser.batteries:
                name = item["name"]
                central = bat.get(name, {})
                # UID and pmax_discharge come from BAT central (authoritative)
                uid = central.get("number", item["number"])
                pmax_d = central.get("pmax", 0.0)
                # Bus comes from plpcenbat.dat (authoritative for new format)
                bus = item.get("bus", central.get("bus", 0))
                # Energy capacity directly from plpcenbat.dat
                emax = item.get("emax", pmax_d * _DEFAULT_HRS_REG)
                emin = item.get("emin", 0.0)
                # Efficiencies from plpcenbat.dat
                injections = item.get("injections", [])
                fpc = injections[0]["fpc"] if injections else _DEFAULT_FPC
                fpd = item.get("fpd", _DEFAULT_FPD)
                # pmax_charge: look up first injection central in plpcnfce.dat
                pmax_c = pmax_d  # default fallback
                if injections:
                    inj_name = injections[0]["name"]
                    inj_central = all_centrals.get(inj_name, {})
                    pmax_c = inj_central.get("pmax", pmax_d) if inj_central else pmax_d
                entries.append(
                    {
                        "number": uid,
                        "name": name,
                        "bus": bus,
                        "pmax_discharge": pmax_d,
                        "pmax_charge": pmax_c,
                        "nc": fpc,
                        "nd": fpd,
                        "emax": emax,
                        "emin": emin,
                        "vini": _DEFAULT_VINI,
                    }
                )
        else:
            # No battery file - derive entries from BAT centrals with defaults
            for name, central in bat.items():
                pmax_d = central.get("pmax", 0.0)
                entries.append(
                    {
                        "number": central["number"],
                        "name": name,
                        "bus": central.get("bus", 0),
                        "pmax_discharge": pmax_d,
                        "pmax_charge": pmax_d,
                        "nc": _DEFAULT_FPC,
                        "nd": _DEFAULT_FPD,
                        "emax": pmax_d * _DEFAULT_HRS_REG,
                        "emin": 0.0,
                        "vini": _DEFAULT_VINI,
                    }
                )

        return entries

    def to_battery_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build battery_array JSON list."""
        if entries is None:
            entries = self._all_entries()

        batteries = []
        for entry in entries:
            emax = entry["emax"]
            emin = entry["emin"]
            vmin = (emin / emax) if emax > 0.0 else 0.0
            bat: Dict[str, Any] = {
                "uid": entry["number"],
                "name": entry["name"],
                "input_efficiency": entry["nc"],
                "output_efficiency": entry["nd"],
                "vmin": vmin,
                "vmax": 1.0,
                "vini": entry["vini"],
                "capacity": emax,
            }
            batteries.append(bat)
        return batteries

    def to_generator_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build discharge-path generator entries."""
        if entries is None:
            entries = self._all_entries()
        gens = []
        for entry in entries:
            pmax_d = entry["pmax_discharge"]
            gens.append(
                {
                    "uid": BATTERY_UID_OFFSET + entry["number"],
                    "name": f"{entry['name']}_disch",
                    "bus": entry["bus"],
                    "pmin": 0.0,
                    "pmax": pmax_d,
                    "gcost": 0.0,
                    "capacity": pmax_d,
                }
            )
        return gens

    def to_demand_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build charge-path demand entries."""
        if entries is None:
            entries = self._all_entries()
        dems = []
        for entry in entries:
            dems.append(
                {
                    "uid": BATTERY_UID_OFFSET + entry["number"],
                    "name": f"{entry['name']}_chrg",
                    "bus": entry["bus"],
                    "lmax": "lmax",
                }
            )
        return dems

    def to_converter_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build converter_array JSON list."""
        if entries is None:
            entries = self._all_entries()
        convs = []
        for entry in entries:
            num = entry["number"]
            convs.append(
                {
                    "uid": num,
                    "name": entry["name"],
                    "battery": num,
                    "generator": BATTERY_UID_OFFSET + num,
                    "demand": BATTERY_UID_OFFSET + num,
                    "capacity": entry["pmax_discharge"],
                }
            )
        return convs

    def _write_lmax_parquet(
        self, entries: List[Dict[str, Any]], output_dir: Path
    ) -> None:
        """Append battery charge-limit columns to Demand/lmax.parquet."""
        if not entries:
            return

        lmax_path = output_dir / "lmax.parquet"
        df = pd.read_parquet(lmax_path) if lmax_path.exists() else pd.DataFrame()

        if df.empty or "block" not in df.columns:
            df = pd.DataFrame({"block": [1]})

        blocks = df["block"].tolist()

        for entry in entries:
            num = entry["number"]
            col = f"uid:{BATTERY_UID_OFFSET + num}"
            default_lmax = entry["pmax_charge"]
            col_values = [default_lmax] * len(blocks)

            # Apply per-stage maintenance overrides when available
            if self._man_parser and "stage" in df.columns:
                man = self._man_parser.get_item_by_name(entry["name"])
                if man is not None:
                    stage_lmax = {
                        int(s): float(v)
                        for s, v in zip(man["stage"], man["pmax_charge"])
                    }
                    col_values = [
                        stage_lmax.get(int(stg), default_lmax)
                        for stg in df["stage"].tolist()
                    ]

            df[col] = col_values

        df.to_parquet(lmax_path, index=False, compression=self.get_compression())

    def process(
        self, existing_gen: List[Dict], existing_dem: List[Dict], output_dir: Path
    ) -> Dict[str, Any]:
        """Produce all battery output arrays.

        Args:
            existing_gen: Generator array from CentralWriter to append to.
            existing_dem: Demand array from DemandWriter to append to.
            output_dir: Base output directory (Demand/ sub-dir for parquet).

        Returns:
            Dict with battery_array, converter_array, updated generator_array
            and demand_array.
        """
        entries = self._all_entries()
        if not entries:
            return {
                "battery_array": [],
                "converter_array": [],
                "generator_array": existing_gen,
                "demand_array": existing_dem,
            }

        demand_dir = output_dir / "Demand"
        demand_dir.mkdir(parents=True, exist_ok=True)
        self._write_lmax_parquet(entries, demand_dir)

        return {
            "battery_array": self.to_battery_array(entries),
            "converter_array": self.to_converter_array(entries),
            "generator_array": existing_gen + self.to_generator_array(entries),
            "demand_array": existing_dem + self.to_demand_array(entries),
        }
