# SPDX-License-Identifier: BSD-3-Clause
"""Parsers for the four CEN public-website CSV exports.

Each parser accepts a path to a downloaded CSV (or fixture) and
returns a long-form pandas frame in the canonical-feed shape. The
URL fetching layer is in ``_cache.py`` / ``main.py``; these parsers
operate on bytes already on disk so they are fully testable without
network.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import pandas as pd

from cen2gtopt._normalize import stable_uid


# Column-name heuristics. CEN sometimes renames columns; we accept
# any of the known variants.
_DATE_COL_NAMES = ("Fecha", "fecha", "Dia")
_HOUR_COL_NAMES = ("Hora", "hora")
_BUS_COL_NAMES = ("Barra", "barra", "Subestacion")
_UNIT_COL_NAMES = ("Central", "central", "Unidad")
_CMG_VALUE_NAMES = ("Costo Marginal (USD/MWh)", "Costo Marginal", "CMg")
_CMG_ONLINE_VALUE_NAMES = (
    "Costo Marginal Online (USD/MWh)",
    "Costo Marginal Online",
)
_GEN_VALUE_NAMES = ("Generacion (MWh)", "Generación (MWh)", "MWh")
_DEM_VALUE_NAMES = ("Demanda (MWh)", "Demanda", "MWh")


def parse_costo_marginal_real(path: Path) -> pd.DataFrame:
    """Parse Costo Marginal Real CSV → long-form (date_utc, hour,
    bus_uid, lmp)."""
    return _parse_bus_value(path, _CMG_VALUE_NAMES, value_col="lmp")


def parse_costo_marginal_online(path: Path) -> pd.DataFrame:
    return _parse_bus_value(path, _CMG_ONLINE_VALUE_NAMES, value_col="lmp")


def parse_demanda_real(path: Path) -> pd.DataFrame:
    return _parse_bus_value(path, _DEM_VALUE_NAMES, value_col="load")


def parse_generacion_real(path: Path) -> pd.DataFrame:
    """Parse Generación Real CSV → long-form (date_utc, hour,
    gen_uid, dispatch)."""
    df = _read_csv(path)
    date_col = _first_present(df, _DATE_COL_NAMES)
    hour_col = _first_present(df, _HOUR_COL_NAMES)
    unit_col = _first_present(df, _UNIT_COL_NAMES)
    val_col = _first_present(df, _GEN_VALUE_NAMES)
    if not all((date_col, hour_col, unit_col, val_col)):
        raise ValueError(
            f"generacion_real CSV missing one of the expected columns: "
            f"{_DATE_COL_NAMES} / {_HOUR_COL_NAMES} / {_UNIT_COL_NAMES} / "
            f"{_GEN_VALUE_NAMES} — got {list(df.columns)}"
        )
    out = pd.DataFrame()
    out["date_utc"] = df[date_col].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(df[hour_col], errors="coerce").astype("Int64")
    out["gen_uid"] = df[unit_col].astype(str).map(stable_uid).astype(int)
    out["gen_name"] = df[unit_col].astype(str)
    out["dispatch"] = pd.to_numeric(df[val_col], errors="coerce").astype(float)
    return out.dropna(subset=["hour", "dispatch"]).reset_index(drop=True)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _parse_bus_value(
    path: Path,
    value_names: tuple[str, ...],
    *,
    value_col: str,
) -> pd.DataFrame:
    df = _read_csv(path)
    date_col = _first_present(df, _DATE_COL_NAMES)
    hour_col = _first_present(df, _HOUR_COL_NAMES)
    bus_col = _first_present(df, _BUS_COL_NAMES)
    val_col = _first_present(df, value_names)
    if not all((date_col, hour_col, bus_col, val_col)):
        raise ValueError(
            f"bus-keyed CSV missing one of the expected columns: "
            f"{_DATE_COL_NAMES} / {_HOUR_COL_NAMES} / {_BUS_COL_NAMES} / "
            f"{value_names} — got {list(df.columns)}"
        )
    out = pd.DataFrame()
    out["date_utc"] = df[date_col].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(df[hour_col], errors="coerce").astype("Int64")
    out["bus_uid"] = df[bus_col].astype(str).map(stable_uid).astype(int)
    out["bus_name"] = df[bus_col].astype(str)
    out[value_col] = pd.to_numeric(df[val_col], errors="coerce").astype(float)
    return out.dropna(subset=["hour", value_col]).reset_index(drop=True)


def _read_csv(path: Path) -> pd.DataFrame:
    """Read a CEN CSV — Latin-1 / UTF-8 / UTF-8-BOM tolerant."""
    p = Path(path)
    for enc in ("utf-8-sig", "utf-8", "latin-1"):
        try:
            return pd.read_csv(p, encoding=enc)
        except UnicodeDecodeError:
            continue
    raise ValueError(f"could not decode {p} with utf-8/utf-8-sig/latin-1")


def _first_present(df: pd.DataFrame, candidates: tuple[str, ...]) -> Optional[str]:
    for c in candidates:
        if c in df.columns:
            return c
    return None
