# -*- coding: utf-8 -*-

"""Writer for converting BESS/ESS data to GTOPT JSON format.

Data sources and their roles
-----------------------------
* ``plpcnfce.dat`` (BAT section via ``central_parser``) – **primary** source for
  battery name, bus number, pmax_discharge (= central pmax), pmin, gcost.
  Present for both BESS and ESS models.
* ``plpbess.dat`` / ``plpess.dat`` (via ``bess_parser`` / ``ess_parser``) –
  **storage-specific** parameters: pmax_charge, nc, nd, hrs_reg, vol_ini;
  for BESS also eta_ini, eta_fin, n_ciclos.  Mutually exclusive.
* ``plpmanbess.dat`` / ``plpmaness.dat`` – per-stage pmax_charge / pmax_discharge
  overrides (maintenance schedules).

Matching rule: BESS/ESS file entry name == BAT central name.

When *no* BESS/ESS file is present but BAT centrals exist in plpcnfce.dat,
default storage parameters are applied (nc=nd=0.95, hrs_reg=4.0, vol_ini=0.5,
pmax_charge = pmax_discharge).

UID allocation
--------------
  Battery   uid = bat_number          (central number for BAT type)
  Generator uid = BESS_UID_OFFSET + bat_number   (discharge path)
  Demand    uid = BESS_UID_OFFSET + bat_number   (charge path)
  Converter uid = bat_number
"""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict

import pandas as pd

from .base_writer import BaseWriter
from .bess_parser import BessParser
from .ess_parser import EssParser
from .central_parser import CentralParser
from .bus_parser import BusParser
from .stage_parser import StageParser
from .manbess_parser import ManbessParser
from .maness_parser import ManessParser

BESS_UID_OFFSET = 10000

# Default storage parameters used when only plpcnfce.dat BAT centrals are present
_DEFAULT_NC = 0.95
_DEFAULT_ND = 0.95
_DEFAULT_HRS_REG = 4.0
_DEFAULT_VOL_INI = 0.5


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


class BessWriter(BaseWriter):
    """Converts BESS/ESS data to GTOPT JSON arrays.

    Combines ``plpcnfce.dat`` BAT centrals (bus, pmax_discharge) with
    ``plpbess.dat`` / ``plpess.dat`` storage parameters.  When no storage
    parameter file is present, defaults are applied to BAT centrals.
    """

    def __init__(
        self,
        bess_parser: Optional[BessParser] = None,
        ess_parser: Optional[EssParser] = None,
        central_parser: Optional[CentralParser] = None,
        bus_parser: Optional[BusParser] = None,
        stage_parser: Optional[StageParser] = None,
        manbess_parser: Optional[ManbessParser] = None,
        maness_parser: Optional[ManessParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        super().__init__(None, options)
        self.bess_parser = bess_parser
        self.ess_parser = ess_parser
        self.central_parser = central_parser
        self.bus_parser = bus_parser
        self.stage_parser = stage_parser
        # Unified maintenance accessor: BESS file takes priority
        self._man_parser = manbess_parser or maness_parser

    def _bat_centrals(self) -> Dict[str, Dict[str, Any]]:
        """Return BAT-type centrals from plpcnfce.dat keyed by name."""
        if not self.central_parser:
            return {}
        return {
            c["name"]: c
            for c in self.central_parser.centrals
            if c.get("type") == "bateria"
        }

    def _all_entries(self) -> List[Dict[str, Any]]:
        """Build the unified list of battery entries.

        Priority:
        1. plpbess.dat entries matched to BAT centrals (BESS model)
        2. plpess.dat entries matched to BAT centrals (ESS model)
        3. BAT centrals with no matching file → defaults applied
        """
        bat = self._bat_centrals()
        entries: List[Dict[str, Any]] = []

        def _merge(storage_entries: List[Dict], is_bess: bool) -> None:
            for item in storage_entries:
                name = item["name"]
                central = bat.get(name, {})
                # Bus and pmax_discharge come from BAT central (authoritative);
                # fall back to values in the storage file if central absent.
                bus = central.get("bus", item.get("bus", 0))
                pmax_d = central.get("pmax", item.get("pmax_discharge", 0.0))
                entries.append(
                    {
                        "number": central.get("number", item["number"]),
                        "name": name,
                        "bus": bus,
                        "pmax_discharge": pmax_d,
                        "pmax_charge": item.get("pmax_charge", pmax_d),
                        "nc": item.get("nc", _DEFAULT_NC),
                        "nd": item.get("nd", _DEFAULT_ND),
                        "hrs_reg": item.get("hrs_reg", _DEFAULT_HRS_REG),
                        "vol_ini": item.get("vol_ini", _DEFAULT_VOL_INI),
                        "eta_ini": item.get("eta_ini", 1),
                        "eta_fin": item.get("eta_fin", 99999),
                        "n_ciclos": item.get("n_ciclos", 1.0),
                        "_is_bess": is_bess,
                    }
                )

        if self.bess_parser and self.bess_parser.besses:
            _merge(self.bess_parser.besses, True)
        elif self.ess_parser and self.ess_parser.esses:
            _merge(self.ess_parser.esses, False)
        else:
            # No storage file – derive entries from BAT centrals with defaults
            for name, central in bat.items():
                pmax_d = central.get("pmax", 0.0)
                entries.append(
                    {
                        "number": central["number"],
                        "name": name,
                        "bus": central.get("bus", 0),
                        "pmax_discharge": pmax_d,
                        "pmax_charge": pmax_d,
                        "nc": _DEFAULT_NC,
                        "nd": _DEFAULT_ND,
                        "hrs_reg": _DEFAULT_HRS_REG,
                        "vol_ini": _DEFAULT_VOL_INI,
                        "eta_ini": 1,
                        "eta_fin": 99999,
                        "n_ciclos": 1.0,
                        "_is_bess": False,
                    }
                )

        return entries

    def _build_active(self, eta_ini: int, eta_fin: int) -> Optional[List[int]]:
        """Return stage UIDs in [eta_ini, eta_fin], or None if no restriction."""
        if not self.stage_parser:
            return None
        stages = self.stage_parser.stages
        total = len(stages)
        if eta_ini <= 1 and eta_fin >= total:
            return None  # all stages – omit active field
        active = [s["number"] for s in stages if eta_ini <= s["number"] <= eta_fin]
        return active or None

    def to_battery_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build battery_array JSON list."""
        if entries is None:
            entries = self._all_entries()

        batteries = []
        for entry in entries:
            bat: Dict[str, Any] = {
                "uid": entry["number"],
                "name": entry["name"],
                "input_efficiency": entry["nc"],
                "output_efficiency": entry["nd"],
                "vmin": 0.0,
                "vmax": 1.0,
                "vini": entry["vol_ini"],
                "capacity": entry["pmax_discharge"] * entry["hrs_reg"],
            }
            if entry.get("_is_bess", False):
                active = self._build_active(entry["eta_ini"], entry["eta_fin"])
                if active is not None:
                    bat["active"] = active
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
                    "uid": BESS_UID_OFFSET + entry["number"],
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
                    "uid": BESS_UID_OFFSET + entry["number"],
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
                    "generator": BESS_UID_OFFSET + num,
                    "demand": BESS_UID_OFFSET + num,
                    "capacity": entry["pmax_discharge"],
                }
            )
        return convs

    def _write_lmax_parquet(
        self, entries: List[Dict[str, Any]], output_dir: Path
    ) -> None:
        """Append BESS/ESS charge-limit columns to Demand/lmax.parquet."""
        if not entries:
            return

        lmax_path = output_dir / "lmax.parquet"
        df = pd.read_parquet(lmax_path) if lmax_path.exists() else pd.DataFrame()

        if df.empty or "block" not in df.columns:
            df = pd.DataFrame({"block": [1]})

        blocks = df["block"].tolist()

        for entry in entries:
            num = entry["number"]
            col = f"uid:{BESS_UID_OFFSET + num}"
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
        """Produce all BESS/ESS output arrays.

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
