# SPDX-License-Identifier: BSD-3-Clause
"""Round-trip + structural tests for plexos_compress_case."""

from __future__ import annotations

import hashlib
import zipfile
from pathlib import Path

import pytest

from plexos_compress_case.main import (
    _group_by_plexos_date,
    _is_outer_wrapper,
    compress_case,
    decompress_case,
)

# ── Fixtures ────────────────────────────────────────────────────────────


def _make_zip(path: Path, members: dict[str, bytes]) -> None:
    """Write a zip at *path* containing *members* (filename → content)."""
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name, data in members.items():
            zf.writestr(name, data)


def _make_outer_wrapper(path: Path, inner_zips: list[Path]) -> None:
    """Write a STORE-compressed wrapper containing *inner_zips* verbatim."""
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as zf:
        for inner in inner_zips:
            zf.write(inner, arcname=inner.name)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


@pytest.fixture
def fake_bundle_dir(tmp_path: Path) -> Path:
    """Build a fake CEN PCP bundle with a DATOS+RES outer wrapper.

    Returns the directory containing PLEXOS20260101.zip.
    """
    work = tmp_path / "bundle"
    work.mkdir()
    # Build the two inner zips first, then bundle them inside the
    # outer PLEXOS{date}.zip with STORE (matching CEN convention).
    datos_zip = work / "DATOS20260101.zip"
    res_zip = work / "RES20260101.zip"
    _make_zip(
        datos_zip,
        {
            "Gen_Rating.csv": (b"unit,hour,mw\n" + b"g1,1,100\n" * 200),
            "Nod_Load.csv": (b"node,hour,mw\n" + b"n1,1,80\n" * 200),
        },
    )
    _make_zip(
        res_zip,
        {
            "Solution/Model Log.txt": b"CPLEX solve: optimal\n" * 200,
        },
    )
    _make_outer_wrapper(work / "PLEXOS20260101.zip", [datos_zip, res_zip])
    # Remove the staged inner zips — the user-facing pre-compress state
    # is just the outer wrapper (matches what the CEN portal ships).
    datos_zip.unlink()
    res_zip.unlink()
    return work


# ── Tests ───────────────────────────────────────────────────────────────


class TestOuterWrapperDetection:
    def test_matches_plexos_date_zip_with_two_inner_zips(
        self, fake_bundle_dir: Path
    ) -> None:
        outer = fake_bundle_dir / "PLEXOS20260101.zip"
        assert _is_outer_wrapper(outer) is True

    def test_rejects_non_plexos_named_zip(self, tmp_path: Path) -> None:
        plain = tmp_path / "random.zip"
        _make_zip(plain, {"a.csv": b"x"})
        assert _is_outer_wrapper(plain) is False

    def test_rejects_plexos_named_but_wrong_payload(self, tmp_path: Path) -> None:
        # Right name, wrong contents (no inner .zip members)
        wrong = tmp_path / "PLEXOS20260101.zip"
        _make_zip(wrong, {"a.csv": b"x", "b.csv": b"y"})
        assert _is_outer_wrapper(wrong) is False

    def test_rejects_broken_zip(self, tmp_path: Path) -> None:
        broken = tmp_path / "PLEXOS20260101.zip"
        broken.write_bytes(b"not a zip")
        assert _is_outer_wrapper(broken) is False


class TestGroupByPlexosDate:
    def test_pairs_matching_datos_and_res(self, tmp_path: Path) -> None:
        d1 = tmp_path / "DATOS20260101.zip"
        r1 = tmp_path / "RES20260101.zip"
        d2 = tmp_path / "DATOS20260202.zip"
        r2 = tmp_path / "RES20260202.zip"
        for p in (d1, r1, d2, r2):
            p.touch()
        pairs = _group_by_plexos_date([d1, r1, d2, r2])
        assert set(pairs.keys()) == {"20260101", "20260202"}
        assert pairs["20260101"] == [d1, r1]
        assert pairs["20260202"] == [d2, r2]

    def test_skips_incomplete_pairs(self, tmp_path: Path) -> None:
        # DATOS without its RES → no pair emitted
        d = tmp_path / "DATOS20260101.zip"
        d.touch()
        assert not _group_by_plexos_date([d])

    def test_ignores_unrelated_zips(self, tmp_path: Path) -> None:
        d = tmp_path / "DATOS20260101.zip"
        r = tmp_path / "RES20260101.zip"
        other = tmp_path / "Other.zip"
        for p in (d, r, other):
            p.touch()
        pairs = _group_by_plexos_date([d, r, other])
        assert pairs == {"20260101": [d, r]}


class TestRoundTrip:
    """Compress then decompress; the recovered files must be byte-exact."""

    def test_unwrap_compress_then_decompress_with_rewrap(
        self, fake_bundle_dir: Path
    ) -> None:
        outer = fake_bundle_dir / "PLEXOS20260101.zip"
        # Capture the wrapper's pre-state. We cannot demand the rewrapped
        # zip to be byte-identical because the local ZIP file headers
        # encode a timestamp; instead we check the inner zip identities
        # via SHA-256.
        with zipfile.ZipFile(outer) as zf:
            inner_hashes_before = {
                name: hashlib.sha256(zf.read(name)).hexdigest()
                for name in zf.namelist()
            }

        # Phase A: compress (unwrap=True by default).
        rc = compress_case(
            fake_bundle_dir,
            codec="xz",
            codec_args=["-T0", "-9"],
            unwrap=True,
        )
        assert rc == 0
        files_after_compress = sorted(p.name for p in fake_bundle_dir.iterdir())
        assert files_after_compress == [
            "DATOS20260101.zip.xz",
            "RES20260101.zip.xz",
        ], "outer wrapper should be dropped, inner zips compressed"

        # Phase B: decompress with --rewrap.
        rc = decompress_case(fake_bundle_dir, rewrap=True)
        assert rc == 0
        files_after_decompress = sorted(p.name for p in fake_bundle_dir.iterdir())
        assert files_after_decompress == ["PLEXOS20260101.zip"], (
            "rewrap should restore the outer wrapper and drop the inners"
        )

        # The rebuilt wrapper must contain the same inner zip identities.
        rebuilt = fake_bundle_dir / "PLEXOS20260101.zip"
        with zipfile.ZipFile(rebuilt) as zf:
            inner_hashes_after = {
                name: hashlib.sha256(zf.read(name)).hexdigest()
                for name in zf.namelist()
            }
        assert inner_hashes_after == inner_hashes_before

    def test_no_unwrap_keeps_wrapper_verbatim(self, fake_bundle_dir: Path) -> None:
        outer = fake_bundle_dir / "PLEXOS20260101.zip"
        sha_before = _sha256(outer)

        # Phase A: compress with --no-unwrap.
        rc = compress_case(
            fake_bundle_dir,
            codec="xz",
            codec_args=["-T0", "-9"],
            unwrap=False,
        )
        assert rc == 0
        assert sorted(p.name for p in fake_bundle_dir.iterdir()) == [
            "PLEXOS20260101.zip.xz",
        ]

        # Phase B: decompress (rewrap=False — no inner pair to rewrap).
        rc = decompress_case(fake_bundle_dir, rewrap=False)
        assert rc == 0
        files_after = sorted(p.name for p in fake_bundle_dir.iterdir())
        assert files_after == ["PLEXOS20260101.zip"]

        # Byte-exact recovery: xz round-trip is lossless.
        assert _sha256(fake_bundle_dir / "PLEXOS20260101.zip") == sha_before


class TestNoOpPaths:
    def test_compress_no_zips_in_dir(self, tmp_path: Path) -> None:
        work = tmp_path / "empty"
        work.mkdir()
        rc = compress_case(work, codec="xz", codec_args=["-T0"], unwrap=True)
        assert rc == 0

    def test_decompress_no_compressed_in_dir(self, tmp_path: Path) -> None:
        work = tmp_path / "empty"
        work.mkdir()
        rc = decompress_case(work, rewrap=False)
        assert rc == 0

    def test_compress_already_compressed_dir(self, tmp_path: Path) -> None:
        # Pre-populate with a .zip.xz so the "already compressed" branch fires.
        work = tmp_path / "already"
        work.mkdir()
        (work / "DATOS20260101.zip.xz").write_bytes(b"\xfd7zXZ\x00")  # xz magic
        rc = compress_case(work, codec="xz", codec_args=["-T0"], unwrap=True)
        assert rc == 0
