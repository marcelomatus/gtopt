# SPDX-License-Identifier: BSD-3-Clause
"""CLI smoke tests — exit codes 0/2/3 plus --help."""

from __future__ import annotations

import json

import pytest

from gtopt_marginal_units.constants import (
    EXIT_INPUT_ERROR,
    EXIT_OK,
)
from gtopt_marginal_units.main import cli


def test_help_exits_zero(capsys):
    with pytest.raises(SystemExit) as exc:
        cli(["--help"])
    assert exc.value.code == EXIT_OK
    captured = capsys.readouterr()
    assert "gtopt-marginal-units" in captured.out


def test_no_args_exits_input_error():
    code = cli([])
    assert code == EXIT_INPUT_ERROR


def test_simulated_mode_requires_planning_and_output(tmp_path):
    code = cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "simulated",
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR


def test_real_mode_with_gtopt_input_kind_rejected(tmp_path):
    planning = tmp_path / "p.json"
    planning.write_text(json.dumps({"system": {}}))
    code = cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "real",  # incompatible
            "--planning",
            str(planning),
            "--output",
            str(tmp_path),
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR


def test_auto_kind_with_both_planning_and_feed_rejected(tmp_path):
    planning = tmp_path / "p.json"
    planning.write_text(json.dumps({"system": {}}))
    feed = tmp_path / "feed.parquet"
    feed.mkdir()
    code = cli(
        [
            "--mode",
            "simulated",
            "--planning",
            str(planning),
            "--output",
            str(tmp_path),
            "--feed",
            str(feed),
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR
