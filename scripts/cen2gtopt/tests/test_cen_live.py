# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end live test against the CEN SIP API.

Skipped by default. Runs only when:
  * ``GTOPT_RUN_CEN_BACKTEST=1`` is set, AND
  * ``$CEN_USER_KEY`` is set with a SIP-subscribed key.

Network-dependent; not part of CI. Captures the live response shape
and confirms the canonical-feed long-form normalisation works on
real (not synthetic) Spanish bus / unit names.
"""

from __future__ import annotations

import os

import pytest


_GATE = os.environ.get("GTOPT_RUN_CEN_BACKTEST") == "1"
_HAS_KEY = bool(os.environ.get("CEN_USER_KEY"))


@pytest.mark.skipif(
    not _GATE,
    reason="set GTOPT_RUN_CEN_BACKTEST=1 to run live CEN tests",
)
@pytest.mark.skipif(
    not _HAS_KEY,
    reason="set $CEN_USER_KEY (SIP-subscribed) to run live CEN tests",
)
def test_live_costo_marginal_real_one_page(tmp_path):
    """Hit the real SIP gateway and confirm the canonical normaliser
    turns one page of the response into a usable long-form frame.

    We deliberately fetch only the first page (limit=10) — a full
    24-hour CMG real query yields 50 000+ rows across 50+ pages, far
    too slow for a verification test. The full paginated fetch is
    exercised by the production cen2gtopt CLI."""
    from cen2gtopt._cen_client import CenApiClient, CenApiConfig
    from cen2gtopt._sip_client import _normalize_cmg
    from cen2gtopt._sip_endpoints import COSTO_MARGINAL_REAL

    config = CenApiConfig(
        user_keys={"sip": os.environ["CEN_USER_KEY"]},
        verify_tls=False,
    )
    with CenApiClient(config) as client:
        body = client.get(
            "sip",
            COSTO_MARGINAL_REAL.path,
            params={
                "startDate": "2025-12-01",
                "endDate": "2025-12-01",
                "page": 0,
                "limit": 10,
            },
        )
    assert "data" in body and len(body["data"]) > 0

    import pandas as pd

    df = _normalize_cmg(pd.DataFrame(body["data"]), value_col="lmp")
    assert not df.empty
    assert {"date_utc", "hour", "bus_uid", "bus_name", "lmp"}.issubset(df.columns)
    assert (df["lmp"] >= 0).all()
    assert (df["lmp"] <= 1000).all()
    assert df["hour"].between(0, 23).all()


@pytest.mark.skipif(
    not _GATE or not _HAS_KEY,
    reason="set GTOPT_RUN_CEN_BACKTEST=1 and $CEN_USER_KEY to run",
)
def test_live_centrales_catalogue():
    from cen2gtopt._cen_client import CenApiClient, CenApiConfig, CenServerError
    from cen2gtopt._sip_client import fetch_centrales

    config = CenApiConfig(
        user_keys={"sip": os.environ["CEN_USER_KEY"]},
        verify_tls=False,
        max_retries=1,  # don't waste 8s on a known-flaky upstream
        backoff_base=0.1,
    )
    with CenApiClient(config) as client:
        try:
            df = fetch_centrales(client)
        except CenServerError as exc:
            pytest.skip(f"centrales upstream is currently down: {exc}")

    if df.empty:
        pytest.skip("centrales endpoint returned empty page")
    assert {"uid", "name"}.issubset(df.columns)
    assert (df["uid"] > 0).all()
