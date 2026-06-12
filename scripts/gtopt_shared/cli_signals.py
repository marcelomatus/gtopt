# SPDX-License-Identifier: BSD-3-Clause
"""Shared SIGINT / SIGTERM handler for converter CLIs.

Three converters (plp2gtopt, plexos2gtopt, sddp2gtopt) carried
identical bodies for the termination handler plus the same
``signal.signal(SIGINT, ...)`` / ``signal.signal(SIGTERM, ...)``
registration boilerplate in their ``main()``.  Centralised here so
the message format is consistent across the toolchain and a future
fix (e.g. emit to stderr instead of stdout, or honour
``NO_COLOR=1``) lands in one place.

Usage::

    from gtopt_shared.cli_signals import install_termination_handlers

    def main(argv: list[str] | None = None) -> int:
        install_termination_handlers()
        parser = make_parser()
        ...
"""

from __future__ import annotations

import signal
import sys


def signal_handler(sig: int, _frame: object) -> None:
    """Terminate cleanly on SIGINT / SIGTERM.

    Prints a one-line ``Caught signal <NAME>. Exiting...`` to stdout
    (matching the historical converter behaviour) and exits with code
    0 — the user-initiated termination is not an error.
    """
    print(f"\nCaught signal {signal.strsignal(sig)}. Exiting...")
    sys.exit(0)


def install_termination_handlers(*, sigint: bool = True, sigterm: bool = True) -> None:
    """Register :func:`signal_handler` for SIGINT and / or SIGTERM.

    Both flags default to ``True`` — every converter wants both.  The
    individual knobs exist so a future CLI that needs to handle one
    signal differently (e.g. SIGTERM for a graceful "drain in flight,
    then exit") can register only the other.
    """
    if sigint:
        signal.signal(signal.SIGINT, signal_handler)
    if sigterm:
        signal.signal(signal.SIGTERM, signal_handler)
