# SPDX-License-Identifier: BSD-3-Clause
"""Bus catalogue — stable per-bus metadata for the Chilean SEN.

A small persistent table indexed by ``bus_id`` (CEN's ``id_info``)
with the metadata needed for bus selection in
``tools/cen_marginal_period.py``.

Columns
-------

* ``bus_id``     — int, CEN's stable identifier (``id_info``).
* ``bar_transf`` — 18-char canonical key (``CRUCERO_______220``).
* ``barra_info`` — human name with voltage (``BA S/E CRUCERO 220KV BP1``).
* ``voltage_kv`` — int, parsed from the trailing 3-digit ``bar_transf``
                   suffix (``066`` / ``110`` / ``154`` / ``220`` / ``500``).
* ``region``     — ``"north" | "centre" | "south"`` heuristic from the
                   substation name.
* ``last_seen``  — ISO date string of the latest cmg-real fetch where
                   this bus appeared.

Discovery
---------

Two paths:

1. **From cache** — scan all cached ``cmg_real`` parquet files in
   ``~/.cache/gtopt/cen2gtopt/sip/costo_marginal_real__v4__findByDate/``
   and collect distinct (``id_info``, ``barra_transf``, ``barra_info``)
   tuples. Fast (seconds), but only contains buses we've already
   fetched (typically the v3 ``REF_BUSES`` set).

2. **From API** — issue a paginated ``cmg_real`` call with no
   ``bar_transf`` filter for one date. Slow (~10-30 min for one day,
   190 buses × 96 quarters = 18 240 rows ÷ 10 rows/page = 1 824 pages).
   Run once per quarter to keep the catalogue fresh.

Either path writes to ``~/.cache/gtopt/cen2gtopt/known_buses.parquet``.
"""

from __future__ import annotations

import logging
import re
from pathlib import Path

import pandas as pd

from cen2gtopt._cached_extractor import cache_root_default, fetch_by_name

_LOG = logging.getLogger("cen2gtopt.bus_catalogue")

#: Default on-disk catalogue path.
DEFAULT_CATALOGUE_PATH = cache_root_default() / "known_buses.parquet"

#: Voltage suffix is the last 3 digits of bar_transf.
_VOLT_RE = re.compile(r"_(\d{3})$")

#: Substring → region heuristic.  Order matters: more-specific first.
_REGION_HINTS: list[tuple[str, str]] = [
    # North (SING / north interconnect)
    ("CRUCERO", "north"),
    ("ESPERANZA", "north"),
    ("ATACAMA", "north"),
    ("ANTOFAG", "north"),
    ("LAGUNAS", "north"),
    ("KIMAL", "north"),
    ("CALAMA", "north"),
    ("CHACAYA", "north"),
    ("ENCUENTRO", "north"),
    ("D.ALMAGRO", "north-centre"),
    ("DIEGO", "north-centre"),
    ("GUACOLDA", "north-centre"),
    ("MAITENCILLO", "north-centre"),
    ("CARDONES", "north-centre"),
    ("PAN_AZUCAR", "north-centre"),
    ("PAN AZUCAR", "north-centre"),
    # Central / Santiago
    ("ALTO_JAHUEL", "centre"),
    ("A.JAHUEL", "centre"),
    ("A.MELIP", "centre"),
    ("ALTO MELIP", "centre"),
    ("MELIPILLA", "centre"),
    ("L.ALMENDR", "centre"),
    ("LOS ALMENDR", "centre"),
    ("EL_SALTO", "centre"),
    ("EL SALTO", "centre"),
    ("CERRO_NAVIA", "centre"),
    ("CERRO NAVIA", "centre"),
    ("POLPAICO", "centre"),
    ("RANCAGUA", "centre"),
    ("LAGUNILLAS", "centre"),
    ("ELMAITEN", "centre"),
    ("EL MAITEN", "centre"),
    ("MAITEN", "centre"),
    # South-central / Bío-Bío
    ("CHARRUA", "south-centre"),
    ("CHARRÚA", "south-centre"),
    ("CONCEPCION", "south-centre"),
    ("CONCEPCIÓN", "south-centre"),
    ("ANCOA", "south-centre"),
    ("ITAHUE", "south-centre"),
    ("PEHUENCHE", "south-centre"),
    ("COLBUN", "south-centre"),
    ("ANTUCO", "south-centre"),
    ("RALCO", "south-centre"),
    ("PANGUE", "south-centre"),
    ("ANGOSTURA", "south-centre"),
    # South (Los Lagos / Aysén)
    ("PUERTO_MONTT", "south"),
    ("PUERTO MONTT", "south"),
    ("VALDIVIA", "south"),
    ("PILMAIQUEN", "south"),
    ("CANUTILLAR", "south"),
    ("CHILOE", "south"),
    ("MELIPULLI", "south"),
    ("BARRO_BLANCO", "south"),
]


def parse_voltage(bar_transf: str | None) -> int:
    """Return voltage in kV parsed from a ``bar_transf`` suffix.

    Returns 0 when no 3-digit suffix is present.

    >>> parse_voltage("CRUCERO_______220")
    220
    >>> parse_voltage("D.ALMAGRO_____110")
    110
    >>> parse_voltage("ELMAITEN______066")
    66
    """
    if not bar_transf:
        return 0
    m = _VOLT_RE.search(str(bar_transf))
    if not m:
        return 0
    try:
        return int(m.group(1))
    except (TypeError, ValueError):
        return 0


def infer_region(barra_info: str | None, bar_transf: str | None = None) -> str:
    """Heuristic geographic region from the substation name.

    Returns one of ``"north" | "north-centre" | "centre" |
    "south-centre" | "south" | "unknown"``.
    """
    if not barra_info and not bar_transf:
        return "unknown"
    haystack = (str(barra_info or "") + " " + str(bar_transf or "")).upper()
    for needle, region in _REGION_HINTS:
        if needle.upper() in haystack:
            return region
    return "unknown"


# ---------------------------------------------------------------------------
# Discovery from cached cmg_real parquets
# ---------------------------------------------------------------------------


def discover_buses_from_cache(
    cache_root: Path | None = None,
) -> pd.DataFrame:
    """Build a catalogue from the *existing* cached cmg_real parquets.

    Fast path — works without any new HTTP fetch but only contains
    buses we have data for.  Returns an empty DataFrame when the
    cache has no cmg_real files.
    """
    cache_root = cache_root or cache_root_default()
    sip_dir = cache_root / "sip" / "costo_marginal_real__v4__findByDate"
    if not sip_dir.exists():
        return pd.DataFrame()
    parts: list[pd.DataFrame] = []
    for p in sorted(sip_dir.glob("*.parquet")):
        try:
            df = pd.read_parquet(
                p,
                columns=["id_info", "barra_info", "barra_transf", "fecha"],
            )
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
            _LOG.warning("skip %s: %s", p.name, exc)
            continue
        parts.append(df)
    if not parts:
        return pd.DataFrame()
    raw = pd.concat(parts, ignore_index=True)
    raw["id_info"] = pd.to_numeric(raw["id_info"], errors="coerce")
    raw = raw.dropna(subset=["id_info", "barra_transf"])
    raw["id_info"] = raw["id_info"].astype(int)
    raw["fecha_str"] = raw["fecha"].astype(str).str.slice(0, 10)
    grouped = raw.groupby(["id_info", "barra_transf"], as_index=False).agg(
        barra_info=("barra_info", "first"),
        last_seen=("fecha_str", "max"),
    )
    grouped = grouped.rename(
        columns={"id_info": "bus_id", "barra_transf": "bar_transf"}
    )
    grouped["voltage_kv"] = grouped["bar_transf"].map(parse_voltage)
    grouped["region"] = grouped.apply(
        lambda r: infer_region(r["barra_info"], r["bar_transf"]),
        axis=1,
    )
    return (
        grouped[
            ["bus_id", "bar_transf", "barra_info", "voltage_kv", "region", "last_seen"]
        ]
        .sort_values(["region", "voltage_kv", "barra_info"])
        .reset_index(drop=True)
    )


# ---------------------------------------------------------------------------
# Discovery from API (slow, paginated)
# ---------------------------------------------------------------------------


def discover_buses_from_api(
    client,
    *,
    date: str,
    use_online: bool = True,
) -> pd.DataFrame:
    """Issue a paginated CMG fetch with no ``bar_transf`` filter for
    one date.  Returns the per-bus catalogue.

    When ``use_online=True`` (default) — uses ``cmg-online`` which is
    far faster than ``cmg-real``: ~1 375 buses discovered in 5-6 s
    via pages of 5 000 rows.  Replaces the older settled-CMG path.
    Set ``use_online=False`` to use ``cmg-real`` (rate-limited;
    typically times out before completing).
    """
    endpoint = "cmg_online" if use_online else "cmg_real"
    raw = fetch_by_name(client, endpoint, start=date)
    if raw.empty:
        return pd.DataFrame()
    raw = raw[["id_info", "barra_info", "barra_transf", "fecha"]].copy()
    raw["id_info"] = pd.to_numeric(raw["id_info"], errors="coerce")
    raw = raw.dropna(subset=["id_info", "barra_transf"])
    raw["id_info"] = raw["id_info"].astype(int)
    grouped = raw.groupby(["id_info", "barra_transf"], as_index=False).agg(
        barra_info=("barra_info", "first")
    )
    grouped = grouped.rename(
        columns={"id_info": "bus_id", "barra_transf": "bar_transf"}
    )
    grouped["voltage_kv"] = grouped["bar_transf"].map(parse_voltage)
    grouped["region"] = grouped.apply(
        lambda r: infer_region(r["barra_info"], r["bar_transf"]), axis=1
    )
    grouped["last_seen"] = date
    return (
        grouped[
            ["bus_id", "bar_transf", "barra_info", "voltage_kv", "region", "last_seen"]
        ]
        .sort_values(["region", "voltage_kv", "barra_info"])
        .reset_index(drop=True)
    )


# ---------------------------------------------------------------------------
# Persistence
# ---------------------------------------------------------------------------


def load_catalogue(path: Path | None = None) -> pd.DataFrame:
    """Load the on-disk catalogue.  Returns empty DataFrame when missing."""
    path = path or DEFAULT_CATALOGUE_PATH
    if not path.exists():
        return pd.DataFrame()
    return pd.read_parquet(path)


def save_catalogue(df: pd.DataFrame, path: Path | None = None) -> Path:
    """Persist a catalogue, creating parent dirs as needed.  Returns
    the written path."""
    path = path or DEFAULT_CATALOGUE_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_parquet(path, index=False)
    return path


def merge_catalogues(
    a: pd.DataFrame,
    b: pd.DataFrame,
) -> pd.DataFrame:
    """Merge two catalogues, preferring the newer ``last_seen`` per bus."""
    if a.empty:
        return b.copy() if not b.empty else pd.DataFrame()
    if b.empty:
        return a.copy()
    cat = pd.concat([a, b], ignore_index=True)
    cat = cat.sort_values("last_seen").drop_duplicates(
        subset=["bus_id"],
        keep="last",
    )
    return cat.sort_values(["region", "voltage_kv", "barra_info"]).reset_index(
        drop=True
    )


# ---------------------------------------------------------------------------
# Selection
# ---------------------------------------------------------------------------


def filter_catalogue(
    catalogue: pd.DataFrame,
    *,
    bus_ids: set[int] | None = None,
    name_regex: str | None = None,
    region: str | None = None,
    min_voltage: int | None = None,
) -> pd.DataFrame:
    """Apply the AND of all non-None selectors to the catalogue."""
    if catalogue.empty:
        return catalogue
    out = catalogue.copy()
    if bus_ids is not None:
        out = out[out["bus_id"].isin(bus_ids)]
    if name_regex:
        out = out[
            out["barra_info"]
            .astype(str)
            .str.contains(name_regex, case=False, regex=True, na=False)
        ]
    if region:
        out = out[out["region"] == region]
    if min_voltage is not None:
        out = out[out["voltage_kv"] >= int(min_voltage)]
    return out.reset_index(drop=True)


def to_ref_buses(catalogue: pd.DataFrame) -> list[tuple[str, int, str]]:
    """Convert a catalogue (or filtered subset) into the
    ``REF_BUSES``-style tuple list expected by
    ``compute_marginal_units_for_day``.

    Each tuple is ``(bar_transf, bus_id, barra_info)``.
    """
    if catalogue.empty:
        return []
    return [
        (str(r["bar_transf"]), int(r["bus_id"]), str(r["barra_info"]))
        for _, r in catalogue.iterrows()
    ]


__all__ = [
    "DEFAULT_CATALOGUE_PATH",
    "parse_voltage",
    "infer_region",
    "discover_buses_from_cache",
    "discover_buses_from_api",
    "load_catalogue",
    "save_catalogue",
    "merge_catalogues",
    "filter_catalogue",
    "to_ref_buses",
]
