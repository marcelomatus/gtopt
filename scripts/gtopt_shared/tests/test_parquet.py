# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.parquet`."""

from __future__ import annotations

from unittest.mock import patch

import pytest

from gtopt_shared import parquet as parquet_helpers


def test_default_compression_is_a_string() -> None:
    """``DEFAULT_COMPRESSION`` is probed at import time and is a string."""
    assert isinstance(parquet_helpers.DEFAULT_COMPRESSION, str)
    # Sanity: probed value is one of the codecs the helper recognises.
    assert parquet_helpers.DEFAULT_COMPRESSION in {
        "zstd",
        "gzip",
        "uncompressed",
        "none",
        "",
    }


@pytest.mark.parametrize("requested", ["", "none", "uncompressed"])
def test_probe_returns_as_is_for_no_compression(requested: str) -> None:
    """Empty / "none" / "uncompressed" short-circuit without touching pyarrow."""
    assert parquet_helpers.probe_parquet_codec(requested) == requested


def test_probe_returns_requested_when_pyarrow_accepts_it() -> None:
    """A codec pyarrow accepts comes back unchanged."""
    # ``"zstd"`` is the universally available codec — but probe through
    # the cache so we don't double-probe if it's already cached.
    parquet_helpers.probe_parquet_codec.cache_clear()
    assert parquet_helpers.probe_parquet_codec("zstd") == "zstd"


def test_probe_falls_back_to_gzip_when_pyarrow_rejects(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Unknown codec → ``"gzip"`` fallback + stderr warning."""
    parquet_helpers.probe_parquet_codec.cache_clear()

    class _FakePa:
        @staticmethod
        def Codec(_name: str) -> None:  # noqa: N802
            raise ValueError("no such codec")

    with patch.dict("sys.modules", {"pyarrow": _FakePa()}):
        result = parquet_helpers.probe_parquet_codec("madeup_codec")

    assert result == "gzip"
    captured = capsys.readouterr()
    assert "madeup_codec" in captured.err
    assert "falling back to gzip" in captured.err


def test_probe_is_cached_per_codec_name() -> None:
    """``lru_cache`` means each unique codec name probes at most once."""
    parquet_helpers.probe_parquet_codec.cache_clear()
    parquet_helpers.probe_parquet_codec("zstd")
    parquet_helpers.probe_parquet_codec("zstd")
    info = parquet_helpers.probe_parquet_codec.cache_info()
    assert info.hits >= 1


@pytest.mark.parametrize(
    "module_path",
    [
        "plp2gtopt.base_writer",
        "ts2gtopt.ts2gtopt",
        "igtopt.igtopt",
    ],
)
def test_converter_reexports_share_the_canonical_helpers(module_path: str) -> None:
    """Each converter's ``_probe_parquet_codec`` / ``_DEFAULT_COMPRESSION``
    must be the SAME object as the canonical one in ``gtopt_shared.parquet``.

    Identity check (``is``) — anything else means the converter has its own
    independent copy and the lift hasn't actually deduplicated the code.
    Also guarantees the ``lru_cache`` is shared (each converter probing
    ``"zstd"`` shouldn't trigger a separate Arrow round-trip).
    """
    # pylint: disable=import-outside-toplevel,protected-access
    import importlib  # noqa: PLC0415

    mod = importlib.import_module(module_path)
    assert mod._probe_parquet_codec is parquet_helpers.probe_parquet_codec, (
        f"{module_path}._probe_parquet_codec is a different object than "
        "gtopt_shared.parquet.probe_parquet_codec"
    )
    assert mod._DEFAULT_COMPRESSION == parquet_helpers.DEFAULT_COMPRESSION
    # ``str`` is interned for the codec names we use, so identity holds too.
    assert mod._DEFAULT_COMPRESSION is parquet_helpers.DEFAULT_COMPRESSION
