# SPDX-License-Identifier: BSD-3-Clause
"""plexos2gtopt converter defaults — share-dir resolution.

Helpers that locate bundled CEN-Chile-specific reference files under
``share/gtopt/...``.  Kept separate from :mod:`plexos2gtopt.parsers` so
the resolution logic has one home and isn't duplicated across
``gtopt_writer`` (cogen CSV) and ``plexos2gtopt`` (emissions JSON).
"""

from __future__ import annotations

from pathlib import Path

# Search order for ``share/gtopt/`` files.  ``Path(__file__).parents[2] /
# "share"`` is the checkout-relative path; it works for editor-style runs
# from arbitrary working directories and under pytest's site-packages
# hardlink, because ``__file__`` resolves back into the source tree.  Kept
# as a tuple so additional install-layout candidates can be appended.
_SHARE_CANDIDATES: tuple[Path, ...] = (
    Path(__file__).resolve().parents[2] / "share" / "gtopt",
)


def _resolve_share_file(*relparts: str) -> Path | None:
    """Return the first existing ``share/gtopt/<relparts...>`` file."""
    for base in _SHARE_CANDIDATES:
        path = base.joinpath(*relparts)
        if path.is_file():
            return path
    return None


def resolve_default_emissions_file() -> Path | None:
    """Return the path to ``share/gtopt/emissions/cen_chile.json``.

    Used by :func:`plexos2gtopt.plexos2gtopt.plexos2gtopt` when the
    operator did NOT pass an explicit ``--emissions-file``: plexos2gtopt
    is CEN-Chile-shaped (PLEXOS DBs in this codebase come from CEN's
    PCP archive), so the CEN-Chile defaults file is the right default.

    Falls back to ``None`` when the file is missing (sandboxed installs
    without ``share/``); ``apply_emission_defaults_from_file(None)`` then
    resolves to the bundled IPCC-2006 defaults at
    ``gtopt_shared/data/ipcc_emission_factors.json`` — the legacy
    pre-2026-06 behavior.
    """
    return _resolve_share_file("emissions", "cen_chile.json")
