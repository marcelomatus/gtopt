#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting block data to JSON format."""

from typing import List, Dict, Any
from pathlib import Path
import json
from .block_parser import BlockParser

class BlockWriter:
    """Converts block parser data to JSON format used by GTOPT.
    
    Handles:
    - Converting block data to JSON format
    - Writing to output files
    - Maintaining format consistency with system_c0.json
    """

    def __init__(self, block_parser: BlockParser):
        """Initialize with a BlockParser instance.
        
        Args:
            block_parser: BlockParser containing parsed block data
        """
        self.block_parser = block_parser
        self.blocks = block_parser.get_blocks()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert block data to JSON array format.
        
        Returns:
            List of block dictionaries in GTOPT JSON format
        """
        json_blocks = []
        for block in self.blocks:
            json_block = {
                "uid": block["number"],
                "duration": block["duration"]
            }
            json_blocks.append(json_block)
        return json_blocks

    def write_to_file(self, output_path: Path) -> None:
        """Write block data to JSON file.
        
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
            raise IOError(f"Failed to write block JSON: {str(e)}") from e

    @staticmethod
    def from_block_file(block_file: Path) -> 'BlockWriter':
        """Create BlockWriter directly from block file.
        
        Args:
            block_file: Path to plpblo.dat file
            
        Returns:
            BlockWriter instance
            
        Raises:
            FileNotFoundError: If block file doesn't exist
        """
        parser = BlockParser(block_file)
        parser.parse()
        return BlockWriter(parser)


if __name__ == "__main__":
    import sys
    from pathlib import Path

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.plpblo.dat> <output.json>")
        sys.exit(1)

    input_file = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    try:
        writer = BlockWriter.from_block_file(input_file)
        writer.write_to_file(output_file)
        print(f"Successfully wrote block data to {output_file}")
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        sys.exit(1)
