# SPDX-License-Identifier: BSD-3-Clause
"""Normalisation helpers — name/UID/timezone canonicalisation."""

from __future__ import annotations

import hashlib
import unicodedata
from datetime import datetime
from typing import Optional


def normalize_name(raw: str) -> str:
    """NFD-decompose + strip combining marks + lowercase + replace
    non-alphanumeric with underscore.

    The NFD decomposition step is what turns ``Charrúa`` into
    ``Charrua`` (accent stripped but the base letter preserved).
    A naive NFC + ascii-encode would drop the accented character
    entirely.

    Used for fuzzy matching of CEN catalogue names (which carry
    accents and ``N°`` glyphs) against the gtopt-side names.
    """
    nfd = unicodedata.normalize("NFD", raw or "")
    no_marks = "".join(ch for ch in nfd if not unicodedata.combining(ch))
    ascii_only = no_marks.encode("ascii", errors="ignore").decode()
    lowered = ascii_only.lower()
    cleaned = "".join(ch if ch.isalnum() else "_" for ch in lowered)
    return "_".join(filter(None, cleaned.split("_")))


def stable_uid(raw_name: str) -> int:
    """Deterministic 31-bit UID from a NFC-folded name.

    Used as a fallback when the SIP catalogue UID is not available.
    Persisted to manifest.json so subsequent runs reuse the value.
    """
    norm = normalize_name(raw_name).encode("utf-8")
    digest = hashlib.sha256(norm).digest()
    # Use the first 4 bytes as a 31-bit integer (truncate the sign bit).
    return int.from_bytes(digest[:4], "big") & 0x7FFFFFFF


def fold_chile_continental(dt_str: str) -> Optional[datetime]:
    """Parse a CEN-format datetime string assuming Chile/Continental
    civil time. CEN omits a tz suffix; we attach it here.

    Accepts either ``YYYY-MM-DD HH:MM:SS`` or ``YYYY-MM-DDTHH:MM:SS``.
    Returns a naive datetime in Chile/Continental — the caller
    should then localise/convert via ``zoneinfo.ZoneInfo``.
    """
    if not dt_str:
        return None
    s = dt_str.strip().replace("T", " ")
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            continue
    return None
