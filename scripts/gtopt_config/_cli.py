# SPDX-License-Identifier: BSD-3-Clause
"""Shared CLI argument helpers for all gtopt scripts.

Provides standard ``--version``, ``--log-level``, ``--no-color``, and
``--config`` argument groups so that every gtopt CLI tool has a uniform
interface.

Usage::

    from gtopt_config import add_common_arguments, configure_logging

    parser = argparse.ArgumentParser(...)
    # ... tool-specific arguments ...
    add_common_arguments(parser)          # version + log-level + color
    args = parser.parse_args()
    configure_logging(args)
"""

from __future__ import annotations

import argparse
import logging
import os
import sys

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------

_PACKAGE_NAME = "gtopt-scripts"


def get_version() -> str:
    """Return the installed *gtopt-scripts* package version, or ``'dev'``."""
    try:
        from importlib.metadata import version as _pkg_version

        return _pkg_version(_PACKAGE_NAME)
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        return "dev"


def add_version_argument(parser: argparse.ArgumentParser) -> None:
    """Add ``-V / --version`` to *parser*."""
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {get_version()}",
    )


# ---------------------------------------------------------------------------
# Log level
# ---------------------------------------------------------------------------

LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")


def add_log_level_argument(
    parser: argparse.ArgumentParser,
    *,
    default: str = "INFO",
) -> None:
    """Add ``-l / --log-level`` with standard choices.

    Also adds a hidden ``-v / --verbose`` alias that maps to DEBUG for
    backward compatibility with tools that previously used ``-v``.
    """
    parser.add_argument(
        "-l",
        "--log-level",
        choices=LOG_LEVELS,
        default=default,
        help=f"logging verbosity (default: {default})",
        metavar="LEVEL",
    )


# ---------------------------------------------------------------------------
# Color
# ---------------------------------------------------------------------------


def add_color_argument(parser: argparse.ArgumentParser) -> None:
    """Add ``--no-color`` to *parser*."""
    parser.add_argument(
        "--no-color",
        action="store_true",
        default=False,
        help="disable coloured output",
    )


def resolve_color(args: argparse.Namespace) -> bool:
    """Return ``True`` if colour output should be enabled.

    Checks (in order): ``args.no_color``, the ``NO_COLOR`` environment
    variable, and whether stdout is a terminal.
    """
    if getattr(args, "no_color", False):
        return False
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------


def configure_logging(
    args: argparse.Namespace,
    *,
    fmt: str | None = None,
) -> None:
    """Configure :mod:`logging` from *args.log_level*.

    If *args* has a ``verbose`` attribute (True) and no explicit
    ``log_level`` override, verbose maps to DEBUG.
    """
    level_str: str = getattr(args, "log_level", "INFO")
    # Backward compat: --verbose overrides default log_level to DEBUG
    if getattr(args, "verbose", False) and level_str == "INFO":
        level_str = "DEBUG"

    level = getattr(logging, level_str.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format=fmt or "%(levelname)s: %(message)s",
    )


# ---------------------------------------------------------------------------
# Convenience: add all common arguments at once
# ---------------------------------------------------------------------------


def add_common_arguments(
    parser: argparse.ArgumentParser,
    *,
    default_log_level: str = "INFO",
) -> None:
    """Add version, log-level, and color arguments to *parser*.

    This is a convenience wrapper that calls :func:`add_version_argument`,
    :func:`add_log_level_argument`, and :func:`add_color_argument`.
    """
    add_version_argument(parser)
    add_log_level_argument(parser, default=default_log_level)
    add_color_argument(parser)
