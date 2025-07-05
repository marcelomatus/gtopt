"""PLP to GTOPT conversion functions."""

from pathlib import Path
from typing import Union

from .demand_parser import DemandParser
from .stage_parser import StageParser
from .block_parser import BlockParser
from .bus_parser import BusParser
from .line_parser import LineParser
from .generator_parser import GeneratorParser


def convert_plp_case(input_dir: Union[str, Path], output_dir: Union[str, Path]) -> None:
    """Convert PLP input files to GTOPT format.

    Args:
        input_dir: Path to directory containing PLP input files
        output_dir: Path to directory to write GTOPT output files

    Raises:
        FileNotFoundError: If input directory doesn't exist
        ValueError: If input files are invalid
    """
    """Convert PLP input files to GTOPT format.

    Args:
        input_dir: Path to directory containing PLP input files
        output_dir: Path to directory to write GTOPT output files

    Raises:
        FileNotFoundError: If input directory doesn't exist
        ValueError: If input files are invalid
    """
    input_path = Path(input_dir)
    if not input_path.exists():
        raise FileNotFoundError(f"Input directory not found: {input_path}")

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print("Parsing plpblo.dat file...")
    block_parser = BlockParser(input_path / "plpblo.dat")
    if not block_parser.file_path.exists():
        raise FileNotFoundError(f"Block file not found: {block_parser.file_path}")
    block_parser.parse()
    print(f"blocks {block_parser.num_blocks}")
    print("Parsing complete.")

    print("Parsing plpeta.dat file...")
    stage_parser = StageParser(input_path / "plpeta.dat")
    if not stage_parser.file_path.exists():
        raise FileNotFoundError(f"Stage file not found: {stage_parser.file_path}")
    stage_parser.parse()
    print(f"stages {stage_parser.num_stages}")
    print("Parsing complete.")

    print("Parsing plpbar.dat file...")
    bus_parser = BusParser(input_path / "plpbar.dat")
    if not bus_parser.file_path.exists():
        raise FileNotFoundError(f"Bus file not found: {bus_parser.file_path}")
    bus_parser.parse()
    print(f"buses {bus_parser.num_buses}")
    print("Parsing complete.")

    print("Parsing plpcnfli.dat file...")
    line_parser = LineParser(input_path / "plpcnfli.dat")
    if not line_parser.file_path.exists():
        raise FileNotFoundError(f"Line file not found: {line_parser.file_path}")
    line_parser.parse()
    print(f"lines {line_parser.num_lines}")
    print("Parsing complete.")

    print("Parsing plpcnfce.dat file...")
    generator_parser = GeneratorParser(input_path / "plpcnfce.dat")
    if not generator_parser.file_path.exists():
        raise FileNotFoundError(f"Generator file not found: {generator_parser.file_path}")
    generator_parser.parse()
    print(f"generators {generator_parser.num_generators}")
    print("Parsing complete.")

    print("Parsing plpdem.dat file...")
    demand_parser = DemandParser(input_path / "plpdem.dat")
    if not demand_parser.file_path.exists():
        raise FileNotFoundError(f"Demand file not found: {demand_parser.file_path}")
    demand_parser.parse()
    print(f"demands {demand_parser.num_bars}")
    print("Parsing complete.")

    # TODO: Implement actual conversion logic
    print(f"Converting PLP files from {input_path} to GTOPT format in {output_path}")
