# SPDX-License-Identifier: BSD-3-Clause
"""Compression utilities for LP files used by gtopt_check_lp.

Supports reading and decompressing gzip-compressed LP files
(``.lp.gz`` and ``.lp.gzip``).  All solver back-ends that need a plain file
path on disk use the :func:`as_plain_lp` context manager, which transparently
decompresses to a temporary file and cleans up afterward.

Public API
----------
.. autofunction:: is_compressed
.. autofunction:: read_lp_text
.. autofunction:: as_plain_lp
.. autofunction:: resolve_lp_path
"""

from __future__ import annotations

import gzip
import os
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Optional

# Recognised compressed-LP extensions (case-insensitive suffix matching).
# ".gzip" is the canonical form mentioned in the problem statement;
# ".gz" is the universally used short form.
_COMPRESSED_SUFFIXES = frozenset({".gz", ".gzip"})


def is_compressed(path: Path) -> bool:
    """Return True when *path* has a recognised gzip extension.

    Recognises ``.gz`` and ``.gzip`` (case-insensitive) as suffixes,
    covering ``error_0.lp.gz`` and ``error_0.lp.gzip``.
    """
    return path.suffix.lower() in _COMPRESSED_SUFFIXES


def read_lp_text(path: Path) -> str:
    """Return the LP file content as text, decompressing gzip if needed.

    Raises :class:`OSError` / :class:`gzip.BadGzipFile` on I/O or format
    errors (callers are responsible for catching in quiet mode).
    """
    if is_compressed(path):
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    return path.read_text(encoding="utf-8", errors="replace")


@contextmanager
def as_plain_lp(path: Path) -> Iterator[Path]:
    """Yield a plain (uncompressed) ``.lp`` file path.

    If *path* is already uncompressed, the original path is yielded
    unchanged.  If *path* is compressed, the content is decompressed to a
    temporary ``.lp`` file which is deleted when the context exits.

    Usage example::

        with as_plain_lp(maybe_compressed_path) as lp_path:
            subprocess.run(["clp", str(lp_path), "solve"], ...)
    """
    if not is_compressed(path):
        yield path
        return

    tmp_fd, tmp_name = tempfile.mkstemp(suffix=".lp", prefix="gtopt_check_")
    tmp_path = Path(tmp_name)
    try:
        # Close the OS-level fd immediately; we write via Path.write_text below.
        os.close(tmp_fd)
        text = read_lp_text(path)
        tmp_path.write_text(text, encoding="utf-8")
        yield tmp_path
    finally:
        tmp_path.unlink(missing_ok=True)


def resolve_lp_path(lp_path: Path) -> Optional[Path]:
    """Resolve *lp_path*, falling back to compressed variants when absent.

    Resolution order:

    1. *lp_path* as-is (if it exists).
    2. ``lp_path + ".gz"``  (e.g. ``error_0.lp`` → ``error_0.lp.gz``).
    3. ``lp_path + ".gzip"`` (e.g. ``error_0.lp`` → ``error_0.lp.gzip``).

    Returns the first existing path, or ``None`` when none are found.
    """
    if lp_path.exists():
        return lp_path
    for suffix in (".gz", ".gzip"):
        candidate = Path(str(lp_path) + suffix)
        if candidate.exists():
            return candidate
    return None
