"""PLP file parser classes.

Handles parsing of all PLP input files into structured data.
"""

from pathlib import Path
from typing import Any, Dict

from .block_parser import BlockParser
from .bus_parser import BusParser
from .central_parser import CentralParser
from .cost_parser import CostParser
from .demand_parser import DemandParser
from .line_parser import LineParser
from .mance_parser import ManceParser
from .manli_parser import ManliParser
from .stage_parser import StageParser


class PLPParser:
    """Handles parsing of all PLP input files."""

    def __init__(self, options: Dict[str, Any]):
        """Initialize PLPParser with input directory."""
        self.input_path: Path = Path(options.get("input_dir", ""))
        self.parsed_data: dict[str, Any] = {}

        self._validate_input_dir()

    def _validate_input_dir(self):
        if not self.input_path.is_dir():
            raise FileNotFoundError(f"Input directory not found: {self.input_path}")

    def parse_all(self):
        """Parse all PLP input files."""
        parsers = [
            ("block_array", BlockParser, "plpblo.dat"),
            ("stage_array", StageParser, "plpeta.dat"),
            ("bus_array", BusParser, "plpbar.dat"),
            ("line_array", LineParser, "plpcnfli.dat"),
            ("central_array", CentralParser, "plpcnfce.dat"),
            ("demand_array", DemandParser, "plpdem.dat"),
            ("cost_array", CostParser, "plpcosce.dat"),
            ("mance_array", ManceParser, "plpmance.dat"),
            ("manli_array", ManliParser, "plpmanli.dat"),
        ]
        for name, parser_class, filename in parsers:
            filepath = self.input_path / filename
            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")
            parser = parser_class(filepath)
            parser.parse()
            self.parsed_data[name] = parser
