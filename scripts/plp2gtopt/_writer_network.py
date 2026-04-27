# -*- coding: utf-8 -*-

"""Network domain mixin for :class:`plp2gtopt.gtopt_writer.GTOptWriter`.

Holds the bus / line / demand / battery converters from PLP parsed data
into gtopt JSON arrays.  See :mod:`plp2gtopt.gtopt_writer` for the
host class wiring; this module purely encapsulates the methods.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict

from .battery_writer import BatteryWriter
from .bus_writer import BusWriter
from .demand_writer import DemandWriter
from .line_writer import LineWriter


class NetworkMixin:
    """Bus / line / demand / battery processing for ``GTOptWriter``."""

    # Attributes provided by the host class.
    parser: Any
    planning: Dict[str, Dict[str, Any]]

    # ``_falla_by_bus`` is provided by
    # :class:`plp2gtopt._writer_generation.GenerationMixin` — reachable
    # through Python's MRO when both mixins are composed into ``GTOptWriter``.

    def process_demands(self, options):
        """Process demand data to include block and stage information."""
        demands = self.parser.parsed_data.get("demand_parser", [])
        if not demands:
            return

        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        dems = demands.get_all()
        for demand in dems:
            bus = buses.get_bus_by_name(demand["name"])
            if bus is None:
                demand["bus"] = 0  # mark as unknown; DemandWriter skips bus==0
            else:
                demand["bus"] = bus["number"]

        blocks = self.parser.parsed_data.get("block_parser", [])
        demand_writer = DemandWriter(demands, blocks, options)
        demand_array = demand_writer.to_json_array()

        # Set fcost from falla centrals (bus → min gcost falla)
        falla_by_bus = self._falla_by_bus()
        if falla_by_bus:
            # Write Demand/fcost for fallas with cost schedules
            filed_buses = demand_writer.write_fcost(
                demand_array,
                falla_by_bus,
                self.parser.parsed_data.get("cost_parser"),
                self.parser.parsed_data.get("stage_parser"),
                self.parser.parsed_data.get("central_parser"),
            )
            for dem in demand_array:
                bus = dem.get("bus")
                if bus in falla_by_bus:
                    if bus in filed_buses:
                        dem["fcost"] = "fcost"
                    else:
                        dem["fcost"] = falla_by_bus[bus].get("gcost", 0.0)

        self.planning["system"]["demand_array"] = demand_array

    def process_buses(self):
        """Process bus data to include block and stage information."""
        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        self.planning["system"]["bus_array"] = BusWriter(buses).to_json_array()

    def process_lines(self, options):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        manlis = self.parser.parsed_data.get("manli_parser", None)

        self.planning["system"]["line_array"] = LineWriter(
            lines, blocks, manlis, options
        ).to_json_array()

    def process_battery(self, options):
        """Process battery/ESS data and append to existing arrays."""
        battery_parser = self.parser.parsed_data.get("battery_parser", None)
        ess_parser = self.parser.parsed_data.get("ess_parser", None)
        centrals = self.parser.parsed_data.get("central_parser", None)

        # Proceed if any storage source is available
        has_battery = centrals and any(
            c.get("type") == "bateria" for c in centrals.centrals
        )
        if battery_parser is None and ess_parser is None and not has_battery:
            return

        stages = self.parser.parsed_data.get("stage_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        manbat = self.parser.parsed_data.get("manbat_parser", None)
        maness = self.parser.parsed_data.get("maness_parser", None)

        output_dir = Path(options["output_dir"]) if options else Path("results")

        writer = BatteryWriter(
            battery_parser=battery_parser,
            ess_parser=ess_parser,
            central_parser=centrals,
            bus_parser=buses,
            stage_parser=stages,
            manbat_parser=manbat,
            maness_parser=maness,
            options=options,
        )

        existing_gen = self.planning["system"].get("generator_array", [])
        existing_dem = self.planning["system"].get("demand_array", [])

        result = writer.process(existing_gen, existing_dem, output_dir)

        self.planning["system"]["battery_array"] = result["battery_array"]
        self.planning["system"]["generator_array"] = result["generator_array"]
        self.planning["system"]["demand_array"] = result["demand_array"]
        if writer._clamped_warnings:  # noqa: SLF001
            self.planning.setdefault("_clamped_battery_warnings", []).extend(
                writer._clamped_warnings  # noqa: SLF001
            )
        if "converter_array" in result:
            self.planning["system"]["converter_array"] = result["converter_array"]

        # DCMod=2 regulation reservoirs: append to existing reservoir_array
        reg_reservoirs = result.get("regulation_reservoirs", [])
        if reg_reservoirs:
            existing_rsv = self.planning["system"].get("reservoir_array", [])
            self.planning["system"]["reservoir_array"] = existing_rsv + reg_reservoirs
