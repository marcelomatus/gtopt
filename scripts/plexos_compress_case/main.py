# SPDX-License-Identifier: BSD-3-Clause
"""plexos_compress_case — compress or decompress PLEXOS bundle directories.

Compresses every ``.zip`` file in a directory using a configurable codec
(default: ``xz -T0 -9``) and writes ``<name>.zip.xz`` next to it.  By
default, ``PLEXOS{YYYYMMDD}.zip`` *outer wrappers* (which only
``store``-bundle the two inner ``DATOS{date}.zip`` and ``RES{date}.zip``
payloads) are unwrapped first: the inner zips are extracted and
compressed individually, and the wrapper is dropped.  This shrinks
repo storage by ~30 MB per CEN PCP bundle without losing any data
(round-trip recovers the inner zips byte-exact, and the wrapper can be
rebuilt with ``zip -0`` at any time).

With ``--decompress``, restores the original ``.zip`` files (and, with
``--rewrap``, repackages each ``PLEXOS{date}`` family back into its
outer wrapper).

All operations use a temporary directory so an interruption leaves the
original files intact.

The companion to ``plp_compress_case`` for ``.dat``/``.csv`` PLP cases —
see ``support/plp_*/`` for examples of that format.  This tool targets
the PLEXOS PCP bundles in ``support/plexos_pcp_2026-04-22/`` (CEN Programa de
Coordinación de Predespacho).

Configuration
-------------
User preferences are stored in ``~/.gtopt.conf`` under
``[plexos_compress_case]``.  See ``plexos_compress_case --show-config``.

Examples::

    plexos_compress_case support/plexos_pcp_2026-04-22
    plexos_compress_case support/plexos_pcp_2026-04-22 --no-unwrap
    plexos_compress_case --decompress support/plexos_pcp_2026-04-22
    plexos_compress_case --decompress support/plexos_pcp_2026-04-22 --rewrap
    plexos_compress_case support/plexos_pcp_2026-04-22 --codec zstd --codec-args "-19"
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from gtopt_config import get_version

from ._config import default_config_path, load_config

__version__ = get_version()

log = logging.getLogger(__name__)

# File extensions considered for compression.
_DATA_EXTS = (".zip",)
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

# Match the CEN PCP outer-wrapper convention: PLEXOS<8 digits>.zip.
_OUTER_WRAPPER_RE = re.compile(r"^(PLEXOS)(\d{8})\.zip$")

_DESCRIPTION = """\
Compress or decompress PLEXOS bundle directories.

Compresses .zip files using a configurable codec (default: xz -T0 -9).
By default, PLEXOS{YYYYMMDD}.zip outer wrappers are unwrapped first
(inner DATOS{date}.zip / RES{date}.zip are extracted, the wrapper is
dropped — round-trip recovers them byte-exact).

With --decompress, restores original .zip files (and with --rewrap,
rebuilds the PLEXOS{date}.zip outer wrappers).

Examples:
  plexos_compress_case support/plexos_pcp_2026-04-22
  plexos_compress_case support/plexos_pcp_2026-04-22 --no-unwrap
  plexos_compress_case --decompress support/plexos_pcp_2026-04-22
  plexos_compress_case --decompress support/plexos_pcp_2026-04-22 --rewrap
  plexos_compress_case support/plexos_pcp_2026-04-22 --codec zstd --codec-args "-19"
"""


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="plexos_compress_case",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=None,
        metavar="DIRECTORY",
        help="PLEXOS bundle directory to compress or decompress.",
    )
    parser.add_argument(
        "-d",
        "--decompress",
        action="store_true",
        help="Decompress mode: restore original .zip files.",
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
            "Extra arguments for the codec command (e.g. '-T0 -9' for xz). "
            "Default from config or '-T0 -9'."
        ),
    )
    unwrap_group = parser.add_mutually_exclusive_group()
    unwrap_group.add_argument(
        "--unwrap",
        dest="unwrap",
        action="store_true",
        default=None,
        help=(
            "On compress: extract PLEXOS{date}.zip outer wrappers into "
            "their inner DATOS/RES zips and drop the wrapper (default)."
        ),
    )
    unwrap_group.add_argument(
        "--no-unwrap",
        dest="unwrap",
        action="store_false",
        help=(
            "On compress: keep PLEXOS{date}.zip outer wrappers verbatim "
            "(do not extract inner zips)."
        ),
    )
    parser.add_argument(
        "--rewrap",
        action="store_true",
        help=(
            "On decompress: re-bundle each unwrapped DATOS{date}.zip + "
            "RES{date}.zip back into a PLEXOS{date}.zip outer wrapper."
        ),
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
        "--no-color",
        action="store_true",
        default=False,
        help="disable coloured output",
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
    """Find uncompressed .zip files in *directory* (non-recursive)."""
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
    """Find compressed files in *directory* whose names end with *ext*."""
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


def _is_outer_wrapper(zip_path: Path) -> bool:
    """Detect the CEN PCP ``PLEXOS{YYYYMMDD}.zip`` outer-wrapper convention.

    Heuristic: filename matches the pattern AND the archive contains
    exactly two members that are themselves ``.zip`` files.  A mismatch
    (e.g. extra files inside) defaults to *not unwrap* so the user keeps
    the data intact.
    """
    if _OUTER_WRAPPER_RE.match(zip_path.name) is None:
        return False
    try:
        with zipfile.ZipFile(zip_path) as zf:
            names = zf.namelist()
    except (zipfile.BadZipFile, OSError) as exc:
        log.warning("not a valid zip: %s (%s)", zip_path, exc)
        return False
    if len(names) != 2:
        return False
    return all(n.lower().endswith(".zip") for n in names)


def _unwrap_outer(zip_path: Path, dest_dir: Path) -> list[Path]:
    """Extract the two inner zips from a PLEXOS{date}.zip wrapper.

    Returns the paths of the extracted inner zips inside *dest_dir*.
    """
    extracted: list[Path] = []
    with zipfile.ZipFile(zip_path) as zf:
        for member in zf.namelist():
            zf.extract(member, dest_dir)
            extracted.append(dest_dir / member)
    return extracted


# ── Compress ─────────────────────────────────────────────────────────────


def compress_case(
    directory: Path,
    codec: str,
    codec_args: list[str],
    unwrap: bool,
) -> int:
    """Compress .zip files in *directory*.  Returns exit code."""
    files = _find_data_files(directory)
    if not files:
        ext = _codec_ext(codec)
        n_compressed = len(_find_compressed_files(directory, f".zip{ext}"))
        if n_compressed > 0:
            print(f"{directory}: already compressed ({n_compressed} .zip{ext} file(s))")
        else:
            print(f"No .zip files found in {directory}")
        return 0

    ext = _codec_ext(codec)
    args_str = " ".join(codec_args) if codec_args else ""
    print(
        f"Compressing {len(files)} zip(s) in {directory} "
        f"({codec} {args_str}, unwrap={'yes' if unwrap else 'no'})..."
    )
    print()

    tmpdir = Path(tempfile.mkdtemp(prefix=".compress.", dir=directory))
    originals_to_remove: list[Path] = []
    try:
        # Phase 1: optionally unwrap PLEXOS{date}.zip outer bundles.
        # Each wrapper expands into its inner zips, which then enter the
        # normal per-zip compression pipeline.
        compress_targets: list[Path] = []
        for f in files:
            if unwrap and _is_outer_wrapper(f):
                inner = _unwrap_outer(f, tmpdir)
                inner_summary = ", ".join(p.name for p in inner)
                print(
                    f"  {f.name}: unwrapped → {len(inner)} inner zip(s) "
                    f"[{inner_summary}]"
                )
                compress_targets.extend(inner)
                originals_to_remove.append(f)
            else:
                # Copy in so all source paths sit under tmpdir for the
                # parallel phase below (keeps the cleanup story simple).
                staged = tmpdir / f.name
                shutil.copy2(f, staged)
                compress_targets.append(staged)
                originals_to_remove.append(f)

        # Phase 2: compress every target in parallel.
        workers = min(len(compress_targets), os.cpu_count() or 4)
        compressed_paths: list[tuple[Path, Path]] = []
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = {}
            for src in compress_targets:
                dst = src.with_name(src.name + ext)
                compressed_paths.append((src, dst))
                futs[pool.submit(_compress_file, src, dst, codec, codec_args)] = src
            for fut in as_completed(futs):
                fut.result()

        # Report + drop the now-redundant uncompressed staged zips.
        for src, dst in compressed_paths:
            fsize = src.stat().st_size
            csize = dst.stat().st_size
            print(
                f"  {src.name:<40s} {fsize // 1024:>6d}K -> "
                f"{csize // 1024:>6d}K  ({codec})"
            )
            src.unlink()

        # Phase 3: move compressed outputs into place + remove originals.
        for _src, dst in compressed_paths:
            shutil.move(str(dst), directory / dst.name)
        for f in originals_to_remove:
            f.unlink()
        shutil.rmtree(tmpdir)
    except Exception:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise

    print()
    print("Done. Files are ready to commit.")
    print("Recover originals with: plexos_compress_case --decompress DIR")
    return 0


# ── Decompress ───────────────────────────────────────────────────────────


def _group_by_plexos_date(zip_paths: list[Path]) -> dict[str, list[Path]]:
    """Group decompressed inner zips by ``PLEXOS{YYYYMMDD}`` date stem.

    Matches ``DATOS{date}.zip`` and ``RES{date}.zip``; returns the
    mapping ``{date_stem -> [datos_zip, res_zip]}`` for every complete
    pair found.  Incomplete pairs (e.g. a stray ``DATOS`` without its
    ``RES``) are skipped silently — the caller will then emit them as
    individual files instead.
    """
    inner_re = re.compile(r"^(DATOS|RES)(\d{8})\.zip$")
    by_date: dict[str, dict[str, Path]] = {}
    for p in zip_paths:
        m = inner_re.match(p.name)
        if m is None:
            continue
        kind, date = m.group(1), m.group(2)
        by_date.setdefault(date, {})[kind] = p
    pairs: dict[str, list[Path]] = {}
    for date, parts in by_date.items():
        if "DATOS" in parts and "RES" in parts:
            pairs[date] = [parts["DATOS"], parts["RES"]]
    return pairs


def _rewrap_pair(
    date: str,
    datos_zip: Path,
    res_zip: Path,
    out_zip: Path,
) -> None:
    """Re-bundle the two inner zips into a PLEXOS{date}.zip wrapper.

    Matches the CEN convention: ``store`` (no recompression) so the
    wrapper is exactly the concatenation overhead — the same shape we
    received from the CEN portal.
    """
    log.debug("rewrap: %s + %s -> %s", datos_zip.name, res_zip.name, out_zip)
    with zipfile.ZipFile(
        out_zip, "w", compression=zipfile.ZIP_STORED, allowZip64=True
    ) as zf:
        zf.write(datos_zip, arcname=f"DATOS{date}.zip")
        zf.write(res_zip, arcname=f"RES{date}.zip")


def decompress_case(directory: Path, rewrap: bool) -> int:
    """Decompress compressed files in *directory*.  Returns exit code."""
    compressed: list[Path] = []
    for f in sorted(directory.iterdir()):
        if f.is_file() and f.suffix.lower() in _COMPRESSED_EXTS:
            compressed.append(f)

    if not compressed:
        n_plain = len(_find_data_files(directory))
        if n_plain > 0:
            print(f"{directory}: already decompressed ({n_plain} .zip file(s))")
        else:
            print(f"No compressed or .zip files found in {directory}")
        return 0

    print(
        f"Decompressing {len(compressed)} file(s) in {directory} "
        f"(rewrap={'yes' if rewrap else 'no'})..."
    )
    print()

    tmpdir = Path(tempfile.mkdtemp(prefix=".decompress.", dir=directory))
    try:
        # Phase 1: decompress every file in parallel.
        targets: list[tuple[Path, Path]] = []
        for f in compressed:
            # foo.zip.xz -> foo.zip; bar.dat.xz -> bar.dat
            outname = f.stem
            targets.append((f, tmpdir / outname))

        workers = min(len(compressed), os.cpu_count() or 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = {
                pool.submit(_decompress_file, src, dst): dst for src, dst in targets
            }
            for fut in as_completed(futs):
                fut.result()

        for _src, dst in targets:
            sz = dst.stat().st_size
            print(f"  {dst.name:<40s} {sz // 1024:>6d}K")

        decompressed_paths = [dst for _src, dst in targets]

        # Phase 2: optionally rewrap matching DATOS+RES pairs.
        if rewrap:
            pairs = _group_by_plexos_date(decompressed_paths)
            for date, (datos_zip, res_zip) in sorted(pairs.items()):
                wrapper = tmpdir / f"PLEXOS{date}.zip"
                _rewrap_pair(date, datos_zip, res_zip, wrapper)
                wsize = wrapper.stat().st_size
                print(
                    f"  {wrapper.name}: rewrapped from "
                    f"{datos_zip.name} + {res_zip.name} ({wsize // 1024}K)"
                )
                # Drop the now-superseded inner zips.
                datos_zip.unlink()
                res_zip.unlink()

        # Phase 3: move outputs into place + drop the compressed originals.
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

    config_path = args.config or default_config_path()
    cfg = load_config(config_path)

    if args.show_config:
        for key, value in sorted(cfg.items()):
            print(f"{key} = {value}")
        sys.exit(0)

    if args.directory is None:
        parser.print_help()
        sys.exit(0)

    directory = args.directory.resolve()
    if not directory.is_dir():
        print(f"Error: '{args.directory}' is not a directory", file=sys.stderr)
        sys.exit(1)

    codec = args.codec if args.codec is not None else cfg.get("codec", "xz")
    codec_args_str = (
        args.codec_args
        if args.codec_args is not None
        else cfg.get("codec_args", "-T0 -9")
    )
    codec_args = codec_args_str.split() if codec_args_str else []
    if args.unwrap is None:
        unwrap = cfg.get("unwrap", "true").strip().lower() not in (
            "0",
            "false",
            "no",
            "off",
        )
    else:
        unwrap = args.unwrap

    if args.decompress:
        rc = decompress_case(directory, args.rewrap)
    else:
        rc = compress_case(directory, codec, codec_args, unwrap)

    sys.exit(rc)


if __name__ == "__main__":
    main()
