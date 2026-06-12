# SPDX-License-Identifier: BSD-3-Clause
"""Runtime environment detection: CPUs, compression codecs, memory."""

from __future__ import annotations

import os
import sys


def detect_cpu_count() -> int:
    """Return the number of available CPUs, at least 1."""
    return os.cpu_count() or 1


def detect_memory_mb() -> float:
    """Return total system memory in MB, or 0.0 if unavailable."""
    try:
        if sys.platform == "linux":
            with open("/proc/meminfo", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        parts = line.split()
                        return float(parts[1]) / 1024.0  # kB → MB
        # Fallback: try os.sysconf
        pages = os.sysconf("SC_PHYS_PAGES")
        page_size = os.sysconf("SC_PAGE_SIZE")
        if pages > 0 and page_size > 0:
            return (pages * page_size) / (1024.0 * 1024.0)
    except (OSError, ValueError, AttributeError):
        pass
    return 0.0


def detect_compression_codec(requested: str = "zstd") -> str:
    """Return *requested* if available in PyArrow, else fall back to gzip.

    Returns the codec string unchanged if it is ``"none"`` or
    ``"uncompressed"``.
    """
    if not requested or requested in ("none", "uncompressed"):
        return requested
    try:
        import pyarrow as pa  # noqa: PLC0415

        pa.Codec(requested)
        return requested
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        print(
            f"Warning: Parquet codec '{requested}' not available; falling back to gzip",
            file=sys.stderr,
        )
        return "gzip"
