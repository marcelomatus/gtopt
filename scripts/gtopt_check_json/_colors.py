# SPDX-License-Identifier: BSD-3-Clause
"""ANSI colour helpers for gtopt_check_json terminal output.

This module re-exports colour constants and helpers from the shared
:mod:`gtopt_check_json._terminal` module for backward compatibility.
"""

from gtopt_check_json._terminal import (
    BOLD,
    CYAN,
    GREEN,
    RED,
    RESET,
    YELLOW,
    section_header,
)

# Module-level flag; set once by the CLI before any output is produced.
USE_COLOR = True


def c(code: str, text: str) -> str:
    """Wrap *text* in the ANSI *code* when colour is enabled."""
    return f"{code}{text}{RESET}" if USE_COLOR else text


def header(title: str) -> str:
    """Return a styled section header string."""
    return section_header(title, colr=USE_COLOR)
