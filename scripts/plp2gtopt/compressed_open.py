"""Transparent reading of optionally compressed and/or split files.

When a requested file does not exist, this module probes for compressed
variants (e.g. ``foo.dat.gz``, ``foo.dat.xz``) and opens them with the
appropriate decompression.  It also supports **split files**: if
``foo.dat.1.xz``, ``foo.dat.2.xz``, … exist they are decompressed in
parallel and concatenated transparently.

Supported compression formats:

- ``.gz``  -- gzip  (stdlib)
- ``.bz2`` -- bzip2 (stdlib)
- ``.xz``  -- lzma  (stdlib, parallel decompression via threads)
- ``.zst`` -- zstandard (optional, requires ``zstandard`` package)
- ``.lz4`` -- lz4   (optional, requires ``lz4`` package)
"""

import bz2
import gzip
import io
import lzma
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import IO

# Probe order: most common first.
_EXTENSIONS = (".xz", ".gz", ".zst", ".lz4", ".bz2")

# Number of threads for parallel decompression of split files.
_DECOMPRESS_THREADS = 4


# ── Single-file openers ─────────────────────────────────────────────────


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


# ── Split-file support ──────────────────────────────────────────────────


def _find_split_parts(path: Path) -> list[Path] | None:
    """Find numbered split parts for *path*.

    Looks for ``path.1``, ``path.2``, … (plain) or
    ``path.1.xz``, ``path.2.xz``, … (compressed, any supported codec).
    Returns the sorted list of part paths, or ``None`` if no parts found.
    All parts must use the same compression extension (or none).
    """
    parent = path.parent
    name = path.name

    # Try plain splits first: foo.dat.1, foo.dat.2, …
    plain_1 = parent / f"{name}.1"
    if plain_1.exists():
        parts = sorted(
            parent.glob(f"{name}.[0-9]*"),
            key=lambda p: int(p.suffix.lstrip(".")),
        )
        if parts:
            return parts

    # Try compressed splits: foo.dat.1.xz, foo.dat.2.gz, …
    for ext in _EXTENSIONS:
        part_1 = parent / f"{name}.1{ext}"
        if part_1.exists():
            parts = sorted(
                parent.glob(f"{name}.[0-9]*{ext}"),
                key=lambda p: int(
                    p.name.removeprefix(f"{name}.").removesuffix(ext)
                ),
            )
            if parts:
                return parts

    return None


def _decompress_part(path: Path) -> bytes:
    """Decompress a single file part, returning raw bytes."""
    suffix = path.suffix.lower()
    opener = _OPENER.get(suffix)
    if opener is not None:
        with opener(path) as fh:
            return fh.read()
    return path.read_bytes()


def _open_split_parts(parts: list[Path]) -> IO[bytes]:
    """Decompress split parts in parallel and return a concatenated stream."""
    if len(parts) == 1:
        data = _decompress_part(parts[0])
    else:
        with ThreadPoolExecutor(max_workers=_DECOMPRESS_THREADS) as pool:
            chunks = list(pool.map(_decompress_part, parts))
        data = b"".join(chunks)
    return io.BytesIO(data)


# ── Path resolution ─────────────────────────────────────────────────────


def resolve_compressed_path(path: Path) -> Path:
    """Return *path* if it exists, otherwise try compressed variants.

    Also checks for split files (``path.1.xz``, etc.).
    Raises ``FileNotFoundError`` if nothing is found.
    """
    if path.exists():
        return path
    for ext in _EXTENSIONS:
        candidate = path.with_name(path.name + ext)
        if candidate.exists():
            return candidate
    # Check for split parts
    parts = _find_split_parts(path)
    if parts:
        return parts[0]  # Return first part as sentinel
    raise FileNotFoundError(
        f"File not found: {path} "
        f"(also tried {', '.join(path.name + e for e in _EXTENSIONS)} "
        f"and split parts {path.name}.1[.xz|.gz|…])"
    )


def find_compressed_path(path: Path) -> Path | None:
    """Return *path* if it exists, or a compressed variant, or ``None``."""
    if path.exists():
        return path
    for ext in _EXTENSIONS:
        candidate = path.with_name(path.name + ext)
        if candidate.exists():
            return candidate
    # Check for split parts
    parts = _find_split_parts(path)
    if parts:
        return parts[0]
    return None


# ── Main entry point ────────────────────────────────────────────────────


def compressed_open(
    path: Path,
    encoding: str = "ascii",
    errors: str = "ignore",
) -> IO[str]:
    """Open a file for text reading, decompressing transparently if needed.

    Handles three cases:

    1. **Single file** (plain or compressed): opened directly.
    2. **Split files** (``path.1.xz``, ``path.2.xz``, …): decompressed
       in parallel (up to 4 threads) and concatenated.

    Args:
        path: Path to the file (possibly compressed or split).
        encoding: Text encoding (default ``ascii``).
        errors: Encoding error handling (default ``ignore``).

    Returns:
        A text-mode file-like object.
    """
    # Check if this is a split-file sentinel (path ends with .N or .N.ext)
    # by looking for sibling parts from the base name.
    base_path = _split_base_path(path)
    if base_path is not None:
        parts = _find_split_parts(base_path)
        if parts and len(parts) > 1:
            binary_stream = _open_split_parts(parts)
            return io.TextIOWrapper(
                binary_stream, encoding=encoding, errors=errors
            )

    # Single file — check suffix for compression
    suffix = path.suffix.lower()
    opener = _OPENER.get(suffix)
    if opener is not None:
        binary_stream = opener(path)
        return io.TextIOWrapper(binary_stream, encoding=encoding, errors=errors)
    return open(path, "r", encoding=encoding, errors=errors)  # noqa: SIM115


def _split_base_path(path: Path) -> Path | None:
    """Extract the base path from a split-file sentinel.

    Given ``foo.dat.1.xz`` returns ``foo.dat``.
    Given ``foo.dat.1`` returns ``foo.dat``.
    Given ``foo.dat.xz`` returns ``None`` (not a split file).
    """
    name = path.name

    # Try: name.N.ext (e.g. foo.dat.1.xz)
    for ext in _EXTENSIONS:
        if name.endswith(ext):
            stem = name.removesuffix(ext)
            # stem should end with .N
            dot = stem.rfind(".")
            if dot >= 0 and stem[dot + 1 :].isdigit():
                return path.parent / stem[:dot]

    # Try: name.N (e.g. foo.dat.1)
    dot = name.rfind(".")
    if dot >= 0 and name[dot + 1 :].isdigit():
        return path.parent / name[:dot]

    return None
