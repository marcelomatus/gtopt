"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

from pathlib import Path
from typing import Dict, List, Union

from plp2gtopt.demand_parser import DemandParser
from plp2gtopt.stage_parser import StageParser
from plp2gtopt.block_parser import BlockParser
from plp2gtopt.bus_parser import BusParser
from plp2gtopt.line_parser import LineParser
from plp2gtopt.generator_parser import GeneratorParser


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
        ("blocks", BlockParser, "plpblo.dat", "num_blocks"),
        ("stages", StageParser, "plpeta.dat", "num_stages"),
        ("buses", BusParser, "plpbar.dat", "num_buses"),
        ("lines", LineParser, "plpcnfli.dat", "num_lines"),
        ("generators", GeneratorParser, "plpcnfce.dat", "num_generators"),
        (
            "demands",
            DemandParser,
            "plpdem.dat",
            "num_bars",
        ),  # Note: DemandParser uses num_bars
    ]

    try:
        for name, parser_class, filename, count_attr in parsers:
            filepath = input_path / filename
            print(f"Parsing {filename}...")

            if not filepath.exists():
                raise FileNotFoundError(f"{name} file not found: {filepath}")

            parser = parser_class(filepath)
            parser.parse()
            results[name] = getattr(parser, count_attr)
            print(f"Found {results[name]} {name}")

        print(f"\nConversion complete. Results saved to: {output_path}")
        return results

    except Exception as e:
        print(f"\nConversion failed: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed: {str(e)}") from e
