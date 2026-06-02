# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``plexos2gtopt.plexos2gtopt._record_plexos_run``.

The run registry is the filesystem-only contract that
``plp2gtopt --plexos-overlay latest`` consumes.  This test file pins
the registry's shape so a downstream code change doesn't silently
break the latest-pointer resolver in plp2gtopt.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from plexos2gtopt.plexos2gtopt import (
    PLEXOS_RUN_REGISTRY,
    _record_plexos_run,
)


def _redirect_registry(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Point the module-level registry constant at a fresh tmp file."""
    import plexos2gtopt.plexos2gtopt as p2g_mod  # noqa: PLC0415

    reg = tmp_path / "registry" / "runs.jsonl"
    monkeypatch.setattr(p2g_mod, "PLEXOS_RUN_REGISTRY", reg)
    return reg


def test_record_creates_registry_if_missing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The first recorded run must create the registry directory + file."""
    reg = _redirect_registry(tmp_path, monkeypatch)
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    out_file = out_dir / "case.json"
    out_file.write_text("{}")
    _record_plexos_run(
        output_dir=out_dir,
        output_file=out_file,
        input_path=tmp_path / "bundle.zip",
    )
    assert reg.exists()
    lines = reg.read_text().splitlines()
    assert len(lines) == 1
    row = json.loads(lines[0])
    assert row["output_dir"] == str(out_dir.resolve())
    assert row["output_file"] == str(out_file.resolve())
    assert row["bundle_stem"] == "case"
    # ISO-8601 UTC timestamp with trailing Z
    assert row["timestamp"].endswith("Z")
    assert len(row["timestamp"]) == 20  # "YYYY-MM-DDTHH:MM:SSZ"


def test_record_appends_to_existing_registry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Each successful run appends ONE row; earlier rows preserved."""
    reg = _redirect_registry(tmp_path, monkeypatch)
    out_dir_a = tmp_path / "a"
    out_dir_a.mkdir()
    out_file_a = out_dir_a / "a.json"
    out_file_a.write_text("{}")
    out_dir_b = tmp_path / "b"
    out_dir_b.mkdir()
    out_file_b = out_dir_b / "b.json"
    out_file_b.write_text("{}")
    _record_plexos_run(
        output_dir=out_dir_a, output_file=out_file_a, input_path=tmp_path / "a.zip"
    )
    _record_plexos_run(
        output_dir=out_dir_b, output_file=out_file_b, input_path=tmp_path / "b.zip"
    )
    lines = reg.read_text().splitlines()
    assert len(lines) == 2
    rows = [json.loads(line) for line in lines]
    # Append order: a then b. The plp2gtopt resolver reads the LAST line.
    assert rows[0]["bundle_stem"] == "a"
    assert rows[1]["bundle_stem"] == "b"


def test_record_uses_absolute_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Stored paths must be absolute so the resolver works from any cwd."""
    _redirect_registry(tmp_path, monkeypatch)
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    out_file = out_dir / "case.json"
    out_file.write_text("{}")
    # Pass relative paths via a Path that's still anchored; resolve()
    # is the responsibility of the recorder, not the caller.
    _record_plexos_run(
        output_dir=out_dir,
        output_file=out_file,
        input_path=tmp_path / "bundle.zip",
    )
    row = json.loads(
        (tmp_path / "registry" / "runs.jsonl").read_text().splitlines()[-1]
    )
    assert Path(row["output_dir"]).is_absolute()
    assert Path(row["output_file"]).is_absolute()
    assert Path(row["input_path"]).is_absolute()


def test_default_registry_location() -> None:
    """The module-level constant points where plp2gtopt expects it."""
    assert (
        PLEXOS_RUN_REGISTRY
        == Path.home() / ".cache" / "gtopt" / "plexos2gtopt" / "runs.jsonl"
    )
