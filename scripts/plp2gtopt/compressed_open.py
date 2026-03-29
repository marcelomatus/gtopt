"""Transparent reading of optionally compressed files.

When a requested file does not exist, this module probes for compressed
variants (e.g. ``foo.dat.gz``, ``foo.dat.zst``) and opens them with the
appropriate decompression.  Supported formats:

- ``.gz``  -- gzip  (stdlib)
- ``.bz2`` -- bzip2 (stdlib)
- ``.xz``  -- lzma  (stdlib)
- ``.zst`` -- zstandard (optional, requires ``zstandard`` package)
- ``.lz4`` -- lz4   (optional, requires ``lz4`` package)
"""

import bz2
import gzip
import io
import lzma
from pathlib import Path
from typing import IO

# Extension -> opener.  Each opener returns a binary file-like object.
_COMPRESSORS: dict[str, type | None] = {
    ".gz": None,
    ".bz2": None,
    ".xz": None,
    ".zst": None,
    ".lz4": None,
}

# Probe order: most common first.
_EXTENSIONS = (".gz", ".zst", ".lz4", ".xz", ".bz2")


def _open_gz(path: Path) -> IO[bytes]:
    return gzip.open(path, "rb")


def _open_bz2(path: Path) -> IO[bytes]:
    return bz2.open(path, "rb")


def _open_xz(path: Path) -> IO[bytes]:
    return lzma.open(path, "rb")


def _open_zst(path: Path) -> IO[bytes]:
    try:
        import zstandard  # type: ignore[import-untyped]
    except ImportError as exc:
        raise ImportError(
            f"zstandard package required to read {path}; "
            "install with: pip install zstandard"
        ) from exc
    dctx = zstandard.ZstdDecompressor()
    fh = open(path, "rb")  # noqa: SIM115
    return dctx.stream_reader(fh, closefd=True)


def _open_lz4(path: Path) -> IO[bytes]:
    try:
        import lz4.frame  # type: ignore[import-untyped]
    except ImportError as exc:
        raise ImportError(
            f"lz4 package required to read {path}; "
            "install with: pip install lz4"
        ) from exc
    return lz4.frame.open(path, "rb")


_OPENER = {
    ".gz": _open_gz,
    ".bz2": _open_bz2,
    ".xz": _open_xz,
    ".zst": _open_zst,
    ".lz4": _open_lz4,
}


def resolve_compressed_path(path: Path) -> Path:
    """Return *path* if it exists, otherwise try compressed variants.

    Raises ``FileNotFoundError`` if neither the original nor any
    compressed variant is found.
    """
    if path.exists():
        return path
    for ext in _EXTENSIONS:
        candidate = path.with_name(path.name + ext)
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        f"File not found: {path} (also tried {', '.join(path.name + e for e in _EXTENSIONS)})"
    )


def find_compressed_path(path: Path) -> Path | None:
    """Return *path* if it exists, or a compressed variant, or ``None``."""
    if path.exists():
        return path
    for ext in _EXTENSIONS:
        candidate = path.with_name(path.name + ext)
        if candidate.exists():
            return candidate
    return None


def compressed_open(
    path: Path,
    encoding: str = "ascii",
    errors: str = "ignore",
) -> IO[str]:
    """Open a file for text reading, decompressing transparently if needed.

    If *path* has a known compression suffix (``.gz``, ``.bz2``, ``.xz``,
    ``.zst``, ``.lz4``), it is decompressed on the fly.  Otherwise the
    file is opened as plain text.

    Args:
        path: Path to the file (possibly compressed).
        encoding: Text encoding (default ``ascii``).
        errors: Encoding error handling (default ``ignore``).

    Returns:
        A text-mode file-like object.
    """
    suffix = path.suffix.lower()
    opener = _OPENER.get(suffix)
    if opener is not None:
        binary_stream = opener(path)
        return io.TextIOWrapper(binary_stream, encoding=encoding, errors=errors)
    return open(path, "r", encoding=encoding, errors=errors)  # noqa: SIM115
