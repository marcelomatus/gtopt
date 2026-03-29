"""Tests for compressed file reading support."""

import bz2
import gzip
import lzma
import tempfile
from pathlib import Path

import pytest

from plp2gtopt.compressed_open import (
    compressed_open,
    find_compressed_path,
    resolve_compressed_path,
)

SAMPLE_TEXT = "Hello World\nLine 2\n"
SAMPLE_BYTES = SAMPLE_TEXT.encode("ascii")


@pytest.fixture
def tmp(tmp_path: Path) -> Path:
    return tmp_path


class TestResolveCompressedPath:
    def test_plain_file_found(self, tmp: Path) -> None:
        f = tmp / "data.dat"
        f.write_text(SAMPLE_TEXT)
        assert resolve_compressed_path(f) == f

    def test_gz_fallback(self, tmp: Path) -> None:
        gz = tmp / "data.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        assert resolve_compressed_path(tmp / "data.dat") == gz

    def test_bz2_fallback(self, tmp: Path) -> None:
        bz = tmp / "data.dat.bz2"
        with bz2.open(bz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        assert resolve_compressed_path(tmp / "data.dat") == bz

    def test_xz_fallback(self, tmp: Path) -> None:
        xz = tmp / "data.dat.xz"
        with lzma.open(xz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        assert resolve_compressed_path(tmp / "data.dat") == xz

    def test_zst_fallback(self, tmp: Path) -> None:
        zstandard = pytest.importorskip("zstandard")
        zst = tmp / "data.dat.zst"
        cctx = zstandard.ZstdCompressor()
        zst.write_bytes(cctx.compress(SAMPLE_BYTES))
        assert resolve_compressed_path(tmp / "data.dat") == zst

    def test_lz4_fallback(self, tmp: Path) -> None:
        lz4_frame = pytest.importorskip("lz4.frame")
        lz = tmp / "data.dat.lz4"
        lz.write_bytes(lz4_frame.compress(SAMPLE_BYTES))
        assert resolve_compressed_path(tmp / "data.dat") == lz

    def test_plain_preferred_over_compressed(self, tmp: Path) -> None:
        plain = tmp / "data.dat"
        plain.write_text(SAMPLE_TEXT)
        gz = tmp / "data.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        assert resolve_compressed_path(plain) == plain

    def test_not_found_raises(self, tmp: Path) -> None:
        with pytest.raises(FileNotFoundError):
            resolve_compressed_path(tmp / "nonexistent.dat")


class TestFindCompressedPath:
    def test_returns_none_when_missing(self, tmp: Path) -> None:
        assert find_compressed_path(tmp / "nope.dat") is None

    def test_returns_plain(self, tmp: Path) -> None:
        f = tmp / "data.dat"
        f.write_text(SAMPLE_TEXT)
        assert find_compressed_path(f) == f

    def test_returns_gz(self, tmp: Path) -> None:
        gz = tmp / "data.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        assert find_compressed_path(tmp / "data.dat") == gz


class TestCompressedOpen:
    def test_plain_file(self, tmp: Path) -> None:
        f = tmp / "data.dat"
        f.write_text(SAMPLE_TEXT)
        with compressed_open(f) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_gz_file(self, tmp: Path) -> None:
        gz = tmp / "data.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        with compressed_open(gz) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_bz2_file(self, tmp: Path) -> None:
        bz = tmp / "data.dat.bz2"
        with bz2.open(bz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        with compressed_open(bz) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_xz_file(self, tmp: Path) -> None:
        xz = tmp / "data.dat.xz"
        with lzma.open(xz, "wb") as fh:
            fh.write(SAMPLE_BYTES)
        with compressed_open(xz) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_zst_file(self, tmp: Path) -> None:
        zstandard = pytest.importorskip("zstandard")
        zst = tmp / "data.dat.zst"
        cctx = zstandard.ZstdCompressor()
        zst.write_bytes(cctx.compress(SAMPLE_BYTES))
        with compressed_open(zst) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_lz4_file(self, tmp: Path) -> None:
        lz4_frame = pytest.importorskip("lz4.frame")
        lz = tmp / "data.dat.lz4"
        lz.write_bytes(lz4_frame.compress(SAMPLE_BYTES))
        with compressed_open(lz) as fh:
            assert fh.read() == SAMPLE_TEXT

    def test_encoding_latin1(self, tmp: Path) -> None:
        f = tmp / "data.csv"
        f.write_bytes("caf\xe9\n".encode("latin-1"))
        with compressed_open(f, encoding="latin-1", errors="strict") as fh:
            assert fh.read() == "caf\xe9\n"


class TestBaseParserIntegration:
    """Verify that BaseParser reads compressed .dat files transparently."""

    def test_gz_dat_file_parsed(self, tmp: Path) -> None:
        from plp2gtopt.base_parser import BaseParser

        # Create a minimal .dat.gz with two data lines
        content = "line1\nline2\n# comment\nline3\n"
        gz = tmp / "test.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(content.encode("ascii"))

        class StubParser(BaseParser):
            def parse(self, parsers=None):
                self.lines = self._read_non_empty_lines()

        p = StubParser(gz)
        p.parse()
        assert p.lines == ["line1", "line2", "line3"]
