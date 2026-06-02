# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``gtopt_shared.cli_flags``.

Issue #507 Phase 2 registrars: each test confirms that the shared
registrar reproduces the per-converter spelling, default, and (where
applicable) BooleanOptionalAction paired form.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pytest

from gtopt_shared.cli_flags import (
    add_emissions_arguments,
    add_use_single_bus_argument,
)


# ---------------------------------------------------------------------------
# add_emissions_arguments
# ---------------------------------------------------------------------------


def _parser_with_emissions() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    add_emissions_arguments(parser)
    return parser


def test_emissions_defaults_off() -> None:
    args = _parser_with_emissions().parse_args([])
    assert args.emissions is False
    assert args.emissions_file is None
    assert args.emissions_report is None


def test_emissions_master_switch_on() -> None:
    args = _parser_with_emissions().parse_args(["--emissions"])
    assert args.emissions is True


def test_emissions_custom_file_and_report(tmp_path: Path) -> None:
    custom = tmp_path / "custom.json"
    report = tmp_path / "report.json"
    args = _parser_with_emissions().parse_args(
        [
            "--emissions",
            "--emissions-file",
            str(custom),
            "--emissions-report",
            str(report),
        ]
    )
    assert args.emissions_file == custom
    assert args.emissions_report == report


def test_emissions_default_override_via_defaults_dict() -> None:
    parser = argparse.ArgumentParser()
    add_emissions_arguments(parser, defaults={"emissions": True})
    args = parser.parse_args([])
    assert args.emissions is True


# ---------------------------------------------------------------------------
# add_use_single_bus_argument
# ---------------------------------------------------------------------------


def test_use_single_bus_plain_store_true_default_false() -> None:
    """plexos2gtopt-style: long-only flag, default False."""
    parser = argparse.ArgumentParser()
    add_use_single_bus_argument(parser)
    args = parser.parse_args([])
    assert args.use_single_bus is False
    args = parser.parse_args(["--use-single-bus"])
    assert args.use_single_bus is True


def test_use_single_bus_plp2gtopt_form() -> None:
    """plp2gtopt-style: -b short flag, paired BooleanOptionalAction,
    default None (auto-detect signal)."""
    parser = argparse.ArgumentParser()
    add_use_single_bus_argument(
        parser,
        short_flag="-b",
        paired=True,
        default=None,
        help_override="plp2gtopt-specific help",
    )
    # Default
    args = parser.parse_args([])
    assert args.use_single_bus is None
    # Short flag still works
    args = parser.parse_args(["-b"])
    assert args.use_single_bus is True
    # Negated paired form
    args = parser.parse_args(["--no-use-single-bus"])
    assert args.use_single_bus is False


def test_use_single_bus_help_override_carried() -> None:
    parser = argparse.ArgumentParser()
    add_use_single_bus_argument(parser, help_override="bespoke help body")
    help_text = parser.format_help()
    assert "bespoke help body" in help_text


@pytest.mark.parametrize(
    ("paired", "expect_no_flag"),
    [(True, True), (False, False)],
)
def test_use_single_bus_paired_controls_no_flag(
    paired: bool, expect_no_flag: bool
) -> None:
    parser = argparse.ArgumentParser()
    add_use_single_bus_argument(parser, paired=paired, default=None)
    help_text = parser.format_help()
    assert ("--no-use-single-bus" in help_text) == expect_no_flag
