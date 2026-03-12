"""Common test utilities for plp2gtopt."""

from pathlib import Path
from typing import Any, Dict, List, Tuple, Type
import pytest

from plp2gtopt.index_utils import parse_index_range, parse_stages_phase


def validate_required_fields(
    item: Dict[str, Any], required_fields: Dict[str, Type | Tuple[Type, ...]]
) -> None:
    """Validate that item contains required fields with correct types."""
    for field, field_type in required_fields.items():
        assert field in item, f"Missing field: {field}"
        if isinstance(field_type, tuple):
            assert isinstance(item[field], field_type), (
                f"Field {field} should be one of {field_type}, got {type(item[field])}"
            )
        else:
            assert isinstance(item[field], field_type), (
                f"Field {field} should be {field_type}, got {type(item[field])}"
            )


def assert_json_output_structure(
    json_data: List[Dict[str, Any]], required_fields: Dict[str, Type | Tuple[Type, ...]]
) -> None:
    """Validate JSON output structure against required fields."""
    assert isinstance(json_data, list)
    for item in json_data:
        validate_required_fields(item, required_fields)


@pytest.fixture
def tmp_output_path(tmp_path: Path) -> Path:
    """Fixture providing temporary output path."""
    return tmp_path / "output"


# ---------------------------------------------------------------------------
# parse_index_range tests
# ---------------------------------------------------------------------------


def test_parse_index_range_single():
    assert parse_index_range("1") == [1]


def test_parse_index_range_list():
    assert parse_index_range("1,2,3") == [1, 2, 3]


def test_parse_index_range_range():
    assert parse_index_range("5-10") == [5, 6, 7, 8, 9, 10]


def test_parse_index_range_mixed():
    assert parse_index_range("1,2,5-10,11") == [1, 2, 5, 6, 7, 8, 9, 10, 11]


def test_parse_index_range_dedup():
    assert parse_index_range("1,1,2") == [1, 2]


# ---------------------------------------------------------------------------
# parse_stages_phase tests
# ---------------------------------------------------------------------------


def test_parse_stages_phase_basic():
    result = parse_stages_phase("1,2,3", 3)
    assert result == [[1], [2], [3]]


def test_parse_stages_phase_range():
    result = parse_stages_phase("1:4", 4)
    assert result == [[1, 2, 3, 4]]


def test_parse_stages_phase_ellipsis():
    result = parse_stages_phase("1:4,...", 6)
    assert result == [[1, 2, 3, 4], [5], [6]]


def test_parse_stages_phase_mixed():
    result = parse_stages_phase("1:4,5,6,...", 8)
    assert result == [[1, 2, 3, 4], [5], [6], [7], [8]]
