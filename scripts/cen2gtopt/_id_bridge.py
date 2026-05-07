# SPDX-License-Identifier: BSD-3-Clause
# pylint: disable=line-too-long  # docstring tables align CEN endpoint names
"""Cross-API name bridge to ``id_central`` (canonical join key).

Different CEN endpoints use different names for the same plant:

  endpoint                                      name field(s)             example
  --------------------------------------------- ----------------------    ----------
  /unidades-generadoras/v4                      central, unidad_nombre,   "TER NEHUENCO 9B" / "TER NEHUENCO 9B" / "CE03U01G0009"
                                                unidad_nemotecnico
  /generacion-real/v3                           central                   "TER NEHUENCO 9B"
  /instrucciones-operacionales-cmg/v4           central, configuracion    "ATACAMA-1" / "ATACAMA_GN"
  SCVIC /cvrpt/show_cv_rpt                      powerStationName,         "NEHUENCO" / "NEHUENCO_9B"
                                                powerPlantName,
                                                configurationName

This module builds an **alias index**:

    {normalised_alias → id_central}

so any name from any source can be resolved to the integer
``id_central``. Aliases come from ``/unidades-generadoras/v4`` (which
exposes all three name forms with the canonical id) and from
``/centrales/v4`` if available.

Once the bridge is built, all downstream joins (SCVIC CV, forced/RO,
pmin, dispatch) happen on ``id_central`` — no fuzzy matching.
"""

from __future__ import annotations

import unicodedata

import pandas as pd


def norm(s: object) -> str:
    """NFD-fold + ASCII-fold + lowercase + alnum-only."""
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return ""
    nfd = unicodedata.normalize("NFD", str(s))
    no_marks = "".join(c for c in nfd if not unicodedata.combining(c))
    ascii_only = no_marks.encode("ascii", errors="ignore").decode()
    return "".join(ch for ch in ascii_only.lower() if ch.isalnum())


def _aliases(name: str) -> list[str]:
    """Produce up to ~6 normalised alias keys for one name.

    Strips common Chilean SEN prefixes/suffixes so all forms of the
    same plant collapse to the same key.
    """
    out: set[str] = set()
    base = norm(name)
    if not base or len(base) < 2:
        return []
    out.add(base)

    # Strip TERMOELECTRICA / TERMICA / TER / CENTRAL prefix
    for pfx in (
        "termoelectrica",
        "termoelec",
        "termica",
        "ter",
        "central",
        "ct",
        "cc",
        "ce",
    ):
        if base.startswith(pfx) and len(base) > len(pfx) + 1:
            out.add(base[len(pfx) :])
            break

    # Strip trailing unit identifiers — applied iteratively so e.g.
    # "cmpclajabl1" → strip "bl1" → "cmpclaja" matches.  Also include
    # block suffixes (BL1-BL9), turbine subtype (TG/TV), and digit
    # tails. Apply in two passes so multi-suffix names converge.
    SUFFIXES = (
        "ccgt",
        "ccgttv",
        "ccgttg",
        "u1",
        "u2",
        "u3",
        "u4",
        "u5",
        "u6",
        "u7",
        "u8",
        "u9",
        "bl1",
        "bl2",
        "bl3",
        "bl4",
        "bl5",
        "bl6",
        "bl7",
        "bl8",
        "bl9",
        "tv",
        "tg",
        "9b",
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
    )
    for _ in range(2):
        for a in list(out):
            for suf in SUFFIXES:
                if a.endswith(suf) and len(a) > len(suf) + 1:
                    out.add(a[: -len(suf)])

    # Add TER+name variant for reverse-direction matching ("ATACAMA"
    # ↔ "TER ATACAMA").
    for a in list(out):
        if not a.startswith("ter") and len(a) >= 3:
            out.add("ter" + a)

    return [a for a in out if a and len(a) >= 2]


def build_alias_index(
    units_df: pd.DataFrame,
    centrales_df: pd.DataFrame | None = None,
) -> dict[str, int]:
    """Build a ``{alias → id_central}`` map from the canonical
    catalogues.

    On collisions (one alias maps to multiple ``id_central``), the
    LOWEST id_central wins (deterministic).
    """
    out: dict[str, int] = {}

    def _add(alias: str, ic: int) -> None:
        if not alias:
            return
        existing = out.get(alias)
        if existing is None or ic < existing:
            out[alias] = ic

    if not units_df.empty and "id_central" in units_df.columns:
        for _, row in units_df.iterrows():
            ic_raw = row.get("id_central")
            try:
                ic = int(ic_raw) if pd.notna(ic_raw) else None
            except (TypeError, ValueError):
                ic = None
            if ic is None:
                continue
            for src in (
                row.get("central"),
                row.get("unidad_nombre"),
                row.get("unidad_nemotecnico"),
            ):
                for a in _aliases(str(src) if src is not None else ""):
                    _add(a, ic)

    if centrales_df is not None and not centrales_df.empty:
        if "id_central" in centrales_df.columns:
            for _, row in centrales_df.iterrows():
                ic_raw = row.get("id_central")
                try:
                    ic = int(ic_raw) if pd.notna(ic_raw) else None
                except (TypeError, ValueError):
                    ic = None
                if ic is None:
                    continue
                for src in (row.get("central"), row.get("instalacion")):
                    for a in _aliases(str(src) if src is not None else ""):
                        _add(a, ic)

    return out


def resolve(
    name: str | None,
    alias_index: dict[str, int],
    *fallbacks: str | None,
) -> int | None:
    """Resolve ``name`` to ``id_central`` via the alias index.

    Tries the primary name first; on miss, tries each fallback
    (typically the unit name, configuration name, etc.).
    """
    candidates = (name, *fallbacks)
    for cand in candidates:
        if not cand:
            continue
        for a in _aliases(str(cand)):
            ic = alias_index.get(a)
            if ic is not None:
                return ic
    return None


def index_stats(alias_index: dict[str, int]) -> dict[str, int]:
    """Return summary stats for the alias index — useful for logging."""
    distinct_centrales = len(set(alias_index.values()))
    return {
        "alias_keys": len(alias_index),
        "distinct_id_centrales": distinct_centrales,
    }


__all__ = [
    "norm",
    "build_alias_index",
    "resolve",
    "index_stats",
]
