"""Readers for the auxiliary AMM spreadsheets shipped alongside the RAW.

A PSS/E ``.raw`` carries no economic data and only cryptic bus names.
The Guatemalan AMM "Programación de Largo Plazo" package ships two
spreadsheets that fill part of that gap:

* **Nomenclatura** (``Nomenclatura.xls``) — a `code → description` table
  that expands the 3-letter bus-name prefixes (``AGU`` → ``Aguacapa``).
* **LDM / Lista de Mérito** (``LDM <month> <year>.xlsx``) — generation
  units in **merit (dispatch) order**, with available MW.  The *order*
  is the only economic signal in the whole package, so it can drive a
  rank-based generation cost that makes the DC OPF dispatch follow the
  real merit order.

Both readers are optional: they're only invoked when the user passes
``--nomenclatura`` / ``--ldm``.  pandas + openpyxl/xlrd are already
runtime dependencies of the scripts package (plp2gtopt uses them).
"""

from __future__ import annotations

import logging
import unicodedata
from pathlib import Path
from typing import Any


logger = logging.getLogger(__name__)


def _excel_engine(path: Path) -> str:
    """Pick the pandas Excel engine from the (de-compressed) extension."""
    name = path.name.lower()
    for ext in (".xz", ".gz", ".zst", ".lz4", ".bz2"):
        if name.endswith(ext):
            name = name[: -len(ext)]
            break
    return "xlrd" if name.endswith(".xls") else "openpyxl"


def _read_excel(path: Path, **kwargs: Any) -> Any:
    """Read an Excel workbook, transparently decompressing ``.xls.xz`` etc."""
    import pandas as pd  # pylint: disable=import-outside-toplevel

    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        open_binary,
    )

    with open_binary(Path(path)) as fh:
        return pd.read_excel(fh, engine=_excel_engine(path), **kwargs)


def ascii_sanitize(text: str) -> str:
    """Fold a label to a reference-safe ASCII token.

    Strips accents, then maps any character outside ``[A-Za-z0-9_-]`` to
    ``_`` and collapses repeats — so names stay valid LP row labels and
    JSON reference keys.
    """
    decomposed = unicodedata.normalize("NFKD", text)
    ascii_text = decomposed.encode("ascii", "ignore").decode("ascii")
    out: list[str] = []
    for ch in ascii_text:
        out.append(ch if (ch.isalnum() or ch in "-_") else "_")
    token = "".join(out).strip("_")
    while "__" in token:
        token = token.replace("__", "_")
    return token or "x"


def parse_nomenclatura(path: str | Path) -> dict[str, str]:
    """Parse the Nomenclatura spreadsheet into ``{code: description}``.

    The sheet has a free-form header then two columns: a short code
    (``AGU``) and its description (``Aguacapa``).  Codes are upper-cased;
    the first non-empty (code, description) pair on each row wins.

    Args:
        path: Path to ``Nomenclatura.xls`` / ``.xlsx``.

    Returns:
        Mapping of upper-cased code → description (may be empty if the
        layout is unrecognised).
    """
    df = _read_excel(Path(path), sheet_name=0, header=None)
    mapping: dict[str, str] = {}
    for _, row in df.iterrows():
        cells = [
            str(c).strip() for c in row.tolist() if str(c).strip() and str(c) != "nan"
        ]
        if len(cells) < 2:
            continue
        code, desc = cells[0], cells[1]
        # Skip the title / header rows ("NOMENCLATURA", etc.).
        if code.upper() in ("NOMENCLATURA", "NEMO", "CODIGO", "CÓDIGO"):
            continue
        if 1 <= len(code) <= 6 and any(c.isalpha() for c in code):
            mapping.setdefault(code.upper(), desc)
    logger.info("parsed Nomenclatura %s: %d codes", Path(path).name, len(mapping))
    return mapping


def parse_ldm_merit_order(path: str | Path) -> list[str]:
    """Parse the LDM (Lista de Mérito) into an ordered list of unit codes.

    The sheet repeats three identical blocks (DEMANDA MÍNIMA / MEDIA /
    MÁXIMA), each a merit-ordered list with columns
    ``Nemo | Planta Generadora | Potencia Disponible``.  We read the
    first block and return the unit ``Nemo`` codes **in merit order**
    (index 0 = first dispatched / cheapest).

    Args:
        path: Path to ``LDM <month> <year>.xlsx``.

    Returns:
        Ordered list of upper-cased unit codes (empty if unrecognised).
    """
    path = Path(path)
    df = _read_excel(path, sheet_name=0, header=None)

    # Find the first "DEMANDA ..." section header, then the column header
    # row ("Nemo"), then read codes until the next section / blank run.
    start = None
    for i, row in df.iterrows():
        joined = " ".join(str(c) for c in row.tolist()).upper()
        if "NEMO" in joined and "PLANTA" in joined:
            start = i + 1
            break
    if start is None:
        logger.warning("LDM %s: no 'Nemo / Planta' header found", path.name)
        return []

    order: list[str] = []
    blanks = 0
    for i in range(start, len(df)):
        code = str(df.iat[i, 0]).strip()
        joined = " ".join(str(c) for c in df.iloc[i].tolist()).upper()
        if "DEMANDA" in joined:  # next section block starts — stop.
            break
        if not code or code.lower() == "nan":
            blanks += 1
            if blanks > 3:
                break
            continue
        blanks = 0
        order.append(code.upper())
    logger.info("parsed LDM %s: %d units in merit order", path.name, len(order))
    return order


def expand_bus_name(pss_name: str, codes: dict[str, str]) -> str:
    """Expand a PSS/E bus name using the Nomenclatura code table.

    ``AGU-230`` → ``Aguacapa-230`` when ``AGU`` is known; otherwise the
    sanitised original name is returned.  The voltage / unit suffix
    after the first ``-`` is preserved so distinct buses stay distinct.
    """
    name = pss_name.strip()
    prefix, sep, rest = name.partition("-")
    desc = codes.get(prefix.upper())
    if desc is None:
        return ascii_sanitize(name)
    base = ascii_sanitize(desc)
    return f"{base}-{ascii_sanitize(rest)}" if sep and rest else base
