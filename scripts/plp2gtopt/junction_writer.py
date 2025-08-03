# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from typing import Any, Dict, List, Optional, cast


from .base_writer import BaseWriter
from .central_parser import CentralParser
from .extrac_parser import ExtracParser
from .aflce_parser import AflceParser


class JunctionWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        extrac_parser: Optional[ExtracParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)
        self.aflce_parser = aflce_parser
        self.extrac_parser = extrac_parser
        self.options: Dict[str, Any] = options if options is not None else {}

        self.num_waterways: int = 0

    def create_waterway(
        self,
        central_name: str,
        central_number: int,
        junction: int,
        max_extrac: Optional[float] = None,
    ) -> Optional[Dict[str, Any]]:
        """Create a waterway dictionary if the junction is valid."""
        if junction == 0:
            return None

        self.num_waterways += 1
        wway = {
            "uid": self.num_waterways,
            "name": f"{central_name}_{central_number}_{junction}",
            "junction_a": central_number,
            "junction_b": junction,
        }

        if max_extrac is not None:
            wway["capacity"] = max_extrac

        return wway

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        central_parser = cast(CentralParser, self.parser)
        if items is None:
            items = (
                central_parser.centrals_of_type.get("embalse", [])
                + central_parser.centrals_of_type.get("serie", [])
                if central_parser
                else []
            )
        if not items:
            return []

        json_junctions: List[Dict[str, Any]] = []
        json_waterways: List[Dict[str, Any]] = []
        json_flows: List[Dict[str, Any]] = []
        json_turbines: List[Dict[str, Any]] = []

        # Process centrals
        for central in items:
            central_name: str = central["name"]
            central_number: int = central["number"]

            wway_gen = self.create_waterway(
                central_name,
                central_number,
                central.get("ser_hid", 0),
            )
            if wway_gen:
                # If the gen waterway is created, add it to the list and create a turbine
                json_waterways.append(wway_gen)
                if central["bus"] > 0:
                    # Create a turbine only if the bus is valid
                    turbine = {
                        "uid": central_number,
                        "name": central_name,
                        "generator": central_number,
                        "waterway": wway_gen["uid"],
                        "convertion_rate": central["efficiency"],
                    }
                    json_turbines.append(turbine)

            wway_ver = self.create_waterway(
                central_name,
                central_number,
                central.get("ser_ver", 0),
            )
            if wway_ver:
                json_waterways.append(wway_ver)

            drain = not (wway_gen and wway_ver)

            junction: Dict[str, Any] = {
                "uid": central_number,
                "name": central_name,
                "drain": drain,
            }
            json_junctions.append(junction)

            aflce = (
                self.aflce_parser.get_item_by_name(central_name)
                if self.aflce_parser
                else None
            )
            afluent = central.get("afluent", 0.0) if aflce is None else "afluent"
            if afluent != 0.0:
                flow = {
                    "uid": central_number,
                    "name": central_name,
                    "junction": central_number,
                    "discharge": afluent,
                }
                json_flows.append(flow)

        # Process extractions
        items = self.extrac_parser.extracs if self.extrac_parser else []
        for extrac in items:
            upstream = extrac
            upstream_name = upstream["name"]
            upstream_number = upstream["number"]

            downstram_name = extrac["downstream"]
            downstream = central_parser.get_central_by_name(downstram_name)
            if not downstream:
                print(
                    f"Skipping extraction {upstream_name} "
                    "invalid downstream {downstram_name}"
                )
                continue
            downstream_number = downstream["number"]

            max_extract = extrac.get("max_extrac", None)
            wway_extract = self.create_waterway(
                upstream_name, upstream_number, downstream_number, max_extract
            )
            if wway_extract:
                json_waterways.append(wway_extract)

        # Process reservoirs
        items = (
            central_parser.centrals_of_type.get("embalse", []) if central_parser else []
        )
        json_reservoirs = []
        for embalse in items:
            reservoir = {
                "uid": embalse["number"],
                "name": embalse["name"],
                "junction": embalse["number"],
                "vini": embalse["vol_ini"],
                "vfin": embalse["vol_fin"],
                "vmin": embalse["vol_min"],
                "vmax": embalse["vol_max"],
                "capacity": embalse["vol_max"],
            }

            json_reservoirs.append(reservoir)

        # Returns all the hydro elements
        return [
            {
                "junction_array": json_junctions,
                "waterway_array": json_waterways,
                "flow_array": json_flows,
                "reservoir_array": json_reservoirs,
            }
        ]
