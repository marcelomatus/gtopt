"""cen_demanda – download hourly demand data from the Chilean ISO SIPUB API.

The Coordinador Eléctrico Nacional publishes hourly time-series through the
SIPUB REST API (``https://sipub.api.coordinador.cl/sipub/api/v2``).  Each
endpoint takes ``user_key`` (per-developer token, obtained at
``portal.api.coordinador.cl``) and ``fecha=YYYY-MM-DD`` query parameters and
returns a JSON payload covering that single day.

This package iterates over a date range, honours the API rate limit
(default 60 requests/hour), and saves one JSON file per day under
``<output>/<endpoint>/YYYY-MM-DD.json``.  Runs are resumable: existing
non-empty files are skipped on the next invocation.

Public API
----------
download_range(start, end, user_key, endpoint, output_dir, ...)
    Bulk-download day-by-day payloads.

RateLimiter
    Minimum-interval token-bucket-lite throttle.

RequestSpec
    Builds the fully-qualified SIPUB URL for one (endpoint, date) pair.

main(argv)
    CLI entry point (registered as the ``cen_demanda`` console script).
"""

from cen_demanda.main import (
    DEFAULT_ENDPOINT,
    DEFAULT_RATE_PER_HOUR,
    SIPUB_BASE_URL,
    RateLimiter,
    RequestSpec,
    daterange,
    download_range,
    main,
)

__all__ = [
    "DEFAULT_ENDPOINT",
    "DEFAULT_RATE_PER_HOUR",
    "SIPUB_BASE_URL",
    "RateLimiter",
    "RequestSpec",
    "daterange",
    "download_range",
    "main",
]
