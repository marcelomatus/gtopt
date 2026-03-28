# SPDX-License-Identifier: BSD-3-Clause
"""Compression utilities for LP files used by gtopt_check_lp.

Supports reading and decompressing gzip-compressed LP files
(``.lp.gz`` and ``.lp.gzip``) and zstd-compressed LP files (``.lp.zst``).
All solver back-ends that need a plain file path on disk use the
:func:`as_plain_lp` context manager, which transparently decompresses to a
temporary file and cleans up afterward.

The :func:`sanitize_lp_names` function and :func:`as_sanitized_lp` context
manager strip illegal ``':'`` characters from variable and constraint names so
that COIN-OR CLP/CBC can parse the file.  In LP format ``':'`` is
strictly reserved as the separator between a constraint (or objective) name
and its expression; it must not appear inside any name.

Public API
----------
.. autofunction:: is_compressed
.. autofunction:: read_lp_text
.. autofunction:: as_plain_lp
.. autofunction:: resolve_lp_path
.. autofunction:: sanitize_lp_names
.. autofunction:: as_sanitized_lp
"""

from __future__ import annotations

import gzip
import os
import re
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Optional

try:
    import zstandard as zstd

    _HAS_ZSTD = True
except ImportError:
    _HAS_ZSTD = False

# Recognised compressed-LP extensions (case-insensitive suffix matching).
# ".gzip" is the canonical form mentioned in the problem statement;
# ".gz" is the universally used short form; ".zst" is for zstd.
_GZIP_SUFFIXES = frozenset({".gz", ".gzip"})
_ZSTD_SUFFIXES = frozenset({".zst"})
_COMPRESSED_SUFFIXES = _GZIP_SUFFIXES | _ZSTD_SUFFIXES

# ---------------------------------------------------------------------------
# LP name-sanitisation helpers
# ---------------------------------------------------------------------------

# Section keywords that mark the start of a new LP section.
_LP_SECTION_STARTS = frozenset(
    {
        "minimize",
        "minimum",
        "min",
        "maximize",
        "maximum",
        "max",
        "subject to",
        "such that",
        "st",
        "s.t.",
        "bounds",
        "bound",
        "general",
        "generals",
        "integer",
        "integers",
        "binary",
        "binaries",
        "end",
    }
)


def is_compressed(path: Path) -> bool:
    """Return True when *path* has a recognised compressed extension.

    Recognises ``.gz``, ``.gzip`` (gzip, case-insensitive) and ``.zst``
    (zstd, case-insensitive), covering ``error_0.lp.gz``,
    ``error_0.lp.gzip``, and ``error_0.lp.zst``.
    """
    return path.suffix.lower() in _COMPRESSED_SUFFIXES


def read_lp_text(path: Path) -> str:
    """Return the LP file content as text, decompressing gzip/zstd if needed.

    Raises :class:`OSError` / :class:`gzip.BadGzipFile` on I/O or format
    errors (callers are responsible for catching in quiet mode).
    """
    if path.suffix.lower() in _ZSTD_SUFFIXES:
        if not _HAS_ZSTD:
            raise ImportError(
                "zstandard package required for reading .zst files; "
                "install with: pip install zstandard"
            )
        dctx = zstd.ZstdDecompressor()
        with open(path, "rb") as fh:
            raw = dctx.decompress(fh.read())
        return raw.decode("utf-8", errors="replace")
    if path.suffix.lower() in _GZIP_SUFFIXES:
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    return path.read_text(encoding="utf-8", errors="replace")


@contextmanager
def as_plain_lp(path: Path) -> Iterator[Path]:
    """Yield a plain (uncompressed) ``.lp`` file path.

    If *path* is already uncompressed, the original path is yielded
    unchanged.  If *path* is compressed (gzip or zstd), the content is
    decompressed to a temporary ``.lp`` file which is deleted when the
    context exits.

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
    2. ``lp_path + ".zst"``  (e.g. ``error_0.lp`` → ``error_0.lp.zst``).
    3. ``lp_path + ".gz"``   (e.g. ``error_0.lp`` → ``error_0.lp.gz``).
    4. ``lp_path + ".gzip"`` (e.g. ``error_0.lp`` → ``error_0.lp.gzip``).

    Returns the first existing path, or ``None`` when none are found.
    """
    if lp_path.exists():
        return lp_path
    for suffix in (".zst", ".gz", ".gzip"):
        candidate = Path(str(lp_path) + suffix)
        if candidate.exists():
            return candidate
    return None


# ---------------------------------------------------------------------------
# LP name sanitisation
# ---------------------------------------------------------------------------


def _sanitize_names_in_expr(expr: str) -> str:
    """Replace ``:`` embedded within identifier tokens in an LP expression.

    The colon character is reserved in LP format as the separator between a
    constraint/objective *name* and its expression.  It is **not** allowed
    inside variable or constraint names themselves.  This helper replaces any
    ``:`` that immediately follows an alphanumeric-or-underscore character
    (i.e.  any ``:`` that is part of a name token, not a standalone separator)
    with ``_``.

    In practice, for well-formed LP files produced by current gtopt, names
    already use ``_`` separators so this function is a no-op.  It exists to
    handle historical LP files or third-party LP files that use the CPLEX
    dialect where ``:`` may appear inside names.
    """
    return re.sub(r"(?<=[A-Za-z0-9_]):", "_", expr)


def _sanitize_definition_line(line: str) -> str:
    """Sanitise a constraint/objective definition line.

    A *definition line* is one that starts a new named constraint or objective:
    ``  name: expression``.  The first ``:`` is the LP separator and must be
    preserved.  Any ``:`` embedded *inside* the name before the separator, and
    any ``:`` inside variable tokens in the expression, are replaced with ``_``.

    Lines that are continuations (no leading ``name:`` prefix) are sanitised
    as plain expressions.
    """
    stripped = line.rstrip("\r\n")
    content = stripped.lstrip()
    leading = stripped[: len(stripped) - len(content)]
    trailing = line[len(stripped) :]

    if not content or content.startswith("\\"):
        return line  # empty or comment line – leave untouched

    # Match a constraint/objective label: identifier (possibly with ':' inside)
    # followed by a ':' separator.  The regex is greedy but backtracks so that
    # the *last* ':' immediately before optional whitespace becomes the
    # separator (leaving one ':' to serve as the constraint delimiter).
    def_match = re.match(r"([A-Za-z_][A-Za-z0-9_:.]*)\s*:", content)
    if def_match:
        name = def_match.group(1)
        rest = content[def_match.end() :]
        sanitized_name = name.replace(":", "_") if ":" in name else name
        sanitized_rest = _sanitize_names_in_expr(rest)
        return leading + sanitized_name + ":" + sanitized_rest + trailing

    # Continuation line (expression only)
    return leading + _sanitize_names_in_expr(content) + trailing


def _sanitize_non_definition_line(line: str) -> str:
    """Sanitise a line outside the objective/constraints section.

    In Bounds, Ranges, General and Binary sections, any ``:`` appearing after
    an identifier character is not a constraint separator and must be replaced
    with ``_``.  This handles the case where CoinLpIO (COIN-OR LP reader)
    writes constraint names with the separator ``:`` attached into these
    sections, causing it to later read them back as invalid column names.
    """
    stripped = line.rstrip("\r\n")
    content = stripped.lstrip()
    leading = stripped[: len(stripped) - len(content)]
    trailing = line[len(stripped) :]

    if not content or content.startswith("\\"):
        return line  # empty or comment line – leave untouched

    return leading + _sanitize_names_in_expr(content) + trailing


def sanitize_lp_names(text: str) -> str:
    """Sanitise LP variable/constraint names that contain ``':'``.

    In the standard CPLEX LP file format, ``':'`` is **strictly reserved** as
    the separator between a constraint (or objective) name and its expression.
    It may not appear inside any variable or constraint name.  COIN-OR
    CLP/CBC (CoinLpIO) enforces this rule and rejects LP files where
    names contain ``':'``.

    This function performs two operations:

    1. **Constraint/objective section**: preserves the single ``:`` separator
       after each constraint/objective name; replaces any ``:`` embedded
       *inside* a name with ``_``.
    2. **All other sections** (Bounds, Ranges, General, Binary): strips any
       trailing ``:`` from identifier tokens (CoinLpIO sometimes writes
       constraint names *with* their separator colon into these sections,
       causing the reader to misparse them as column names).

    For well-formed LP files produced by current gtopt (names use ``_``
    separators only), this function is effectively a no-op.

    Returns the sanitised LP text, preserving original line endings.
    """
    lines = text.splitlines(keepends=True)
    result: list[str] = []
    section = "preamble"

    for line in lines:
        content = line.strip()
        lower = content.lower()

        # Detect section transitions.
        new_section: Optional[str] = None
        for kw in _LP_SECTION_STARTS:
            if lower == kw or lower.startswith(kw + " ") or lower.startswith(kw + "\t"):
                new_section = kw
                break

        if new_section is not None:
            section = new_section
            result.append(line)  # section keyword lines are left untouched
            continue

        if section in (
            "minimize",
            "minimum",
            "min",
            "maximize",
            "maximum",
            "max",
            "subject to",
            "such that",
            "st",
            "s.t.",
        ):
            result.append(_sanitize_definition_line(line))
        else:
            result.append(_sanitize_non_definition_line(line))

    return "".join(result)


@contextmanager
def as_sanitized_lp(path: Path) -> Iterator[Path]:
    """Yield a sanitised, plain (uncompressed) ``.lp`` file path.

    Combines decompression (if the file is gzip- or zstd-compressed) with LP
    name sanitisation (see :func:`sanitize_lp_names`).  The temporary file is
    always created and deleted when the context exits.

    Usage example::

        with as_sanitized_lp(maybe_compressed_path) as lp_path:
            subprocess.run(["clp", str(lp_path), "solve"], ...)
    """
    text = read_lp_text(path)
    sanitized = sanitize_lp_names(text)

    tmp_fd, tmp_name = tempfile.mkstemp(suffix=".lp", prefix="gtopt_san_")
    tmp_path = Path(tmp_name)
    try:
        os.close(tmp_fd)
        tmp_path.write_text(sanitized, encoding="utf-8")
        yield tmp_path
    finally:
        tmp_path.unlink(missing_ok=True)
