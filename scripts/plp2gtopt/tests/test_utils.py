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


# ---------------------------------------------------------------------------
# parse_index_range error-handling tests
# ---------------------------------------------------------------------------


def test_parse_index_range_empty_string():
    """Empty string raises ValueError (line 33)."""
    with pytest.raises(ValueError, match="Empty index specification"):
        parse_index_range("")


def test_parse_index_range_whitespace_only():
    """Whitespace-only string raises ValueError (line 33)."""
    with pytest.raises(ValueError, match="Empty index specification"):
        parse_index_range("   ")


def test_parse_index_range_non_numeric_single():
    """Non-numeric token raises ValueError (lines 57-58)."""
    with pytest.raises(ValueError, match="Invalid index"):
        parse_index_range("abc")


def test_parse_index_range_non_numeric_in_range():
    """Non-numeric token in a range raises ValueError (lines 45-46)."""
    with pytest.raises(ValueError, match="Invalid range"):
        parse_index_range("a-b")


def test_parse_index_range_reversed_range():
    """Range with start > end raises ValueError (line 50)."""
    with pytest.raises(ValueError, match="Range start .* > end"):
        parse_index_range("10-5")


def test_parse_index_range_trailing_comma():
    """Trailing comma is tolerated; empty part skipped (line 39)."""
    assert parse_index_range("1,2,") == [1, 2]


# ---------------------------------------------------------------------------
# parse_stages_phase error-handling and edge-case tests
# ---------------------------------------------------------------------------


def test_parse_stages_phase_empty_string():
    """Empty string raises ValueError (line 99)."""
    with pytest.raises(ValueError, match="Empty stages-phase specification"):
        parse_stages_phase("", 5)


def test_parse_stages_phase_whitespace_only():
    """Whitespace-only string raises ValueError (line 99)."""
    with pytest.raises(ValueError, match="Empty stages-phase specification"):
        parse_stages_phase("   ", 5)


def test_parse_stages_phase_non_numeric_range():
    """Non-numeric range token raises ValueError (lines 115-116)."""
    with pytest.raises(ValueError, match="Invalid stage range"):
        parse_stages_phase("a:b", 5)


def test_parse_stages_phase_reversed_range():
    """Range with start > end raises ValueError (line 118)."""
    with pytest.raises(ValueError, match="Stage range start .* > end"):
        parse_stages_phase("5:2", 10)


def test_parse_stages_phase_range_out_of_bounds():
    """Range exceeding num_stages raises ValueError (lines 120)."""
    with pytest.raises(ValueError, match="out of bounds"):
        parse_stages_phase("1:10", 5)


def test_parse_stages_phase_range_below_one():
    """Range starting below 1 raises ValueError (line 120)."""
    with pytest.raises(ValueError, match="out of bounds"):
        parse_stages_phase("0:3", 5)


def test_parse_stages_phase_non_numeric_single():
    """Non-numeric single token raises ValueError (lines 126-127)."""
    with pytest.raises(ValueError, match="Invalid stage token"):
        parse_stages_phase("abc", 5)


def test_parse_stages_phase_single_out_of_bounds():
    """Single stage index exceeding num_stages raises ValueError (line 129)."""
    with pytest.raises(ValueError, match="out of bounds"):
        parse_stages_phase("10", 5)


def test_parse_stages_phase_single_below_one():
    """Single stage index below 1 raises ValueError (line 129)."""
    with pytest.raises(ValueError, match="out of bounds"):
        parse_stages_phase("0", 5)


def test_parse_stages_phase_ellipsis_only():
    """Ellipsis alone expands from stage 1 to num_stages (line 142)."""
    result = parse_stages_phase("...", 4)
    assert result == [[1], [2], [3], [4]]
