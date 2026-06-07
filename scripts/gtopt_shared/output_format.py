# SPDX-License-Identifier: BSD-3-Clause
"""Shared output-format helpers (issue #507 cross-converter sharing).

Centralises the ``output_format == "parquet" / "csv"`` dispatch that
was previously duplicated across plp2gtopt, ts2gtopt, igtopt, and
inlined as constants in plexos2gtopt / sddp2gtopt / pp2gtopt.

The two formats are both consumed by gtopt's C++ input reader; the
choice is per-case (drives ``options.output_format`` in the planning
JSON) and applies uniformly across every writer's per-table emission.

This module exports:

* :data:`OUTPUT_FORMATS` — the canonical pair, in declaration order.
* :func:`format_suffix` — map a format string to the file extension.
* :func:`write_dataframe` — dispatch a single DataFrame to disk under
  the right serializer with the right compression knobs.

Compression knobs (Parquet only) come from
:mod:`gtopt_shared.parquet`; CSV writes go through
:func:`gtopt_shared.csv_io.write_csv` so the numeric formatting
matches the gtopt C++ Arrow CSV reader bit-for-bit.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Final, Literal

import pandas as pd

from gtopt_shared.csv_io import write_csv


OutputFormat = Literal["parquet", "csv"]

#: Canonical pair of output formats in declaration order.  Iterate this
#: tuple when emitting a side-by-side benchmark / test fixture (rather
#: than re-listing the strings inline).
OUTPUT_FORMATS: Final[tuple[OutputFormat, ...]] = ("parquet", "csv")

#: Map an output-format string to the file-name extension (with the
#: leading dot).  Use this instead of inline ``f"{stem}.parquet"`` /
#: ``f"{stem}.csv"`` ladders so a future addition (e.g. ``"feather"``)
#: stays in one place.
_FORMAT_SUFFIXES: Final[dict[str, str]] = {
    "parquet": ".parquet",
    "csv": ".csv",
}


def format_suffix(output_format: str) -> str:
    """Return ``".parquet"`` or ``".csv"`` for a writer-facing format.

    Raises :class:`ValueError` for unknown formats so callers don't
    silently fall back to a default that would corrupt the output tree.
    """
    suffix = _FORMAT_SUFFIXES.get(output_format)
    if suffix is None:
        raise ValueError(
            f"unsupported output_format {output_format!r}; expected one of "
            f"{sorted(_FORMAT_SUFFIXES)}"
        )
    return suffix


def _build_parquet_kwargs(
    *,
    compression: str | None,
    compression_level: int | None,
) -> dict[str, Any]:
    """Build the ``to_parquet`` kwargs dict, dropping ``None`` values
    so callers can ``**kwargs`` without worrying about unsupported args."""
    kw: dict[str, Any] = {}
    if compression is not None:
        kw["compression"] = compression
    if compression_level is not None:
        kw["compression_level"] = compression_level
    return kw


def write_dataframe(
    df: pd.DataFrame,
    output_dir: Path,
    stem: str,
    *,
    output_format: str = "parquet",
    compression: str | None = None,
    compression_level: int | None = None,
) -> Path:
    """Write ``df`` to ``output_dir/<stem>.<ext>`` under the chosen format.

    Parameters
    ----------
    df:
        The DataFrame to emit.  ``index=False`` is forced on both
        Parquet and CSV writes so the round-trip back through gtopt's
        Arrow reader is clean.
    output_dir:
        Destination directory; not created automatically — caller is
        responsible for ``mkdir(parents=True, exist_ok=True)``.
    stem:
        Filename without extension.  The extension is appended by
        :func:`format_suffix`.
    output_format:
        ``"parquet"`` (default) or ``"csv"``.
    compression / compression_level:
        Parquet-only.  Ignored for CSV writes.  ``None`` falls back to
        the Parquet writer's built-in default.

    Returns
    -------
    Path
        The full path that was written, with extension.
    """
    out = output_dir / f"{stem}{format_suffix(output_format)}"
    if output_format == "csv":
        write_csv(df, out)
    else:
        df.to_parquet(
            out,
            index=False,
            **_build_parquet_kwargs(
                compression=compression, compression_level=compression_level
            ),
        )
    return out
