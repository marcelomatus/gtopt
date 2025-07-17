"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

from typing import Any

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.gtopt_writer import GTOptWriter


def convert_plp_case(options: dict[str, Any]):
    """Convert PLP input files to GTOPT format."""
    try:
        # Parse all files
        parser = PLPParser(options)
        parser.parse_all()
        # Convert to GTOPT format and write output
        writer = GTOptWriter(parser)
        writer.write(options)
        print(f"\nConversion successful! Output written to {options['output_file']}")
    except Exception as e:
        print(f"\nConversion failed with error: {str(e)}")
        raise RuntimeError(f"PLP to GTOPT conversion failed. Details: {str(e)}") from e
