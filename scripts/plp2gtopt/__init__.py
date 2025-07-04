"""PLP to GTOPT conversion tool.

This package provides utilities to convert PLP input files to GTOPT format.
"""

__version__ = "1.0.0"

try:
    from .plp2gtopt import convert_plp_case
    from .main import main
    from .demand_parser import DemandParser

    __all__ = ["convert_plp_case", "main", "DemandParser"]
except ImportError:
    # Allow imports to fail for testing purposes
    __all__ = []
