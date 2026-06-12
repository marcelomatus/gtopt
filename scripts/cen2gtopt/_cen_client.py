# SPDX-License-Identifier: BSD-3-Clause
"""``CenApiClient`` — unified HTTP client for CEN's 3scale-managed
public APIs.

Six services (one per gateway hostname):

* ``operacion``     → ``operacion.api.coordinador.cl``
* ``sip``           → ``sipub.api.coordinador.cl``
* ``medidas``       → ``medidas.api.coordinador.cl/medidas-v2``
* ``planificacion`` → ``planificacion.api.coordinador.cl``
* ``mercados``      → ``mercados.api.coordinador.cl``  (write-only)
* ``infotecnica``   → ``infotecnica.api.coordinador.cl/info-uni/v1/public``

Each service has its own user_key issued from the CEN portal. The
client is configured with one user_key per service via the
``user_keys`` dict.

Auth scheme (3scale default): ``?user_key=<key>`` query parameter.
The key is appended to every outgoing request automatically; callers
pass the canonical params they care about.

Pagination: most SIP/operación endpoints carry a
``{"data": [...], "page": N, "totalPages": M}`` envelope (variants
exist — operación uses ``content`` instead of ``data``). The
client's ``paginate(...)`` helper concatenates pages transparently
and stops at ``page >= totalPages``.

TLS: the gateway certs are not in standard system bundles in the
sandbox we develop in. Production use should ship a CEN-issued CA
bundle and set ``verify_tls=True`` (the default). For local probes,
set ``verify_tls=False`` — a one-time INFO log records the
downgrade.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Iterator
from dataclasses import dataclass
from typing import Any, Optional
from urllib.parse import urlencode

import requests


_LOG = logging.getLogger("cen2gtopt.client")


# Gateway hostnames per master plan §9.2.2 + the live-probe findings
# of 2026-05-06.
SERVICE_BASE_URLS: dict[str, str] = {
    "operacion": "https://operacion.api.coordinador.cl",
    "sip": "https://sipub.api.coordinador.cl",
    "medidas": "https://medidas.api.coordinador.cl",
    "planificacion": "https://planificacion.api.coordinador.cl",
    "mercados": "https://mercados.api.coordinador.cl",
    "infotecnica": "https://infotecnica.api.coordinador.cl",
}


class CenApiError(Exception):
    """Base error for CEN API problems."""


class CenAuthError(CenApiError):
    """3scale gateway returned 'Authentication failed' (HTTP 403)."""


class CenNotFoundError(CenApiError):
    """3scale gateway returned 'No Mapping Rule matched' (HTTP 404)."""


class CenServerError(CenApiError):
    """The upstream service returned 5xx after retries."""


@dataclass(slots=True)
class CenApiConfig:
    """Configuration for one CenApiClient instance."""

    user_keys: dict[str, str]  # service → user_key (provide what you have)
    timeout: float = 30.0
    max_retries: int = 3
    backoff_base: float = 0.5  # seconds; doubles each retry
    verify_tls: bool = True
    user_agent: str = "cen2gtopt/1.0 (+https://github.com/marcelomatus/gtopt)"


class CenApiClient:
    """Multi-service HTTP client for the CEN public APIs."""

    def __init__(self, config: CenApiConfig) -> None:
        self.config = config
        self._session = requests.Session()
        self._session.headers["Accept"] = "application/json"
        self._session.headers["User-Agent"] = config.user_agent
        if not config.verify_tls:
            _LOG.info(
                "TLS verification DISABLED — production deployments should ship "
                "a CEN-issued CA bundle and set verify_tls=True"
            )
            # Suppress the urllib3 InsecureRequestWarning when verify=False.
            try:
                import urllib3  # noqa: PLC0415  # pylint: disable=import-outside-toplevel

                urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
            except ImportError:  # pragma: no cover
                pass

    def close(self) -> None:
        self._session.close()

    def __enter__(self) -> "CenApiClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Core request
    # ------------------------------------------------------------------

    def get(
        self,
        service: str,
        path: str,
        params: Optional[dict[str, Any]] = None,
    ) -> dict[str, Any]:
        """One GET request against the named service.

        Returns the parsed JSON body. Raises ``CenAuthError`` /
        ``CenNotFoundError`` / ``CenServerError`` for the typed
        failure modes. Honours ``self.config.max_retries`` with
        exponential back-off on 429/502/503/504.
        """
        if service not in SERVICE_BASE_URLS:
            raise CenApiError(
                f"unknown service {service!r}; "
                f"expected one of {sorted(SERVICE_BASE_URLS.keys())}"
            )
        if service not in self.config.user_keys:
            raise CenAuthError(
                f"no user_key configured for service {service!r}; "
                f"set CEN_USER_KEY for that service in the config"
            )

        merged: dict[str, Any] = dict(params or {})
        merged["user_key"] = self.config.user_keys[service]
        url = f"{SERVICE_BASE_URLS[service]}{path}?{urlencode(merged)}"

        last_exc: Optional[Exception] = None
        for attempt in range(self.config.max_retries + 1):
            try:
                resp = self._session.get(
                    url,
                    timeout=self.config.timeout,
                    verify=self.config.verify_tls,
                )
            except requests.RequestException as exc:
                last_exc = exc
                wait = self.config.backoff_base * (2**attempt)
                _LOG.warning(
                    "request error on %s%s (attempt %d/%d): %s — retrying in %.1fs",
                    service,
                    path,
                    attempt + 1,
                    self.config.max_retries + 1,
                    exc,
                    wait,
                )
                time.sleep(wait)
                continue

            # Classify response.
            if resp.status_code == 200:
                try:
                    return resp.json()
                except ValueError as exc:
                    raise CenApiError(
                        f"expected JSON body from {url}, got "
                        f"{resp.headers.get('content-type')}"
                    ) from exc
            if resp.status_code == 403:
                # 3scale "Authentication failed" — typically 21 bytes body.
                raise CenAuthError(
                    f"403 from {service}{path}: "
                    f"{resp.text[:120]} (key probably not subscribed to {service})"
                )
            if resp.status_code == 404:
                raise CenNotFoundError(f"404 from {service}{path}: {resp.text[:120]}")
            if resp.status_code in (429, 500, 502, 503, 504):
                wait = self.config.backoff_base * (2**attempt)
                _LOG.warning(
                    "%d on %s%s (attempt %d/%d): %s — retrying in %.1fs",
                    resp.status_code,
                    service,
                    path,
                    attempt + 1,
                    self.config.max_retries + 1,
                    resp.text[:80],
                    wait,
                )
                time.sleep(wait)
                continue
            # Other 4xx/5xx — give up.
            raise CenApiError(
                f"unexpected HTTP {resp.status_code} from {service}{path}: "
                f"{resp.text[:200]}"
            )

        raise CenServerError(
            f"{self.config.max_retries + 1} attempts exhausted for "
            f"{service}{path}; last error: {last_exc}"
        )

    # ------------------------------------------------------------------
    # Pagination helper
    # ------------------------------------------------------------------

    def paginate(
        self,
        service: str,
        path: str,
        params: Optional[dict[str, Any]] = None,
        *,
        page_param: str = "page",
        limit_param: str = "limit",
        page_size: int = 1000,
        max_pages: int = 1000,
    ) -> Iterator[dict[str, Any]]:
        """Yield each page's parsed JSON body until exhausted.

        Auto-detects the response envelope:
          * SIP-style:        ``{"data": [...], ...}``
          * operación-style:  ``{"content": [...], "totalPages": M, ...}``

        Stops when:
          * ``totalPages`` is known and we've hit it, or
          * the response's ``data``/``content`` array is empty, or
          * ``max_pages`` is reached (safety fuse).
        """
        merged: dict[str, Any] = dict(params or {})
        merged.setdefault(limit_param, page_size)
        page = merged.get(page_param, 0)

        for _ in range(max_pages):
            merged[page_param] = page
            body = self.get(service, path, params=merged)
            yield body
            total_pages = body.get("totalPages")
            content = body.get("content") or body.get("data") or []
            if not content:
                return
            if total_pages is not None and page + 1 >= int(total_pages):
                return
            page += 1
