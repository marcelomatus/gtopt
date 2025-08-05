# -*- coding: utf-8 -*-

"""Writer for converting line data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict, cast
from .base_writer import BaseWriter
from .line_parser import LineParser
from .manli_parser import ManliParser
from .manli_writer import ManliWriter
from .block_parser import BlockParser


class Line(TypedDict, total=False):
    """Represents a line in the system."""

    uid: int
    name: str
    active: int | str
    bus_a: int
    bus_b: int
    resistance: float
    reactance: float
    tmax_ab: float | str
    tmax_ba: float | str
    voltage: float
    is_hvdc: int


class LineWriter(BaseWriter):
    """Converts line parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        line_parser: Optional[LineParser] = None,
        block_parser: Optional[BlockParser] = None,
        manli_parser: Optional[ManliParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a LineParser instance."""
        super().__init__(line_parser, options)
        self.manli_parser = manli_parser
        self.line_parser = line_parser
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert line data to JSON array format."""
        if items is None:
            items = self.items

        if not items:
            return []

        pcols = self._write_parquet_files()

        json_lines: List[Line] = []
        for line in items:
            line_name = line["name"]
            line_number = line["number"]

            bus_number_a = line.get("bus_a", -1)
            bus_number_b = line.get("bus_b", -1)
            if bus_number_a <= 0 or bus_number_b <= 0 or bus_number_a == bus_number_b:
                continue

            # lookup for cols in parquet files
            pcol_name = self.pcol_name(line_name, line_number)
            tmax_ab = "tmax_ab" if pcol_name in pcols["tmax_ab"] else line["tmax_ab"]
            tmax_ba = "tmax_ba" if pcol_name in pcols["tmax_ba"] else line["tmax_ba"]
            active = "active" if pcol_name in pcols["active"] else line["operational"]

            json_line: Line = {
                "uid": line_number,
                "name": line_name,
                "active": active,
                "bus_a": bus_number_a,
                "bus_b": bus_number_b,
                "resistance": line["r"],
                "reactance": line["x"],
                "tmax_ab": tmax_ab,
                "tmax_ba": tmax_ba,
                "voltage": line["voltage"],
                **({"is_hvdc": 1} if "hvdc" in line else {}),
            }
            json_lines.append(json_line)

        return cast(List[Dict[str, Any]], json_lines)

    def _write_parquet_files(self) -> Dict[str, List[str]]:
        """Write line data to Parquet file format."""
        cols: Dict[str, List[str]] = {"tmax_ab": [], "tmax_ba": [], "active": []}

        if not self.manli_parser:
            return cols

        output_dir = (
            self.options["output_dir"] / "Line"
            if "output_dir" in self.options
            else Path("Line")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        manli_writer = ManliWriter(
            self.manli_parser, self.line_parser, self.block_parser, self.options
        )
        cols = manli_writer.to_parquet(output_dir)

        return cols
