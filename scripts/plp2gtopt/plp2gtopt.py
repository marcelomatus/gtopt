"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

from typing import Dict, Any

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.gtopt_writer import GTOptWriter


def convert_plp_case(options: Dict[str, Any] = None) -> Dict[str, int]:
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
        parser = PLPParser(options["input_dir"])
        parser.parse_all()
        # Convert to GTOPT format and write output
        writer = GTOptWriter(parser)
        writer.write(options)
        print(f"\nConversion successful! Output written to {options['output_file']}")
    except Exception as e:
        print(f"\nConversion failed with error: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed. Details: {str(e)}") from e
