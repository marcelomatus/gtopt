"""Shared pytest fixtures for plp2gtopt tests."""

from pathlib import Path
import pytest


def get_example_file(filename: str) -> Path:
    """Get path to example file in plp_dat_ex directory."""
    path = Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / filename
    if not path.exists():
        raise FileNotFoundError(f"Example file not found: {path}")
    return path


@pytest.fixture
def valid_mance_file() -> Path:
    """Fixture providing path to valid test maintenance file."""
    return get_example_file("plpmance.dat")
