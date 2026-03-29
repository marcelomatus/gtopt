"""Tests for compressed and split file reading support."""

import bz2
import gzip
import lzma
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


# ── resolve_compressed_path ──────────────────────────────────────────────


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

    def test_split_xz_fallback(self, tmp: Path) -> None:
        """Split .dat.1.xz + .dat.2.xz found when .dat is missing."""
        part1 = tmp / "data.dat.1.xz"
        part2 = tmp / "data.dat.2.xz"
        with lzma.open(part1, "wb") as fh:
            fh.write(b"part1\n")
        with lzma.open(part2, "wb") as fh:
            fh.write(b"part2\n")
        result = resolve_compressed_path(tmp / "data.dat")
        assert result == part1  # returns first part as sentinel


# ── find_compressed_path ─────────────────────────────────────────────────


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

    def test_returns_split_xz(self, tmp: Path) -> None:
        part1 = tmp / "data.dat.1.xz"
        with lzma.open(part1, "wb") as fh:
            fh.write(b"x")
        assert find_compressed_path(tmp / "data.dat") == part1

    def test_returns_none_no_split(self, tmp: Path) -> None:
        assert find_compressed_path(tmp / "data.dat") is None


# ── compressed_open — single files ───────────────────────────────────────


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


# ── compressed_open — split files ────────────────────────────────────────


class TestSplitFiles:
    def _write_xz_part(self, path: Path, data: bytes) -> None:
        with lzma.open(path, "wb") as fh:
            fh.write(data)

    def test_two_xz_parts_concatenated(self, tmp: Path) -> None:
        self._write_xz_part(tmp / "big.dat.1.xz", b"AAA\nBBB\n")
        self._write_xz_part(tmp / "big.dat.2.xz", b"CCC\nDDD\n")
        # Open via the sentinel (first part path)
        with compressed_open(tmp / "big.dat.1.xz") as fh:
            assert fh.read() == "AAA\nBBB\nCCC\nDDD\n"

    def test_four_xz_parts_in_order(self, tmp: Path) -> None:
        for i in range(1, 5):
            self._write_xz_part(tmp / f"data.dat.{i}.xz", f"part{i}\n".encode())
        with compressed_open(tmp / "data.dat.1.xz") as fh:
            assert fh.read() == "part1\npart2\npart3\npart4\n"

    def test_split_gz_parts(self, tmp: Path) -> None:
        for i in range(1, 3):
            gz = tmp / f"data.dat.{i}.gz"
            with gzip.open(gz, "wb") as fh:
                fh.write(f"gz{i}\n".encode())
        with compressed_open(tmp / "data.dat.1.gz") as fh:
            assert fh.read() == "gz1\ngz2\n"

    def test_split_plain_parts(self, tmp: Path) -> None:
        (tmp / "data.dat.1").write_text("plain1\n")
        (tmp / "data.dat.2").write_text("plain2\n")
        with compressed_open(tmp / "data.dat.1") as fh:
            assert fh.read() == "plain1\nplain2\n"

    def test_resolve_then_open_split(self, tmp: Path) -> None:
        """Full flow: resolve finds split parts, open reads them."""
        self._write_xz_part(tmp / "file.dat.1.xz", b"line1\n")
        self._write_xz_part(tmp / "file.dat.2.xz", b"line2\n")
        path = resolve_compressed_path(tmp / "file.dat")
        with compressed_open(path) as fh:
            assert fh.read() == "line1\nline2\n"


# ── BaseParser integration ───────────────────────────────────────────────


class TestBaseParserIntegration:
    """Verify that BaseParser reads compressed .dat files transparently."""

    def test_gz_dat_file_parsed(self, tmp: Path) -> None:
        from plp2gtopt.base_parser import BaseParser

        content = "line1\nline2\n# comment\nline3\n"
        gz = tmp / "test.dat.gz"
        with gzip.open(gz, "wb") as fh:
            fh.write(content.encode("ascii"))

        class StubParser(BaseParser):
            lines: list[str] = []

            def parse(self, parsers=None):
                self.lines = self._read_non_empty_lines()

        p = StubParser(gz)
        p.parse()
        assert p.lines == ["line1", "line2", "line3"]

    def test_split_xz_dat_file_parsed(self, tmp: Path) -> None:
        from plp2gtopt.base_parser import BaseParser

        part1 = tmp / "test.dat.1.xz"
        part2 = tmp / "test.dat.2.xz"
        with lzma.open(part1, "wb") as fh:
            fh.write(b"line1\nline2\n")
        with lzma.open(part2, "wb") as fh:
            fh.write(b"# comment\nline3\n")

        class StubParser(BaseParser):
            lines: list[str] = []

            def parse(self, parsers=None):
                self.lines = self._read_non_empty_lines()

        p = StubParser(part1)
        p.parse()
        assert p.lines == ["line1", "line2", "line3"]
