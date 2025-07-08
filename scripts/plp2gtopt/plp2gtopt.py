"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

from pathlib import Path
from typing import Dict, Union

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.gtopt_writer import GTOptWriter


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
