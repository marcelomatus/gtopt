"""Unit tests for ror_equivalence_parser module."""

from __future__ import annotations

from pathlib import Path

import pytest

from ..ror_equivalence_parser import (
    parse_ror_equivalence_file,
    parse_ror_selection,
)


def _write(tmp_path: Path, content: str) -> Path:
    """Write *content* to a CSV inside *tmp_path* and return the path."""
    path = tmp_path / "ror_equivalence.csv"
    path.write_text(content, encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file — happy paths
# ---------------------------------------------------------------------------


def test_parse_happy_path_basic(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3\nCentralA,1.23\nCentralB,0.45\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec == {"CentralA": 1.23, "CentralB": 0.45}
    # File order preserved.
    assert list(spec.keys()) == ["CentralA", "CentralB"]


def test_parse_ignores_unknown_columns(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,notes,extra\nCentralA,1.0,hello,world\n",
    )
    spec = parse_ror_equivalence_file(path)
    assert spec == {"CentralA": 1.0}


def test_parse_comment_column_ignored(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "name,vmax_hm3,comment\nCentralA,1.0,this is a note\n",
    )
    assert parse_ror_equivalence_file(path) == {"CentralA": 1.0}


def test_parse_enabled_truthy_values(tmp_path: Path) -> None:
    content = (
        "name,vmax_hm3,enabled\n"
        "A,1.0,true\n"
        "B,2.0,t\n"
        "C,3.0,yes\n"
        "D,4.0,y\n"
        "E,5.0,on\n"
        "F,6.0,1\n"
        "G,7.0,True\n"
    )
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {
        "A": 1.0,
        "B": 2.0,
        "C": 3.0,
        "D": 4.0,
        "E": 5.0,
        "F": 6.0,
        "G": 7.0,
    }


def test_parse_enabled_falsy_values_skip(tmp_path: Path) -> None:
    content = (
        "name,vmax_hm3,enabled\n"
        "Keep,1.0,true\n"
        "SkipFalse,2.0,false\n"
        "SkipUpper,3.0,FALSE\n"
        "SkipNo,4.0,no\n"
        "SkipZero,5.0,0\n"
    )
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {"Keep": 1.0}


def test_parse_enabled_empty_or_unset_is_enabled(tmp_path: Path) -> None:
    # Explicit empty string in the enabled column → treated as enabled.
    content = "name,vmax_hm3,enabled\nA,1.0,\nB,2.0,   \n"
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {"A": 1.0, "B": 2.0}


def test_parse_skipped_row_allows_bad_vmax(tmp_path: Path) -> None:
    # A disabled row is skipped before vmax validation runs.
    content = "name,vmax_hm3,enabled\nKept,1.0,true\nDisabled,abc,false\n"
    spec = parse_ror_equivalence_file(_write(tmp_path, content))
    assert spec == {"Kept": 1.0}


# ---------------------------------------------------------------------------
# parse_ror_equivalence_file — error paths
# ---------------------------------------------------------------------------


def test_parse_missing_file_raises(tmp_path: Path) -> None:
    missing = tmp_path / "does_not_exist.csv"
    with pytest.raises(FileNotFoundError, match="ROR equivalence file not found"):
        parse_ror_equivalence_file(missing)


def test_parse_missing_name_column(tmp_path: Path) -> None:
    path = _write(tmp_path, "foo,vmax_hm3\nCentralA,1.0\n")
    with pytest.raises(ValueError, match=r"missing required column.*'name'"):
        parse_ror_equivalence_file(path)


def test_parse_missing_vmax_column(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,other\nCentralA,1.0\n")
    with pytest.raises(ValueError, match=r"missing required column.*'vmax_hm3'"):
        parse_ror_equivalence_file(path)


def test_parse_empty_name_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\n,1.0\n")
    with pytest.raises(ValueError, match=r"line 2: empty name"):
        parse_ror_equivalence_file(path)


def test_parse_whitespace_only_name_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nA,1.0\n   ,2.0\n")
    with pytest.raises(ValueError, match=r"line 3: empty name"):
        parse_ror_equivalence_file(path)


def test_parse_empty_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nCentralA,\n")
    with pytest.raises(
        ValueError, match=r"line 2: missing vmax_hm3 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_non_numeric_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nCentralA,abc\n")
    with pytest.raises(
        ValueError,
        match=r"line 2: non-numeric vmax_hm3 'abc' for central 'CentralA'",
    ):
        parse_ror_equivalence_file(path)


def test_parse_zero_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nCentralA,0\n")
    with pytest.raises(
        ValueError, match=r"line 2: vmax_hm3 must be > 0 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_negative_vmax_raises(tmp_path: Path) -> None:
    path = _write(tmp_path, "name,vmax_hm3\nCentralA,-1.5\n")
    with pytest.raises(
        ValueError, match=r"line 2: vmax_hm3 must be > 0 for central 'CentralA'"
    ):
        parse_ror_equivalence_file(path)


def test_parse_duplicate_name_raises(tmp_path: Path) -> None:
    content = "name,vmax_hm3\nCentralA,1.0\nCentralB,2.0\nCentralA,3.0\n"
    path = _write(tmp_path, content)
    with pytest.raises(ValueError, match=r"line 4: duplicate central name 'CentralA'"):
        parse_ror_equivalence_file(path)


# ---------------------------------------------------------------------------
# parse_ror_selection
# ---------------------------------------------------------------------------


def test_selection_none_input_returns_none() -> None:
    assert parse_ror_selection(None) is None


@pytest.mark.parametrize("value", ["", "   ", "none", "NONE", "None", "  none  "])
def test_selection_none_like_returns_none(value: str) -> None:
    assert parse_ror_selection(value) is None


@pytest.mark.parametrize("value", ["all", "ALL", "All", "  all  "])
def test_selection_all_returns_empty_frozenset(value: str) -> None:
    result = parse_ror_selection(value)
    assert result == frozenset()
    assert isinstance(result, frozenset)


def test_selection_explicit_list() -> None:
    result = parse_ror_selection("A,B,C")
    assert result == frozenset({"A", "B", "C"})
    assert isinstance(result, frozenset)


def test_selection_trims_whitespace_and_drops_empty_tokens() -> None:
    result = parse_ror_selection("A, ,B")
    assert result == frozenset({"A", "B"})


def test_selection_trims_leading_trailing_whitespace() -> None:
    result = parse_ror_selection("  A , B  ,  C ")
    assert result == frozenset({"A", "B", "C"})


def test_selection_single_name() -> None:
    assert parse_ror_selection("Solo") == frozenset({"Solo"})


def test_selection_only_commas_raises() -> None:
    with pytest.raises(ValueError, match=r"--ror-as-reservoirs"):
        parse_ror_selection(",")


def test_selection_only_commas_and_whitespace_raises() -> None:
    with pytest.raises(ValueError, match=r"--ror-as-reservoirs"):
        parse_ror_selection(" , , ")
