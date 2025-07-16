"""PLP file parser classes.

Handles parsing of all PLP input files into structured data.
"""

from pathlib import Path
from typing import Union

from plp2gtopt.block_parser import BlockParser
from plp2gtopt.stage_parser import StageParser
from plp2gtopt.bus_parser import BusParser
from plp2gtopt.demand_parser import DemandParser
from plp2gtopt.central_parser import CentralParser
from plp2gtopt.line_parser import LineParser
from plp2gtopt.cost_parser import CostParser
from plp2gtopt.mance_parser import ManceParser


class PLPParser:
    """Handles parsing of all PLP input files."""

    def __init__(self, input_dir: Union[str, Path]):
        """Initialize PLPParser with input directory."""
        self.input_path = Path(input_dir)
        self._validate_input_dir()
        self.parsed_data = {}

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
        ]
        for name, parser_class, filename in parsers:
            filepath = self.input_path / filename
            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")
            parser = parser_class(filepath)
            parser.parse()
            self.parsed_data[name] = parser
