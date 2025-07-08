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
from plp2gtopt.block_writer import BlockWriter
from plp2gtopt.stage_parser import StageParser
from plp2gtopt.stage_writer import StageWriter

from plp2gtopt.bus_parser import BusParser
from plp2gtopt.bus_writer import BusWriter
from plp2gtopt.demand_parser import DemandParser
from plp2gtopt.demand_writer import DemandWriter
from plp2gtopt.generator_parser import CentralParser
from plp2gtopt.generator_writer import CentralWriter
from plp2gtopt.line_parser import LineParser
from plp2gtopt.line_writer import LineWriter
from plp2gtopt.cost_parser import CostParser
from plp2gtopt.cost_writer import CostWriter


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
            'generators': int,
            'demands': int,
        }

    Raises:
        FileNotFoundError: If input directory or files don't exist
        ValueError: If input files are invalid or inconsistent
        RuntimeError: If conversion fails
    """
    input_path = Path(input_dir)
    output_path = Path(output_dir)

    # Validate paths
    if not input_path.is_dir():
        raise FileNotFoundError(f"Input directory not found: {input_path}")
    output_path.mkdir(parents=True, exist_ok=True)

    output_file = output_path / "plp2gtopt.json"

    results = {}
    parsers = [
        ("block_array", BlockParser, BlockWriter, "plpblo.dat"),
        ("stage_array", StageParser, StageWriter, "plpeta.dat"),
        ("bus_array", BusParser, BusWriter, "plpbar.dat"),
        ("line_array", LineParser, LineWriter, "plpcnfli.dat"),
        ("generator_array", CentralParser, CentralWriter, "plpcnfce.dat"),
        ("demand_array", DemandParser, DemandWriter, "plpdem.dat"),
        ("cost_array", CostParser, CostWriter, "plpcosce.dat"),
    ]

    try:
        for name, parser_class, writer_class, filename in parsers:
            filepath = input_path / filename
            print(f"Parsing {filename}...")

            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")

            parser = parser_class(filepath)
            parser.parse()
            writer = writer_class(parser)
            results[name] = writer.to_json_array()
            print(f"Found {name} {len(results[name])}")

        #
        # Finish the Stage definition first_block and count_block
        #

        # Complete the stage data with block information
        stages = results.get("stage_array", [])
        blocks = results.get("block_array", [])
        for stage in stages:
            # find first block that matches stage number
            stage_blocks = [
                index
                for index, block in enumerate(blocks)
                if block["stage"] == stage["uid"]
            ]

            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        #
        # Defining Planning Dictionary
        #
        options = {
            "input_dir": str(input_path),
            "output_dir": str(output_path),
        }

        # Create simulation dictionary with block and stage arrays
        # and remove them from results to avoid duplication.
        simulation = {}
        for key in ["block_array", "stage_array"]:
            if key in results:
                simulation[key] = results[key]
                del results[key]

        # Create system dictionary with all remaining parsed data found in results.
        system = results

        planning = {
            "options": options,
            "simulation": simulation,
            "system": system,
        }

        # Write output to JSON file
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(planning, f, indent=4)

        print(f"\nConversion successful! Output written to {output_file}")
        print(f"Total entities parsed: {len(results)}")

    except Exception as e:
        print(f"\nConversion failed: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed: {str(e)}") from e
