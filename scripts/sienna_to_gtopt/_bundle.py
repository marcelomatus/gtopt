"""Extract the bundled Sienna .tar.zst archives into a tempdir.

The data archives live in ``integration_test/data/sienna_<size>.tar.zst``
and are checked into git as pre-compressed snapshots of the upstream
NREL-Sienna ``PowerSystems.jl`` test data.  The converter MUST NOT
expand them onto disk permanently — per the package contract they are
extracted lazily into ``$TMPDIR`` (or ``~/tmp/`` when that's unset)
and the OS reclaims the temp dir.
"""

from __future__ import annotations

import functools
import os
import tarfile
import tempfile
from pathlib import Path

try:
    import zstandard
except ImportError:  # pragma: no cover — zstandard is in scripts/pyproject.toml
    zstandard = None  # type: ignore[assignment]

# Resolve the repo-root-relative data dir once at import time.  This
# module lives at ``scripts/sienna_to_gtopt/_bundle.py``; the bundle
# files are two levels up at ``integration_test/data/``.
_REPO_ROOT = Path(__file__).resolve().parents[2]
_DATA_DIR = _REPO_ROOT / "integration_test" / "data"

BUNDLES: dict[str, Path] = {
    "5bus": _DATA_DIR / "sienna_5bus.tar.zst",
    "14bus": _DATA_DIR / "sienna_14bus.tar.zst",
}


def available_bundles() -> list[str]:
    """Names of bundles whose .tar.zst file is present on disk."""

    return sorted(name for name, path in BUNDLES.items() if path.exists())


def _tempdir_base() -> Path:
    """Return a writable base dir for extraction.

    Honours ``$TMPDIR`` (POSIX) when set; falls back to ``~/tmp/`` so we
    don't write under ``/tmp/`` (project policy forbids it).
    """

    env = os.environ.get("TMPDIR")
    if env:
        base = Path(env)
        base.mkdir(parents=True, exist_ok=True)
        return base
    home_tmp = Path.home() / "tmp"
    home_tmp.mkdir(parents=True, exist_ok=True)
    return home_tmp


def _decompress_zst(src: Path, dst: Path) -> None:
    if zstandard is None:
        raise RuntimeError(
            "zstandard is required to extract Sienna bundles; install with "
            "`pip install zstandard` or add the scripts/ dev dependencies."
        )
    dctx = zstandard.ZstdDecompressor()
    with src.open("rb") as fh_in, dst.open("wb") as fh_out:
        dctx.copy_stream(fh_in, fh_out)


@functools.lru_cache(maxsize=None)
def extract_bundle(name: str = "5bus") -> Path:
    """Extract the named bundle and return the extracted ``sienna_<size>/`` dir.

    The result is cached per process so repeated calls hit the same
    tempdir.  Each call from a fresh process creates a new tempdir
    (the OS cleans these up on reboot).
    """

    if name not in BUNDLES:
        raise KeyError(f"unknown sienna bundle: {name!r}; available={list(BUNDLES)}")
    src = BUNDLES[name]
    if not src.exists():
        raise FileNotFoundError(f"bundle not found on disk: {src}")

    base = _tempdir_base()
    tempdir = Path(tempfile.mkdtemp(prefix=f"sienna_{name}_", dir=str(base)))

    # Decompress .tar.zst → .tar in tempdir, then extract.  Using
    # disk-roundtrip rather than a stream so zstandard's input chunking
    # can't deadlock against tarfile's read pattern.
    tar_path = tempdir / f"sienna_{name}.tar"
    _decompress_zst(src, tar_path)

    with tarfile.open(tar_path, mode="r") as tar:
        # Python 3.12 deprecated the implicit `filter` arg; pin "data" so
        # we get the strict-but-not-pedantic policy (no abs paths, no
        # symlinks outside the dest dir).
        try:
            tar.extractall(path=tempdir, filter="data")
        except TypeError:
            tar.extractall(path=tempdir)
    tar_path.unlink()
    return tempdir / f"sienna_{name}"
