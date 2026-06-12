# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests for the cached paginated fetcher."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import pandas as pd

from cen2gtopt._cached_extractor import (
    cache_path,
    cached_paginated_fetch,
    clear_cache,
    fetch_by_name,
)
from cen2gtopt._cen_client import CenApiClient, CenApiConfig, CenServerError


def _mk_client() -> CenApiClient:
    return CenApiClient(
        CenApiConfig(
            user_keys={"sip": "k"},
            backoff_base=0.0,
            verify_tls=False,
        )
    )


def test_cache_path_is_deterministic(tmp_path: Path) -> None:
    p1 = cache_path(
        service="sip",
        endpoint="/foo/v1/findByDate",
        params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
        cache_root=tmp_path,
    )
    p2 = cache_path(
        service="sip",
        endpoint="/foo/v1/findByDate",
        params={"endDate": "2026-04-22", "startDate": "2026-04-22"},
        cache_root=tmp_path,
    )
    assert p1 == p2  # canonicalised (sort_keys)
    assert "2026-04-22_to_2026-04-22" in p1.name


def test_paginates_and_caches(tmp_path: Path) -> None:
    """Three pages of 10 rows each → 30 rows; second call hits cache."""
    pages = [
        {"data": [{"i": i} for i in range(10)]},
        {"data": [{"i": i} for i in range(10, 20)]},
        {"data": [{"i": i} for i in range(20, 25)]},  # short → stop
    ]

    client = _mk_client()

    def fake_get(_service: str, _endpoint: str, *, params=None):
        page = (params or {}).get("page", 1)
        return pages[page - 1] if 1 <= page <= len(pages) else {"data": []}

    with patch.object(client, "get", side_effect=fake_get):
        df = cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1/findByDate",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            page_size=10,
            cache_root=tmp_path,
        )
    assert len(df) == 25
    cpath = cache_path(
        service="sip",
        endpoint="/foo/v1/findByDate",
        params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
        cache_root=tmp_path,
    )
    assert cpath.exists()

    # Second call should NOT touch the network.
    with patch.object(
        client,
        "get",
        side_effect=AssertionError("network should not be hit"),
    ):
        df2 = cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1/findByDate",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            cache_root=tmp_path,
        )
    pd.testing.assert_frame_equal(df, df2)


def test_bypass_cache_flag_re_fetches(tmp_path: Path) -> None:
    pages = [{"data": [{"x": 1}]}]
    client = _mk_client()

    def fake_get(_service: str, _endpoint: str, *, params=None):
        page = (params or {}).get("page", 1)
        return pages[page - 1] if 1 <= page <= len(pages) else {"data": []}

    with patch.object(client, "get", side_effect=fake_get) as m:
        cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            cache_root=tmp_path,
        )
        assert m.call_count >= 1

    with patch.object(client, "get", side_effect=fake_get) as m:
        cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            cache_root=tmp_path,
            bypass_cache=True,
        )
        assert m.call_count >= 1  # network was hit


def test_tolerates_transient_5xx(tmp_path: Path) -> None:
    """Two 5xx then a real page; the cache populates without crashing."""
    state = {"page1_calls": 0}

    client = _mk_client()

    def fake_get(_service: str, _endpoint: str, *, params=None):
        page = (params or {}).get("page", 1)
        if page == 1:
            state["page1_calls"] += 1
            if state["page1_calls"] <= 2:
                raise CenServerError("simulated 502")
            return {"data": [{"x": 1}, {"x": 2}]}  # short page → stop
        return {"data": []}

    with patch.object(client, "get", side_effect=fake_get):
        df = cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            cache_root=tmp_path,
            page_size=10,
            sleep_on_fail_seconds=0.0,
        )
    assert len(df) == 2


def test_fetch_by_name_uses_registry(tmp_path: Path) -> None:
    pages = [{"data": [{"plant": "X"}]}]
    client = _mk_client()

    def fake_get(_service: str, endpoint: str, *, params=None):
        # The registered endpoint's path should be hit.
        assert endpoint == "/unidades-generadoras/v4/findByDate"
        page = (params or {}).get("page", 1)
        return pages[page - 1] if 1 <= page <= len(pages) else {"data": []}

    with patch.object(client, "get", side_effect=fake_get):
        df = fetch_by_name(
            client,
            "unidades_generadoras",
            start="2026-04-22",
            cache_root=tmp_path,
        )
    assert len(df) == 1
    assert df.iloc[0]["plant"] == "X"


def test_clear_cache_removes_files(tmp_path: Path) -> None:
    pages = [{"data": [{"x": 1}]}]
    client = _mk_client()

    def fake_get(_service: str, _endpoint: str, *, params=None):
        page = (params or {}).get("page", 1)
        return pages[page - 1] if 1 <= page <= len(pages) else {"data": []}

    with patch.object(client, "get", side_effect=fake_get):
        cached_paginated_fetch(
            client,
            service="sip",
            endpoint="/foo/v1",
            params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
            cache_root=tmp_path,
        )
    assert clear_cache(cache_root=tmp_path) >= 1
    # second clear is a no-op
    assert clear_cache(cache_root=tmp_path) == 0
