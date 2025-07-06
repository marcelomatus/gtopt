"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

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
from plp2gtopt.generator_parser import GeneratorParser
from plp2gtopt.generator_writer import GeneratorWriter
from plp2gtopt.line_parser import LineParser
from plp2gtopt.line_writer import LineWriter


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
            'demands': int
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

    results = {}
    parsers = [
        ("block_array", BlockParser, BlockWriter, "plpblo.dat"),
        ("stage_array", StageParser, StageWriter, "plpeta.dat"),
        ("bus_array", BusParser, BusWriter, "plpbar.dat"),
        ("line_array", LineParser, LineWriter, "plpcnfli.dat"),
        ("generator_array", GeneratorParser, GeneratorWriter, "plpcnfce.dat"),
        ("demand_array", DemandParser, DemandWriter, "plpdem.dat"),
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

        # defining simulation and system

        simulation = {
            "block_array": results["block_array"],
            "stage_array": results["stage_array"],
        }

        del results["block_array"]
        del results["stage_array"]


        system = {
            "bus_array": results["bus_array"],
            "line_array": results["line_array"],
            "generator_array": results["generator_array"],
            "demand_array": results["demand_array"],
        }


        return results

    except Exception as e:
        print(f"\nConversion failed: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed: {str(e)}") from e
