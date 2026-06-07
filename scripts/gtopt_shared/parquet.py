# SPDX-License-Identifier: BSD-3-Clause
"""Shared Parquet codec helpers (issue #507 cross-converter sharing).

Centralises:

* :func:`probe_parquet_codec` — verify a PyArrow codec is compiled
  into the linked Arrow library, fall back to ``"gzip"`` with a stderr
  warning when not.
* :data:`DEFAULT_COMPRESSION` — best available codec probed once at
  module import time.  ``"zstd"`` preferred (best ratio × decode-speed
  for wide field tables, matches the gtopt C++ default
  ``PlanningOptionsLP::default_output_compression``, and unlike
  ``"lz4"`` is a single unambiguous codec every downstream reader
  including Power BI / Power Query opens natively).

Previously duplicated as ``_probe_parquet_codec`` /
``_DEFAULT_COMPRESSION`` in plp2gtopt/base_writer.py, ts2gtopt/ts2gtopt.py,
and igtopt/igtopt.py.  The converter modules now re-export from here so
existing import paths continue to work.
"""

from __future__ import annotations

import sys
from functools import lru_cache


@lru_cache(maxsize=None)
def probe_parquet_codec(requested: str) -> str:
    """Return the best available PyArrow Parquet codec for ``requested``.

    Uses ``pyarrow.Codec`` to test whether the codec is compiled into
    the linked Arrow library.  Falls back to ``"gzip"`` when the
    requested codec is unavailable, printing a one-shot warning to
    stderr.  Results are cached so the probe runs at most once per
    unique codec name across the entire program run.

    Parameters
    ----------
    requested:
        Codec name to probe (e.g. ``"zstd"``, ``"gzip"``).  Empty
        string / ``"none"`` / ``"uncompressed"`` returns as-is
        (uncompressed; no probe needed).
    """
    if not requested or requested in ("none", "uncompressed"):
        return requested
    try:
        # Local import keeps pyarrow off the eager-import path for
        # callers that never need a Parquet codec.  pylint: disable
        # below covers the C0415 it would otherwise raise.
        import pyarrow as pa  # noqa: PLC0415  pylint: disable=import-outside-toplevel

        pa.Codec(requested)
        return requested
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        print(
            f"Warning: Parquet codec '{requested}' is not available in "
            "this Arrow build; falling back to gzip",
            file=sys.stderr,
        )
        return "gzip"


#: Best available codec — probed once at module import time.  See the
#: module docstring for why zstd is preferred.
DEFAULT_COMPRESSION: str = probe_parquet_codec("zstd")
