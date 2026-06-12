# SPDX-License-Identifier: BSD-3-Clause
"""On-disk cache for CEN CSV downloads.

Layout: ``<cache>/<endpoint>/<date>.csv.zst`` keyed by endpoint name
and date range. ``--no-cache`` bypasses; cache hit reuses without
network. The cache is purely a reliability/cost optimisation —
nothing relies on its presence.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Optional


_DEFAULT_CACHE_ROOT = Path.home() / ".cache" / "gtopt" / "cen"


def cache_path(
    endpoint: str,
    *,
    start: str,
    end: str,
    cache_root: Optional[Path] = None,
) -> Path:
    """Compute the cache path for one endpoint × date-range pull."""
    root = Path(cache_root) if cache_root is not None else _DEFAULT_CACHE_ROOT
    key = hashlib.sha256(f"{start}|{end}".encode("utf-8")).hexdigest()[:16]
    return root / endpoint / f"{start}_{end}_{key}.csv"


def cache_dir(cache_root: Optional[Path] = None) -> Path:
    return Path(cache_root) if cache_root is not None else _DEFAULT_CACHE_ROOT
