# SPDX-License-Identifier: BSD-3-Clause
"""ANSI colour helpers for gtopt_check_lp terminal output."""

_BOLD = "\033[1m"
_RED = "\033[31m"
_YELLOW = "\033[33m"
_GREEN = "\033[32m"
_CYAN = "\033[36m"
_RESET = "\033[0m"

# Public aliases (no underscore) for use outside this module.
BOLD   = _BOLD
RED    = _RED
YELLOW = _YELLOW
GREEN  = _GREEN
CYAN   = _CYAN
RESET  = _RESET

# Module-level flag; set once by the CLI before any output is produced.
USE_COLOR = True


def c(code: str, text: str) -> str:
    """Wrap *text* in the ANSI *code* when colour is enabled."""
    return f"{code}{text}{_RESET}" if USE_COLOR else text


def header(title: str) -> str:
    """Return a boxed section header string."""
    line = "─" * (len(title) + 4)
    return (
        f"\n{c(_BOLD, f'┌{line}┐')}"
        f"\n{c(_BOLD, f'│  {title}  │')}"
        f"\n{c(_BOLD, f'└{line}┘')}"
    )
