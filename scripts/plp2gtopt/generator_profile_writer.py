# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict, cast
import typing

import pandas as pd

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .bus_parser import BusParser
from .aflce_parser import AflceParser
from .aflce_writer import AflceWriter
from .block_parser import BlockParser


class GeneratorProfile(TypedDict):
    """Represents a generator profile."""

    uid: int
    name: str
    generator: int
    profile: str | float


class GeneratorProfileWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        block_parser: Optional[BlockParser] = None,
        bus_parser: Optional[BusParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        scenarios: Optional[List[Dict[str, Any]]] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser, options)
        self.block_parser = block_parser
        self.bus_parser = bus_parser
        self.aflce_parser = aflce_parser
        self.scenarios = scenarios

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return typing.cast(CentralParser, self.parser)

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert pasada centrals classified for profile mode to JSON.

        Only pasada centrals whose names appear in
        ``options["_pasada_profile_names"]`` are included.  When the set
        is empty (e.g. all pasada are hydro), returns ``[]``.

        For legacy ``--pasada-mode profile`` (all pasada), the set is
        populated by ``classify_pasada_centrals`` to include all.
        """
        profile_names: set[str] | None = None
        if self.options:
            profile_names = self.options.get("_pasada_profile_names")

        # When _pasada_profile_names is explicitly empty (set()), no profiles.
        # When absent (None), include all pasada (backward compat / unit tests).
        if profile_names is not None and not profile_names:
            return []

        if items is None:
            items = (
                self.central_parser.centrals_of_type.get("pasada", [])
                if self.central_parser
                else []
            )
        if not items:
            return []

        # Write own parquet files for profile centrals (not using Flow@discharge)
        if profile_names is not None:
            self._write_profile_parquet(profile_names)

        json_profiles: List[GeneratorProfile] = []
        for central in items:
            central_name = central["name"]
            # When profile_names is set, only include listed centrals
            if profile_names is not None and central_name not in profile_names:
                continue

            # Skip centrals without a bus or with bus 0
            bus_number = central.get("bus", -1)
            if bus_number <= 0:
                continue

            if self.bus_parser:
                bus = self.bus_parser.get_bus_by_number(bus_number)
                if bus is None or bus.get("number", -1) <= 0:
                    continue

            central_number = central["number"]

            aflce = (
                self.aflce_parser.get_item_by_name(central_name)
                if self.aflce_parser
                else None
            )
            if aflce is None:
                afluent = central.get("afluent", 0.0)
            elif profile_names is not None:
                # Auto/explicit profile mode: own GeneratorProfile parquet
                afluent = "profile"
            else:
                # Legacy/backward compat: reference Flow@discharge
                afluent = "Flow@discharge"

            if isinstance(afluent, float) and afluent <= 0.0:
                continue

            profile: GeneratorProfile = {
                "uid": central_number,
                "name": central_name,
                "generator": central_name,
                "profile": afluent,
            }
            json_profiles.append(profile)

        return cast(List[Dict[str, Any]], json_profiles)

    def _write_profile_parquet(self, profile_names: set[str]) -> None:
        """Write profile Parquet files for solar/wind pasada centrals.

        Writes to ``GeneratorProfile/profile.parquet`` containing the
        affluent data (capacity factor) for each profile central.
        """
        if not self.aflce_parser or not profile_names:
            return

        output_dir = (
            self.options["output_dir"] / "GeneratorProfile"
            if self.options and "output_dir" in self.options
            else Path("GeneratorProfile")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        # Filter aflce items to only include profile centrals
        profile_items = [
            f for f in self.aflce_parser.flows if f.get("name") in profile_names
        ]

        if not profile_items:
            return

        aflce_writer = AflceWriter(
            self.aflce_parser,
            self.central_parser,
            self.block_parser,
            self.scenarios,
            self.options,
        )
        # Write as profile.parquet (not discharge.parquet) to match the
        # C++ GeneratorProfile.profile field name.
        df = aflce_writer.to_dataframe(items=profile_items)
        if isinstance(df, pd.DataFrame) and not df.empty:
            aflce_writer.write_dataframe(df, output_dir, "profile")
