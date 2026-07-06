# SPDX-License-Identifier: BSD-3-Clause
"""pytest configuration for the plexos2gtopt test suite.

Isolate ``os.environ`` around every test.  ``ConversionOptions.install_env``
(``plexos2gtopt.py`` calls it with no argument) writes the converter's
``GTOPT_*`` options — e.g. ``GTOPT_NSEG_LOSSES`` (default 6) — directly into
the process environment with no cleanup.  On a shared pytest-xdist worker that
leaks into later tests: ``test_writer.py::
test_build_line_array_dlr_emits_matrix_and_loss_mode`` read ``loss_segments=6``
(the adaptive default the leaked env var selects) instead of the uniform
default 4 whenever it happened to run after a test that invoked the converter.
Snapshotting and restoring the environment per test contains every such leak
at its source (not just the loss vars), so tests are order-independent under
parallel execution.
"""

from __future__ import annotations

import os

import pytest


@pytest.fixture(autouse=True)
def _isolate_environ():
    """Restore ``os.environ`` to its pre-test state after every test."""
    saved = dict(os.environ)
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)
