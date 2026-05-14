# SPDX-License-Identifier: BSD-3-Clause
"""CLI smoke tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from gtopt_reduce_network.main import main


def test_reduce_subcommand_writes_outputs(
    tmp_path: Path, ieee14_path: Path, capsys
) -> None:
    out = tmp_path / "ieee14_reduced.json"
    with pytest.raises(SystemExit) as excinfo:
        main(
            [
                "reduce",
                str(ieee14_path),
                "-K",
                "7",
                "-o",
                str(out),
                "--summary",
            ]
        )
    assert excinfo.value.code == 0
    assert out.exists()
    data = json.loads(out.read_text())
    assert "system" in data
    assert len(data["system"]["bus_array"]) <= 8
    base = out.with_suffix("")
    assert base.with_name(base.name + ".busmap.csv").exists()
    assert base.with_name(base.name + ".linemap.csv").exists()
    assert base.with_name(base.name + ".aggregator.csv").exists()
    assert base.with_name(base.name + ".reducer_config.json").exists()


def test_invalid_subcommand_errors(capsys) -> None:
    with pytest.raises(SystemExit):
        main(["bogus"])


def test_reduce_help(capsys) -> None:
    with pytest.raises(SystemExit) as excinfo:
        main(["reduce", "--help"])
    assert excinfo.value.code == 0


def test_main_module_importable() -> None:
    # Confirms the entry point is loadable without invoking it.
    import gtopt_reduce_network.main as cli  # noqa: F401

    assert hasattr(cli, "main")
