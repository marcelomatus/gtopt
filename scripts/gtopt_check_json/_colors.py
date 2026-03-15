# SPDX-License-Identifier: BSD-3-Clause
"""ANSI colour helpers for gtopt_check_json terminal output."""

BOLD = "\033[1m"
RED = "\033[31m"
YELLOW = "\033[33m"
GREEN = "\033[32m"
CYAN = "\033[36m"
RESET = "\033[0m"

# Module-level flag; set once by the CLI before any output is produced.
USE_COLOR = True


def c(code: str, text: str) -> str:
    """Wrap *text* in the ANSI *code* when colour is enabled."""
    return f"{code}{text}{RESET}" if USE_COLOR else text


def header(title: str) -> str:
    """Return a boxed section header string."""
    line = "─" * (len(title) + 4)
    return (
        f"\n{c(BOLD, f'┌{line}┐')}"
        f"\n{c(BOLD, f'│  {title}  │')}"
        f"\n{c(BOLD, f'└{line}┘')}"
    )
