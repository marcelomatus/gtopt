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
from .idsim_parser import IdSimParser
from .idape_parser import IdApeParser
from .idap2_parser import IdAp2Parser
from .cenpmax_parser import CenpmaxParser
from .minembh_parser import MinembhParser
from .planos_parser import PlanosParser, find_planos_files
from .plpmat_parser import PlpmatParser
from .vrebemb_parser import VrebembParser
from .ralco_parser import RalcoParser
from .gnl_parser import GnlParser
from .laja_parser import LajaParser
from .maule_parser import MauleParser
from .compressed_open import find_compressed_path, resolve_compressed_path


class PLPParser:
    """Handles parsing of all PLP input files."""

    def __init__(self, options: Dict[str, Any]):
        """Initialize PLPParser with input directory."""
        self.input_path: Path = Path(options.get("input_dir", ""))
        self.parsed_data: dict[str, Any] = {}
        self._options = options

        self._validate_input_dir()

    def _validate_input_dir(self):
        if not self.input_path.is_dir():
            raise FileNotFoundError(f"Input directory not found: {self.input_path}")

    def parse_all(self):
        """Parse all PLP input files."""
        # Store conversion options so downstream parsers can read them
        self.parsed_data["_options"] = self._options

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
            filepath = resolve_compressed_path(self.input_path / filename)
            parser = parser_class(filepath)
            parser.parse(self.parsed_data)
            self.parsed_data[name] = parser

        # Helper: try to find an optional file (plain or compressed)
        def _opt(filename: str) -> Path | None:
            return find_compressed_path(self.input_path / filename)

        # Optional storage files – ESS (plpess.dat) takes priority over battery
        # (plpcenbat.dat). If both exist, only ESS is read.
        ess_path = _opt("plpess.dat")
        cenbat_path = _opt("plpcenbat.dat")

        if ess_path is not None:
            ep = EssParser(ess_path)
            ep.parse(self.parsed_data)
            self.parsed_data["ess_parser"] = ep

            maness_path = _opt("plpmaness.dat")
            if maness_path is not None:
                maness_p = ManessParser(maness_path)
                maness_p.parse(self.parsed_data)
                self.parsed_data["maness_parser"] = maness_p
        elif cenbat_path is not None:
            bp = BatteryParser(cenbat_path)
            bp.parse(self.parsed_data)
            self.parsed_data["battery_parser"] = bp

            manbat_path = _opt("plpmanbat.dat")
            if manbat_path is not None:
                mp = ManbatParser(manbat_path)
                mp.parse(self.parsed_data)
                self.parsed_data["manbat_parser"] = mp

        # Optional parsers — each tries plain and compressed variants
        optional_parsers = [
            ("cenre_parser", CenreParser, "plpcenre.dat"),
            ("cenfi_parser", CenfiParser, "plpcenfi.dat"),
            ("filemb_parser", FilembParser, "plpfilemb.dat"),
            ("ralco_parser", RalcoParser, "plpralco.dat"),
            ("idsim_parser", IdSimParser, "plpidsim.dat"),
            ("idape_parser", IdApeParser, "plpidape.dat"),
            ("idap2_parser", IdAp2Parser, "plpidap2.dat"),
            ("plpmat_parser", PlpmatParser, "plpmat.dat"),
            ("vrebemb_parser", VrebembParser, "plpvrebemb.dat"),
            ("minembh_parser", MinembhParser, "plpminembh.dat"),
            ("cenpmax_parser", CenpmaxParser, "plpcenpmax.dat"),
            ("maule_parser", MauleParser, "plpmaulen.dat"),
            ("laja_parser", LajaParser, "plplajam.dat"),
            ("gnl_parser", GnlParser, "plpcnfgnl.dat"),
        ]
        for name, parser_class, filename in optional_parsers:
            filepath = _opt(filename)
            if filepath is not None:
                p = parser_class(filepath)
                p.parse(self.parsed_data)
                self.parsed_data[name] = p

        # Optional: indhor.csv block-to-hour mapping
        indhor_path = _opt("indhor.csv")
        if indhor_path is not None:
            indhor = IndhorParser(indhor_path)
            indhor.parse()
            self.parsed_data["indhor_parser"] = indhor

        # Optional: plpplaem1/plpplem1 + plpplaem2/plpplem2 — boundary cuts
        # These define the future-cost function at the last planning stage
        # boundary, analogous to the SDDP varphi variable.
        planos_files = find_planos_files(self.input_path)
        if planos_files is not None:
            plaem1_path, plaem2_path = planos_files
            planos = PlanosParser(plaem1_path, plaem2_path)
            planos.parse(self.parsed_data)
            self.parsed_data["planos_parser"] = planos
