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
import re
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


def infer_horizon_days_from_input(xml_path: Path) -> int | None:
    """Infer the solve horizon length (days) from the PLEXOS INPUT.

    Used for the uniform-hourly fallback when no solution ``.accdb`` is
    available: the per-period block grid is then uniform 1-hour, but we
    still want the right NUMBER of blocks (``days × 24``).

    The CEN PCP input encodes the horizon ONLY in the ``Horizon`` object's
    NAME (e.g. ``Coordinador_diario_1H_7d`` → 7 days); the explicit length
    properties ship as ``0`` and the per-class CSVs carry a *superset* of
    days, so the name is the authoritative signal.  Parses the first
    ``<digits>d`` token in the Horizon object name.

    Returns the inferred day count (1..366), or ``None`` when no Horizon
    object / no parseable ``Nd`` token is found.
    """
    if not xml_path.is_file():
        return None
    try:
        text = xml_path.read_text(encoding="latin1", errors="ignore")
    except OSError:
        return None
    # Horizon class id (from the <t_class> table).
    horizon_cls: str | None = None
    for m in re.finditer(r"<t_class>(.*?)</t_class>", text, re.DOTALL):
        blk = m.group(1)
        nm = re.search(r"<name>([^<]+)</name>", blk)
        cid = re.search(r"<class_id>(\d+)</class_id>", blk)
        if nm and cid and nm.group(1) == "Horizon":
            horizon_cls = cid.group(1)
            break
    if horizon_cls is None:
        return None
    # First Horizon-class object name with an ``Nd`` token.
    for m in re.finditer(r"<t_object>(.*?)</t_object>", text, re.DOTALL):
        blk = m.group(1)
        cid = re.search(r"<class_id>(\d+)</class_id>", blk)
        nm = re.search(r"<name>([^<]+)</name>", blk)
        if not (cid and nm and cid.group(1) == horizon_cls):
            continue
        day_tok = re.search(r"(\d+)\s*[dD]\b", nm.group(1))
        if day_tok:
            days = int(day_tok.group(1))
            if 1 <= days <= 366:
                logger.info(
                    "inferred horizon = %d days from Horizon name %r",
                    days,
                    nm.group(1),
                )
                return days
    return None


def compute_day_ending_blocks(
    block_layout: tuple[tuple[int, ...], ...],
) -> tuple[int, ...]:
    """For each calendar day in the horizon, return the 1-indexed block
    whose intervals END inside (or AT the end of) that day.

    Used by the Gas_MaxOpDay consolidator (and any other per-day daily-sum
    constraint with PLEXOS-side per-day RHS profile) to map gtopt's
    daily-row index ``d`` onto the block where gtopt's UC LP flushes
    that day's accumulator — see
    ``source/user_constraint_lp.cpp:1027`` for the lookup site
    (``m_rhs_.optval(stage.uid(), day_end_block)``).

    The returned tuple has length ``ceil(max_hour / 24)`` ≈ horizon_days,
    with entry ``[d]`` = the block_id whose ``max(intervals) ≥ (d+1)*24``
    is smallest (the LAST block whose hours fall on day ``d``).

    For a uniform 168-block hourly layout, this yields
    ``(24, 48, 72, 96, 120, 144, 168)``.  For the CEN PCP 111-block
    PLEXOS layout, the day boundaries fall at the specific block_ids
    that PLEXOS itself ends each 24h day on (block intervals are
    chronological and may have non-uniform durations).

    Returns ``()`` when ``block_layout`` is empty (caller is expected to
    fall back to uniform hourly day-ending blocks `24*(d+1)`).
    """
    if not block_layout:
        return ()
    # block_id is 1-indexed by load_block_layout_from_accdb contract.
    max_hour_per_block: list[int] = [
        max(intervals) if intervals else 0 for intervals in block_layout
    ]
    horizon_last_hour = max(max_hour_per_block) if max_hour_per_block else 0
    n_days = (horizon_last_hour + 23) // 24
    out: list[int] = []
    for d in range(n_days):
        cutoff = (d + 1) * 24
        # Largest block_id whose max(intervals) is <= cutoff.
        last_block_id = 0
        for block_zero_idx, mh in enumerate(max_hour_per_block):
            if mh <= cutoff:
                last_block_id = block_zero_idx + 1  # 1-indexed
        if last_block_id > 0:
            out.append(last_block_id)
    return tuple(out)


def parse_user_block_layout(spec: str) -> tuple[tuple[int, ...], ...]:
    """Build the interval→block grouping from a USER block-layout spec.

    Lets the user define the block structure WITHOUT a PLEXOS solution
    ``.accdb``.  ``spec`` is either:

    * **a path to a CSV file** with a header and a ``duration`` (or
      ``hours`` / ``dur``) column — one row per block, in chronological
      order.  An optional ``block_uid`` (or ``block`` / ``uid``) column
      sets the order explicitly; otherwise row order is used.  Example::

          block_uid,duration
          1,1
          2,4
          3,2

    * **an inline mapping string** ``"{1:1,2:4,3:2}"`` (``block_uid:duration``)
      or a bare comma list ``"1,4,2"`` (durations in block order).

    ``duration`` is the number of consecutive 1-hour intervals the block
    spans.  Returns the chronological interval grouping (1-indexed hour
    ids) in the same shape as :func:`load_block_layout_from_accdb`:
    block ``k`` covers the next ``duration[k]`` hours, and
    ``sum(durations)`` is the horizon length in hours.

    Raises ``ValueError`` when no positive durations can be parsed.
    """
    durations: list[float] = []
    path = Path(spec)
    if path.is_file():
        with path.open("r", encoding="utf-8", newline="") as fh:
            reader = csv.DictReader(fh)
            cols = {(c or "").strip().lower(): c for c in (reader.fieldnames or [])}
            dur_col = next(
                (cols[k] for k in ("duration", "hours", "dur") if k in cols), None
            )
            blk_col = next(
                (cols[k] for k in ("block_uid", "block", "uid") if k in cols), None
            )
            if dur_col is None:
                raise ValueError(
                    f"block-layout CSV {spec!r} needs a 'duration' column "
                    f"(got {reader.fieldnames})"
                )
            rows: list[tuple[float, float]] = []
            for i, row in enumerate(reader):
                try:
                    dur = float(row[dur_col])
                except (TypeError, ValueError):
                    continue
                order = float(row[blk_col]) if blk_col and row.get(blk_col) else i
                rows.append((order, dur))
            durations = [d for _, d in sorted(rows)]
    else:
        # Inline string: dict form "{1:1,2:4}" or bare list "1,4,2".
        body = spec.strip().lstrip("{").rstrip("}")
        pairs: list[tuple[float, float]] = []
        for idx, tok in enumerate(body.split(",")):
            tok = tok.strip()
            if not tok:
                continue
            if ":" in tok:
                key, val = tok.split(":", 1)
                pairs.append((float(key), float(val)))
            else:
                pairs.append((float(idx), float(tok)))
        durations = [d for _, d in sorted(pairs)]

    layout: list[tuple[int, ...]] = []
    hour = 1
    for dur in durations:
        n = int(round(dur))
        if n <= 0:
            continue
        layout.append(tuple(range(hour, hour + n)))
        hour += n
    if not layout:
        raise ValueError(f"block-layout spec {spec!r} yielded no positive durations")
    logger.info(
        "Loaded USER block layout from %s: %d blocks across %d hours",
        "CSV" if path.is_file() else "inline spec",
        len(layout),
        sum(len(b) for b in layout),
    )
    return tuple(layout)


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
        # Level 3 (fast), NOT 19: this is a transient re-run cache, and
        # ``-19`` on the multi-hundred-MB ``t_data_0`` solution table took
        # >120 s and timed out into a half-written, undecompressable file.
        try:
            proc = subprocess.run(
                ["zstd", "-q", "-f", "-3", "-o", str(out_path)],
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


def _read_cached_csv(cache_dir: Path, table: str) -> list[dict[str, str]] | None:
    """Read a zstd-compressed cache file written by ``cache_plexos_tables``.

    Returns ``None`` when the file is missing or unreadable; otherwise
    returns the parsed CSV as a list-of-dicts (one per row).
    """
    import io

    path = cache_dir / f"{table}.csv.zst"
    if not path.is_file():
        return None
    try:
        proc = subprocess.run(
            ["zstd", "-dc", str(path)],
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
        )
    except (
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        FileNotFoundError,
    ) as exc:
        logger.warning("zstd -dc %s failed: %s", path, exc)
        return None
    return list(csv.DictReader(io.StringIO(proc.stdout)))


def extract_storage_solution_efin(
    cache_dir: Path,
) -> dict[str, float] | None:
    """Read PLEXOS End Volume (property 646) at the LAST horizon period
    for every Storage object in the solution ``.accdb`` cache.

    Returns a mapping ``{storage_name: last_period_end_volume_CMD}`` so
    callers can pin gtopt's ``Reservoir.efin`` to the value PLEXOS
    actually achieved (instead of the loose operational floor from
    ``Hydro_MinVolume.csv``).  Volumes are in CMD = cumec·day (PLEXOS
    ``t_unit`` id 24).

    Returns ``None`` when the cache is missing any of the required
    tables (``t_object``, ``t_class``, ``t_membership``, ``t_key``,
    ``t_data_0``).  Reservoirs absent from the solution are silently
    omitted; callers should fall back to the CSV-derived floor for
    those.
    """
    objects = _read_cached_csv(cache_dir, "t_object")
    classes = _read_cached_csv(cache_dir, "t_class")
    members = _read_cached_csv(cache_dir, "t_membership")
    keys = _read_cached_csv(cache_dir, "t_key")
    data0 = _read_cached_csv(cache_dir, "t_data_0")
    if (
        objects is None
        or classes is None
        or members is None
        or keys is None
        or data0 is None
    ):
        return None

    storage_cls_id = next(
        (c["class_id"] for c in classes if c["name"] == "Storage"),
        None,
    )
    if storage_cls_id is None:
        return None

    storage_oids: dict[str, str] = {  # oid → name
        o["object_id"]: o["name"]
        for o in objects
        if o.get("class_id") == storage_cls_id
    }
    if not storage_oids:
        return None

    # All memberships whose CHILD is one of our Storage objects (any
    # collection — PLEXOS exposes End Volume on multiple Storage
    # memberships, the prop_id 646 lookup below filters to the right
    # ones).
    oid_by_mid: dict[str, str] = {}
    for m in members:
        cid = m.get("child_object_id", "")
        if cid in storage_oids:
            oid_by_mid[m["membership_id"]] = cid

    # Keys for property_id 646 (End Volume).
    ev_kid_to_oid: dict[str, str] = {}
    for k in keys:
        if k.get("property_id") == "646" and k.get("membership_id") in oid_by_mid:
            ev_kid_to_oid[k["key_id"]] = oid_by_mid[k["membership_id"]]
    if not ev_kid_to_oid:
        return {}

    # Walk t_data and keep the LAST-period value per storage.
    last_period: dict[str, int] = {}
    last_value: dict[str, float] = {}
    for d in data0:
        kid = d.get("key_id", "")
        oid = ev_kid_to_oid.get(kid)
        if oid is None:
            continue
        try:
            p = int(d.get("period_id", "0"))
            v = float(d.get("value", "0"))
        except (TypeError, ValueError):
            continue
        if p > last_period.get(oid, -1):
            last_period[oid] = p
            last_value[oid] = v

    return {storage_oids[oid]: v for oid, v in last_value.items()}


def extract_storage_volume_trajectory(
    cache_dir: Path,
) -> dict[str, dict[int, float]] | None:
    """Read PLEXOS per-period ``End Volume`` (pid 646) for every Storage.

    Returns ``{storage_name: {period_id: volume_CMD}}``.  Unlike
    :func:`extract_storage_solution_efin` which keeps only the LAST
    period's value, this returns the full per-period trajectory so
    callers can pin gtopt's reservoir volume to PLEXOS's solved
    storage curve at every block (forcing drain TIMING to match, not
    just the integrated total).

    Returns ``None`` if any required table is missing.
    """
    objects = _read_cached_csv(cache_dir, "t_object")
    classes = _read_cached_csv(cache_dir, "t_class")
    members = _read_cached_csv(cache_dir, "t_membership")
    keys = _read_cached_csv(cache_dir, "t_key")
    data0 = _read_cached_csv(cache_dir, "t_data_0")
    if (
        objects is None
        or classes is None
        or members is None
        or keys is None
        or data0 is None
    ):
        return None

    sto_cls_id = next((c["class_id"] for c in classes if c["name"] == "Storage"), None)
    if sto_cls_id is None:
        return None

    sto_oids = {
        o["object_id"]: o["name"] for o in objects if o.get("class_id") == sto_cls_id
    }

    mid_to_oid: dict[str, str] = {}
    for m in members:
        cid = m.get("child_object_id", "")
        if cid in sto_oids:
            mid_to_oid[m["membership_id"]] = cid

    # Property 646 = End Volume (CMD per period)
    ev_kid_to_oid: dict[str, str] = {}
    for k in keys:
        if k.get("property_id") == "646" and k.get("membership_id") in mid_to_oid:
            ev_kid_to_oid[k["key_id"]] = mid_to_oid[k["membership_id"]]

    out: dict[str, dict[int, float]] = {}
    for d in data0:
        kid = d.get("key_id", "")
        oid = ev_kid_to_oid.get(kid)
        if oid is None:
            continue
        try:
            p = int(d.get("period_id", "0"))
            v = float(d.get("value", "0"))
        except (TypeError, ValueError):
            continue
        out.setdefault(sto_oids[oid], {})[p] = v

    return out


def extract_generator_generation_per_period(
    cache_dir: Path,
) -> dict[str, dict[int, float]] | None:
    """Read PLEXOS per-period ``Generation`` (pid 2) for every Generator.

    Returns ``{generator_name: {period_id: MW_dispatched}}``.  This is
    the actual per-period MW PLEXOS dispatched in its solution — the
    tightest possible curve-fit envelope.  Callers can use this as a
    hard per-block pmax cap on hydro generators to force gtopt's LP
    to match PLEXOS dispatch exactly (sub-MIP equivalent without
    running MIP).
    """
    objects = _read_cached_csv(cache_dir, "t_object")
    classes = _read_cached_csv(cache_dir, "t_class")
    members = _read_cached_csv(cache_dir, "t_membership")
    keys = _read_cached_csv(cache_dir, "t_key")
    data0 = _read_cached_csv(cache_dir, "t_data_0")
    if (
        objects is None
        or classes is None
        or members is None
        or keys is None
        or data0 is None
    ):
        return None

    gen_cls_id = next(
        (c["class_id"] for c in classes if c["name"] == "Generator"), None
    )
    if gen_cls_id is None:
        return None

    gen_oids = {
        o["object_id"]: o["name"] for o in objects if o.get("class_id") == gen_cls_id
    }

    mid_to_oid: dict[str, str] = {}
    for m in members:
        cid = m.get("child_object_id", "")
        if cid in gen_oids:
            mid_to_oid[m["membership_id"]] = cid

    # Property 2 = Generation (MW per period)
    gen_kid_to_oid: dict[str, str] = {}
    for k in keys:
        if k.get("property_id") == "2" and k.get("membership_id") in mid_to_oid:
            gen_kid_to_oid[k["key_id"]] = mid_to_oid[k["membership_id"]]

    out: dict[str, dict[int, float]] = {}
    for d in data0:
        kid = d.get("key_id", "")
        oid = gen_kid_to_oid.get(kid)
        if oid is None:
            continue
        try:
            p = int(d.get("period_id", "0"))
            v = float(d.get("value", "0"))
        except (TypeError, ValueError):
            continue
        out.setdefault(gen_oids[oid], {})[p] = v

    return out


def extract_generator_commit_per_period(
    cache_dir: Path,
) -> dict[str, dict[int, float]] | None:
    """Read PLEXOS per-period `Units Generating` (property id 7) for every
    Generator in the solution ``.accdb`` cache.

    Returns ``{generator_name: {period_id: units_generating}}``.  The
    inner value is the number of units PLEXOS committed at that period:
    0 means the generator was OFF, 1+ means N units committed (multi-
    unit Generator like RUCUE has 2 max).  Callers can use this to
    override gtopt's ``pmax_profile`` and force the LP to follow
    PLEXOS's commitment schedule (useful to remove
    cascade-compounding hydro over-dispatch caused by the LP-relax
    not making the OFF decisions PLEXOS's MIP made).

    Returns ``None`` when the cache is missing any required table.
    """
    objects = _read_cached_csv(cache_dir, "t_object")
    classes = _read_cached_csv(cache_dir, "t_class")
    members = _read_cached_csv(cache_dir, "t_membership")
    keys = _read_cached_csv(cache_dir, "t_key")
    data0 = _read_cached_csv(cache_dir, "t_data_0")
    if (
        objects is None
        or classes is None
        or members is None
        or keys is None
        or data0 is None
    ):
        return None

    gen_cls_id = next(
        (c["class_id"] for c in classes if c["name"] == "Generator"), None
    )
    if gen_cls_id is None:
        return None

    gen_oids: dict[str, str] = {  # oid → name
        o["object_id"]: o["name"] for o in objects if o.get("class_id") == gen_cls_id
    }
    if not gen_oids:
        return None

    mid_to_oid: dict[str, str] = {}
    for m in members:
        cid = m.get("child_object_id", "")
        if cid in gen_oids:
            mid_to_oid[m["membership_id"]] = cid

    # Property 7 = Units Generating (per-period commitment state)
    ug_kid_to_oid: dict[str, str] = {}
    for k in keys:
        if k.get("property_id") == "7" and k.get("membership_id") in mid_to_oid:
            ug_kid_to_oid[k["key_id"]] = mid_to_oid[k["membership_id"]]
    if not ug_kid_to_oid:
        return {}

    out: dict[str, dict[int, float]] = {}
    for d in data0:
        kid = d.get("key_id", "")
        oid = ug_kid_to_oid.get(kid)
        if oid is None:
            continue
        try:
            p = int(d.get("period_id", "0"))
            v = float(d.get("value", "0"))
        except (TypeError, ValueError):
            continue
        gen_name = gen_oids[oid]
        out.setdefault(gen_name, {})[p] = v

    return out


def extract_fuel_offtake_caps(
    cache_dir: Path,
) -> dict[str, tuple[float, float]] | None:
    """Read PLEXOS ``FueMaxOff*`` Constraint RHS values from the cached
    solution tables.

    PLEXOS-CEN-PCP creates ``FueMaxOffWeek_<fuel>`` and
    ``FueMaxOffDay_<fuel>`` Constraint objects ONLY in the solution
    ``.accdb`` (the constraints are synthesised by PLEXOS at solve
    time from contractual data that isn't in the input
    ``DBSEN_PRGDIARIO.xml``).  These constraints cap the total fuel
    offtake (Σ_g heat_rate(g) × generation(g)) over a week or a day
    per fuel.

    Reads:
      - ``t_object`` to find ``FueMaxOff*`` Constraint objects.
      - ``t_membership`` (collection 54: Fuel→Constraints) to map
        each constraint to its specific Fuel.
      - ``t_property``/``t_key``/``t_data_0`` to read the per-block
        decomposed RHS (property id ``RHS``, typically 3073) and
        sum it into the total weekly/daily cap.

    Returns a mapping ``{fuel_name: (cap_value, scope_hours)}`` where
    ``scope_hours`` is 168 for Week-scoped constraints, 24 for
    Day-scoped, and ``cap_value`` is the SUM of the per-block RHS
    over the horizon (in PLEXOS fuel units — typically GWh thermal
    or TJ).  When ``cache_dir`` is missing or any table can't be
    read, returns ``None``.
    """
    objects = _read_cached_csv(cache_dir, "t_object")
    classes = _read_cached_csv(cache_dir, "t_class")
    members = _read_cached_csv(cache_dir, "t_membership")
    keys = _read_cached_csv(cache_dir, "t_key")
    data0 = _read_cached_csv(cache_dir, "t_data_0")
    props = _read_cached_csv(cache_dir, "t_property")
    if objects is None or classes is None or members is None:
        return None
    if keys is None or data0 is None or props is None:
        return None

    # Locate the "Constraint" class id and the "RHS" property id.
    cls_id_constraint = next(
        (c["class_id"] for c in classes if c["name"] == "Constraint"), None
    )
    rhs_prop_ids = {p["property_id"] for p in props if p.get("name") == "RHS"}
    if cls_id_constraint is None or not rhs_prop_ids:
        return None

    obj_name = {o["object_id"]: o["name"] for o in objects}

    # Constraint objects whose name starts with ``FueMaxOff``.
    fue_constraints: dict[str, tuple[str, float]] = {}
    for o in objects:
        if o.get("class_id") != cls_id_constraint:
            continue
        name = o.get("name", "") or ""
        if not name.startswith(("FueMaxOffWeek_", "FueMaxOffDay_")):
            continue
        scope_hours = 168.0 if name.startswith("FueMaxOffWeek_") else 24.0
        fue_constraints[o["object_id"]] = (name, scope_hours)

    if not fue_constraints:
        return {}

    # Fuel→Constraint memberships (collection 54): parent=Fuel,
    # child=Constraint.  Group by constraint oid to find the
    # capped fuel.
    constraint_to_fuel: dict[str, str] = {}
    for m in members:
        if m.get("child_object_id") in fue_constraints:
            parent_name = obj_name.get(m.get("parent_object_id", ""))
            if parent_name:
                constraint_to_fuel[m["child_object_id"]] = parent_name

    # Map (constraint_oid → set of key_ids with RHS property).
    # Memberships involving the constraint span multiple collections
    # (System→Constraints, Fuel→Constraints); we accept ANY membership
    # whose either endpoint is the constraint's oid.
    constraint_mid: dict[str, set[str]] = {}
    for m in members:
        oid_p = m.get("parent_object_id", "")
        oid_c = m.get("child_object_id", "")
        mid = m.get("membership_id", "")
        if oid_p in fue_constraints:
            constraint_mid.setdefault(oid_p, set()).add(mid)
        if oid_c in fue_constraints:
            constraint_mid.setdefault(oid_c, set()).add(mid)

    rhs_key_to_constraint: dict[str, str] = {}
    for k in keys:
        if k.get("property_id") in rhs_prop_ids:
            mid = k.get("membership_id", "")
            for cid, mset in constraint_mid.items():
                if mid in mset:
                    rhs_key_to_constraint[k["key_id"]] = cid
                    break

    # Collect per-period RHS values per constraint (PLEXOS publishes a
    # row per period).  We keep both the per-period profile and the
    # rolled-up sum; the per-period values are the authoritative
    # PLEXOS limits (they vary substantially across the horizon —
    # e.g. ``FueMaxOffWeek_Gas_Yungay_GN_A`` ranges 0.488 → 14.51 per
    # block), and the uniform decomposition currently used by
    # ``_build_fuel_offtake_caps_ucs`` collides head-on with PLEXOS
    # Fixed-Load forced dispatch when the gen's fuel coef × forced MW
    # exceeds the uniform-average per-block RHS.
    rhs_sum: dict[str, float] = {}
    rhs_per_period: dict[str, dict[int, float]] = {}
    for d in data0:
        kid = d.get("key_id", "")
        if kid in rhs_key_to_constraint:
            try:
                v = float(d.get("value", "0"))
                p = int(d.get("period_id", "0"))
            except ValueError:
                continue
            cid = rhs_key_to_constraint[kid]
            rhs_sum[cid] = rhs_sum.get(cid, 0.0) + v
            rhs_per_period.setdefault(cid, {})[p] = v

    # Build the final {fuel_name: (cap, scope_hours, rhs_profile)}
    # mapping.  When multiple constraints reference the same fuel
    # (Day + Week), keep the TIGHTER cap (smaller per-hour rate);
    # propagate the chosen constraint's per-period profile too.
    result: dict[str, tuple[float, float]] = {}
    chosen_per_period: dict[str, dict[int, float]] = {}
    for cid, (_, scope_h) in fue_constraints.items():
        fuel = constraint_to_fuel.get(cid)
        cap = rhs_sum.get(cid)
        if not fuel or cap is None or cap <= 0.0:
            continue
        existing = result.get(fuel)
        # Compare in per-hour terms (cap / scope) so Day vs Week are
        # apples-to-apples.
        if existing is None or (cap / scope_h) < (existing[0] / existing[1]):
            result[fuel] = (cap, scope_h)
            chosen_per_period[fuel] = rhs_per_period.get(cid, {})
    # Stash the per-period profiles on the function object so
    # ``_build_fuel_offtake_caps_ucs`` can fetch them by fuel name
    # without changing the legacy ``(cap, scope_hours)`` return-type
    # contract.  Read by callers as
    # ``extract_fuel_offtake_caps.rhs_per_period.get(fuel)``.
    extract_fuel_offtake_caps.rhs_per_period = chosen_per_period  # type: ignore[attr-defined]

    return result


def auto_discover_res_zip(input_bundle: Path) -> Path | None:
    """Locate the ``RES*.zip[.xz]`` solution bundle for ``input_bundle``.

    CEN PCP daily bundles pair ``DATOS<DATE>.zip[.xz]`` with
    ``RES<DATE>.zip[.xz]``.  Two input shapes are supported:

    * ``input_bundle`` is a **directory** holding both files — scan the
      directory itself for ``RES*`` (preferring the date that matches a
      sibling ``DATOS*``).
    * ``input_bundle`` is the **DATOS file** — replace the leading
      ``DATOS`` with ``RES`` and check the sibling next to it.

    Returns ``None`` when no matching ``RES`` archive is found.
    """
    input_bundle = input_bundle.resolve()
    if input_bundle.is_dir():
        # CEN PCP bundle dirs ship ``DATOS<DATE>.zip[.xz]`` next to
        # ``RES<DATE>.zip[.xz]`` INSIDE the dir, so scan the dir itself
        # (not its parent).  Prefer the RES whose date matches a sibling
        # DATOS when several are present.
        res = [
            p
            for p in input_bundle.iterdir()
            if p.name.startswith("RES") and (p.suffix in (".zip", ".xz"))
        ]
        if not res:
            return None
        datos = next(
            (p for p in input_bundle.iterdir() if p.name.startswith("DATOS")),
            None,
        )
        if datos is not None:
            date_tag = datos.name[len("DATOS") :].split(".")[0]
            for p in res:
                if date_tag and date_tag in p.name:
                    return p
        return res[0]
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
