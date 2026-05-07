# SPDX-License-Identifier: BSD-3-Clause
"""cen2gtopt CLI smoke tests."""

from __future__ import annotations

import json

import pytest

from cen2gtopt.main import (
    EXIT_INPUT_ERROR,
    EXIT_OK,
    cli,
)


def test_help_exits_zero(capsys):
    with pytest.raises(SystemExit) as exc:
        cli(["--help"])
    assert exc.value.code == EXIT_OK


def test_start_after_end_exits_input_error(tmp_path):
    code = cli(
        [
            "--start",
            "2026-04-10",
            "--end",
            "2026-04-01",
            "--out",
            str(tmp_path / "feed.parquet"),
            "--manifest-only",
        ]
    )
    assert code == EXIT_INPUT_ERROR


def test_manifest_only_works_without_network(tmp_path):
    out = tmp_path / "feed.parquet"
    code = cli(
        [
            "--start",
            "2026-04-01",
            "--end",
            "2026-04-07",
            "--out",
            str(out),
            "--manifest-only",
        ]
    )
    assert code == EXIT_OK
    manifest = json.loads((out / "manifest.json").read_text())
    assert manifest["producer"] == "cen2gtopt"
    assert manifest["manifest_only"] is True
    assert manifest["args"]["source"] == "csv"


def test_sip_source_v1_returns_input_error(tmp_path, monkeypatch):
    from unittest.mock import patch

    monkeypatch.setenv("CEN_USER_KEY", "fake-key-not-used")

    # Patch out the HTTP layer so this test doesn't hit the real CEN
    # gateway during CI. We simulate the live behaviour: a fake key
    # gets a 403 "Authentication failed" from the 3scale gateway.
    class _FakeResp:
        status_code = 403
        text = "Authentication failed"
        headers = {"content-type": "text/plain"}

        def json(self):
            return None

    with patch("requests.Session.get", return_value=_FakeResp()):
        code = cli(
            [
                "--start",
                "2026-04-01",
                "--end",
                "2026-04-02",
                "--out",
                str(tmp_path / "feed.parquet"),
                "--source",
                "sip",
            ]
        )
    assert code == EXIT_INPUT_ERROR


def test_csv_path_without_topology_errors(tmp_path):
    """v1 CSV path requires --topology unless --manifest-only."""
    code = cli(
        [
            "--start",
            "2026-04-01",
            "--end",
            "2026-04-02",
            "--out",
            str(tmp_path / "feed.parquet"),
            "--source",
            "csv",
        ]
    )
    assert code == EXIT_INPUT_ERROR
