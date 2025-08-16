# -*- coding: utf-8 -*-

"""Writer for converting central data to hydro system JSON format.

Converts central plant data into:
- Junctions (nodes in the hydro system)
- Waterways (connections between nodes)
- Flows (water discharges)
- Reservoirs (storage nodes)
- Turbines (energy conversion points)
"""

from typing import Any, Dict, List, Optional, cast, TypedDict

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .extrac_parser import ExtracParser
from .aflce_parser import AflceParser


class Waterway(TypedDict, total=False):
    """Represents a waterway connection between junctions in the hydro system."""

    uid: int
    name: str
    junction_a: int
    junction_b: int
    fmin: float
    fmax: float
    capacity: float


class Junction(TypedDict):
    """Represents a node in the hydro system."""

    uid: int
    name: str
    drain: bool


class Flow(TypedDict):
    """Represents a water discharge in the hydro system."""

    uid: int
    name: str
    junction: int
    discharge: float | str


class Reservoir(TypedDict):
    """Represents a storage node in the hydro system."""

    uid: int
    name: str
    junction: int
    vini: float
    vfin: float
    vmin: float
    vmax: float
    capacity: float


class Turbine(TypedDict):
    """Represents an energy conversion point in the hydro system."""

    uid: int
    name: str
    generator: int
    waterway: int
    conversion_rate: float


class HydroSystemOutput(TypedDict):
    """Output structure for hydro system JSON format."""

    junction_array: List[Junction]
    waterway_array: List[Waterway]
    flow_array: List[Flow]
    reservoir_array: List[Reservoir]
    turbine_array: List[Turbine]


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
        super().__init__(central_parser, options)
        self.aflce_parser = aflce_parser
        self.extrac_parser = extrac_parser
        self._waterway_counter = 0

    def _create_waterway(
        self,
        source_name: str,
        source_id: int,
        target_id: int,
        fmin: float = 0.0,
        fmax: Optional[float] = None,
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
            "fmin": fmin,
        }

        if fmax is not None:
            waterway["fmax"] = fmax
        if capacity is not None:
            waterway["capacity"] = capacity

        return waterway

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
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
        for central in items:
            self._process_central(central, system, central_parser)

        # Process reservoirs
        if central_parser:
            self._process_reservoirs(system, central_parser)

        # Process extraction plants
        if self.extrac_parser and central_parser:
            self._process_extractions(system, central_parser)

        return [cast(Dict[str, Any], system)]

    def _process_central(
        self,
        central: Dict[str, Any],
        system: HydroSystemOutput,
        _central_parser: CentralParser,
    ) -> None:
        """Process a single central central into hydro system elements."""
        central_id = central["number"]
        central_name = central["name"]

        # Create waterways
        gen_waterway = self._create_waterway(
            central_name + "_gen",
            central_id,
            central["ser_hid"],
        )
        ver_waterway = self._create_waterway(
            central_name + "_ver",
            central_id,
            central["ser_ver"],
            central["vert_min"],
            central["vert_max"],
        )

        # Add waterways if they exist
        if gen_waterway:
            system["waterway_array"].append(gen_waterway)
            if central["bus"] > 0:  # Only create turbine if connected to bus
                turbine: Turbine = {
                    "uid": central_id,
                    "name": central_name,
                    "generator": central_id,
                    "waterway": gen_waterway["uid"],
                    "conversion_rate": central["efficiency"],
                }
                system["turbine_array"].append(turbine)

        if ver_waterway:
            system["waterway_array"].append(ver_waterway)

        # Create junction
        junction: Junction = {
            "uid": central_id,
            "name": central_name,
            "drain": not (gen_waterway and ver_waterway),
        }
        system["junction_array"].append(junction)

        # Add flow if exists
        afluent = self._get_central_flow(central_name, central)
        if isinstance(afluent, float) and afluent == 0.0:
            return

        flow: Flow = {
            "uid": central_id,
            "name": central_name,
            "junction": central_id,
            "discharge": afluent,
        }
        system["flow_array"].append(flow)

    def _get_central_flow(
        self, central_name: str, central: Dict[str, Any]
    ) -> float | str:
        """Get flow value for central, checking aflce parser if available."""
        if self.aflce_parser:
            aflce = self.aflce_parser.get_item_by_name(central_name)
            if aflce is not None:
                return "Afluent@afluent"
        return central.get("afluent", 0.0)

    def _process_extractions(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process extraction centrals into waterways."""
        if not self.extrac_parser:
            return
        for i, extraction in enumerate(self.extrac_parser.extracs):
            upstream_name = extraction["name"]
            upstream_central = central_parser.get_central_by_name(upstream_name)
            if not upstream_central:
                print(
                    f"Warning: Upstream central '{upstream_name}' not found in central parser."
                )
                continue

            downstream_name = extraction["downstream"]
            downstream_central = central_parser.get_central_by_name(downstream_name)
            if not downstream_central:
                print(
                    f"Warning: Downstream central '{downstream_name}' "
                    "not found for extraction '{upstream_name}'"
                )

                continue  # Skip invalid downstream

            waterway = self._create_waterway(
                upstream_name + "_extrac_" + str(i),
                upstream_central["number"],
                downstream_central["number"],
                fmin=0.0,
                fmax=extraction.get("max_extrac", 0.0),
            )
            if waterway:
                system["waterway_array"].append(waterway)

    def _process_reservoirs(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process reservoir centrals into reservoir elements."""
        reservoirs = central_parser.centrals_of_type.get("embalse", [])
        for reservoir_data in reservoirs:
            reservoir: Reservoir = {
                "uid": reservoir_data["number"],
                "name": reservoir_data["name"],
                "junction": reservoir_data["number"],
                "vini": reservoir_data["vol_ini"],
                "vfin": reservoir_data["vol_fin"],
                "vmin": reservoir_data["vol_min"],
                "vmax": reservoir_data["vol_max"],
                "capacity": reservoir_data["vol_max"],
            }
            system["reservoir_array"].append(reservoir)
