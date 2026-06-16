# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plexos2gtopt._defaults share-file resolution (portability).

Regression guard for the hardcoded ``/home/marce/git/gtopt/share/gtopt``
first candidate that used to live in ``_SHARE_CANDIDATES``: it resolved
only on one developer machine and silently returned ``None`` (→ a
downstream FileNotFoundError, or a fall back to the IPCC defaults) on
every other checkout and CI runner.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt._defaults import (
    _resolve_share_file,
    resolve_default_emissions_file,
)


def test_resolve_default_emissions_file_is_checkout_relative() -> None:
    """The bundled CEN-Chile emissions file resolves from *this* checkout.

    Asserting the resolved path lives under this checkout's root (rather
    than absence of any specific developer path) is what fails for a
    hardcoded ``/home/<user>/...`` literal on a CI runner rooted elsewhere.
    """
    path = resolve_default_emissions_file()
    assert path is not None, "cen_chile.json must resolve from the checkout"
    assert path.is_file()
    assert path.name == "cen_chile.json"
    # Checkout-relative, not a fixed literal: the resolved path must live
    # under the same repo root as this test file (parents[3]).
    repo_root = Path(__file__).resolve().parents[3]
    assert path.resolve().is_relative_to(repo_root), (
        f"{path} must resolve under the checkout root {repo_root}"
    )


def test_resolve_share_file_missing_returns_none() -> None:
    """A non-existent share file resolves to None, not a stale path."""
    assert _resolve_share_file("definitely", "missing", "nope.json") is None
