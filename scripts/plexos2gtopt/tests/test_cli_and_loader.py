"""Coverage tests for CLI surface + bundle loader + CSV edge cases.

These complement the per-extractor / per-builder tests by exercising
the seams that aren't covered by the topology pipeline:

* :mod:`plexos2gtopt.info_display` (``--info`` rendering)
* :mod:`plexos2gtopt.main` (CLI flow, signal handler, dispatch paths)
* :mod:`plexos2gtopt.plexos_loader` (outer-wrapper unwrap, .zip.xz
  decompression, unknown-suffix rejection)
* :mod:`plexos2gtopt.plexos_csv` (malformed rows, BOM stripping)
"""

from __future__ import annotations

import lzma
import signal
import zipfile
from pathlib import Path

import pytest

from plexos2gtopt import main as main_module
from plexos2gtopt.info_display import display_plexos_info
from plexos2gtopt.main import (
    _resolve_bundle,
    _signal_handler,
    main,
    make_parser,
)
from plexos2gtopt.plexos2gtopt import (
    convert_plexos_bundle,
    validate_plexos_bundle,
)
from plexos2gtopt.plexos_csv import read_long, read_wide
from plexos2gtopt.plexos_loader import (
    DBSEN_FILENAME,
    PlexosBundle,
    locate_bundle,
)
from plexos2gtopt.plexos_xml import NS


_TINY_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>22</class_id><name>bus_only</name>
  </t_object>
</MasterDataSet>
"""


def _make_dir_bundle(tmp_path: Path) -> Path:
    """Write a minimal DBSEN_PRGDIARIO.xml into a fresh bundle directory."""
    bundle_dir = tmp_path / "bundle"
    bundle_dir.mkdir()
    (bundle_dir / DBSEN_FILENAME).write_text(_TINY_XML)
    return bundle_dir


# ---------------------------------------------------------------------------
# info_display
# ---------------------------------------------------------------------------


def test_display_plexos_info_smoke(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """`display_plexos_info` prints class counts + CSV table for a real bundle."""
    bundle_dir = _make_dir_bundle(tmp_path)
    # Add one CSV so the "Top-level CSV files" table has at least one row.
    (bundle_dir / "Nod_Load.csv").write_text(
        "YEAR,MONTH,DAY,PERIOD,bus_only\n2026,1,1,1,10.0\n"
    )
    display_plexos_info({"input_bundle": str(bundle_dir)})
    captured = capsys.readouterr().out
    assert "PLEXOS bundle:" in captured
    assert "XML classes: 3" in captured
    assert "Core class counts:" in captured
    assert "Generator" in captured
    assert "Top-level CSV files:" in captured
    assert "Nod_Load.csv" in captured


def test_display_plexos_info_empty_csv_table(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """No CSVs → the table prints the ``(none)`` placeholder row."""
    _make_dir_bundle(tmp_path)
    display_plexos_info({"input_bundle": str(tmp_path / "bundle")})
    captured = capsys.readouterr().out
    assert "(none)" in captured


# ---------------------------------------------------------------------------
# main CLI
# ---------------------------------------------------------------------------


def test_signal_handler_exits_cleanly(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """SIGINT/SIGTERM handler prints + exits with code 0."""
    with pytest.raises(SystemExit) as exc:
        _signal_handler(signal.SIGINT, None)
    assert exc.value.code == 0
    captured = capsys.readouterr().out
    assert "Caught signal" in captured


def test_resolve_bundle_prefers_input_flag(tmp_path: Path) -> None:
    """`-i` / `--input-bundle` wins over the positional argument."""
    parser = make_parser()
    args = parser.parse_args(["-i", str(tmp_path), "ignored_pos"])
    assert _resolve_bundle(args) == tmp_path


def test_resolve_bundle_falls_back_to_positional(tmp_path: Path) -> None:
    """The positional argument is used when ``-i`` is absent."""
    parser = make_parser()
    args = parser.parse_args([str(tmp_path)])
    assert _resolve_bundle(args) == tmp_path


def test_main_info_dispatch(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """``--info`` invokes `display_plexos_info` and returns normally."""
    bundle_dir = _make_dir_bundle(tmp_path)
    # No SystemExit expected here — info exits via the function's `return`.
    main(["--info", str(bundle_dir)])
    captured = capsys.readouterr().out
    assert "XML classes:" in captured


def test_main_info_reports_missing_bundle(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """`--info` on a non-existent path exits non-zero with a clear error."""
    missing = tmp_path / "ghost.zip"
    with pytest.raises(SystemExit) as exc:
        main(["--info", str(missing)])
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "error:" in err.lower()


def test_main_validate_on_dir(tmp_path: Path) -> None:
    """`--validate` on a populated bundle exits 0."""
    bundle_dir = _make_dir_bundle(tmp_path)
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(bundle_dir)])
    assert exc.value.code == 0


def test_main_convert_reports_filenotfound(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Convert path exits 1 with stderr message when the bundle is missing."""
    missing = tmp_path / "absent"
    with pytest.raises(SystemExit) as exc:
        main([str(missing)])
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "error:" in err.lower()


def test_main_convert_propagates_not_implemented(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """``NotImplementedError`` from convert → exit code 2 with stderr."""
    bundle_dir = _make_dir_bundle(tmp_path)

    def _boom(_opts: dict) -> int:
        raise NotImplementedError("placeholder")

    monkeypatch.setattr(main_module, "convert_plexos_bundle", _boom)
    with pytest.raises(SystemExit) as exc:
        main([str(bundle_dir)])
    assert exc.value.code == 2
    err = capsys.readouterr().err
    assert "placeholder" in err


def test_main_convert_critical_findings(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """A non-zero return from convert → exit 1 with the CRITICAL banner."""
    bundle_dir = _make_dir_bundle(tmp_path)

    def _fake(_opts: dict) -> int:
        return 3

    monkeypatch.setattr(main_module, "convert_plexos_bundle", _fake)
    with pytest.raises(SystemExit) as exc:
        main([str(bundle_dir)])
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "CRITICAL" in err
    assert "3" in err


# ---------------------------------------------------------------------------
# plexos_loader
# ---------------------------------------------------------------------------


def test_locate_bundle_directory(tmp_path: Path) -> None:
    """A directory already containing the XML returns a bundle without cleanup."""
    bundle_dir = _make_dir_bundle(tmp_path)
    with locate_bundle(bundle_dir) as bundle:
        assert bundle.root == bundle_dir
        assert bundle._cleanup is None  # noqa: SLF001 — internal lifecycle check
        assert bundle.xml_path.is_file()


def test_locate_bundle_missing_xml_in_dir(tmp_path: Path) -> None:
    """A directory without the XML raises FileNotFoundError."""
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(FileNotFoundError, match="does not contain"):
        locate_bundle(empty)


def test_locate_bundle_unknown_suffix(tmp_path: Path) -> None:
    """An archive with neither .zip nor .zip.xz suffix is rejected."""
    bogus = tmp_path / "data.tar"
    bogus.write_bytes(b"not a zip")
    with pytest.raises(ValueError, match="unsupported archive type"):
        locate_bundle(bogus)


def test_locate_bundle_missing_path(tmp_path: Path) -> None:
    """A non-existent path raises FileNotFoundError."""
    with pytest.raises(FileNotFoundError, match="not found"):
        locate_bundle(tmp_path / "ghost.zip")


def test_locate_bundle_inner_datos_zip(tmp_path: Path) -> None:
    """A ``DATOS{date}.zip`` extracts directly into a tempdir."""
    inner = tmp_path / "DATOS20260101.zip"
    with zipfile.ZipFile(inner, "w") as zf:
        zf.writestr(DBSEN_FILENAME, _TINY_XML)
    with locate_bundle(inner) as bundle:
        assert bundle.xml_path.is_file()
        assert str(bundle.root).startswith("/tmp/")


def test_locate_bundle_outer_wrapper(tmp_path: Path) -> None:
    """A ``PLEXOS{date}.zip`` outer wrapper unwraps to its inner DATOS zip."""
    inner = tmp_path / "DATOS20260101.zip"
    with zipfile.ZipFile(inner, "w") as zf:
        zf.writestr(DBSEN_FILENAME, _TINY_XML)
    outer = tmp_path / "PLEXOS20260101.zip"
    with zipfile.ZipFile(outer, "w") as zf:
        zf.write(inner, arcname=inner.name)
    with locate_bundle(outer) as bundle:
        assert bundle.xml_path.is_file()


def test_locate_bundle_outer_wrapper_missing_inner(tmp_path: Path) -> None:
    """An outer wrapper that ships no DATOS*.zip is rejected loudly."""
    outer = tmp_path / "PLEXOS20260101.zip"
    with zipfile.ZipFile(outer, "w") as zf:
        zf.writestr("bogus.txt", "not the right payload")
    with pytest.raises(ValueError, match="missing the expected DATOS"):
        locate_bundle(outer)


def test_locate_bundle_xz(tmp_path: Path) -> None:
    """``.zip.xz`` decompresses to a tempdir."""
    inner = tmp_path / "DATOS20260101.zip"
    with zipfile.ZipFile(inner, "w") as zf:
        zf.writestr(DBSEN_FILENAME, _TINY_XML)
    xz_path = tmp_path / "DATOS20260101.zip.xz"
    with inner.open("rb") as src, lzma.open(xz_path, "wb") as dst:
        dst.write(src.read())
    with locate_bundle(xz_path) as bundle:
        assert bundle.xml_path.is_file()


def test_locate_bundle_extracted_zip_without_xml(tmp_path: Path) -> None:
    """An archive that extracts without DBSEN_PRGDIARIO.xml raises."""
    bad = tmp_path / "DATOS20260101.zip"
    with zipfile.ZipFile(bad, "w") as zf:
        zf.writestr("README.txt", "no PLEXOS data here")
    with pytest.raises(FileNotFoundError, match="missing"):
        locate_bundle(bad)


def test_bundle_csv_paths_skip_subdirs(tmp_path: Path) -> None:
    """``csv_paths()`` returns top-level CSVs only — sub-directory files skipped."""
    bundle_dir = _make_dir_bundle(tmp_path)
    (bundle_dir / "Nod_Load.csv").write_text("YEAR,MONTH,DAY,PERIOD,b\n")
    sub = bundle_dir / "CFdata"
    sub.mkdir()
    (sub / "extra.csv").write_text("ignored")
    bundle = PlexosBundle(root=bundle_dir, source=bundle_dir)
    names = [p.name for p in bundle.csv_paths()]
    assert "Nod_Load.csv" in names
    assert "extra.csv" not in names


def test_bundle_csv_raises_for_missing(tmp_path: Path) -> None:
    """`PlexosBundle.csv(name)` raises FileNotFoundError when absent."""
    bundle_dir = _make_dir_bundle(tmp_path)
    bundle = PlexosBundle(root=bundle_dir, source=bundle_dir)
    assert not bundle.has("Bogus.csv")
    with pytest.raises(FileNotFoundError):
        bundle.csv("Bogus.csv")


# ---------------------------------------------------------------------------
# plexos_csv edge cases
# ---------------------------------------------------------------------------


def test_read_long_strips_bom_and_skips_blank_name(tmp_path: Path) -> None:
    """UTF-8 BOM on the header + blank-NAME rows do not crash the reader."""
    csv_path = tmp_path / "Gen_HeatRate.csv"
    csv_path.write_text(
        "﻿NAME,YEAR,MONTH,DAY,PERIOD,VALUE\n,2026,1,1,1,99\nGEN_A,2026,1,1,1,0.5\n",
        encoding="utf-8",
    )
    out = read_long(csv_path)
    assert set(out) == {"GEN_A"}
    assert out["GEN_A"][0] == 0.5


def test_read_long_ignores_malformed_band_and_period(tmp_path: Path) -> None:
    """Non-integer BAND or PERIOD fields skip the row instead of crashing."""
    csv_path = tmp_path / "Gen_Rating.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "GEN_A,2026,1,1,X,1,10\n"
        "GEN_A,2026,1,1,1,abc,20\n"
        "GEN_A,2026,1,1,2,1,30\n"
    )
    out = read_long(csv_path)
    # Only the period-2 row survives the filters.
    assert out["GEN_A"][1] == 30.0


def test_read_long_clamps_out_of_range_periods(tmp_path: Path) -> None:
    """Period > 24 is logged + dropped."""
    csv_path = tmp_path / "Gen_Rating.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "GEN_A,2026,1,1,99,1,777\n"
        "GEN_A,2026,1,1,1,1,1\n"
    )
    out = read_long(csv_path)
    assert out["GEN_A"][0] == 1.0
    # 99 is out of [1, 24] — never lands.
    assert 777 not in out["GEN_A"]


def test_read_wide_empty_csv(tmp_path: Path) -> None:
    """A CSV with no rows returns an empty dict (no fieldnames → bail out)."""
    csv_path = tmp_path / "empty.csv"
    csv_path.write_text("")
    out = read_wide(csv_path)
    assert out == {}


def test_read_wide_malformed_value(tmp_path: Path) -> None:
    """Non-numeric data cells are silently skipped (period+row valid)."""
    csv_path = tmp_path / "Nod_Load.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,bus_a,bus_b\n2026,1,1,1,oops,20\n2026,1,1,2,11,21\n"
    )
    out = read_wide(csv_path)
    assert out["bus_a"][0] == 0.0  # malformed cell stays zero
    assert out["bus_a"][1] == 11.0
    assert out["bus_b"][0] == 20.0


def test_read_wide_band_filter_skips_other_bands(tmp_path: Path) -> None:
    """``band=1`` (default) drops rows tagged with BAND=2 entirely."""
    csv_path = tmp_path / "Lin_Units.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,BAND,line_a\n2026,1,1,1,1,1\n2026,1,1,1,2,9\n"  # ignored
    )
    out = read_wide(csv_path, drop_zero_cols=False)
    assert out["line_a"][0] == 1.0
    # Band-2 row didn't leak into the result.
    assert all(v in (0.0, 1.0) for v in out["line_a"])


# ---------------------------------------------------------------------------
# plexos2gtopt facade (high-level)
# ---------------------------------------------------------------------------


def test_validate_missing_option_returns_false() -> None:
    """`validate_plexos_bundle` without `input_bundle` returns False, not raises."""
    assert validate_plexos_bundle({}) is False


def test_validate_handles_value_error(tmp_path: Path) -> None:
    """ValueError from locate_bundle → False (e.g. unsupported archive type)."""
    bogus = tmp_path / "data.tar"
    bogus.write_bytes(b"not a zip")
    assert validate_plexos_bundle({"input_bundle": str(bogus)}) is False


def test_convert_requires_input_bundle() -> None:
    """`convert_plexos_bundle` without `input_bundle` raises ValueError."""
    with pytest.raises(ValueError, match="input_bundle"):
        convert_plexos_bundle({})


def test_convert_inferred_output_dir(tmp_path: Path) -> None:
    """When ``output_dir`` is absent, the converter writes to ``gtopt_<stem>/``."""
    bundle_dir = _make_dir_bundle(tmp_path)
    rc = convert_plexos_bundle({"input_bundle": bundle_dir})
    # Returns 0 for an empty-topology bundle (no CRITICAL findings).
    assert rc == 0
    inferred = bundle_dir.parent / f"gtopt_{bundle_dir.name}"
    assert inferred.is_dir()
    assert (inferred / f"{bundle_dir.name}.json").is_file()
