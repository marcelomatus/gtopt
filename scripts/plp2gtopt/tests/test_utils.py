"""Common test utilities for plp2gtopt."""

from pathlib import Path
from typing import Any, Dict, List, Type, Tuple, Union
import pytest


def validate_required_fields(
    item: Dict[str, Any], required_fields: Dict[str, Union[Type, Tuple[Type, ...]]]
) -> None:
    """Validate that item contains required fields with correct types."""
    for field, field_type in required_fields.items():
        assert field in item, f"Missing field: {field}"
        if isinstance(field_type, tuple):
            assert isinstance(
                item[field], field_type
            ), f"Field {field} should be one of {field_type}, got {type(item[field])}"
        else:
            assert isinstance(
                item[field], field_type
            ), f"Field {field} should be {field_type}, got {type(item[field])}"


def assert_json_output_structure(
    json_data: List[Dict[str, Any]],
    required_fields: Dict[str, Union[Type, Tuple[Type, ...]]],
) -> None:
    """Validate JSON output structure against required fields."""
    assert isinstance(json_data, list)
    for item in json_data:
        validate_required_fields(item, required_fields)


@pytest.fixture
def tmp_output_path(tmp_path: Path) -> Path:
    """Fixture providing temporary output path."""
    return tmp_path / "output"
