from pathlib import Path
from typing import Any, Dict, List, Type, TypeVar
import json

T = TypeVar('T', bound='BaseWriter')
P = TypeVar('P', bound='BaseParser')

class BaseWriter:
    """Base class for all GTOPT JSON writers."""
    
    def __init__(self, parser: P) -> None:
        """Initialize with a parser instance.
        
        Args:
            parser: Parser containing parsed data
        """
        self.parser = parser
        self.items = parser.get_items()  # Will be overridden by child classes

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert data to JSON array format (to be implemented by subclasses)."""
        raise NotImplementedError

    def write_to_file(self, output_path: Path) -> None:
        """Write data to JSON file.
        
        Args:
            output_path: Path to output JSON file
            
        Raises:
            IOError: If file writing fails
            ValueError: If JSON conversion fails
        """
        json_data = self.to_json_array()
        try:
            with open(output_path, "w", encoding="utf-8") as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except (IOError, json.JSONEncodeError) as e:
            raise IOError(f"Failed to write JSON: {str(e)}") from e

    @classmethod
    def from_file(cls: Type[T], input_file: Path, parser_class: Type[P]) -> T:
        """Create writer directly from input file.
        
        Args:
            input_file: Path to input data file
            parser_class: Parser class to use
            
        Returns:
            Writer instance
            
        Raises:
            FileNotFoundError: If input file doesn't exist
        """
        parser = parser_class(input_file)
        parser.parse()
        return cls(parser)

    @staticmethod
    def main(writer_class: Type[T], parser_class: Type[P]) -> None:
        """Standard main method for CLI execution.
        
        Args:
            writer_class: Writer class to use
            parser_class: Parser class to use
        """
        import sys
        from pathlib import Path

        if len(sys.argv) != 3:
            print(f"Usage: {sys.argv[0]} <input.dat> <output.json>")
            sys.exit(1)

        input_file = Path(sys.argv[1])
        output_file = Path(sys.argv[2])

        try:
            writer = writer_class.from_file(input_file, parser_class)
            writer.write_to_file(output_file)
            print(f"Successfully wrote data to {output_file}")
        except (IOError, ValueError, FileNotFoundError) as e:
            print(f"Error: {str(e)}", file=sys.stderr)
            sys.exit(1)
