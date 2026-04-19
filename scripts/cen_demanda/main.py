"""Downloader for the Coordinador Eléctrico Nacional SIPUB hourly API.

Usage
-----
::

    export CEN_USER_KEY=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    cen_demanda \\
        --start 2024-01-01 --end 2024-12-31 \\
        --endpoint demanda_sistema_real \\
        --output-dir ./cen_demanda_2024

One JSON file is written per day under
``<output-dir>/<endpoint>/YYYY-MM-DD.json``.  Re-running the command resumes
from the first missing day; use ``--no-resume`` to force a full re-download.

The API default rate limit is 60 requests/hour, which downloads one year in
roughly six hours.  Use ``--rate-per-hour`` to match the limit registered for
your token.

Per-barra demand
----------------
The exact endpoint name for per-barra hourly demand is in the SIPUB v2
documentation (``portal.api.coordinador.cl/documentacion?service=sipubv2``,
login required).  Pass the endpoint name via ``--endpoint``; the rest of the
pipeline is endpoint-agnostic.  The default ``demanda_sistema_real`` returns
the system total and is useful for smoke-testing the token and network path.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import time
from dataclasses import dataclass
from datetime import date, timedelta
from pathlib import Path
from typing import Any, Callable, Iterator, Optional
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

SIPUB_BASE_URL = "https://sipub.api.coordinador.cl/sipub/api/v2"
DEFAULT_ENDPOINT = "demanda_sistema_real"
DEFAULT_RATE_PER_HOUR = 60
DEFAULT_TIMEOUT_S = 60.0

JsonFetcher = Callable[[str], Any]

logger = logging.getLogger(__name__)


def _default_fetcher(url: str, timeout: float = DEFAULT_TIMEOUT_S) -> Any:
    """Fetch ``url`` and return the decoded JSON body.

    Uses :mod:`urllib` so the package has no third-party runtime deps.
    """
    req = Request(url, headers={"Accept": "application/json"})
    with urlopen(req, timeout=timeout) as resp:  # noqa: S310  # trusted HTTPS endpoint
        return json.loads(resp.read().decode("utf-8"))


@dataclass(frozen=True)
class RequestSpec:
    """Immutable description of a single SIPUB API request."""

    endpoint: str
    fecha: date
    user_key: str
    base_url: str = SIPUB_BASE_URL

    def url(self) -> str:
        """Fully-qualified URL with url-encoded query parameters."""
        qs = urlencode({"user_key": self.user_key, "fecha": self.fecha.isoformat()})
        return f"{self.base_url.rstrip('/')}/{self.endpoint}/?{qs}"


class RateLimiter:
    """Enforce a minimum interval between successive calls.

    Not a full token bucket — just ensures at most ``per_hour`` requests in
    any rolling interval by sleeping ``3600/per_hour`` seconds between calls.
    Safe for single-threaded use.
    """

    def __init__(
        self,
        per_hour: int,
        *,
        sleep_fn: Callable[[float], None] = time.sleep,
        now_fn: Callable[[], float] = time.monotonic,
    ) -> None:
        if per_hour <= 0:
            raise ValueError("per_hour must be positive")
        self._interval = 3600.0 / per_hour
        self._last_call: Optional[float] = None
        self._sleep = sleep_fn
        self._now = now_fn

    @property
    def interval_s(self) -> float:
        """Minimum spacing between calls in seconds."""
        return self._interval

    def wait(self) -> None:
        """Block until the next call is allowed; no-op on the first call."""
        now = self._now()
        if self._last_call is not None:
            needed = self._interval - (now - self._last_call)
            if needed > 0:
                self._sleep(needed)
                now = self._now()
        self._last_call = now


def daterange(start: date, end: date) -> Iterator[date]:
    """Inclusive day iterator from ``start`` to ``end``."""
    if end < start:
        raise ValueError(f"end ({end}) must be >= start ({start})")
    current = start
    while current <= end:
        yield current
        current += timedelta(days=1)


def _atomic_write_json(path: Path, payload: Any) -> None:
    """Write ``payload`` as JSON to ``path`` atomically (tmp + rename)."""
    tmp = path.with_suffix(path.suffix + ".partial")
    tmp.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    tmp.replace(path)


def download_range(
    start: date,
    end: date,
    user_key: str,
    endpoint: str,
    output_dir: Path,
    *,
    fetcher: Optional[JsonFetcher] = None,
    limiter: Optional[RateLimiter] = None,
    base_url: str = SIPUB_BASE_URL,
    resume: bool = True,
) -> list[Path]:
    """Download one JSON file per day into ``output_dir/<endpoint>/``.

    Parameters
    ----------
    start, end
        Inclusive date range.
    user_key
        SIPUB API token (the ``user_key`` query parameter).
    endpoint
        SIPUB endpoint name, e.g. ``demanda_sistema_real``.
    output_dir
        Root directory; a per-endpoint subdirectory is created inside.
    fetcher
        Callable ``url -> json``; defaults to :func:`_default_fetcher`.
        Overridable for tests and alternative transports.
    limiter
        Optional :class:`RateLimiter`; defaults to the API's documented
        60 req/h cap.
    base_url
        Override the SIPUB base URL (useful for tests against a mock server).
    resume
        If ``True`` (default), skip days whose output file already exists
        and is non-empty.  If ``False``, re-download and overwrite.

    Returns
    -------
    list[Path]
        Absolute paths of the files newly written in this invocation
        (excludes files that were skipped because they already existed).
    """
    fetch = fetcher if fetcher is not None else _default_fetcher
    limiter = limiter if limiter is not None else RateLimiter(DEFAULT_RATE_PER_HOUR)

    target_dir = (output_dir / endpoint).resolve()
    target_dir.mkdir(parents=True, exist_ok=True)

    written: list[Path] = []
    for day in daterange(start, end):
        out = target_dir / f"{day.isoformat()}.json"
        if resume and out.exists() and out.stat().st_size > 0:
            logger.debug("skip existing %s", out)
            continue
        limiter.wait()
        spec = RequestSpec(
            endpoint=endpoint, fecha=day, user_key=user_key, base_url=base_url
        )
        try:
            payload = fetch(spec.url())
        except (HTTPError, URLError) as exc:
            logger.error("fetch failed for %s: %s", day.isoformat(), exc)
            raise
        _atomic_write_json(out, payload)
        written.append(out)
        logger.info("wrote %s (%d bytes)", out, out.stat().st_size)
    return written


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cen_demanda",
        description=(
            "Download hourly demand data from the Coordinador Eléctrico "
            "Nacional SIPUB API (one JSON per day)."
        ),
    )
    parser.add_argument(
        "--start",
        type=date.fromisoformat,
        required=True,
        help="First day to download (YYYY-MM-DD).",
    )
    parser.add_argument(
        "--end",
        type=date.fromisoformat,
        required=True,
        help="Last day to download, inclusive (YYYY-MM-DD).",
    )
    parser.add_argument(
        "--endpoint",
        default=DEFAULT_ENDPOINT,
        help=(
            f"SIPUB endpoint name (default: {DEFAULT_ENDPOINT}). For per-barra "
            "hourly demand, look up the exact endpoint in the SIPUB v2 docs."
        ),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Destination root directory; a <endpoint>/ subdir is created.",
    )
    parser.add_argument(
        "--user-key",
        default=os.environ.get("CEN_USER_KEY"),
        help="SIPUB API user_key (or set CEN_USER_KEY environment variable).",
    )
    parser.add_argument(
        "--rate-per-hour",
        type=int,
        default=DEFAULT_RATE_PER_HOUR,
        help=f"Max requests per hour (default: {DEFAULT_RATE_PER_HOUR}).",
    )
    parser.add_argument(
        "--base-url",
        default=SIPUB_BASE_URL,
        help="Override SIPUB base URL (default: %(default)s).",
    )
    parser.add_argument(
        "--no-resume",
        action="store_true",
        help="Re-download and overwrite days that already have an output file.",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Debug logging.")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    """CLI entry point.  Returns the process exit code."""
    parser = _build_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    if not args.user_key:
        parser.error("missing --user-key (or CEN_USER_KEY environment variable)")

    if args.rate_per_hour <= 0:
        parser.error("--rate-per-hour must be positive")

    try:
        files = download_range(
            start=args.start,
            end=args.end,
            user_key=args.user_key,
            endpoint=args.endpoint,
            output_dir=args.output_dir,
            limiter=RateLimiter(args.rate_per_hour),
            base_url=args.base_url,
            resume=not args.no_resume,
        )
    except ValueError as exc:
        parser.error(str(exc))
    except (HTTPError, URLError) as exc:
        logger.error("download aborted: %s", exc)
        return 2
    logger.info("done: %d new files written", len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
