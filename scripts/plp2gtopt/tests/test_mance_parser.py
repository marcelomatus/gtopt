"""Unit tests for mance_parser.py"""

from pathlib import Path
import pytest
from ..mance_parser import ManceParser


def test_parse_valid_file(valid_mance_file: Path) -> None:
    """Test parsing a valid maintenance file."""
    parser = ManceParser(valid_mance_file)
    parser.parse()
    
    assert parser.num_maintenances == 2
    assert len(parser.maintenances[0]["blocks"]) == 4
    assert parser.maintenances[0]["name"] == "ABANICO"
    assert parser.maintenances[0]["p_min"][0] == 5.0
    assert parser.maintenances[0]["p_max"][0] == 69.75


def test_parse_empty_file(tmp_path: Path) -> None:
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()
    
    parser = ManceParser(empty_file)
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path: Path) -> None:
    """Test handling of malformed maintenance file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'CENTRAL'\n2 1\n03 001 1 5.0 69.75")  # Missing entries
    
    parser = ManceParser(bad_file)
    with pytest.raises(ValueError):
        parser.parse()


def test_get_maintenance_by_name(valid_mance_file: Path) -> None:
    """Test lookup of maintenance by name."""
    parser = ManceParser(valid_mance_file)
    parser.parse()
    
    maint = parser.get_maintenance_by_name("ABANICO")
    assert maint is not None
    assert maint["name"] == "ABANICO"
    
    assert parser.get_maintenance_by_name("NON_EXISTENT") is None
