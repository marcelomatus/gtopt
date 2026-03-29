# SPDX-License-Identifier: BSD-3-Clause
"""plp_compress_case — compress or decompress PLP case directories.

Compresses all ``.dat``, ``.csv``, ``.prn``, and ``.png`` files in a
directory using a configurable codec (default: ``xz -T0``).  Files larger
than a threshold after compression are automatically split into numbered
parts (``foo.dat.1.xz``, ``foo.dat.2.xz``, ...).

With ``--decompress``, restores the original files from compressed
(including reassembling split parts).

All operations use a temporary directory so that an interruption leaves
the original files intact.

plp2gtopt reads compressed/split files transparently via the
``compressed_open`` module.

Configuration
-------------
User preferences are stored in ``~/.gtopt.conf`` under
``[plp_compress_case]``.  See ``plp_compress_case --show-config``.

Examples::

    plp_compress_case support/plp_long_term
    plp_compress_case scripts/cases/plp_case_2y --split-mb 8
    plp_compress_case --decompress support/plp_long_term
    plp_compress_case cases/my_case --codec zstd --codec-args ""
"""

from __future__ import annotations

import argparse
import logging
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from ._config import default_config_path, load_config

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

log = logging.getLogger(__name__)

# File extensions considered for compression.
_DATA_EXTS = (".dat", ".csv", ".prn", ".png")
# Compressed extensions to exclude from data file search.
_COMPRESSED_EXTS = (".xz", ".gz", ".bz2", ".zst", ".lz4")

# Map codec name → compressed file extension.
_CODEC_EXT: dict[str, str] = {
    "xz": ".xz",
    "gzip": ".gz",
    "bzip2": ".bz2",
    "zstd": ".zst",
    "lz4": ".lz4",
}

_DESCRIPTION = """\
Compress or decompress PLP case directories.

Compresses .dat, .csv, .prn, and .png files using a configurable codec
(default: xz -T0).  Large compressed files are split into numbered parts.

With --decompress, restores original files (reassembling splits).

Examples:
  plp_compress_case support/plp_long_term
  plp_compress_case cases/plp_case_2y --split-mb 8
  plp_compress_case --decompress support/plp_long_term
  plp_compress_case cases/my_case --codec zstd --codec-args ""
"""


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="plp_compress_case",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=None,
        metavar="DIRECTORY",
        help="PLP case directory to compress or decompress.",
    )
    parser.add_argument(
        "-d",
        "--decompress",
        action="store_true",
        help="Decompress mode: restore original files from compressed.",
    )
    parser.add_argument(
        "--codec",
        default=None,
        metavar="CODEC",
        help=(
            "Compression program (e.g. 'xz', 'gzip', 'zstd', 'lz4'). "
            "Default from config or 'xz'."
        ),
    )
    parser.add_argument(
        "--codec-args",
        default=None,
        metavar="ARGS",
        help=(
            "Extra arguments for the codec command (e.g. '-T0' for xz). "
            "Default from config or '-T0'."
        ),
    )
    parser.add_argument(
        "--split-mb",
        type=int,
        default=None,
        metavar="N",
        help="Max compressed file size in MB before splitting (default: 10).",
    )
    parser.add_argument(
        "--config",
        default=None,
        type=Path,
        metavar="PATH",
        help="Path to the config file (default: ~/.gtopt.conf).",
    )
    parser.add_argument(
        "--show-config",
        action="store_true",
        help="Print the active configuration and exit.",
    )
    parser.add_argument(
        "--color",
        default=None,
        choices=["auto", "always", "never"],
        help="Terminal colour output (default: auto).",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="WARNING",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        metavar="LEVEL",
        help="Logging verbosity (default: %(default)s).",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


# ── Helpers ──────────────────────────────────────────────────────────────


def _find_data_files(directory: Path) -> list[Path]:
    """Find uncompressed data files in *directory* (non-recursive)."""
    files = []
    for f in sorted(directory.iterdir()):
        if not f.is_file():
            continue
        if f.suffix.lower() in _COMPRESSED_EXTS:
            continue
        if f.suffix.lower() in _DATA_EXTS:
            files.append(f)
    return files


def _find_compressed_files(directory: Path, ext: str) -> list[Path]:
    """Find compressed files in *directory* with the given extension."""
    return sorted(
        f for f in directory.iterdir() if f.is_file() and f.name.endswith(ext)
    )


def _codec_ext(codec: str) -> str:
    """Return the file extension for a codec name."""
    return _CODEC_EXT.get(codec, f".{codec}")


def _compress_file(src: Path, dst: Path, codec: str, codec_args: list[str]) -> None:
    """Compress *src* to *dst* using the given codec."""
    cmd = [codec, *codec_args, "-c", str(src)]
    log.debug("compress: %s", cmd)
    with open(dst, "wb") as fh:
        subprocess.run(cmd, stdout=fh, check=True)


def _decompress_file(src: Path, dst: Path) -> None:
    """Decompress *src* to *dst*, auto-detecting codec from extension."""
    ext = src.suffix.lower()
    codec_map: dict[str, list[str]] = {
        ".xz": ["xz", "-d", "-T0", "-c"],
        ".gz": ["gzip", "-d", "-c"],
        ".bz2": ["bzip2", "-d", "-c"],
        ".zst": ["zstd", "-d", "-c"],
        ".lz4": ["lz4", "-d", "-c"],
    }
    cmd = codec_map.get(ext)
    if cmd is None:
        raise ValueError(f"Unknown compressed extension: {ext}")
    cmd = [*cmd, str(src)]
    log.debug("decompress: %s", cmd)
    with open(dst, "wb") as fh:
        subprocess.run(cmd, stdout=fh, check=True)


# ── Compress ─────────────────────────────────────────────────────────────


def compress_case(
    directory: Path,
    codec: str,
    codec_args: list[str],
    split_mb: int,
) -> int:
    """Compress data files in *directory*.  Returns exit code."""
    files = _find_data_files(directory)
    if not files:
        ext = _codec_ext(codec)
        n_compressed = len(_find_compressed_files(directory, ext))
        if n_compressed > 0:
            print(f"{directory}: already compressed ({n_compressed} {ext} file(s))")
        else:
            print(f"No {'/'.join(_DATA_EXTS)} files found in {directory}")
        return 0

    split_bytes = split_mb * 1024 * 1024
    ext = _codec_ext(codec)
    args_str = " ".join(codec_args) if codec_args else ""
    print(
        f"Compressing {len(files)} file(s) in {directory} "
        f"({codec} {args_str}, split > {split_mb}MB)..."
    )
    print()

    tmpdir = Path(tempfile.mkdtemp(prefix=".compress.", dir=directory))
    try:
        # Phase 1: compress all files into tmpdir in parallel
        workers = min(len(files), os.cpu_count() or 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = {
                pool.submit(
                    _compress_file, f, tmpdir / (f.name + ext), codec, codec_args
                ): f
                for f in files
            }
            for fut in as_completed(futs):
                fut.result()  # propagate exceptions

        # Phase 2: check sizes, split oversized files
        for f in files:
            compressed = tmpdir / (f.name + ext)
            fsize = f.stat().st_size
            csize = compressed.stat().st_size

            if split_bytes <= 0 or csize <= split_bytes:
                print(
                    f"  {f.name:<40s} {fsize // 1024:>6d}K -> "
                    f"{csize // 1024:>6d}K  ({codec})"
                )
            else:
                # Split oversized file
                compressed.unlink()
                print(
                    f"  {f.name}: {csize // 1024}K compressed > "
                    f"{split_bytes // 1024}K limit, splitting..."
                )
                _split_and_compress(f, tmpdir, codec, codec_args, split_bytes, csize)

        # Success — move results into place and remove originals
        for result in tmpdir.iterdir():
            shutil.move(str(result), directory / result.name)
        for f in files:
            f.unlink()
        shutil.rmtree(tmpdir)
    except Exception:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise

    print()
    print("Done. Files are ready to commit.")
    print("plp2gtopt reads compressed and split files transparently.")
    return 0


def _split_and_compress(
    src: Path,
    tmpdir: Path,
    codec: str,
    codec_args: list[str],
    split_bytes: int,
    compressed_size: int,
) -> None:
    """Split *src* into parts that compress below *split_bytes*.

    Uses an iterative approach: estimate the number of parts with a 100%
    safety margin, compress in parallel, then check if any part still
    exceeds the limit.  If so, re-estimate using the actual largest part
    and retry.
    """
    data = src.read_bytes()
    lines = data.split(b"\n")
    ext = _codec_ext(codec)

    # Initial estimate: N = compressed_size / split_bytes * 2.0
    n_parts = max(2, math.ceil(compressed_size / split_bytes * 2.0))

    max_attempts = 5
    for attempt in range(max_attempts):
        compressed_paths = _do_split_compress(
            src.name, data, lines, n_parts, ext, tmpdir, codec, codec_args
        )

        # Check if any part exceeds the limit
        max_csize = max(p.stat().st_size for p in compressed_paths)
        if max_csize <= split_bytes:
            break

        # Re-estimate: scale up based on the worst offender
        new_n = max(n_parts + 1, math.ceil(max_csize / split_bytes * n_parts * 2.0))
        log.info(
            "  %s: part too large (%dK > %dK), retrying with %d parts (attempt %d)",
            src.name,
            max_csize // 1024,
            split_bytes // 1024,
            new_n,
            attempt + 2,
        )
        # Clean up previous attempt
        for p in compressed_paths:
            p.unlink(missing_ok=True)
        n_parts = new_n
    else:
        log.warning(
            "  %s: could not fit all parts under %dMB after %d attempts",
            src.name,
            split_bytes // (1024 * 1024),
            max_attempts,
        )

    # Report final sizes
    for cpath in compressed_paths:
        csize = cpath.stat().st_size
        print(f"    {cpath.name}: {csize // 1024}K")


def _do_split_compress(
    name: str,
    data: bytes,
    lines: list[bytes],
    n_parts: int,
    ext: str,
    tmpdir: Path,
    codec: str,
    codec_args: list[str],
) -> list[Path]:
    """Write *n_parts* raw chunks, compress in parallel, return paths."""
    lines_per_part = math.ceil(len(lines) / n_parts)

    raw_paths: list[Path] = []
    for i in range(n_parts):
        start = i * lines_per_part
        end = min((i + 1) * lines_per_part, len(lines))
        if start >= len(lines):
            break
        chunk = b"\n".join(lines[start:end])
        if i < n_parts - 1:
            chunk += b"\n"
        raw = tmpdir / f"{name}.{i + 1}"
        raw.write_bytes(chunk)
        raw_paths.append(raw)

    # Compress all parts in parallel
    compressed_paths: list[Path] = []
    workers = min(len(raw_paths), os.cpu_count() or 4)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {}
        for raw in raw_paths:
            dst = raw.with_suffix(raw.suffix + ext)
            compressed_paths.append(dst)
            futs[pool.submit(_compress_file, raw, dst, codec, codec_args)] = raw
        for fut in as_completed(futs):
            fut.result()

    # Clean up raw parts
    for raw in raw_paths:
        raw.unlink(missing_ok=True)

    return compressed_paths


# ── Decompress ───────────────────────────────────────────────────────────


def decompress_case(directory: Path) -> int:
    """Decompress compressed files in *directory*.  Returns exit code."""
    # Find all compressed files
    compressed: list[Path] = []
    for f in sorted(directory.iterdir()):
        if f.is_file() and f.suffix.lower() in _COMPRESSED_EXTS:
            compressed.append(f)

    if not compressed:
        n_plain = len(_find_data_files(directory))
        if n_plain > 0:
            print(f"{directory}: already decompressed ({n_plain} data file(s))")
        else:
            print(f"No .xz or data files found in {directory}")
        return 0

    # Classify: single files vs split parts
    split_bases: dict[str, list[Path]] = {}
    single_files: list[Path] = []

    for f in compressed:
        # Match foo.dat.N.xz pattern
        m = re.match(r"^(.+)\.(\d+)(\.[a-z0-9]+)$", f.name)
        if m:
            base = m.group(1)
            split_bases.setdefault(base, []).append(f)
        else:
            single_files.append(f)

    # Sort split parts by part number
    for base, parts in split_bases.items():
        parts.sort(
            key=lambda p: int(
                re.search(r"\.(\d+)\.[a-z0-9]+$", p.name).group(1)  # type: ignore[union-attr]
            )
        )

    n_single = len(single_files)
    n_split = len(split_bases)
    print(
        f"Decompressing in {directory}: "
        f"{n_single} single file(s), {n_split} split file(s)..."
    )
    print()

    tmpdir = Path(tempfile.mkdtemp(prefix=".decompress.", dir=directory))
    try:
        # Phase 1: decompress single files in parallel
        if single_files:
            workers = min(len(single_files), os.cpu_count() or 4)
            targets: list[tuple[Path, Path]] = []
            for f in single_files:
                outname = f.stem  # e.g. "foo.dat.xz" -> "foo.dat"
                targets.append((f, tmpdir / outname))

            with ThreadPoolExecutor(max_workers=workers) as pool:
                futs = {
                    pool.submit(_decompress_file, src, dst): (src, dst)
                    for src, dst in targets
                }
                for fut in as_completed(futs):
                    fut.result()

            for _src, dst in targets:
                sz = dst.stat().st_size
                print(f"  {dst.name:<40s} {sz // 1024:>6d}K")

        # Phase 2: reassemble split files (decompress parts in parallel,
        # then concatenate in order)
        for base in sorted(split_bases):
            parts = split_bases[base]
            outfile = tmpdir / base
            print(f"  {base}: reassembling split parts...")

            # Decompress all parts in parallel
            part_targets = [(p, tmpdir / f"_part_{p.name}") for p in parts]
            workers = min(len(parts), os.cpu_count() or 4)
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futs = {
                    pool.submit(_decompress_file, src, dst): dst
                    for src, dst in part_targets
                }
                for fut in as_completed(futs):
                    fut.result()

            # Concatenate decompressed parts in order
            with open(outfile, "wb") as out_fh:
                for _src, part_tmp in part_targets:
                    with open(part_tmp, "rb") as part_fh:
                        shutil.copyfileobj(part_fh, out_fh)
                    part_tmp.unlink()

            sz = outfile.stat().st_size
            print(f"    -> {base:<38s} {sz // 1024:>6d}K  ({len(parts)} parts)")

        # Success — move results into place and remove originals
        for result in tmpdir.iterdir():
            shutil.move(str(result), directory / result.name)
        for f in compressed:
            f.unlink()
        shutil.rmtree(tmpdir)
    except Exception:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise

    print()
    print("Done. Original files restored.")
    return 0


# ── Entry point ──────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and run compress/decompress."""
    parser = _build_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(levelname)s: %(message)s",
    )

    # Load config
    config_path = args.config or default_config_path()
    cfg = load_config(config_path)

    # --show-config
    if args.show_config:
        for key, value in sorted(cfg.items()):
            print(f"{key} = {value}")
        sys.exit(0)

    # No directory → show help
    if args.directory is None:
        parser.print_help()
        sys.exit(0)

    directory = args.directory.resolve()
    if not directory.is_dir():
        print(f"Error: '{args.directory}' is not a directory", file=sys.stderr)
        sys.exit(1)

    # Merge CLI overrides with config
    codec = args.codec if args.codec is not None else cfg.get("codec", "xz")
    codec_args_str = (
        args.codec_args if args.codec_args is not None else cfg.get("codec_args", "-T0")
    )
    codec_args = codec_args_str.split() if codec_args_str else []
    split_mb = (
        args.split_mb if args.split_mb is not None else int(cfg.get("split_mb", "10"))
    )

    # Dispatch
    if args.decompress:
        rc = decompress_case(directory)
    else:
        rc = compress_case(directory, codec, codec_args, split_mb)

    sys.exit(rc)


if __name__ == "__main__":
    main()
