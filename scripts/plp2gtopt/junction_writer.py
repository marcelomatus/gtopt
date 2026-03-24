# -*- coding: utf-8 -*-

"""Writer for converting central data to hydro system JSON format.

Converts central plant data into:
- Junctions (nodes in the hydro system)
- Waterways (connections between nodes)
- Flows (water discharges)
- Reservoirs (storage nodes)
- Turbines (energy conversion points)
- ReservoirSeepages (waterway → reservoir seepage links)
- ReservoirEfficiencies (volume-dependent turbine efficiency curves)
"""

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional, cast, TypedDict

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cenfi_parser import CenfiParser
from .cenre_parser import CenreParser
from .extrac_parser import ExtracParser
from .aflce_parser import AflceParser
from .filemb_parser import FilembParser
from .manem_parser import ManemParser
from .ralco_parser import RalcoParser
from .manem_writer import ManemWriter
from .minembh_parser import MinembhParser
from .stage_parser import StageParser

_logger = logging.getLogger(__name__)

# UIDs for synthetic "ocean" drain junctions start above this offset so they
# cannot collide with central UIDs (which are typically in the range 1–999).
_OCEAN_UID_OFFSET = 10000


class Waterway(TypedDict, total=False):
    """Represents a waterway connection between junctions in the hydro system."""

    uid: int
    name: str
    junction_a: str
    junction_b: str
    fmin: float
    fmax: float
    capacity: float


class Junction(TypedDict):
    """Represents a node in the hydro system."""

    uid: int
    name: str
    drain: bool


class _FlowRequired(TypedDict):
    """Required fields for Flow (always present)."""

    uid: int
    name: str
    junction: str
    discharge: float | str


class Flow(_FlowRequired, total=False):
    """Represents a water discharge in the hydro system."""


class _ReservoirRequired(TypedDict):
    """Required fields for Reservoir (always present)."""

    uid: int
    name: str
    junction: str
    eini: float
    efin: float
    emin: float | str
    emax: float | str
    capacity: float
    fmin: float
    fmax: float
    flow_conversion_rate: float
    spillway_cost: float
    spillway_capacity: float
    annual_loss: float


class Reservoir(_ReservoirRequired, total=False):
    """Represents a storage node in the hydro system.

    ``use_state_variable`` is optional: when set to ``False`` the reservoir
    state (energy level) is not linked across blocks, which models small /
    independent hydro reservoirs (PLP ``Hid_Indep='T'``).
    """

    use_state_variable: bool
    soft_emin: list[float]
    soft_emin_cost: list[float]


class Turbine(TypedDict):
    """Represents an energy conversion point in the hydro system."""

    uid: int
    name: str
    generator: str
    waterway: str
    conversion_rate: float


class ProductionFactorSegment(TypedDict):
    """One segment of a piecewise-linear efficiency curve."""

    volume: float
    slope: float
    constant: float


class ReservoirProductionFactor(TypedDict):
    """Volume-dependent turbine efficiency (PLP rendimiento).

    Maps reservoir volume to turbine conversion rate [MW·s/m³] via a
    piecewise-linear concave envelope.
    """

    uid: int
    name: str
    turbine: str
    reservoir: str
    mean_production_factor: float
    segments: List[ProductionFactorSegment]


class ReservoirSeepageSegment(TypedDict):
    """One segment of a piecewise-linear seepage curve."""

    volume: float
    slope: float
    constant: float


class _ReservoirSeepageRequired(TypedDict):
    """Required fields for ReservoirSeepage (always present)."""

    uid: int
    name: str
    waterway: str
    reservoir: str
    slope: float
    constant: float


class ReservoirSeepage(_ReservoirSeepageRequired, total=False):
    """Represents water seepage from a waterway into a reservoir.

    When ``segments`` is non-empty the piecewise-linear concave envelope
    is used: ``seepage(V) = slope_i × V + constant_i`` where the active
    segment is selected based on the current reservoir volume.  The LP
    constraint coefficients (slope on eini/efin columns and the constant RHS)
    are updated dynamically by ReservoirSeepageLP.

    ``slope`` and ``constant`` may be a scalar, an inline array (per-stage
    schedule), or a filename string referencing a Parquet schedule file.
    When ``segments`` is present these fields hold the mean/fallback values
    used before the first volume-dependent update.
    """

    segments: List[ReservoirSeepageSegment]


class ReservoirDischargeLimitSegment(TypedDict):
    """One segment of a piecewise-linear drawdown limit curve."""

    volume: float
    slope: float
    intercept: float


class _ReservoirDischargeLimitRequired(TypedDict):
    """Required fields for ReservoirDischargeLimit."""

    uid: int
    name: str
    waterway: str
    reservoir: str


class ReservoirDischargeLimit(_ReservoirDischargeLimitRequired, total=False):
    """Volume-dependent discharge limit for a reservoir.

    The LP constraint per stage is:
      qeh ≤ slope × V_avg + intercept
    where the active segment is selected by reservoir volume.
    """

    segments: List[ReservoirDischargeLimitSegment]


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
        stage_parser: Optional[StageParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        extrac_parser: Optional[ExtracParser] = None,
        manem_parser: Optional[ManemParser] = None,
        cenre_parser: Optional[CenreParser] = None,
        cenfi_parser: Optional[CenfiParser] = None,
        filemb_parser: Optional[FilembParser] = None,
        ralco_parser: Optional[RalcoParser] = None,
        minembh_parser: Optional[MinembhParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize hydro system writer.

        Args:
            central_parser: Parser for central plant data
            stage_parser: Parser for stage data
            aflce_parser: Parser for inflow data
            extrac_parser: Parser for extraction data
            manem_parser: Parser for reservoir maintenance schedules
            cenre_parser: Parser for reservoir efficiency (plpcenre.dat)
            cenfi_parser: Parser for seepage data (plpcenfi.dat)
            filemb_parser: Parser for primary PLP seepage model
                (plpfilemb.dat); takes precedence over cenfi_parser when
                both are present.
            ralco_parser: Parser for drawdown limit data (plpralco.dat).
            minembh_parser: Parser for soft minimum volume constraints
                (plpminembh.dat).
            options: Configuration options for the writer
        """
        super().__init__(central_parser, options)
        self.stage_parser = stage_parser
        self.aflce_parser = aflce_parser
        self.extrac_parser = extrac_parser
        self.manem_parser = manem_parser
        self.cenre_parser = cenre_parser
        self.cenfi_parser = cenfi_parser
        self.filemb_parser = filemb_parser
        self.ralco_parser = ralco_parser
        self.minembh_parser = minembh_parser
        self._waterway_counter = 0
        self._ocean_junction_counter = 0
        self._junction_names: dict[int, str] = {}
        self._skipped_isolated: list[str] = []
        self._referenced_junctions: set[int] = set()

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return cast(CentralParser, self.parser)

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

        Uses the junction name map (built during to_json_array) to
        resolve numeric IDs to names. Falls back to the numeric ID
        for ocean junctions that aren't in the map yet.
        """
        if target_id == 0:
            return None

        self._waterway_counter += 1
        ja_name = self._junction_names.get(source_id, str(source_id))
        jb_name = self._junction_names.get(target_id, str(target_id))
        waterway: Waterway = {
            "uid": self._waterway_counter,
            "name": f"{source_name}_{source_id}_{target_id}",
            "junction_a": ja_name,
            "junction_b": jb_name,
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
        parquet_cols = self._write_parquet_files()

        central_parser = self.central_parser

        # Get default items if none provided
        if items is None and central_parser:
            items = (
                central_parser.centrals_of_type.get("embalse", [])
                + central_parser.centrals_of_type.get("serie", [])
            ) or []
            # In full-hydro mode, pasada centrals get junctions+waterways.
            # In flow-turbine mode, they're handled separately (no junctions).
            pasada_mode = self.options.get("pasada_mode", "flow-turbine")
            if pasada_mode == "hydro":
                items = items + [
                    c
                    for c in (central_parser.centrals_of_type.get("pasada", []) or [])
                    if c.get("bus", 0) > 0
                ]

        if not items:
            return []

        # Build number→name map for junction name references.
        # This lets _create_waterway resolve numeric IDs to names.
        self._junction_names = {}
        for c in items:
            self._junction_names[c["number"]] = c["name"]

        system: HydroSystemOutput = {
            "junction_array": [],
            "waterway_array": [],
            "flow_array": [],
            "reservoir_array": [],
            "turbine_array": [],
        }

        # Track isolated centrals that were skipped
        self._skipped_isolated = []

        # Build set of junction numbers referenced as downstream targets by
        # other centrals.  A central with ser_hid=0/ser_ver=0 that IS referenced
        # by others acts as a drain/sink junction and must NOT be skipped.
        self._referenced_junctions = set()
        for c in items:
            hid = c.get("ser_hid", 0)
            ver = c.get("ser_ver", 0)
            if hid > 0:
                self._referenced_junctions.add(hid)
            if ver > 0:
                self._referenced_junctions.add(ver)

        # Process central plants
        for central in items:
            self._process_central(central, system, central_parser)

        # Process reservoirs
        if central_parser:
            self._process_reservoirs(system, central_parser, parquet_cols)

        # Process extraction plants
        if self.extrac_parser and central_parser:
            self._process_extractions(system, central_parser)

        # Process seepage data (plpfilemb.dat takes precedence over plpcenfi.dat)
        if self.filemb_parser and central_parser:
            self._process_seepages_filemb(system, central_parser)
        elif self.cenfi_parser and central_parser:
            self._process_seepages(system, central_parser)

        # Process drawdown limit data (plpralco.dat)
        if self.ralco_parser and central_parser:
            self._process_reservoir_discharge_limits(system, central_parser)

        # Process reservoir efficiency data (plpcenre.dat)
        if self.cenre_parser and central_parser:
            self._process_reservoir_efficiencies(system)

        return [cast(Dict[str, Any], system)]

    def _process_central(
        self,
        central: Dict[str, Any],
        system: HydroSystemOutput,
        _central_parser: CentralParser,
    ) -> None:
        """Process a single central into hydro system elements.

        For ``embalse`` centrals whose PLP ``ser_hid`` field is 0 (generation
        waterway has no modelled downstream junction — the water discharges
        directly to the sea / river mouth), a synthetic drain junction is
        created regardless of ``bus``:

            <central_name>_ocean   uid = _OCEAN_UID_OFFSET + N   drain = True

        This ensures the hydro topology (junction, waterway, reservoir, flow)
        is always complete for every embalse.  An embalse with ``bus <= 0``
        operates as a hydro dam only — no turbine or generator is created.

        When ``ser_ver`` is 0 (no spillway downstream junction) the central's
        own junction is flagged ``drain = True`` so excess water can leave the
        system without an explicit spillway waterway.
        """
        central_id = central["number"]
        central_name = central["name"]
        central_type = central.get("type", "serie")

        # Skip truly isolated serie/pasada centrals: bus<=0, no outgoing
        # waterways (ser_hid=0 AND ser_ver=0), AND not referenced as a
        # downstream target by any other central.  Centrals that ARE
        # referenced act as drain/sink junctions receiving water from
        # upstream and must be kept.
        if central_type in ("serie", "pasada"):
            if (
                central.get("bus", 0) <= 0
                and central.get("ser_hid", 0) == 0
                and central.get("ser_ver", 0) == 0
                and central_id not in self._referenced_junctions
            ):
                self._skipped_isolated.append(central_name)
                _logger.debug(
                    "Skipping isolated %s central '%s' (bus<=0, no waterways,"
                    " not referenced by others)",
                    central_type,
                    central_name,
                )
                return

        # Create waterways from the PLP ser_hid / ser_ver connections.
        gen_waterway = self._create_waterway(
            central_name + "_gen",
            central_id,
            central["ser_hid"],
        )
        ver_waterway = self._create_waterway(
            central_name + "_ver",
            central_id,
            central["ser_ver"],
            central.get("vert_min", 0.0),
            central.get("vert_max", 0.0),
        )

        # For embalse/serie/pasada centrals with ser_hid=0, complete the
        # missing generation waterway outlet by adding a synthetic
        # "{name}_ocean" drain junction.  This covers plants that discharge
        # directly to the sea.  The ocean junction is created regardless of
        # bus so the hydro topology is always complete.
        # Note: the spillway (ser_ver=0) is handled differently — by enabling
        # drain=True on the central junction itself (see drain logic below).
        if central_type in ("embalse", "serie", "pasada") and gen_waterway is None:
            self._ocean_junction_counter += 1
            ocean_uid = _OCEAN_UID_OFFSET + self._ocean_junction_counter
            ocean_name = f"{central_name}_ocean"
            ocean_junction: Junction = {
                "uid": ocean_uid,
                "name": ocean_name,
                "drain": True,
            }
            system["junction_array"].append(ocean_junction)
            self._junction_names[ocean_uid] = ocean_name
            _logger.debug(
                "Created ocean drain junction '%s' (uid=%d) for central '%s'.",
                ocean_name,
                ocean_uid,
                central_name,
            )
            gen_waterway = self._create_waterway(
                central_name + "_gen",
                central_id,
                ocean_uid,
            )

        # Add waterways if they exist
        if gen_waterway:
            system["waterway_array"].append(gen_waterway)
            if central["bus"] > 0:  # Only create turbine if connected to bus
                turbine: Turbine = {
                    "uid": central_id,
                    "name": central_name,
                    "generator": central_name,
                    "waterway": gen_waterway["name"],
                    "conversion_rate": central["efficiency"],
                }
                system["turbine_array"].append(turbine)

        if ver_waterway:
            system["waterway_array"].append(ver_waterway)

        # Drain logic:
        #   embalse: drain=True when ver_waterway is None (ser_ver==0), so
        #            excess water can leave the reservoir without an explicit
        #            spillway waterway (spillage flows directly to sea).
        #   others:  drain=True when any outlet waterway is absent.
        if central_type == "embalse":
            drain = ver_waterway is None
        else:
            drain = not (gen_waterway and ver_waterway)
        junction: Junction = {
            "uid": central_id,
            "name": central_name,
            "drain": drain,
        }
        system["junction_array"].append(junction)

        # Add flow if exists
        afluent = self._get_central_flow(central_name, central)
        if isinstance(afluent, float) and afluent == 0.0:
            return

        flow: Flow = {
            "uid": central_id,
            "name": central_name,
            "junction": central_name,
            "discharge": afluent,
        }
        system["flow_array"].append(flow)

    def _get_central_flow(
        self, central_name: str, central: Dict[str, Any]
    ) -> float | str:
        """Get flow value for central, checking aflce parser if available.

        When the aflce parser has data for this central, returns the string
        ``"discharge"`` which tells the C++ ``FlowLP`` (with
        ``ClassName = {"Flow", "flw"}``) to read from
        ``{input_directory}/Flow/discharge.parquet``.
        """
        if self.aflce_parser:
            aflce = self.aflce_parser.get_item_by_name(central_name)
            if aflce is not None:
                return "discharge"
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
                _logger.warning(
                    "Upstream central '%s' not found in central parser.",
                    upstream_name,
                )
                continue

            downstream_name = extraction["downstream"]
            downstream_central = central_parser.get_central_by_name(downstream_name)
            if not downstream_central:
                _logger.warning(
                    "Downstream central '%s' not found for extraction '%s'.",
                    downstream_name,
                    upstream_name,
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
        parquet_cols,
    ) -> None:
        """Process reservoir centrals into reservoir elements."""
        reservoirs = central_parser.centrals_of_type.get("embalse", [])
        for central in reservoirs:
            central_name = central["name"]
            central_number = central["number"]
            pcol_name = self.pcol_name(central_name, central_number)

            emin = "emin" if pcol_name in parquet_cols["emin"] else central["emin"]
            emax = "emax" if pcol_name in parquet_cols["emax"] else central["emax"]

            reservoir: Reservoir = {
                "uid": central["number"],
                "name": central["name"],
                "junction": central["name"],
                "eini": central["vol_ini"],
                "efin": central["vol_fin"],
                "emin": emin,
                "emax": emax,
                "capacity": central["emax"],
                "fmin": -10000.0,
                "fmax": +10000.0,
                "spillway_cost": 1.0,
                "spillway_capacity": 6000.0,
                "annual_loss": 0.0,
                "flow_conversion_rate": 3.6 / 1000.0,
            }

            # Small / independent reservoirs (PLP Hid_Indep='T') do not
            # carry state across blocks — disable the state variable.
            if central.get("hid_indep", False):
                reservoir["use_state_variable"] = False

            # Soft minimum volume (plpminembh.dat "holgura" / slack)
            self._apply_soft_emin(reservoir, central_name)

            system["reservoir_array"].append(reservoir)

    def _apply_soft_emin(self, reservoir: Reservoir, central_name: str) -> None:
        """Add soft_emin and soft_emin_cost from plpminembh.dat if available.

        Builds per-stage arrays from the sparse minembh data.  Stages without
        data get 0 (no constraint).  The cost defaults to the CLI option
        ``--soft-emin-cost`` (default 0.1) when the file cost is zero.
        """
        if not self.minembh_parser or not self.stage_parser:
            return

        entry = self.minembh_parser.get_minembh_by_name(central_name)
        if entry is None:
            return

        num_stages = self.stage_parser.num_stages
        default_cost = self.options.get("soft_emin_cost", 0.1) if self.options else 0.1
        if default_cost <= 0:
            return

        # Build per-stage arrays (0-indexed); minembh stages are 1-based.
        soft_emin_arr = [0.0] * num_stages
        soft_cost_arr = [0.0] * num_stages

        stages = entry["stage"]  # numpy int32 array, 1-based
        vmins = entry["vmin"]  # numpy float64 array [dam³]
        costs = entry["cost"]  # numpy float64 array [$/dam³]

        for i, stage_num in enumerate(stages):
            idx = int(stage_num) - 1  # convert to 0-based
            if 0 <= idx < num_stages and float(vmins[i]) > 0:
                soft_emin_arr[idx] = float(vmins[i])
                file_cost = float(costs[i])
                soft_cost_arr[idx] = file_cost if file_cost > 0 else default_cost

        # Only add if there's at least one non-zero soft_emin
        if any(v > 0 for v in soft_emin_arr):
            reservoir["soft_emin"] = soft_emin_arr
            reservoir["soft_emin_cost"] = soft_cost_arr

    @staticmethod
    def _find_reservoir(
        system: HydroSystemOutput, name: str
    ) -> Optional[Dict[str, Any]]:
        """Find a reservoir dict by name in the system."""
        for r in system["reservoir_array"]:
            if r["name"] == name:
                return r
        return None

    def _process_seepages(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process seepage data and embed inside reservoir definitions.

        Each entry in plpcenfi.dat links a central (waterway source) to a
        receiving reservoir with a slope/constant seepage model.
        """
        if not self.cenfi_parser:
            return

        # Build name→waterway name lookup from the already-created turbines
        turbine_waterway: Dict[str, str] = {
            t["name"]: t["waterway"] for t in system["turbine_array"]
        }

        for entry in self.cenfi_parser.seepages:
            central_name = entry["name"]
            reservoir_name = entry["reservoir"]

            # Resolve waterway uid
            ww_uid = turbine_waterway.get(central_name)
            if ww_uid is None:
                central = central_parser.get_central_by_name(central_name)
                if central is None:
                    _logger.warning(
                        "ReservoirSeepage central '%s' not found; skipping.",
                        central_name,
                    )
                    continue
                ww_uid = central["name"]

            # Find the target reservoir
            rsv = self._find_reservoir(system, reservoir_name)
            if rsv is None:
                central = central_parser.get_central_by_name(reservoir_name)
                if central is not None:
                    rsv = self._find_reservoir(system, central["name"])
                if rsv is None:
                    _logger.warning(
                        "ReservoirSeepage reservoir '%s' not found; skipping.",
                        reservoir_name,
                    )
                    continue

            seep_idx = len(rsv.get("seepage", []))
            seepage: Dict[str, Any] = {
                "uid": seep_idx + 1,
                "name": f"{rsv['name']}_seepage_{seep_idx + 1}",
                "waterway": ww_uid,
                "reservoir": rsv["name"],
                "slope": entry["slope"],
                "constant": entry["constant"],
            }

            # Include piecewise segments when present
            segments = entry.get("segments", [])
            if segments:
                seepage["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "constant": seg["constant"],
                    }
                    for seg in segments
                ]

            rsv.setdefault("seepage", []).append(seepage)

    def _process_seepages_filemb(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process seepages from plpfilemb.dat (primary PLP seepage model).

        Each entry in plpfilemb.dat provides:
        - ``embalse``: source reservoir name (filtered reservoir)
        - ``central``: receiving central name (destination of filtrated water)
        - ``mean_seepage``: mean flow [m³/s], used as initial slope fallback
        - ``segments``: piecewise-linear seepage curve (volume→slope/constant)

        A new seepage waterway is created from the source reservoir's
        junction to the receiving central's junction.  The source reservoir
        uid drives the volume-dependent LP update, exactly as described in
        the PLP Fortran subroutine ``LeeFilEmb`` / ``GenPDFilAi``.

        This method is called instead of ``_process_seepages`` when
        ``filemb_parser`` is available.
        """
        if not self.filemb_parser:
            return

        # Build a name→uid lookup from already-created reservoirs and junctions
        reservoir_name_map: Dict[str, str] = {
            r["name"]: r["name"] for r in system["reservoir_array"]
        }

        # Build central name→number lookup for receiving centrals
        central_number: Dict[str, int] = {}
        for central_entry in central_parser.centrals:
            central_number[str(central_entry["name"])] = int(central_entry["number"])

        for entry in self.filemb_parser.seepages:
            embalse_name = entry["embalse"]
            receiving_name = entry["central"]
            segments = entry.get("segments", [])

            # Resolve source reservoir name and number
            rsv_name = reservoir_name_map.get(embalse_name)
            embalse_central = central_parser.get_central_by_name(embalse_name)
            if rsv_name is None or embalse_central is None:
                if embalse_central is None:
                    _logger.warning(
                        "Filemb embalse '%s' not found; skipping.", embalse_name
                    )
                    continue
                rsv_name = embalse_central["name"]
            embalse_number = int(embalse_central["number"])

            # Resolve receiving central junction id (NomCen → gtopt junction)
            rcv_id = central_number.get(receiving_name)
            if rcv_id is None:
                _logger.warning(
                    "Filemb receiving central '%s' not found; skipping.",
                    receiving_name,
                )
                continue

            # Create a new seepage waterway from source reservoir's junction
            # to receiving central's junction (matching PLP GenPDFilAi behaviour)
            filt_waterway = self._create_waterway(
                f"filt_{embalse_name}",
                embalse_number,
                rcv_id,
            )
            if filt_waterway is None:
                _logger.warning(
                    "Filemb seepage waterway for '%s'→'%s' could not be created"
                    " (source == target?); skipping.",
                    embalse_name,
                    receiving_name,
                )
                continue
            system["waterway_array"].append(filt_waterway)

            # Use first segment's values as the default slope/constant
            default_slope = segments[0]["slope"] if segments else 0.0
            default_constant = segments[0]["constant"] if segments else 0.0

            # Find the source reservoir and embed seepage
            rsv = self._find_reservoir(system, rsv_name)
            if rsv is None:
                _logger.warning("Filemb reservoir '%s' not found; skipping.", rsv_name)
                continue

            seep_idx = len(rsv.get("seepage", []))
            seepage: Dict[str, Any] = {
                "uid": seep_idx + 1,
                "name": f"{rsv['name']}_seepage_{seep_idx + 1}",
                "waterway": filt_waterway["name"],
                "reservoir": rsv["name"],
                "slope": default_slope,
                "constant": default_constant,
            }

            if segments:
                seepage["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "constant": seg["constant"],
                    }
                    for seg in segments
                ]

            rsv.setdefault("seepage", []).append(seepage)

    def _process_reservoir_discharge_limits(
        self,
        system: HydroSystemOutput,
        central_parser: CentralParser,
    ) -> None:
        """Process drawdown limit data from plpralco.dat.

        Creates ReservoirDischargeLimit elements that constrain the stage-average
        discharge from a reservoir as a piecewise-linear function of volume.
        The waterway reference is resolved from the reservoir's turbine.
        """
        if not self.ralco_parser:
            return

        # Build reservoir name → turbine waterway name map
        turbine_waterway: Dict[str, str] = {}
        for turbine in system["turbine_array"]:
            turbine_waterway[turbine["name"]] = turbine["waterway"]

        for entry in self.ralco_parser.reservoir_discharge_limits:
            rsv_name = entry["reservoir"]
            segments = entry.get("segments", [])

            rsv = self._find_reservoir(system, rsv_name)
            if rsv is None:
                _logger.warning(
                    "Ralco reservoir '%s' not found in reservoir_array; skipping.",
                    rsv_name,
                )
                continue

            # Resolve waterway from the turbine associated with this reservoir
            ww_name = turbine_waterway.get(rsv_name)
            if not ww_name:
                _logger.warning(
                    "Ralco reservoir '%s' has no turbine waterway; skipping.",
                    rsv_name,
                )
                continue

            ddl_idx = len(rsv.get("discharge_limit", []))
            ddl: Dict[str, Any] = {
                "uid": ddl_idx + 1,
                "name": f"{rsv['name']}_dlim_{ddl_idx + 1}",
                "waterway": ww_name,
                "reservoir": rsv["name"],
            }

            if segments:
                ddl["segments"] = [
                    {
                        "volume": seg["volume"],
                        "slope": seg["slope"],
                        "intercept": seg["intercept"],
                    }
                    for seg in segments
                ]

            rsv.setdefault("discharge_limit", []).append(ddl)

    def _process_reservoir_efficiencies(
        self,
        system: HydroSystemOutput,
    ) -> None:
        """Process reservoir efficiency data and embed inside reservoirs.

        Each entry in plpcenre.dat links a central (turbine) to a reservoir
        and provides a piecewise-linear efficiency curve (rendimiento) that
        maps reservoir volume to turbine conversion rate [MW·s/m³].
        """
        if not self.cenre_parser:
            return

        # Build turbine name→uid lookup from already-created turbines
        turbine_name_map: Dict[str, str] = {
            t["name"]: t["name"] for t in system["turbine_array"]
        }

        for entry in self.cenre_parser.efficiencies:
            central_name = entry["name"]
            reservoir_name = entry["reservoir"]

            turb_uid = turbine_name_map.get(central_name)
            if turb_uid is None:
                central_data = self.central_parser.get_central_by_name(central_name)
                if central_data is not None and central_data.get("bus", 0) <= 0:
                    _logger.debug(
                        "Efficiency central '%s': reservoir-only central"
                        " (bus<=0), no turbine; skipping.",
                        central_name,
                    )
                else:
                    _logger.warning(
                        "Efficiency central '%s': no matching turbine found;"
                        " skipping efficiency entry.",
                        central_name,
                    )
                continue

            rsv = self._find_reservoir(system, reservoir_name)
            if rsv is None:
                _logger.warning(
                    "Efficiency reservoir '%s' not found; skipping.",
                    reservoir_name,
                )
                continue

            segments: List[ProductionFactorSegment] = [
                {
                    "volume": seg["volume"],
                    "slope": seg["slope"],
                    "constant": seg["constant"],
                }
                for seg in entry["segments"]
            ]

            pfac_idx = len(rsv.get("production_factor", []))
            pfac: Dict[str, Any] = {
                "uid": pfac_idx + 1,
                "name": f"{rsv['name']}_pfac_{pfac_idx + 1}",
                "turbine": turb_uid,
                "reservoir": rsv["name"],
                "mean_production_factor": entry["mean_production_factor"],
                "segments": segments,
            }
            rsv.setdefault("production_factor", []).append(pfac)

    def _write_parquet_files(self) -> Dict[str, List[str]]:
        """Write demand data to Parquet file format."""
        #
        # write the manem data
        #
        output_dir = (
            self.options["output_dir"] / "Reservoir"
            if "output_dir" in self.options
            else Path("Reservoir")
        )
        output_dir.mkdir(parents=True, exist_ok=True)
        manem_writer = ManemWriter(
            self.manem_parser, self.central_parser, self.stage_parser, self.options
        )
        manem_cols = manem_writer.to_parquet(output_dir)

        #
        # collect the cols
        #

        return manem_cols
