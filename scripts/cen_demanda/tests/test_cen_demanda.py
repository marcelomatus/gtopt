"""Unit tests for :mod:`cen_demanda.main`.

No network traffic: the downloader's ``fetcher`` hook is injected with a
stub that records the requested URLs and returns canned payloads.  The
rate limiter uses fake ``sleep`` / ``now`` callables so timing tests are
deterministic.
"""

from __future__ import annotations

import importlib
import json
from datetime import date
from pathlib import Path
from urllib.error import HTTPError

import pytest

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

# The package __init__ re-exports ``main`` (the CLI function), which shadows
# the ``cen_demanda.main`` submodule attribute.  Load the submodule via
# importlib so tests can monkeypatch its internals.
_cen_main_module = importlib.import_module("cen_demanda.main")


# ---------------------------------------------------------------------------
# RequestSpec
# ---------------------------------------------------------------------------


def test_request_spec_url_encoding() -> None:
    spec = RequestSpec(
        endpoint="demanda_sistema_real",
        fecha=date(2024, 3, 15),
        user_key="abc123",
    )
    assert spec.url() == (
        f"{SIPUB_BASE_URL}/demanda_sistema_real/?user_key=abc123&fecha=2024-03-15"
    )


def test_request_spec_url_special_chars_encoded() -> None:
    spec = RequestSpec(
        endpoint="demanda_sistema_real",
        fecha=date(2024, 1, 1),
        user_key="key with/special&chars",
    )
    url = spec.url()
    assert "key+with%2Fspecial%26chars" in url or "key%20with%2Fspecial%26chars" in url


def test_request_spec_trailing_slash_in_base_url_ok() -> None:
    spec = RequestSpec(
        endpoint="demanda_sistema_real",
        fecha=date(2024, 1, 1),
        user_key="k",
        base_url=f"{SIPUB_BASE_URL}/",
    )
    # Exactly one slash between base and endpoint.
    assert "//demanda" not in spec.url().replace("https://", "")


# ---------------------------------------------------------------------------
# daterange
# ---------------------------------------------------------------------------


def test_daterange_inclusive() -> None:
    days = list(daterange(date(2024, 1, 1), date(2024, 1, 3)))
    assert days == [date(2024, 1, 1), date(2024, 1, 2), date(2024, 1, 3)]


def test_daterange_single_day() -> None:
    assert list(daterange(date(2024, 2, 29), date(2024, 2, 29))) == [date(2024, 2, 29)]


def test_daterange_full_year_leap() -> None:
    days = list(daterange(date(2024, 1, 1), date(2024, 12, 31)))
    assert len(days) == 366  # 2024 is a leap year


def test_daterange_rejects_reversed() -> None:
    with pytest.raises(ValueError):
        list(daterange(date(2024, 1, 3), date(2024, 1, 1)))


# ---------------------------------------------------------------------------
# RateLimiter
# ---------------------------------------------------------------------------


class _FakeClock:
    """Deterministic monotonic clock with controllable advance."""

    def __init__(self) -> None:
        self.t = 0.0
        self.sleeps: list[float] = []

    def now(self) -> float:
        return self.t

    def sleep(self, dt: float) -> None:
        self.sleeps.append(dt)
        self.t += dt


def _make_limiter(per_hour: int) -> tuple[RateLimiter, _FakeClock]:
    clk = _FakeClock()
    lim = RateLimiter(per_hour, sleep_fn=clk.sleep, now_fn=clk.now)
    return lim, clk


def test_rate_limiter_rejects_nonpositive() -> None:
    with pytest.raises(ValueError):
        RateLimiter(0)
    with pytest.raises(ValueError):
        RateLimiter(-1)


def test_rate_limiter_first_call_does_not_sleep() -> None:
    lim, clk = _make_limiter(per_hour=60)
    lim.wait()
    assert not clk.sleeps


def test_rate_limiter_enforces_interval() -> None:
    lim, clk = _make_limiter(per_hour=60)  # 60 s between calls
    assert lim.interval_s == pytest.approx(60.0)

    lim.wait()  # t=0, no sleep
    clk.t = 10.0  # 10 s of external work
    lim.wait()  # needs 50 s more
    assert clk.sleeps == [pytest.approx(50.0)]
    assert clk.t == pytest.approx(60.0)


def test_rate_limiter_skips_sleep_when_enough_elapsed() -> None:
    lim, clk = _make_limiter(per_hour=60)
    lim.wait()  # t=0
    clk.t = 150.0  # far beyond 60 s
    lim.wait()
    assert not clk.sleeps


def test_rate_limiter_high_rate() -> None:
    lim, _ = _make_limiter(per_hour=3600)  # 1 s between calls
    assert lim.interval_s == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# download_range
# ---------------------------------------------------------------------------


class _RecordingFetcher:
    """Test double: records URLs, returns a canned payload keyed by date."""

    def __init__(self) -> None:
        self.calls: list[str] = []

    def __call__(self, url: str) -> dict:
        self.calls.append(url)
        return {"url": url, "hours": list(range(24))}


def _no_sleep_limiter() -> RateLimiter:
    """Zero-cost limiter for tests."""
    return RateLimiter(per_hour=3600, sleep_fn=lambda _dt: None, now_fn=lambda: 0.0)


def test_download_range_writes_one_file_per_day(tmp_path: Path) -> None:
    fetcher = _RecordingFetcher()
    files = download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 3),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher,
        limiter=_no_sleep_limiter(),
    )
    assert len(files) == 3
    assert len(fetcher.calls) == 3
    expected = {
        tmp_path / "demanda_sistema_real" / f"2024-01-0{i}.json" for i in (1, 2, 3)
    }
    assert {p.resolve() for p in files} == {p.resolve() for p in expected}
    for f in files:
        data = json.loads(f.read_text(encoding="utf-8"))
        assert data["hours"] == list(range(24))


def test_download_range_resume_skips_existing(tmp_path: Path) -> None:
    fetcher = _RecordingFetcher()
    download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 2),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher,
        limiter=_no_sleep_limiter(),
    )
    # Second run over an overlapping range: only the new day is fetched.
    fetcher2 = _RecordingFetcher()
    new_files = download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 3),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher2,
        limiter=_no_sleep_limiter(),
    )
    assert len(new_files) == 1
    assert len(fetcher2.calls) == 1
    assert new_files[0].name == "2024-01-03.json"


def test_download_range_no_resume_overwrites(tmp_path: Path) -> None:
    fetcher = _RecordingFetcher()
    download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 1),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher,
        limiter=_no_sleep_limiter(),
    )
    fetcher2 = _RecordingFetcher()
    files = download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 1),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher2,
        limiter=_no_sleep_limiter(),
        resume=False,
    )
    assert len(files) == 1
    assert len(fetcher2.calls) == 1


def test_download_range_propagates_http_error(tmp_path: Path) -> None:
    def bad_fetcher(_url: str) -> dict:
        raise HTTPError(  # type: ignore[arg-type]
            url=_url,
            code=429,
            msg="Too Many Requests",
            hdrs=None,
            fp=None,
        )

    with pytest.raises(HTTPError):
        download_range(
            start=date(2024, 1, 1),
            end=date(2024, 1, 1),
            user_key="K",
            endpoint="demanda_sistema_real",
            output_dir=tmp_path,
            fetcher=bad_fetcher,
            limiter=_no_sleep_limiter(),
        )


def test_download_range_url_contains_userkey_and_date(tmp_path: Path) -> None:
    fetcher = _RecordingFetcher()
    download_range(
        start=date(2024, 6, 30),
        end=date(2024, 6, 30),
        user_key="MYKEY",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=fetcher,
        limiter=_no_sleep_limiter(),
    )
    assert len(fetcher.calls) == 1
    url = fetcher.calls[0]
    assert "user_key=MYKEY" in url
    assert "fecha=2024-06-30" in url
    assert "/demanda_sistema_real/" in url


def test_download_range_rate_limiter_is_invoked(tmp_path: Path) -> None:
    """Every fetched day must pass through ``RateLimiter.wait``."""
    call_count = {"wait": 0}

    class _CountingLimiter(RateLimiter):
        def wait(self) -> None:  # type: ignore[override]
            call_count["wait"] += 1

    lim = _CountingLimiter(per_hour=3600, sleep_fn=lambda _dt: None)
    download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 3),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=_RecordingFetcher(),
        limiter=lim,
    )
    assert call_count["wait"] == 3


def test_download_range_atomic_write_no_partial_left(tmp_path: Path) -> None:
    download_range(
        start=date(2024, 1, 1),
        end=date(2024, 1, 1),
        user_key="K",
        endpoint="demanda_sistema_real",
        output_dir=tmp_path,
        fetcher=_RecordingFetcher(),
        limiter=_no_sleep_limiter(),
    )
    leftovers = list(tmp_path.rglob("*.partial"))
    assert not leftovers


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def test_cli_happy_path(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    fetcher = _RecordingFetcher()

    # Replace the fetcher the default path uses, and disable sleeping.
    monkeypatch.setattr(_cen_main_module, "_default_fetcher", fetcher)
    monkeypatch.setattr(_cen_main_module.RateLimiter, "wait", lambda self: None)

    rc = main(
        [
            "--start",
            "2024-01-01",
            "--end",
            "2024-01-02",
            "--output-dir",
            str(tmp_path),
            "--user-key",
            "TESTKEY",
        ]
    )
    assert rc == 0
    out = tmp_path / DEFAULT_ENDPOINT
    assert (out / "2024-01-01.json").exists()
    assert (out / "2024-01-02.json").exists()
    assert len(fetcher.calls) == 2


def test_cli_requires_user_key(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("CEN_USER_KEY", raising=False)
    with pytest.raises(SystemExit):
        main(
            [
                "--start",
                "2024-01-01",
                "--end",
                "2024-01-02",
                "--output-dir",
                str(tmp_path),
            ]
        )


def test_cli_reads_user_key_from_env(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fetcher = _RecordingFetcher()
    monkeypatch.setattr(_cen_main_module, "_default_fetcher", fetcher)
    monkeypatch.setattr(_cen_main_module.RateLimiter, "wait", lambda self: None)
    monkeypatch.setenv("CEN_USER_KEY", "FROMENV")

    rc = main(
        [
            "--start",
            "2024-01-01",
            "--end",
            "2024-01-01",
            "--output-dir",
            str(tmp_path),
        ]
    )
    assert rc == 0
    assert "user_key=FROMENV" in fetcher.calls[0]


def test_cli_rejects_nonpositive_rate(tmp_path: Path) -> None:
    with pytest.raises(SystemExit):
        main(
            [
                "--start",
                "2024-01-01",
                "--end",
                "2024-01-01",
                "--output-dir",
                str(tmp_path),
                "--user-key",
                "K",
                "--rate-per-hour",
                "0",
            ]
        )


def test_cli_rejects_reversed_dates(tmp_path: Path) -> None:
    with pytest.raises(SystemExit):
        main(
            [
                "--start",
                "2024-01-03",
                "--end",
                "2024-01-01",
                "--output-dir",
                str(tmp_path),
                "--user-key",
                "K",
            ]
        )


def test_default_rate_matches_api_limit() -> None:
    # Documented SIPUB default is 60 requests/hour.
    assert DEFAULT_RATE_PER_HOUR == 60
