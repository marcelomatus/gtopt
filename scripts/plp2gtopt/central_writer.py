# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

import csv
from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict
import typing

from .base_writer import BaseWriter
from .block_parser import BlockParser
from .bus_parser import BusParser
from .central_parser import CentralParser
from .cost_parser import CostParser
from .cost_writer import CostWriter
from .mance_parser import ManceParser
from .mance_writer import ManceWriter
from .stage_parser import StageParser
from .tech_detect import detect_technology, suspect_technology


# ----------------------------------------------------------------------
# CEN cogen reference cache (best-effort load of the bundled CSV).
# See ``share/gtopt/cogen/cen_chile_cogen.csv``.
# ----------------------------------------------------------------------
_CEN_COGEN_NAMES: Optional[set[str]] = None
_CEN_COGEN_PREFIXES: tuple[str, ...] = ()


def _load_cen_cogen_reference() -> tuple[set[str], tuple[str, ...]]:
    """Load (exact-names, prefix-tuple) from
    ``share/gtopt/cogen/cen_chile_cogen.csv``.  Cached after first call.
    Returns ``(set(), ())`` if the file is missing.
    """
    global _CEN_COGEN_NAMES, _CEN_COGEN_PREFIXES
    if _CEN_COGEN_NAMES is not None:
        return _CEN_COGEN_NAMES, _CEN_COGEN_PREFIXES
    names: set[str] = set()
    prefixes: list[str] = []
    candidates = [
        Path("/home/marce/git/gtopt/share/gtopt/cogen/cen_chile_cogen.csv"),
        Path(__file__).resolve().parents[2]
        / "share"
        / "gtopt"
        / "cogen"
        / "cen_chile_cogen.csv",
    ]
    for path in candidates:
        if not path.is_file():
            continue
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    name = str(row.get("name_upper", "")).strip().upper()
                    if not name:
                        continue
                    source = str(row.get("source", "")).lower()
                    if source.startswith("pattern_prefix:"):
                        prefixes.append(name)
                    else:
                        names.add(name)
            break
        except OSError:
            continue
    _CEN_COGEN_NAMES = names
    _CEN_COGEN_PREFIXES = tuple(prefixes)
    return _CEN_COGEN_NAMES, _CEN_COGEN_PREFIXES


def _is_cen_cogen(gen_name: str) -> bool:
    """True iff ``gen_name`` matches the CEN cogen reference list."""
    name = str(gen_name).strip().upper()
    if not name:
        return False
    names, prefixes = _load_cen_cogen_reference()
    if name in names:
        return True
    return any(name.startswith(p) for p in prefixes)


class Generator(TypedDict):
    """Represents a generator in the system."""

    uid: int
    name: str
    bus: int
    gcost: float | str
    capacity: float
    pmax: float | str
    pmin: float | str
    type: str


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        block_parser: Optional[BlockParser] = None,
        cost_parser: Optional[CostParser] = None,
        bus_parser: Optional[BusParser] = None,
        mance_parser: Optional[ManceParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser, options)
        self.stage_parser = stage_parser
        self.block_parser = block_parser
        self.cost_parser = cost_parser
        self.bus_parser = bus_parser
        self.mance_parser = mance_parser

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return typing.cast(CentralParser, self.parser)

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = self.items
        if not items:
            return []

        parquet_cols = self._write_parquet_files()

        # Load centipo.csv overrides from the input directory (PLP's own
        # technology classification file) and merge with user overrides.
        # User overrides take precedence over centipo.csv.
        from .tech_detect import load_centipo_csv  # noqa: PLC0415

        input_dir = self.options.get("input_dir", "") if self.options else ""
        centipo_overrides = load_centipo_csv(input_dir) if input_dir else {}

        # Build set of bateria names so we can recognise their phantom
        # ``<bateria>_LOAD`` twins emitted by PLP as ``termica`` entries
        # with ``pmax == 0`` and a large negative ``pmin`` (the battery
        # charge side).  The Battery LP object already models both
        # discharge and charge via ``pmax_discharge`` / ``pmax_charge``,
        # so these phantoms must not be emitted as separate generators —
        # otherwise the catch-all later promotes them to
        # ``renewable:hydro`` and leaves their negative pmin pinning the
        # LP column to a fake "consumer" generator (pmax=0 so it's
        # solver-presolved away, but the JSON shows up in indicators and
        # breaks gen-min-stable parity with PLP).
        bateria_names = {
            str(c["name"]) for c in items if str(c.get("type", "")).lower() == "bateria"
        }

        json_centrals: List[Dict[str, Any]] = []
        for central in items:
            central_name = central["name"]
            central_number = central["number"]

            # skip centrals that are "falla" or "bateria" type –
            # falla is a modelling artefact; bateria is handled by BessWriter
            if central["type"] in ("falla", "bateria"):
                continue

            # skip ``<bateria>_LOAD`` phantoms — the battery charge side
            # is owned by the Battery LP object.
            if (
                central_name.endswith("_LOAD")
                and central_name[: -len("_LOAD")] in bateria_names
            ):
                continue

            # Skip centrals without a bus or with bus 0
            bus_number = central.get("bus", -1)
            if bus_number <= 0:
                continue

            if self.bus_parser:
                bus = self.bus_parser.get_bus_by_number(bus_number)
                if bus is None or bus["number"] != bus_number:
                    print(
                        f"Skipping central {central_name} with invalid bus {bus_number}."
                    )
                    continue

            # lookup for cols in parquet files
            pcol_name = self.pcol_name(central_name, central_number)
            gcost = "gcost" if pcol_name in parquet_cols["gcost"] else central["gcost"]
            pmin = "pmin" if pcol_name in parquet_cols["pmin"] else central["pmin"]
            pmax = "pmax" if pcol_name in parquet_cols["pmax"] else central["pmax"]

            plp_type = central.get("type", "unknown")
            user_overrides = (
                self.options.get("tech_overrides") if self.options else None
            )
            # Merge: user overrides > centipo.csv overrides
            effective_overrides = {**centipo_overrides}
            if user_overrides:
                effective_overrides.update(user_overrides)
            auto_detect_tech = (
                self.options.get("auto_detect_tech", False) if self.options else False
            )
            gen_type = detect_technology(
                plp_type,
                central_name,
                overrides=effective_overrides,
                auto_detect=auto_detect_tech,
            )

            # Last-resort fallback (#524): when auto-detect is on AND
            # detect_technology still returned bare ``"thermal"`` AND
            # the central has no fuel-cost signal (``gcost`` is the
            # zero scalar), the central is almost certainly a small
            # distributed renewable that PLP catch-all'd into the
            # thermal bucket and that name-pattern detection couldn't
            # refine (e.g. local plant names without the canonical
            # ``_FV`` / ``_EO`` suffix).  Default to
            # ``renewable:hydro`` — the most common case for CEN
            # PLP-only thermals with HR=0 is small ROR / mini-hydros.
            # Downstream consumers can refine via ``--tech-overrides``.
            #
            # Gated on ``auto_detect_tech=True`` so the legacy "suspected"
            # description workflow (auto-detect off) is preserved.
            # Only reclassify a GENUINELY zero-cost thermal (scalar 0 or
            # absent gcost).  A STRING ``gcost`` is a time-varying fuel-cost
            # SCHEDULE — the unit DOES burn fuel, so it must stay thermal.
            # The old check treated a string as "no cost" (because it is not
            # an int/float), which reclassified real coal / gas / diesel
            # plants (Guacolda, Angamos, Kelar, San Isidro, …) as
            # ``renewable:hydro`` — hiding coal from the tech breakdown and
            # mis-attributing their fuel cost to "hydro".
            gcost_is_zero = gcost is None or (
                isinstance(gcost, (int, float)) and float(gcost) == 0.0
            )
            if auto_detect_tech and gen_type == "thermal" and gcost_is_zero:
                gen_type = "renewable:hydro"

            generator: Dict[str, Any] = {
                "uid": central_number,
                "name": central_name,
                "bus": bus_number,
                "gcost": gcost,
                "capacity": central["pmax"],
                "pmax": pmax,
                "pmin": pmin,
                "type": gen_type,
            }
            # Self-describing cogen flag — same lookup used by
            # plexos2gtopt + gtopt_marginal_units.  Bundled CSV at
            # ``share/gtopt/cogen/cen_chile_cogen.csv`` carries the full
            # CEN cogen list (Cogeneración SIP tags + pulp-mill / sulfur
            # / refinery prefixes from CEN-SIP + Informe-CEN sources).
            # The emissions-overlay fallback later sets the same flag
            # when ``--emissions-file`` carries ``thermal:cogen``
            # generator_overrides; this layer catches cogens BEFORE the
            # overlay runs so the flag is present even without
            # ``--emissions``.
            if _is_cen_cogen(central_name):
                # First-class C++ field; see include/gtopt/generator.hpp +
                # generator_enums.hpp.  L1 ("dispatched") means LP-free
                # dispatch — downstream tools still skip in merit walk-up.
                generator["cogen_mode"] = "dispatched"
            if not auto_detect_tech:
                suspected = suspect_technology(plp_type, central_name)
                if suspected:
                    generator["description"] = f"suspected {suspected}"
            json_centrals.append(generator)

        return typing.cast(List[Dict[str, Any]], json_centrals)

    def _write_parquet_files(self) -> Dict[str, List[str]]:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        cost_writer = CostWriter(
            self.cost_parser,
            self.central_parser,
            self.stage_parser,
            self.options,
        )
        cost_cols = cost_writer.to_parquet(output_dir)

        mance_writer = ManceWriter(
            self.mance_parser, self.central_parser, self.block_parser, self.options
        )
        mance_cols = mance_writer.to_parquet(output_dir)

        #
        # collect the cols
        #
        mcols = {}
        for d in cost_cols, mance_cols:
            for key, value in d.items():
                mcols[key] = value

        return mcols
