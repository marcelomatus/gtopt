# -*- coding: utf-8 -*-

"""Writer for converting BESS/ESS data to GTOPT JSON format.

Produces:
  - battery_array   – one Battery per BESS/ESS
  - generator entries (discharge path) appended to generator_array
  - demand entries (charge path) appended to demand_array
  - lmax.parquet columns for charge demands (appended to existing file if any)
  - converter_array

UID allocation:
  Battery  uid = bess_number          (1-based)
  Generator uid = BESS_UID_OFFSET + bess_number
  Demand    uid = BESS_UID_OFFSET + bess_number
  Converter uid = bess_number
"""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict

import numpy as np
import pandas as pd

from .base_writer import BaseWriter
from .bess_parser import BessParser
from .ess_parser import EssParser
from .bus_parser import BusParser
from .stage_parser import StageParser
from .manbess_parser import ManbessParser

BESS_UID_OFFSET = 10000


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
    """Converts BESS/ESS parser data to GTOPT JSON arrays."""

    def __init__(
        self,
        bess_parser: Optional[BessParser] = None,
        ess_parser: Optional[EssParser] = None,
        bus_parser: Optional[BusParser] = None,
        stage_parser: Optional[StageParser] = None,
        manbess_parser: Optional[ManbessParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize BessWriter.

        Args:
            bess_parser: Parser for plpbess.dat (stage-limited BESS)
            ess_parser: Parser for plpess.dat (always-active ESS)
            bus_parser: Parser for plpbar.dat (to look up bus names)
            stage_parser: Parser for plpeta.dat (to build active arrays)
            manbess_parser: Parser for plpmanbess.dat (per-stage overrides)
            options: Writer options dict
        """
        super().__init__(None, options)
        self.bess_parser = bess_parser
        self.ess_parser = ess_parser
        self.bus_parser = bus_parser
        self.stage_parser = stage_parser
        self.manbess_parser = manbess_parser

    def _all_entries(self) -> List[Dict[str, Any]]:
        """Return combined list of BESS + ESS entries."""
        entries: List[Dict[str, Any]] = []
        if self.bess_parser:
            for b in self.bess_parser.besses:
                entries.append({**b, "_is_bess": True})
        if self.ess_parser:
            for e in self.ess_parser.esses:
                entries.append({**e, "_is_bess": False})
        return entries

    def _get_bus_number(self, entry: Dict[str, Any]) -> int:
        """Return the bus number for a BESS/ESS entry."""
        return int(entry.get("bus", 0))

    def _build_active(self, eta_ini: int, eta_fin: int) -> Optional[List[int]]:
        """Build an active-stage list for a stage-limited BESS.

        Returns a list of stage UIDs in [eta_ini, eta_fin], or None if
        stage_parser is unavailable.
        """
        if not self.stage_parser:
            return None
        stages = self.stage_parser.stages
        active = [
            s["number"]
            for s in stages
            if eta_ini <= s["number"] <= eta_fin
        ]
        return active if active else None

    def to_battery_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build the battery_array JSON list."""
        if entries is None:
            entries = self._all_entries()

        batteries: List[BatteryEntry] = []
        for entry in entries:
            number = entry["number"]
            capacity = entry["pmax_discharge"] * entry["hrs_reg"]

            bat: BatteryEntry = {
                "uid": number,
                "name": entry["name"],
                "input_efficiency": entry["nc"],
                "output_efficiency": entry["nd"],
                "vmin": 0.0,
                "vmax": 1.0,
                "vini": entry["vol_ini"],
                "capacity": capacity,
            }

            # Stage-limited BESS: add active list
            if entry.get("_is_bess", True):
                eta_ini = entry.get("eta_ini", 1)
                eta_fin = entry.get("eta_fin", 9999)
                # Only add active if it restricts stages
                if self.stage_parser:
                    total_stages = len(self.stage_parser.stages)
                    if eta_ini > 1 or eta_fin < total_stages:
                        active = self._build_active(eta_ini, eta_fin)
                        if active is not None:
                            bat["active"] = active

            batteries.append(bat)

        return batteries  # type: ignore[return-value]

    def to_generator_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build the generator entries (discharge path) for each BESS/ESS."""
        if entries is None:
            entries = self._all_entries()

        generators = []
        for entry in entries:
            number = entry["number"]
            gen_uid = BESS_UID_OFFSET + number
            bus_number = self._get_bus_number(entry)

            gen = {
                "uid": gen_uid,
                "name": f"{entry['name']}_disch",
                "bus": bus_number,
                "pmin": 0.0,
                "pmax": entry["pmax_discharge"],
                "gcost": 0.0,
                "capacity": entry["pmax_discharge"],
            }
            generators.append(gen)

        return generators

    def to_demand_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build the demand entries (charge path) for each BESS/ESS."""
        if entries is None:
            entries = self._all_entries()

        demands = []
        for entry in entries:
            number = entry["number"]
            dem_uid = BESS_UID_OFFSET + number
            bus_number = self._get_bus_number(entry)

            dem = {
                "uid": dem_uid,
                "name": f"{entry['name']}_chrg",
                "bus": bus_number,
                "lmax": "lmax",
            }
            demands.append(dem)

        return demands

    def to_converter_array(
        self, entries: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Build the converter_array JSON list."""
        if entries is None:
            entries = self._all_entries()

        converters: List[ConverterEntry] = []
        for entry in entries:
            number = entry["number"]
            conv: ConverterEntry = {
                "uid": number,
                "name": entry["name"],
                "battery": number,
                "generator": BESS_UID_OFFSET + number,
                "demand": BESS_UID_OFFSET + number,
                "capacity": entry["pmax_discharge"],
            }
            converters.append(conv)

        return converters  # type: ignore[return-value]

    def _write_lmax_parquet(
        self,
        entries: List[Dict[str, Any]],
        output_dir: Path,
        existing_df: Optional[pd.DataFrame] = None,
    ) -> None:
        """Append BESS charge-limit columns to Demand/lmax.parquet.

        Each BESS/ESS gets a column ``uid:<BESS_UID_OFFSET+number>`` with
        a constant lmax (pmax_charge) across all blocks.  If a manbess
        override exists the stage-level value overrides the default.
        """
        if not entries:
            return

        lmax_path = output_dir / "lmax.parquet"

        # Load or initialise the existing lmax dataframe
        if existing_df is not None:
            df = existing_df.copy()
        elif lmax_path.exists():
            df = pd.read_parquet(lmax_path)
        else:
            df = pd.DataFrame()

        # We need block numbers to build the column
        # Fall back to a single row with block=1 if no stage/block info
        if df.empty or "block" not in df.columns:
            # Build a minimal single-block frame
            df = pd.DataFrame({"block": [1]})

        blocks = df["block"].tolist()

        for entry in entries:
            number = entry["number"]
            col = f"uid:{BESS_UID_OFFSET + number}"
            default_lmax = entry["pmax_charge"]
            col_values = [default_lmax] * len(blocks)

            # Apply manbess per-stage overrides if present
            if self.manbess_parser and self.stage_parser:
                manbess = self.manbess_parser.get_manbess_by_name(entry["name"])
                if manbess is not None:
                    stage_lmax: Dict[int, float] = {}
                    for s_idx, stage_num in enumerate(manbess["stage"].tolist()):
                        stage_lmax[stage_num] = float(
                            manbess["pmax_charge"][s_idx]
                        )

                    # Map block index to stage
                    block_stage = {}
                    for s in self.stage_parser.stages:
                        sn = s["number"]
                        # blocks in df are 1-based indices
                        for b_idx, b in enumerate(blocks):
                            # We can't easily map block→stage without
                            # block_parser here; leave default for now
                            _ = b_idx  # unused

                    # If we have a block→stage mapping in df, use it
                    if "stage" in df.columns:
                        for i, (blk, stg) in enumerate(
                            zip(blocks, df["stage"].tolist())
                        ):
                            _ = blk
                            if stg in stage_lmax:
                                col_values[i] = stage_lmax[stg]

            df[col] = col_values

        df.to_parquet(
            lmax_path,
            index=False,
            compression=self.get_compression(),
        )

    def process(
        self, existing_gen: List[Dict], existing_dem: List[Dict], output_dir: Path
    ) -> Dict[str, Any]:
        """Process all BESS/ESS entries and return result arrays.

        Args:
            existing_gen: Existing generator_array to append discharge gens to.
            existing_dem: Existing demand_array to append charge demands to.
            output_dir: Base output directory (Demand/ sub-dir used for parquet).

        Returns:
            Dict with keys: battery_array, converter_array, and updated
            generator_array and demand_array.
        """
        entries = self._all_entries()
        if not entries:
            return {
                "battery_array": [],
                "converter_array": [],
                "generator_array": existing_gen,
                "demand_array": existing_dem,
            }

        battery_array = self.to_battery_array(entries)
        new_gens = self.to_generator_array(entries)
        new_dems = self.to_demand_array(entries)
        converter_array = self.to_converter_array(entries)

        demand_dir = output_dir / "Demand"
        demand_dir.mkdir(parents=True, exist_ok=True)
        self._write_lmax_parquet(entries, demand_dir)

        return {
            "battery_array": battery_array,
            "converter_array": converter_array,
            "generator_array": existing_gen + new_gens,
            "demand_array": existing_dem + new_dems,
        }
