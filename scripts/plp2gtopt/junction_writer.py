# -*- coding: utf-8 -*-

"""Writer for converting central data to hydro system JSON format.

Converts central plant data into:
- Junctions (nodes in the hydro system)
- Waterways (connections between nodes)
- Flows (water discharges)
- Reservoirs (storage nodes)
- Turbines (energy conversion points)
"""

from typing import Any, Dict, List, Optional, cast
from dataclasses import dataclass

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .extrac_parser import ExtracParser
from .aflce_parser import AflceParser


# class Waterway(TypedDict):
#     """Represents a waterway connection between junctions in the hydro system."""

#     uid: int
#     name: str
#     junction_a: int
#     junction_b: int
#     capacity: Optional[float]

# class HydroSystemOutput(TypedDict):
#     """Output structure for hydro system JSON format."""

#     junction_array: List[Dict[str, Any]]
#     waterway_array: List[Waterway]
#     flow_array: List[Dict[str, Any]]
#     reservoir_array: List[Dict[str, Any]]
#     turbine_array: List[Dict[str, Any]]


Waterway = Dict[str, Any]
HydroSystemOutput = Dict[str, Any]


@dataclass
class HydroElement:
    """Base class for hydro system elements."""

    uid: int
    name: str


class JunctionWriter(BaseWriter):
    """Converts central plant data to hydro system JSON format for GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        extrac_parser: Optional[ExtracParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize hydro system writer.

        Args:
            central_parser: Parser for central plant data
            aflce_parser: Parser for flow data
            extrac_parser: Parser for extraction data
            options: Configuration options for the writer
        """
        super().__init__(central_parser)
        self.aflce_parser = aflce_parser
        self.extrac_parser = extrac_parser
        self.options = options or {}
        self._waterway_counter = 0

    def _create_waterway(
        self,
        source_name: str,
        source_id: int,
        target_id: int,
        capacity: Optional[float] = None,
    ) -> Optional[Waterway]:
        """Create a waterway connection between two junctions.

        Args:
            source_name: Name of source junction
            source_id: ID of source junction
            target_id: ID of target junction (0 means no connection)
            capacity: Optional maximum flow capacity

        Returns:
            Waterway dictionary or None if target_id is 0
        """
        if target_id == 0:
            return None

        self._waterway_counter += 1
        waterway: Waterway = {
            "uid": self._waterway_counter,
            "name": f"{source_name}_{source_id}_{target_id}",
            "junction_a": source_id,
            "junction_b": target_id,
        }

        if capacity is not None:
            waterway["capacity"] = capacity

        return waterway

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[HydroSystemOutput]:
        """Convert central plant data to hydro system JSON format.

        Args:
            items: Optional list of central plants to process. If None,
                   uses embalse and serie type plants from central_parser.

        Returns:
            List containing single hydro system dictionary with all elements
        """
        central_parser = cast(CentralParser, self.parser)

        # Get default items if none provided
        if items is None and central_parser:
            items = (
                central_parser.centrals_of_type.get("embalse", [])
                + central_parser.centrals_of_type.get("serie", [])
            ) or []

        if not items:
            return []

        system: HydroSystemOutput = {
            "junction_array": [],
            "waterway_array": [],
            "flow_array": [],
            "reservoir_array": [],
            "turbine_array": [],
        }

        # Process central plants
        for plant in items:
            self._process_plant(plant, system, central_parser)

        # Process extraction plants
        if self.extrac_parser and central_parser:
            self._process_extractions(system, central_parser)

        # Process reservoirs
        if central_parser:
            self._process_reservoirs(system, central_parser)

        return [system]

    def _process_plant(
        self,
        plant: Dict[str, Any],
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process a single central plant into hydro system elements."""
        plant_id = plant["number"]
        plant_name = plant["name"]

        # Create waterways
        gen_waterway = self._create_waterway(
            plant_name, plant_id, plant.get("ser_hid", 0)
        )
        ver_waterway = self._create_waterway(
            plant_name, plant_id, plant.get("ser_ver", 0)
        )

        # Add waterways if they exist
        if gen_waterway:
            system["waterway_array"].append(gen_waterway)
            if plant["bus"] > 0:  # Only create turbine if connected to bus
                system["turbine_array"].append(
                    {
                        "uid": plant_id,
                        "name": plant_name,
                        "generator": plant_id,
                        "waterway": gen_waterway["uid"],
                        "convertion_rate": plant["efficiency"],
                    }
                )

        if ver_waterway:
            system["waterway_array"].append(ver_waterway)

        # Create junction
        system["junction_array"].append(
            {
                "uid": plant_id,
                "name": plant_name,
                "drain": not (gen_waterway and ver_waterway),
            }
        )

        # Add flow if exists
        afluent = self._get_plant_flow(plant_name, plant)
        if afluent != 0.0:
            system["flow_array"].append(
                {
                    "uid": plant_id,
                    "name": plant_name,
                    "junction": plant_id,
                    "discharge": afluent,
                }
            )

    def _get_plant_flow(self, plant_name: str, plant: Dict[str, Any]) -> float | str:
        """Get flow value for plant, checking aflce parser if available."""
        if self.aflce_parser:
            aflce = self.aflce_parser.get_item_by_name(plant_name)
            if aflce is not None:
                return "afluent"
        return plant.get("afluent", 0.0)

    def _process_extractions(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process extraction plants into waterways."""
        items = self.extrac_parser.extracs if self.extrac_parser else []
        for extraction in items:
            upstream_id = extraction["number"]
            upstream_name = extraction["name"]
            downstream_name = extraction["downstream"]

            downstream = central_parser.get_central_by_name(downstream_name)
            if not downstream:
                continue  # Skip invalid downstream

            waterway = self._create_waterway(
                upstream_name,
                upstream_id,
                downstream["number"],
                extraction.get("max_extrac"),
            )
            if waterway:
                system["waterway_array"].append(waterway)

    def _process_reservoirs(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process reservoir plants into reservoir elements."""
        reservoirs = central_parser.centrals_of_type.get("embalse", [])
        for reservoir in reservoirs:
            system["reservoir_array"].append(
                {
                    "uid": reservoir["number"],
                    "name": reservoir["name"],
                    "junction": reservoir["number"],
                    "vini": reservoir["vol_ini"],
                    "vfin": reservoir["vol_fin"],
                    "vmin": reservoir["vol_min"],
                    "vmax": reservoir["vol_max"],
                    "capacity": reservoir["vol_max"],
                }
            )
