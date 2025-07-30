# -*- coding: utf-8 -*-

"""Writer for converting line data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .line_parser import LineParser
from .manli_parser import ManliParser
from .manli_writer import ManliWriter
from .block_parser import BlockParser


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
        super().__init__(line_parser)
        self.manli_parser = manli_parser
        self.line_parser = line_parser
        self.block_parser = block_parser
        self.options = options if options is not None else {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert line data to JSON array format."""
        if items is None:
            items = self.items

        if not items:
            return []

        json_lines = []
        for line in items:
            bus_a = line.get("bus_a", -1)
            bus_b = line.get("bus_b", -1)
            if bus_a <= 0 or bus_b <= 0:
                continue

            # lookup manli by name if cost_parser is available, and use it
            manli = (
                self.manli_parser.get_manli_by_name(line["name"])
                if self.manli_parser
                else None
            )
            tmax_ba, tmax_ab, active = (
                (
                    line.get("p_max_ba", 0.0),
                    line.get("p_max_ab", 0.0),
                    line.get("operational", 1),
                )
                if not manli
                else ("tmax_ba", "tmax_ab", "active")
            )

            json_lines.append(
                {
                    "uid": line["number"],
                    "name": line["name"],
                    "active": active,
                    "bus_a": line["bus_a"],
                    "bus_b": line["bus_b"],
                    "resistance": line["r"],
                    "reactance": line["x"],
                    "tmax_ab": tmax_ab,
                    "tmax_ba": tmax_ba,
                    "voltage": line["voltage"],
                    **({"is_hvdc": 1} if "hdvc" in line else {}),
                }
            )

        self._write_parquet_files()

        return json_lines

    def _write_parquet_files(self) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Line"
            if "output_dir" in self.options
            else Path("Line")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        manli_writer = ManliWriter(
            self.manli_parser, self.line_parser, self.block_parser, self.options
        )
        manli_writer.to_parquet(output_dir)
