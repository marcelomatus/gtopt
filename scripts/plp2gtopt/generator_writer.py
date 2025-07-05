#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator data to JSON format."""

from typing import List, Dict, Any
from pathlib import Path
import json
from .generator_parser import GeneratorParser

class GeneratorWriter:
    """Converts generator parser data to JSON format used by GTOPT.
    
    Handles:
    - Converting generator data to JSON array format
    - Writing to output files
    - Maintaining format consistency with system_c0.json
    """

    def __init__(self, generator_parser: GeneratorParser):
        """Initialize with a GeneratorParser instance.
        
        Args:
            generator_parser: GeneratorParser containing parsed generator data
        """
        self.generator_parser = generator_parser
        self.generators = generator_parser.get_generators()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert generator data to JSON array format.
        
        Returns:
            List of generator dictionaries in GTOPT JSON format
        """
        json_generators = []
        for generator in self.generators:
            json_generator = {
                "uid": generator["id"],
                "name": generator["name"],
                "bus": generator["bus"],
                "gcost": generator["variable_cost"],
                "capacity": generator["p_max"],
                "expcap": None,  # Not in PLP format
                "expmod": None,  # Not in PLP format 
                "annual_capcost": None  # Not in PLP format
            }
            json_generators.append(json_generator)
        return json_generators

    def write_to_file(self, output_path: Path) -> None:
        """Write generator data to JSON file.
        
        Args:
            output_path: Path to output JSON file
            
        Raises:
            IOError: If file writing fails
        """
        json_data = self.to_json_array()
        try:
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except IOError as e:
            raise IOError(f"Failed to write generator JSON: {str(e)}") from e

    @staticmethod
    def from_generator_file(generator_file: Path) -> 'GeneratorWriter':
        """Create GeneratorWriter directly from generator file.
        
        Args:
            generator_file: Path to plpcnfce.dat file
            
        Returns:
            GeneratorWriter instance
            
        Raises:
            FileNotFoundError: If generator file doesn't exist
        """
        parser = GeneratorParser(generator_file)
        parser.parse()
        return GeneratorWriter(parser)


if __name__ == "__main__":
    import sys
    from pathlib import Path

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.plpcnfce.dat> <output.json>")
        sys.exit(1)

    input_file = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    try:
        writer = GeneratorWriter.from_generator_file(input_file)
        writer.write_to_file(output_file)
        print(f"Successfully wrote generator data to {output_file}")
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        sys.exit(1)
