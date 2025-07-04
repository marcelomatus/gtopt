"""PLP to GTOPT conversion functions."""

from typing import Union
from pathlib import Path

def convert_plp_case(input_dir: Union[str, Path], output_dir: Union[str, Path]) -> None:
    """Convert PLP input files to GTOPT format.
    
    Args:
        input_dir: Path to directory containing PLP input files
        output_dir: Path to directory to write GTOPT output files
    
    Raises:
        FileNotFoundError: If input directory doesn't exist
        ValueError: If input files are invalid
    """
    input_path = Path(input_dir)
    output_path = Path(output_dir)

    if not input_path.exists():
        raise FileNotFoundError(f"Input directory not found: {input_path}")

    output_path.mkdir(parents=True, exist_ok=True)

    # TODO: Implement actual conversion logic
    print(f"Converting PLP files from {input_path} to GTOPT format in {output_path}")
