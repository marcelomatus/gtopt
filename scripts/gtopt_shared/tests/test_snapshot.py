# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.testing.snapshot`."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from gtopt_shared.testing.snapshot import (
    _REFRESH_ENV_VAR,
    assert_golden_file,
    assert_snapshot,
    canonicalise_json,
)


def test_canonicalise_json_sorts_keys_and_appends_newline() -> None:
    """Canonical form is sort-keyed, 2-space indented, newline-terminated."""
    out = canonicalise_json({"b": 1, "a": 2})
    assert out == '{\n  "a": 2,\n  "b": 1\n}\n'
    assert out.endswith("\n")


def test_canonicalise_json_handles_lists() -> None:
    """Lists are emitted with stable ordering inside."""
    out = canonicalise_json([{"k": 1}, {"k": 2}])
    assert "k" in out
    # No exception on nested structures.
    canonicalise_json({"nested": [[1, 2], [3, 4]]})


def test_assert_snapshot_skips_when_golden_missing(tmp_path: Path) -> None:
    """Missing golden → skip with clear refresh instructions."""
    with pytest.raises(pytest.skip.Exception, match=_REFRESH_ENV_VAR):
        assert_snapshot("foo", {"x": 1}, tmp_path / "fixtures")


def test_assert_snapshot_writes_on_refresh(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """``PYTEST_UPDATE_GOLDEN=1`` writes the golden + skips."""
    monkeypatch.setenv(_REFRESH_ENV_VAR, "1")
    fixtures = tmp_path / "fixtures"
    with pytest.raises(pytest.skip.Exception, match="golden fixture written"):
        assert_snapshot("foo", {"x": 1}, fixtures)
    assert (fixtures / "foo.json").exists()
    assert json.loads((fixtures / "foo.json").read_text())["x"] == 1


def test_assert_snapshot_passes_when_payload_matches(tmp_path: Path) -> None:
    """Payload identical to golden → passes silently."""
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    payload = {"x": 1, "y": [1, 2, 3]}
    (fixtures / "ok.json").write_text(canonicalise_json(payload))
    assert_snapshot("ok", payload, fixtures)  # no exception


def test_assert_snapshot_fails_when_payload_drifts(tmp_path: Path) -> None:
    """Payload differs from golden → AssertionError mentioning refresh."""
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    (fixtures / "drift.json").write_text(canonicalise_json({"x": 1}))
    with pytest.raises(AssertionError, match=_REFRESH_ENV_VAR):
        assert_snapshot("drift", {"x": 2}, fixtures)


def test_assert_golden_file_compares_file_content(tmp_path: Path) -> None:
    """``assert_golden_file`` reads + canonicalises the file before comparing."""
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    payload = {"a": 1, "b": [2, 3]}
    (fixtures / "file_ok.json").write_text(canonicalise_json(payload))

    converter_output = tmp_path / "out.json"
    # converter writes its own (non-canonical, indent-2) representation.
    converter_output.write_text(json.dumps(payload, indent=2))

    assert_golden_file("file_ok", converter_output, fixtures)


def test_assert_golden_file_supports_scrub_hook(tmp_path: Path) -> None:
    """``scrub`` callback can mask non-deterministic fields (e.g. tmp paths)."""
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    expected = {"options": {"input_directory": "<scrubbed>"}}
    (fixtures / "scrub_ok.json").write_text(canonicalise_json(expected))

    converter_output = tmp_path / "out.json"
    converter_output.write_text(
        json.dumps({"options": {"input_directory": "/tmp/run_abc123"}})
    )

    def _scrub(data):
        if "options" in data and "input_directory" in data["options"]:
            data["options"]["input_directory"] = "<scrubbed>"
        return data

    assert_golden_file("scrub_ok", converter_output, fixtures, scrub=_scrub)


def test_assert_golden_file_missing_file_fails_with_clear_message(
    tmp_path: Path,
) -> None:
    """Missing converter output → AssertionError mentioning the file path."""
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    (fixtures / "nope.json").write_text("{}\n")
    with pytest.raises(AssertionError, match="converter did not produce"):
        assert_golden_file("nope", tmp_path / "missing.json", fixtures)


def test_refresh_target_threads_into_messages(tmp_path: Path) -> None:
    """``refresh_target`` appears in the skip / fail message."""
    with pytest.raises(pytest.skip.Exception, match="my/test/path.py"):
        assert_snapshot(
            "miss",
            {},
            tmp_path / "fixtures",
            refresh_target="my/test/path.py",
        )
