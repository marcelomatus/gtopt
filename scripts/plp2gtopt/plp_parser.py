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
from .aflce_parser import AflceParser
from .stage_parser import StageParser
from .extrac_parser import ExtracParser
from .manem_parser import ManemParser
from .bess_parser import BessParser
from .ess_parser import EssParser
from .manbess_parser import ManbessParser
from .maness_parser import ManessParser


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
            ("block_parser", BlockParser, "plpblo.dat"),
            ("stage_parser", StageParser, "plpeta.dat"),
            ("bus_parser", BusParser, "plpbar.dat"),
            ("line_parser", LineParser, "plpcnfli.dat"),
            ("central_parser", CentralParser, "plpcnfce.dat"),
            ("demand_parser", DemandParser, "plpdem.dat"),
            ("cost_parser", CostParser, "plpcosce.dat"),
            ("mance_parser", ManceParser, "plpmance.dat"),
            ("manli_parser", ManliParser, "plpmanli.dat"),
            ("aflce_parser", AflceParser, "plpaflce.dat"),
            ("extrac_parser", ExtracParser, "plpextrac.dat"),
            ("manem_parser", ManemParser, "plpmanem.dat"),
        ]
        for name, parser_class, filename in parsers:
            filepath = self.input_path / filename
            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")
            parser = parser_class(filepath)
            parser.parse(self.parsed_data)
            self.parsed_data[name] = parser

        # Optional BESS/ESS files – mutually exclusive: if plpbess.dat exists
        # parse it (and its maintenance); otherwise try plpess.dat + plpmaness.dat.
        bess_path = self.input_path / "plpbess.dat"
        ess_path = self.input_path / "plpess.dat"

        if bess_path.exists():
            parser = BessParser(bess_path)
            parser.parse(self.parsed_data)
            self.parsed_data["bess_parser"] = parser

            manbess_path = self.input_path / "plpmanbess.dat"
            if manbess_path.exists():
                mp = ManbessParser(manbess_path)
                mp.parse(self.parsed_data)
                self.parsed_data["manbess_parser"] = mp
        elif ess_path.exists():
            parser = EssParser(ess_path)
            parser.parse(self.parsed_data)
            self.parsed_data["ess_parser"] = parser

            maness_path = self.input_path / "plpmaness.dat"
            if maness_path.exists():
                mp = ManessParser(maness_path)
                mp.parse(self.parsed_data)
                self.parsed_data["maness_parser"] = mp
