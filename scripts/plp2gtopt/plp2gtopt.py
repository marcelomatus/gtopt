"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

import json
from pathlib import Path
from typing import Dict, Union

from plp2gtopt.block_parser import BlockParser
from plp2gtopt.stage_parser import StageParser
from plp2gtopt.bus_parser import BusParser
from plp2gtopt.demand_parser import DemandParser
from plp2gtopt.central_parser import CentralParser
from plp2gtopt.line_parser import LineParser
from plp2gtopt.cost_parser import CostParser


class PLPParser:
    """Handles parsing of all PLP input files."""
    def __init__(self, input_dir: Union[str, Path]):
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
        ]
        
        for name, parser_class, filename in parsers:
            filepath = self.input_path / filename
            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")
                
            parser = parser_class(filepath)
            parser.parse()
            self.parsed_data[name] = parser


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""
    
    def __init__(self, parser: PLPParser):
        self.parser = parser
        self.output_path = None
        
    def _process_stage_blocks(self):
        """Calculate first_block and count_block for stages."""
        stages = self.parser.parsed_data.get("stage_array", [])
        blocks = self.parser.parsed_data.get("block_array", [])
        
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(blocks)
                if block["stage"] == stage["uid"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1
            
    def to_json(self) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        self._process_stage_blocks()
        
        # Convert parser data to JSON arrays
        json_data = {}
        for name, parser in self.parser.parsed_data.items():
            writer_class = globals()[f"{name.split('_')[0].capitalize()}Writer"]
            json_data[name] = writer_class(parser).to_json_array()
            
        # Organize into planning structure
        options = {
            "input_dir": str(self.parser.input_path),
            "output_dir": str(self.output_path),
        }
        
        simulation = {}
        for key in ["block_array", "stage_array"]:
            if key in json_data:
                simulation[key] = json_data[key]
                del json_data[key]
                
        return {
            "options": options,
            "simulation": simulation,
            "system": json_data
        }
        
    def write(self, output_path: Union[str, Path]):
        """Write JSON output to file."""
        self.output_path = Path(output_path)
        output_path = self.output_path
        output_path.mkdir(parents=True, exist_ok=True)
        output_file = output_path / "plp2gtopt.json"
        
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(self.to_json(), f, indent=4)


def convert_plp_case(
    input_dir: Union[str, Path], output_dir: Union[str, Path]
) -> Dict[str, int]:
    """Convert PLP input files to GTOPT format.

    Args:
        input_dir: Path to directory containing PLP input files
        output_dir: Path to directory to write GTOPT output files

    Returns:
        Dictionary containing counts of parsed entities:
        {
            'blocks': int,
            'stages': int,
            'buses': int,
            'lines': int,
            'centrals': int,
            'demands': int,
        }

    Raises:
        FileNotFoundError: If input directory or files don't exist
        ValueError: If input files are invalid or inconsistent
        RuntimeError: If conversion fails
    """
    try:
        # Parse all files
        parser = PLPParser(input_dir)
        parser.parse_all()
        
        # Convert to GTOPT format and write output
        writer = GTOptWriter(parser)
        writer.write(output_dir)
        
        print(f"\nConversion successful! Output written to {output_dir}/plp2gtopt.json")
        return {k: len(v) for k, v in parser.parsed_data.items()}
        
    except Exception as e:
        print(f"\nConversion failed: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed: {str(e)}") from e
