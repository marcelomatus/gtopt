"""PLP file parser classes.

Handles parsing of all PLP input files into structured data.
"""

from pathlib import Path
from typing import Any, Dict

from .block_parser import BlockParser
from .bus_parser import BusParser
from .central_parser import CentralParser
from .cenfi_parser import CenfiParser
from .cenre_parser import CenreParser
from .filemb_parser import FilembParser
from .cost_parser import CostParser
from .demand_parser import DemandParser
from .line_parser import LineParser
from .mance_parser import ManceParser
from .manli_parser import ManliParser
from .aflce_parser import AflceParser
from .stage_parser import StageParser
from .extrac_parser import ExtracParser
from .manem_parser import ManemParser
from .battery_parser import BatteryParser
from .manbat_parser import ManbatParser
from .ess_parser import EssParser
from .maness_parser import ManessParser
from .indhor_parser import IndhorParser


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

        # Optional storage files – ESS (plpess.dat) takes priority over battery
        # (plpcenbat.dat). If both exist, only ESS is read.
        ess_path = self.input_path / "plpess.dat"
        cenbat_path = self.input_path / "plpcenbat.dat"

        if ess_path.exists():
            ep = EssParser(ess_path)
            ep.parse(self.parsed_data)
            self.parsed_data["ess_parser"] = ep

            maness_path = self.input_path / "plpmaness.dat"
            if maness_path.exists():
                maness_p = ManessParser(maness_path)
                maness_p.parse(self.parsed_data)
                self.parsed_data["maness_parser"] = maness_p
        elif cenbat_path.exists():
            bp = BatteryParser(cenbat_path)
            bp.parse(self.parsed_data)
            self.parsed_data["battery_parser"] = bp

            manbat_path = self.input_path / "plpmanbat.dat"
            if manbat_path.exists():
                mp = ManbatParser(manbat_path)
                mp.parse(self.parsed_data)
                self.parsed_data["manbat_parser"] = mp

        # Optional hydro efficiency file – plpcenre.dat (Rendimiento de Embalses)
        cenre_path = self.input_path / "plpcenre.dat"
        if cenre_path.exists():
            crp = CenreParser(cenre_path)
            crp.parse(self.parsed_data)
            self.parsed_data["cenre_parser"] = crp

        # Optional filtration file – plpcenfi.dat (Centrales Filtración)
        cenfi_path = self.input_path / "plpcenfi.dat"
        if cenfi_path.exists():
            cfp = CenfiParser(cenfi_path)
            cfp.parse(self.parsed_data)
            self.parsed_data["cenfi_parser"] = cfp

        # Primary PLP filtration model – plpfilemb.dat (Filtraciones de Embalses)
        # Takes precedence over plpcenfi.dat when both are present.
        filemb_path = self.input_path / "plpfilemb.dat"
        if filemb_path.exists():
            fmp = FilembParser(filemb_path)
            fmp.parse(self.parsed_data)
            self.parsed_data["filemb_parser"] = fmp

        # Optional: indhor.csv block-to-hour mapping
        indhor_path = self.input_path / "indhor.csv"
        if indhor_path.exists():
            indhor = IndhorParser(indhor_path)
            indhor.parse()
            self.parsed_data["indhor_parser"] = indhor
