# SPDX-License-Identifier: BSD-3-Clause
"""Per-mode minimum-data validation."""

from __future__ import annotations


from gtopt_marginal_units.constants import EXIT_INPUT_ERROR
from gtopt_marginal_units.main import cli


def test_feed_mode_requires_feed_arg(tmp_path):
    code = cli(
        [
            "--input-kind",
            "feed-parquet",
            "--mode",
            "real",
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR


def test_real_reconstruct_with_gtopt_kind_rejected(tmp_path):
    code = cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "real-reconstruct",
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR


def test_simulated_with_feed_kind_rejected(tmp_path):
    feed = tmp_path / "feed.parquet"
    feed.mkdir()
    code = cli(
        [
            "--input-kind",
            "feed-parquet",
            "--mode",
            "simulated",
            "--feed",
            str(feed),
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert code == EXIT_INPUT_ERROR
