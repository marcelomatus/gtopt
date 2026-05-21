"""Load PLEXOS's ``t_phase_3`` interval→block layout from a solution
``.accdb`` (or its containing ``RES*.zip[.xz]`` archive).

PLEXOS's CEN PCP daily MT Schedule clusters 168 hourly intervals into
111 representative blocks (24 hourly + 87 aggregated for the days-2-to-7
look-ahead).  The interval→block mapping lives in the solution
database's ``t_phase_3`` table, written when PLEXOS finishes the MT
step.  Reading it lets the gtopt converter reproduce PLEXOS's exact
block distribution instead of reconstructing it heuristically.

Two entry points:

* :func:`load_block_layout_from_accdb` — read ``t_phase_3`` directly
  via ``mdb-export``.  Requires the ``mdbtools`` system package.
* :func:`load_block_layout_from_res_zip` — auto-extract the nested
  ``.accdb`` from a ``RES*.zip[.xz]`` bundle and call the first.

Returns a tuple of tuples: ``layout[k] = (interval_id_1, interval_id_2,
…)`` for each block ``k``, in ``period_id`` order.  Empty tuple when
the source can't be read (caller falls back to uniform aggregation).
"""

from __future__ import annotations

import collections
import csv
import logging
import lzma
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

logger = logging.getLogger(__name__)


def _have_mdb_tools() -> bool:
    return shutil.which("mdb-export") is not None


def load_block_layout_from_accdb(
    accdb_path: Path,
) -> tuple[tuple[int, ...], ...]:
    """Read ``t_phase_3`` and return ``(block_intervals_1, …)``.

    Each ``block_intervals_k`` is a tuple of 1-indexed hourly interval
    ids that PLEXOS grouped into block ``k`` (= ``period_id == k`` in
    the table).  The outer tuple is sorted by ``period_id`` (= block
    id) ascending so the writer can emit them in that order.

    Returns an empty tuple when ``mdb-export`` is unavailable, the
    accdb can't be read, or ``t_phase_3`` is missing / empty.  The
    caller is expected to fall back to uniform hourly aggregation in
    that case.
    """
    if not _have_mdb_tools():
        logger.warning(
            "mdb-export not found on PATH; cannot read t_phase_3 from %s. "
            "Falling back to uniform hourly aggregation.",
            accdb_path,
        )
        return ()
    if not accdb_path.exists():
        logger.warning("accdb not found: %s", accdb_path)
        return ()
    try:
        result = subprocess.run(
            ["mdb-export", str(accdb_path), "t_phase_3"],
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        logger.warning("mdb-export t_phase_3 failed on %s: %s", accdb_path, exc)
        return ()

    block_intervals: dict[int, list[int]] = collections.defaultdict(list)
    reader = csv.reader(result.stdout.splitlines())
    try:
        header = next(reader)
    except StopIteration:
        return ()
    # Expect: interval_id, period_id  (case-insensitive match in case
    # PLEXOS versions differ slightly).
    lower = [h.strip().lower() for h in header]
    try:
        iv_idx = lower.index("interval_id")
        pe_idx = lower.index("period_id")
    except ValueError:
        logger.warning("t_phase_3 header missing interval_id/period_id: %s", header)
        return ()

    for row in reader:
        if len(row) <= max(iv_idx, pe_idx):
            continue
        iv_str = row[iv_idx].strip()
        pe_str = row[pe_idx].strip()
        if not iv_str or not pe_str:
            continue
        try:
            iv = int(iv_str)
            pe = int(pe_str)
        except ValueError:
            continue
        block_intervals[pe].append(iv)

    if not block_intervals:
        return ()

    # Sort blocks by period_id; within each block, sort intervals.
    layout = tuple(
        tuple(sorted(block_intervals[blk])) for blk in sorted(block_intervals)
    )
    logger.info(
        "Loaded PLEXOS block layout from %s: %d blocks across %d intervals",
        accdb_path.name,
        len(layout),
        sum(len(b) for b in layout),
    )
    return layout


def extract_accdb_from_res_zip(res_zip_path: Path) -> Path | None:
    """Unpack the nested ``.accdb`` from a ``RES*.zip[.xz]`` bundle.

    Returns the resolved path on success or ``None`` on any failure
    (and logs the reason at WARNING).  The extracted .accdb lives
    in a tempfile.mkdtemp directory that is intentionally NOT
    cleaned up — the file is large and re-extraction across runs
    is expensive; ``/tmp`` cleanup is good enough.
    """
    if not res_zip_path.exists():
        logger.warning("RES bundle not found: %s", res_zip_path)
        return None

    scratch = Path(tempfile.mkdtemp(prefix="plexos_layout_"))
    plain_zip = res_zip_path
    if res_zip_path.suffix == ".xz":
        plain_zip = scratch / res_zip_path.with_suffix("").name
        try:
            with lzma.open(res_zip_path, "rb") as src, plain_zip.open("wb") as dst:
                dst.write(src.read())
        except (lzma.LZMAError, OSError) as exc:
            logger.warning("could not decompress %s: %s", res_zip_path, exc)
            return None

    try:
        with zipfile.ZipFile(plain_zip) as zf:
            accdb_name = next(
                (n for n in zf.namelist() if n.endswith(".accdb")),
                None,
            )
            if accdb_name is None:
                logger.warning("no .accdb found in %s", res_zip_path)
                return None
            zf.extract(accdb_name, scratch)
    except (zipfile.BadZipFile, OSError) as exc:
        logger.warning("could not open %s: %s", plain_zip, exc)
        return None

    return scratch / accdb_name


def load_block_layout_from_res_zip(
    res_zip_path: Path,
) -> tuple[tuple[int, ...], ...]:
    """Auto-extract the nested ``.accdb`` from a ``RES*.zip[.xz]`` and
    delegate to :func:`load_block_layout_from_accdb`.

    CEN PCP RES bundles ship the .accdb at:
        ``Model <name> Solution/Model <name> Solution.accdb``
    inside the outer zip, optionally wrapped in a .xz layer.
    """
    accdb_path = extract_accdb_from_res_zip(res_zip_path)
    if accdb_path is None:
        return ()
    return load_block_layout_from_accdb(accdb_path)


#: PLEXOS solution-database tables that downstream comparison tools
#: (chiefly :mod:`compare_with_plexos`) repeatedly read via
#: ``mdb-export`` shell-out — slow (each pass costs ~1-3 s of CSV
#: serialization).  When plexos2gtopt extracts the .accdb anyway to
#: read t_phase_3, we mirror these tables as zstd-compressed CSVs
#: into ``<output_dir>/plexos_cache/`` so later compare runs read
#: the cache directly without re-invoking ``mdb-export``.  The
#: cache key is the .accdb path; if the user re-downloads a fresh
#: solution the cache is silently invalidated by the new path.
_CACHED_PLEXOS_TABLES = (
    "t_key",
    "t_data_0",
    "t_membership",
    "t_object",
    "t_property",
    "t_category",
    "t_class",
    "t_phase_3",
    "t_period_0",
    "t_unit",
)


def cache_plexos_tables(
    accdb_path: Path,
    output_dir: Path,
    *,
    tables: tuple[str, ...] = _CACHED_PLEXOS_TABLES,
) -> Path | None:
    """Dump key PLEXOS solution tables to a zstd-compressed CSV
    cache for fast re-reads by downstream tools.

    Writes one ``<table>.csv.zst`` per requested table to
    ``output_dir / "plexos_cache"``.  Returns the cache dir path
    on success, ``None`` on any failure (mdb-tools missing, accdb
    unreadable, output dir not writable, etc).

    Skips tables that are already present in the cache dir AND
    newer than the source .accdb — incremental re-cache is a no-op
    on warm runs.
    """
    if not _have_mdb_tools():
        logger.warning(
            "mdb-export not found; skipping plexos_cache dump for %s",
            accdb_path,
        )
        return None
    if not accdb_path.exists():
        return None

    cache_dir = output_dir / "plexos_cache"
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        logger.warning("cannot create %s: %s", cache_dir, exc)
        return None

    src_mtime = accdb_path.stat().st_mtime
    for table in tables:
        out_path = cache_dir / f"{table}.csv.zst"
        if out_path.exists() and out_path.stat().st_mtime >= src_mtime:
            # Cache is fresh; skip re-extraction.
            continue
        try:
            result = subprocess.run(
                ["mdb-export", str(accdb_path), table],
                capture_output=True,
                text=False,
                check=True,
                timeout=300,
            )
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            logger.warning("mdb-export %s failed: %s", table, exc)
            continue
        # zstd-compress the CSV bytes via subprocess to avoid a hard
        # Python-side zstandard dep (the project already ships zstd
        # binaries through apt — see CLAUDE.md ``apt install zstd``).
        try:
            proc = subprocess.run(
                ["zstd", "-q", "-f", "-19", "-o", str(out_path)],
                input=result.stdout,
                capture_output=True,
                check=True,
                timeout=120,
            )
            _ = proc  # silence "unused" lint
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            logger.warning("zstd compress %s failed: %s", table, exc)
            continue

    logger.info("wrote PLEXOS cache to %s (%d tables)", cache_dir, len(tables))
    return cache_dir


def auto_discover_res_zip(input_bundle: Path) -> Path | None:
    """Look for a sibling ``RES*.zip[.xz]`` matching the bundle date.

    CEN PCP daily bundles are named ``DATOS<DATE>.zip[.xz]`` next to
    ``RES<DATE>.zip[.xz]``.  We replace the leading ``DATOS`` with
    ``RES`` and check whether the sibling exists.  Returns ``None``
    when no matching sibling is found.
    """
    input_bundle = input_bundle.resolve()
    if input_bundle.is_dir():
        candidates = [
            p
            for p in input_bundle.parent.iterdir()
            if p.name.startswith("RES") and (p.suffix in (".zip", ".xz"))
        ]
        return candidates[0] if candidates else None
    # File case: replace DATOS prefix with RES.
    name = input_bundle.name
    if name.startswith("DATOS"):
        sibling = input_bundle.parent / ("RES" + name[len("DATOS") :])
        if sibling.exists():
            return sibling
    return None


__all__ = [
    "auto_discover_res_zip",
    "load_block_layout_from_accdb",
    "load_block_layout_from_res_zip",
]
