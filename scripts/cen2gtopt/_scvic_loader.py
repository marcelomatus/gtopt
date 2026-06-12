# SPDX-License-Identifier: BSD-3-Clause
"""SCVIC ``Consolidado Mensual`` Excel loader.

The Coordinador Eléctrico Nacional discontinued the SIP REST endpoint
``/costo-combustible/v3/findAll`` on 2024-12-31. Per-unit declared
fuel costs (USD/fuel-unit, declared by each ``Coordinado`` under DS
130 / Norma Técnica) now live in the SCVIC platform at
``https://costosvariables.coordinador.cl/`` — but its REST API is
gated by NetIQ SSO (Coordinado-only).

The same data is published — at monthly granularity, aggregated — as
a public Excel report on the Coordinador's web portal:

    https://www.coordinador.cl/mercados/documentos/
        costos-variables-de-generacion-y-stock-de-combustible/
        costos-variables-de-generacion/
        consolidado-mensual-de-costos-variables-y-costos-de-partida-y-detencion/

This module reads that Excel file and emits the same long-form
schema the deprecated REST endpoint produced, so the rest of the
pipeline (``compose_declared_mc``) is unchanged.

The actual column names in the SCVIC consolidado are not standardised
across months — accents, casing, and trailing whitespace vary. The
parser uses fuzzy header detection (NFD-fold + lower + alnum-only)
to map whichever variant appears onto the canonical schema.
"""

from __future__ import annotations

import logging
import unicodedata
from pathlib import Path

import pandas as pd

_LOG = logging.getLogger("cen2gtopt.scvic")


# Canonical output schema — same as the deprecated SIP endpoint:
CANONICAL_COLUMNS = (
    "date_utc",  # ISO yyyy-mm-01  (consolidado is monthly)
    "central_name",  # plant name
    "configuracion",  # plant configuration (the join key)
    "empresa",  # owner Coordinado
    "tipo_combustible",  # primary fuel
    "costo_combustible",  # USD per fuel-unit (numeric)
    "fuel_unit",  # ton, m³, MMBTU, …
    "costo_variable_total",  # USD/MWh (CV total)
    "costo_variable_no_combustible",  # USD/MWh (CVNC)
    "costo_partida",  # USD per start
    "costo_detencion",  # USD per stop
)


def _norm_header(s: object) -> str:
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return ""
    nfd = unicodedata.normalize("NFD", str(s))
    no_marks = "".join(c for c in nfd if not unicodedata.combining(c))
    ascii_only = no_marks.encode("ascii", errors="ignore").decode()
    return "".join(ch for ch in ascii_only.lower() if ch.isalnum())


# Header → canonical-column heuristics.  For each canonical column we
# list a tuple of regex-style fragments that, when found inside the
# normalised header, map that column.  Order matters: the first match
# wins, so the more specific patterns must come first (e.g.
# ``costovariableno`` before ``costovariable``).
_HEADER_RULES: list[tuple[str, tuple[str, ...]]] = [
    (
        "costo_variable_no_combustible",
        ("costovariableno", "cvnc", "novariable", "costovarianocombus"),
    ),
    (
        "costo_partida",
        ("costopartida", "costoarranque", "startupcost", "costodepartida"),
    ),
    (
        "costo_detencion",
        ("costodetencion", "costoparada", "shutdowncost", "costodedetencion"),
    ),
    (
        "costo_combustible",
        ("costocombustible", "costodelcombustible", "preciocombustible", "fuelcost"),
    ),
    (
        "costo_variable_total",
        ("costovariabletotal", "cvt", "costovariable", "cvgeneracion"),
    ),
    # Note: "combustible" alone must come AFTER costo_combustible
    # because "costo_combustible" has already claimed any header with
    # the "costocombustible" / "costodelcombustible" prefix and is
    # marked as used. "combustible" then catches a bare "Combustible"
    # header that names the fuel type.
    (
        "tipo_combustible",
        (
            "tipocombustible",
            "combustibleprincipal",
            "tipocombustibleprincipal",
            "fueltype",
            "combustible",
        ),
    ),
    ("fuel_unit", ("unidadcombustible", "unidaddecombustible", "unidadcomb")),
    ("configuracion", ("configuracion",)),
    ("empresa", ("empresa", "coordinado", "propietario", "owner")),
    ("central_name", ("central", "nombrecentral", "plantname", "nombredelacentral")),
    ("date_utc", ("fecha", "mes", "periodo")),
]


def _detect_header_row(df: pd.DataFrame) -> int | None:
    """Find the row that contains the column headers.

    SCVIC Excels often have a logo / title / metadata in the first
    few rows; the actual table header sits at row 4–8. We scan the
    first 30 rows and pick the one with the most cells that match
    any of our header rules.
    """
    best_row = -1
    best_score = 0
    scan_rows = min(30, len(df))
    for i in range(scan_rows):
        row = df.iloc[i].tolist()
        normalised = [_norm_header(c) for c in row]
        score = sum(
            1
            for cell in normalised
            if any(any(pat in cell for pat in pats) for _, pats in _HEADER_RULES)
        )
        if score > best_score:
            best_score = score
            best_row = i
    if best_score < 3:
        return None  # no plausible header row found
    return best_row


def _map_columns(headers: list[str]) -> dict[int, str]:
    """Return a mapping ``{column_index → canonical_name}``.

    Columns that don't match any rule are skipped.
    """
    out: dict[int, str] = {}
    used: set[str] = set()
    for col_idx, raw in enumerate(headers):
        norm = _norm_header(raw)
        if not norm:
            continue
        for canonical, patterns in _HEADER_RULES:
            if canonical in used:
                continue
            if any(pat in norm for pat in patterns):
                out[col_idx] = canonical
                used.add(canonical)
                break
    return out


def _coerce_decimal(s: object) -> float:
    """Spanish-decimals: comma is decimal separator, dot is thousands.

    ``"1.234,56"`` → ``1234.56`` ; ``"3,17"`` → ``3.17`` ;
    plain numerics pass through.
    """
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return float("nan")
    if isinstance(s, (int, float)):
        return float(s)
    txt = str(s).strip()
    if not txt:
        return float("nan")
    # If both . and , present → '.' is thousands separator.
    if "," in txt and "." in txt:
        txt = txt.replace(".", "").replace(",", ".")
    elif "," in txt:
        txt = txt.replace(",", ".")
    try:
        return float(txt)
    except ValueError:
        return float("nan")


def _coerce_date(s: object, default_year_month: str | None = None) -> str:
    """Try to parse the SCVIC date-ish field into a YYYY-MM-01 string.

    Accepts:
      * ``2026-04-01``
      * ``2026-04`` / ``04-2026``
      * ``Abril 2026`` (Spanish month name + year)
      * a 4-digit year fragment alongside a numeric month elsewhere

    Falls back to ``default_year_month`` (already in ``YYYY-MM``
    form) if no parse succeeds.
    """
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return f"{default_year_month}-01" if default_year_month else ""
    if isinstance(s, pd.Timestamp):
        return s.strftime("%Y-%m-01")
    txt = str(s).strip()
    if not txt:
        return f"{default_year_month}-01" if default_year_month else ""
    spanish_months = {
        "enero": 1,
        "febrero": 2,
        "marzo": 3,
        "abril": 4,
        "mayo": 5,
        "junio": 6,
        "julio": 7,
        "agosto": 8,
        "septiembre": 9,
        "setiembre": 9,
        "octubre": 10,
        "noviembre": 11,
        "diciembre": 12,
    }
    norm = _norm_header(txt)
    for name, m in spanish_months.items():
        if name in norm:
            for token in txt.replace("-", " ").replace("/", " ").split():
                if token.isdigit() and len(token) == 4:
                    return f"{int(token):04d}-{m:02d}-01"
    try:
        ts = pd.to_datetime(txt, errors="coerce", dayfirst=True)
        if pd.notna(ts):
            return ts.strftime("%Y-%m-01")
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        pass
    return f"{default_year_month}-01" if default_year_month else ""


def read_scvic_consolidado(
    path: str | Path,
    *,
    sheet_name: int | str = 0,
    default_year_month: str | None = None,
) -> pd.DataFrame:
    """Read a SCVIC consolidado-mensual Excel file → long-form frame.

    Args:
        path: Excel file (.xlsx or .xls).
        sheet_name: which sheet to read; defaults to the first.
        default_year_month: fallback ``YYYY-MM`` when the file's date
            cells are unparseable (the consolidado is monthly so a
            single date applies to every row).

    Returns:
        Long-form ``DataFrame`` with the columns listed in
        :data:`CANONICAL_COLUMNS`. Numeric columns (``costo_*``)
        are coerced from Spanish-decimal strings to float.
    """
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"SCVIC consolidado not found: {p}")

    raw = pd.read_excel(p, sheet_name=sheet_name, header=None, dtype=object)
    if raw.empty:
        return pd.DataFrame(columns=list(CANONICAL_COLUMNS))

    header_row = _detect_header_row(raw)
    if header_row is None:
        raise ValueError(
            f"Could not locate the table header in {p}. Expected a row "
            "with at least 3 cells whose normalised text matches one of "
            f"{[r[0] for r in _HEADER_RULES]}."
        )
    _LOG.info("SCVIC header row detected at index %d", header_row)

    headers = raw.iloc[header_row].tolist()
    col_map = _map_columns(headers)
    if not col_map:
        raise ValueError(f"No canonical columns recognised in {p}: {headers}")

    body = raw.iloc[header_row + 1 :].reset_index(drop=True)
    out = pd.DataFrame(index=body.index)
    for col_idx, canonical in col_map.items():
        out[canonical] = body.iloc[:, col_idx]

    # Coerce numerics.
    for c in (
        "costo_combustible",
        "costo_variable_total",
        "costo_variable_no_combustible",
        "costo_partida",
        "costo_detencion",
    ):
        if c in out.columns:
            out[c] = out[c].map(_coerce_decimal).astype(float)

    # Coerce date.
    if "date_utc" in out.columns:
        out["date_utc"] = out["date_utc"].map(
            lambda v: _coerce_date(v, default_year_month=default_year_month)
        )
    elif default_year_month is not None:
        out["date_utc"] = f"{default_year_month}-01"

    # String-typed canonicals.
    for c in (
        "central_name",
        "configuracion",
        "empresa",
        "tipo_combustible",
        "fuel_unit",
    ):
        if c in out.columns:
            out[c] = out[c].astype(str).str.strip()
            out[c] = out[c].replace({"nan": "", "None": ""})

    # Drop rows with no central or no fuel cost — those are blank lines.
    keep_cols = [c for c in ("central_name", "costo_combustible") if c in out.columns]
    if keep_cols:
        mask = pd.Series(True, index=out.index)
        for c in keep_cols:
            if c == "costo_combustible":
                mask &= out[c].notna()
            else:
                mask &= out[c].astype(str).str.len() > 0
        out = out[mask]

    out = out.reset_index(drop=True)

    # Pad missing canonical columns with empty values so downstream
    # code can rely on the schema being stable.
    for c in CANONICAL_COLUMNS:
        if c not in out.columns:
            out[c] = pd.NA
    return out[list(CANONICAL_COLUMNS)]


def to_costo_combustible_frame(scvic: pd.DataFrame) -> pd.DataFrame:
    """Project a SCVIC frame to the deprecated ``costo-combustible``
    schema so that :func:`cen2gtopt._mc_composition.compose_declared_mc`
    can consume it unchanged.

    The deprecated REST endpoint returned:
      ``date_utc, central_name, configuracion, empresa,
        tipo_combustible, costo_combustible``
    """
    cols = [
        "date_utc",
        "central_name",
        "configuracion",
        "empresa",
        "tipo_combustible",
        "costo_combustible",
    ]
    out = scvic.copy()
    for c in cols:
        if c not in out.columns:
            out[c] = pd.NA
    return out[cols]


__all__ = [
    "CANONICAL_COLUMNS",
    "read_scvic_consolidado",
    "to_costo_combustible_frame",
]
