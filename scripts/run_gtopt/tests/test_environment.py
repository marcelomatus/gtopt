# SPDX-License-Identifier: BSD-3-Clause
"""Tests for runtime environment detection."""

from run_gtopt._environment import detect_compression_codec, detect_cpu_count


def test_detect_cpu_count():
    """CPU count is at least 1."""
    assert detect_cpu_count() >= 1


def test_detect_compression_zstd():
    """zstd should be available with pyarrow."""
    codec = detect_compression_codec("zstd")
    assert codec in ("zstd", "gzip")


def test_detect_compression_none():
    """'none' passes through unchanged."""
    assert detect_compression_codec("none") == "none"


def test_detect_compression_uncompressed():
    """'uncompressed' passes through unchanged."""
    assert detect_compression_codec("uncompressed") == "uncompressed"


def test_detect_compression_unavailable():
    """Unavailable codec falls back to gzip."""
    codec = detect_compression_codec("nonexistent_codec_xyz")
    assert codec == "gzip"
