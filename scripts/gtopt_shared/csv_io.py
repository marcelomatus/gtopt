# SPDX-License-Identifier: BSD-3-Clause
"""Centralised CSV writer for files that gtopt will consume.

Policy (``CLAUDE.md`` / project memory): every CSV that gtopt's C++
parser will read MUST be produced by the Arrow CSV writer, not pandas'
``DataFrame.to_csv`` or Python's stdlib ``csv`` module.  Arrow's writer
formats numerics in a way that round-trips losslessly through Arrow's
strict CSV reader on the C++ side — pandas' default formatting can
trim trailing zeros, drop decimal points on int-valued floats, or
elide cells in ways that the strict numeric guard rejects.

This module exposes a single :func:`write_csv` helper that accepts a
``pandas.DataFrame``, a ``pyarrow.Table``, or a row-iterable + header
list, normalises to an Arrow ``Table``, and writes via
:func:`pyarrow.csv.write_csv`.

Test/fixture writers (under ``tests/``) and Python-only output (e.g.
``gtopt2pbi``, ``cen2gtopt`` summaries, ``gtopt_reduce_network``
reports) are NOT covered by this policy and may keep using
``DataFrame.to_csv`` for convenience.
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

import pandas as pd
import pyarrow as pa
import pyarrow.csv as pa_csv


def write_csv(
    data: pd.DataFrame | pa.Table | Iterable[Sequence[Any]],
    path: str | Path,
    *,
    headers: Sequence[str] | None = None,
) -> None:
    """Write ``data`` to ``path`` using :func:`pyarrow.csv.write_csv`.

    Accepted shapes:

    * ``pyarrow.Table`` — written directly.
    * ``pandas.DataFrame`` — converted via
      :meth:`pyarrow.Table.from_pandas` with ``preserve_index=False``.
    * ``Iterable[Sequence]`` — a sequence of rows; ``headers`` is then
      required and supplies the column names.  Each row must have the
      same length as ``headers``.  Useful for the stdlib-``csv.writer``
      legacy call sites that emit a handful of rows by hand.

    Args:
        data: The table or row stream to serialise.
        path: Destination file path (``str`` or :class:`pathlib.Path`).
        headers: Column names; required when ``data`` is a row iterable,
            forbidden otherwise.

    Raises:
        TypeError: if ``data`` is a row iterable but ``headers`` is
            missing, or if ``data`` is a Table/DataFrame but ``headers``
            is supplied (ambiguous).
    """
    if isinstance(data, pa.Table):
        if headers is not None:
            raise TypeError(
                "write_csv: headers must not be supplied when data is a "
                "pyarrow.Table (its schema already carries column names)"
            )
        table = data
    elif isinstance(data, pd.DataFrame):
        if headers is not None:
            raise TypeError(
                "write_csv: headers must not be supplied when data is a "
                "pandas.DataFrame (its columns already carry names)"
            )
        table = pa.Table.from_pandas(data, preserve_index=False)
    else:
        if headers is None:
            raise TypeError(
                "write_csv: headers is required when data is a row iterable"
            )
        rows = [list(row) for row in data]
        n = len(headers)
        for i, row in enumerate(rows):
            if len(row) != n:
                raise ValueError(
                    f"write_csv: row {i} has {len(row)} values, "
                    f"expected {n} (matching headers={list(headers)})"
                )
        columns = {h: [row[i] for row in rows] for i, h in enumerate(headers)}
        table = pa.table(columns)

    pa_csv.write_csv(table, str(path))
