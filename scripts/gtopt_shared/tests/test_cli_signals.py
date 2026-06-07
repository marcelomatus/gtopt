# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.cli_signals`."""

from __future__ import annotations

import signal
from typing import Callable
from unittest.mock import patch

import pytest

from gtopt_shared.cli_signals import (
    install_termination_handlers,
    signal_handler,
)


def test_signal_handler_prints_signal_name_and_exits_zero(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """SIGINT → ``Caught signal <NAME>. Exiting...`` to stdout + ``sys.exit(0)``."""
    with pytest.raises(SystemExit) as exc:
        signal_handler(signal.SIGINT, None)
    assert exc.value.code == 0
    out = capsys.readouterr().out
    assert "Caught signal" in out
    assert "Exiting" in out


def test_install_termination_handlers_registers_both_by_default() -> None:
    """``install_termination_handlers()`` wires SIGINT + SIGTERM."""
    captured: dict[int, Callable] = {}

    def _capture(sig: int, handler: Callable) -> None:
        captured[sig] = handler

    with patch("signal.signal", side_effect=_capture):
        install_termination_handlers()

    assert signal.SIGINT in captured
    assert signal.SIGTERM in captured
    assert captured[signal.SIGINT] is signal_handler
    assert captured[signal.SIGTERM] is signal_handler


def test_install_termination_handlers_can_skip_each_signal() -> None:
    """``sigint=False`` / ``sigterm=False`` skip that specific registration."""
    captured: dict[int, Callable] = {}

    def _capture(sig: int, handler: Callable) -> None:
        captured[sig] = handler

    with patch("signal.signal", side_effect=_capture):
        install_termination_handlers(sigterm=False)
    assert signal.SIGINT in captured
    assert signal.SIGTERM not in captured

    captured.clear()
    with patch("signal.signal", side_effect=_capture):
        install_termination_handlers(sigint=False)
    assert signal.SIGINT not in captured
    assert signal.SIGTERM in captured


@pytest.mark.parametrize(
    "module_path",
    [
        "plp2gtopt.main",
        "plexos2gtopt.main",
        "sddp2gtopt.main",
    ],
)
def test_converter_main_uses_shared_handler(module_path: str) -> None:
    """Each migrated converter's ``main`` module exposes ``signal_handler``
    that IS the canonical one from ``gtopt_shared.cli_signals``.

    Identity check (``is``) — anything else means the converter still has
    its own copy and the lift hasn't actually deduplicated the code.
    """
    # pylint: disable=import-outside-toplevel
    import importlib

    mod = importlib.import_module(module_path)
    # All three converters re-export the handler under one of two names
    # historical to each: plexos/sddp use ``_signal_handler`` (private),
    # plp uses ``signal_handler`` (public).  Accept either.
    handler = getattr(mod, "_signal_handler", None) or getattr(
        mod, "signal_handler", None
    )
    assert handler is signal_handler, (
        f"{module_path} signal handler is a different object than "
        "gtopt_shared.cli_signals.signal_handler"
    )
