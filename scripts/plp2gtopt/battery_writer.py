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
* ``plpess.dat`` (via ``ess_parser``) – ESS model; capacity = pmax × hrs_reg.
* ``plpmaness.dat`` (via ``maness_parser``) – per-stage pmax_charge / pmax_discharge
  overrides for ESS (maintenance schedules).

Matching rule: plpcenbat.dat battery name == BAT central name in plpcnfce.dat.

When *no* plpcenbat.dat file is present but BAT centrals exist in plpcnfce.dat,
default battery parameters are applied (FPC=FPD=0.95, emax derived from pmax,
emin=0.0).

UID allocation
--------------
  Battery   uid = bat_central_number    (BAT central number in plpcnfce.dat)
  Generator uid = bat_central_number    (discharge path)
  Demand    uid = bat_central_number    (charge path)
  Converter uid = bat_central_number

Maintenance schedules
---------------------
When ``plpmanbat.dat`` or ``plpmaness.dat`` provides per-block overrides:

**plpmanbat.dat** (battery model, Fortran ``LeeManBat``):
  Each data line contains ``IBind EMin EMax`` (3 fields).
  These modify battery energy bounds per block.  In gtopt, they map to
  Battery ``vmin``/``vmax`` schedule files (``Battery/vmin.parquet`` and
  ``Battery/vmax.parquet``), normalised by the battery capacity.

**plpmaness.dat** (ESS model, Fortran ``LeeManEss``):
  Each data line contains ``IBind Emin Emax DCMin DCMax [DCMod]`` (5-6 fields).
  Energy bounds map to Battery ``vmin``/``vmax`` schedules (same as above).
  DC power bounds map to Generator ``pmax`` (``Generator/pmax.parquet``)
  and Demand ``lmax`` (``Demand/lmax.parquet``) schedule files.
"""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict


from .base_writer import BaseWriter
from .battery_parser import BatteryParser
from .central_parser import CentralParser
from .bus_parser import BusParser
from .stage_parser import StageParser
from .manbat_parser import ManbatParser
from .ess_parser import EssParser
from .maness_parser import ManessParser

# Default battery parameters used when only plpcnfce.dat BAT centrals are present
_DEFAULT_FPC = 0.95
_DEFAULT_FPD = 0.95


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
    """Converts battery/ESS data to GTOPT JSON arrays.

    Combines storage file data with ``plpcnfce.dat`` BAT centrals
    (for UID and pmax_discharge).  Source priority:

    1. ``plpess.dat`` (via ``ess_parser``) – ESS model; capacity = pmax × hrs_reg.
    2. ``plpcenbat.dat`` (via ``battery_parser``) – battery model; capacity = emax.
    3. BAT centrals only → default parameters applied.

    ``plpess.dat`` and ``plpcenbat.dat`` are mutually exclusive; the caller
    (PLPParser) ensures only one is populated.
    """

    def __init__(
        self,
        battery_parser: Optional[BatteryParser] = None,
        ess_parser: Optional[EssParser] = None,
        central_parser: Optional[CentralParser] = None,
        bus_parser: Optional[BusParser] = None,
        stage_parser: Optional[StageParser] = None,
        manbat_parser: Optional[ManbatParser] = None,
        maness_parser: Optional[ManessParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize the BatteryWriter with optional parsers and configuration options."""
        super().__init__(None, options)
        self.battery_parser = battery_parser
        self.ess_parser = ess_parser
        self.central_parser = central_parser
        self.bus_parser = bus_parser
        self.stage_parser = stage_parser
        # Unified maintenance accessor: manbat for battery, maness for ESS
        self._man_parser = manbat_parser or maness_parser

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

        Each entry contains all fields needed by ``to_battery_array()``,
        ``to_generator_array()``, ``to_demand_array()`` and
        ``to_converter_array()``:

          number, name, bus, busc, nc, nd, emin, emax,
          pmax_charge, pmax_discharge, annual_loss, has_maintenance,
          man_stages, man_pmax_charge, man_pmax_discharge.

        Priority:
        1. plpess.dat entries matched to BAT centrals (ESS model)
        2. plpcenbat.dat entries matched to BAT centrals (battery model)
        3. BAT centrals with no matching file → defaults applied
        """
        batteries = self._bat_centrals()
        entries: List[Dict[str, Any]] = []

        # Determine maintenance data accessor
        man_parser = self._man_parser

        if self.ess_parser and self.ess_parser.esses:
            # ESS path: emax from plpess.dat directly (MWh capacity)
            for item in self.ess_parser.esses:
                name = item["name"]
                central = batteries.get(name, {})
                uid = central.get("number", 0)
                bus = central.get("bus", 1)
                nc = item["nc"]
                nd = item["nd"]
                emax = item["emax"]
                dcmax = item["dcmax"]
                mloss = item["mloss"]
                # pmax_discharge: dcmax from ESS, or central pmax
                pmax_discharge = dcmax if dcmax > 0 else central.get("pmax", 0.0)
                pmax_charge = pmax_discharge

                man = man_parser.get_item_by_name(name) if man_parser else None

                entries.append(
                    {
                        "number": uid,
                        "name": name,
                        "bus": bus,
                        "busc": bus,
                        "nc": nc,
                        "nd": nd,
                        "emin": 0.0,
                        "emax": emax,
                        "pmax_charge": pmax_charge,
                        "pmax_discharge": pmax_discharge,
                        "annual_loss": mloss * 12,
                        "has_maintenance": man is not None,
                        "man_data": man,
                    }
                )
        elif self.battery_parser and self.battery_parser.batteries:
            for item in self.battery_parser.batteries:
                name = item["name"]
                central = batteries.get(name, {})
                # UID and pmax_discharge come from BAT central (authoritative)
                uid = central.get("number", item["number"])
                bus = item.get("bus", central.get("bus", 1))
                # Energy capacity directly from plpcenbat.dat
                emax = item.get("emax", 0.0)
                emin = item.get("emin", 0.0)
                # Efficiencies from plpcenbat.dat
                injections = item.get("injections", [])
                nc = injections[0]["fpc"] if injections else _DEFAULT_FPC
                nd = item.get("fpd", _DEFAULT_FPD)
                pmax_discharge = central.get("pmax", 0.0)
                pmax_charge = pmax_discharge

                man = man_parser.get_item_by_name(name) if man_parser else None

                entries.append(
                    {
                        "number": uid,
                        "name": name,
                        "bus": bus,
                        "busc": bus,
                        "nc": nc,
                        "nd": nd,
                        "emin": emin,
                        "emax": emax,
                        "pmax_charge": pmax_charge,
                        "pmax_discharge": pmax_discharge,
                        "annual_loss": 0.0,
                        "has_maintenance": man is not None,
                        "man_data": man,
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
            has_man = entry.get("has_maintenance")
            bat: Dict[str, Any] = {
                "uid": entry["number"],
                "name": entry["name"],
                "input_efficiency": entry["nc"],
                "output_efficiency": entry["nd"],
                "vmin": "vmin" if has_man else vmin,
                "vmax": "vmax" if has_man else 1.0,
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
            man = entry.get("man_data")
            has_dc_man = man is not None and "dcmax" in man
            pmax_val: Any = "pmax" if has_dc_man else pmax_d
            gens.append(
                {
                    "uid": entry["number"],
                    "name": f"{entry['name']}_disch",
                    "bus": entry["bus"],
                    "pmin": 0.0,
                    "pmax": pmax_val,
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
            man = entry.get("man_data")
            has_dc_man = man is not None and "dcmax" in man
            lmax_val: Any = "lmax" if has_dc_man else entry["pmax_charge"]
            dems.append(
                {
                    "uid": entry["number"],
                    "name": f"{entry['name']}_chrg",
                    "bus": entry["busc"],
                    "lmax": lmax_val,
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
                    "generator": num,
                    "demand": num,
                    "capacity": entry.get("capacity", entry["pmax_discharge"]),
                }
            )
        return convs

    def _write_lmax_parquet(
        self, entries: List[Dict[str, Any]], output_dir: Path
    ) -> None:
        """Write charge demand lmax values to Demand/lmax.parquet.

        When maintenance schedules are present, the per-stage pmax_charge
        values are written as block-level columns.
        """
        # Placeholder: maintenance parquet writing is handled in process()

    def _write_maintenance_parquet(
        self, entries: List[Dict[str, Any]], output_dir: Path
    ) -> None:
        """Write maintenance-schedule parquet files for battery/ESS.

        For entries with ``has_maintenance == True``, writes:

        **plpmanbat.dat** (battery model, ``man_data`` has ``emin``/``emax``):
        - ``Battery/vmin.parquet`` with per-block vmin = emin/capacity
        - ``Battery/vmax.parquet`` with per-block vmax = emax/capacity

        **plpmaness.dat** (ESS model, ``man_data`` has ``emin``/``emax``
        + ``dcmin``/``dcmax``):
        - ``Battery/vmin.parquet`` + ``Battery/vmax.parquet`` (energy bounds)
        - ``Generator/pmax.parquet`` (dcmax → discharge gen pmax)
        - ``Demand/lmax.parquet`` (dcmax → charge demand lmax)
        """
        import pandas as pd  # pylint: disable=import-outside-toplevel

        man_entries = [e for e in entries if e.get("has_maintenance")]
        if not man_entries:
            return

        # --- Battery energy bounds → Battery/vmin.parquet, Battery/vmax.parquet ---
        bat_dir = output_dir / "Battery"
        bat_dir.mkdir(parents=True, exist_ok=True)
        vmin_data: Dict[str, Any] = {"block": []}
        vmax_data: Dict[str, Any] = {"block": []}
        for entry in man_entries:
            man = entry["man_data"]
            col = f"uid:{entry['number']}"
            block_idx = man["block_index"]
            capacity = entry["emax"]
            if len(block_idx) > 0 and capacity > 0:
                vmin_data["block"] = list(block_idx)
                vmax_data["block"] = list(block_idx)
                vmin_data[col] = [e / capacity for e in man["emin"]]
                vmax_data[col] = [e / capacity for e in man["emax"]]
        if vmin_data["block"]:
            pd.DataFrame(vmin_data).to_parquet(bat_dir / "vmin.parquet", index=False)
            pd.DataFrame(vmax_data).to_parquet(bat_dir / "vmax.parquet", index=False)

        # --- DC power bounds (ESS only) → Generator/pmax, Demand/lmax ---
        has_dc = any("dcmax" in e.get("man_data", {}) for e in man_entries)
        if not has_dc:
            return

        gen_dir = output_dir / "Generator"
        gen_dir.mkdir(parents=True, exist_ok=True)
        pmax_data: Dict[str, Any] = {"block": []}
        for entry in man_entries:
            man = entry["man_data"]
            if "dcmax" not in man:
                continue
            col = f"uid:{entry['number']}"
            block_idx = man["block_index"]
            if len(block_idx) > 0:
                pmax_data["block"] = list(block_idx)
                pmax_data[col] = list(man["dcmax"])
        if pmax_data["block"]:
            pd.DataFrame(pmax_data).to_parquet(gen_dir / "pmax.parquet", index=False)

        dem_dir = output_dir / "Demand"
        dem_dir.mkdir(parents=True, exist_ok=True)
        lmax_data: Dict[str, Any] = {"block": []}
        for entry in man_entries:
            man = entry["man_data"]
            if "dcmax" not in man:
                continue
            col = f"uid:{entry['number']}"
            block_idx = man["block_index"]
            if len(block_idx) > 0:
                lmax_data["block"] = list(block_idx)
                lmax_data[col] = list(man["dcmax"])
        if lmax_data["block"]:
            pd.DataFrame(lmax_data).to_parquet(dem_dir / "lmax.parquet", index=False)

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

        # Write maintenance schedule parquet files if needed
        self._write_maintenance_parquet(entries, output_dir)

        return {
            "battery_array": self.to_battery_array(entries),
            "converter_array": self.to_converter_array(entries),
            "generator_array": existing_gen + self.to_generator_array(entries),
            "demand_array": existing_dem + self.to_demand_array(entries),
        }
