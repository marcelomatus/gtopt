"""Edge-case coverage tests for remaining branches.

Targets the residual uncovered lines in:

* :mod:`plexos2gtopt.parsers` — ``extract_bundle_spec`` PLEXOS_Param.xml
  parsing branches (Step_Count / Step_Type / Day_Beginning / Date_From).
* :mod:`plexos2gtopt.plexos2gtopt` — `validate_plexos_bundle` happy path
  on a populated directory bundle (the success branch).
* :mod:`plexos2gtopt.plexos_csv` — read_wide on an all-zero column with
  ``drop_zero_cols=True`` (default).
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.parsers import extract_bundle_spec
from plexos2gtopt.plexos2gtopt import validate_plexos_bundle
from plexos2gtopt.plexos_csv import read_wide
from plexos2gtopt.plexos_loader import (
    DBSEN_FILENAME,
    PLEXOS_PARAM_FILENAME,
    PlexosBundle,
)


_PARAM_XML = """<?xml version="1.0" standalone="yes"?>
<Params>
  <Step_Count>48</Step_Count>
  <Step_Type>Hour</Step_Type>
  <Day_Beginning>3</Day_Beginning>
  <Date_From>2026-04-22T00:00:00</Date_From>
</Params>
"""


_PARAM_XML_BAD_INTS = """<?xml version="1.0" standalone="yes"?>
<Params>
  <Step_Count>nope</Step_Count>
  <Step_Type>Hour</Step_Type>
  <Day_Beginning>also-bad</Day_Beginning>
</Params>
"""


def test_extract_bundle_spec_parses_param_xml(tmp_path: Path) -> None:
    """All four supported PLEXOS_Param fields populate the BundleSpec."""
    (tmp_path / DBSEN_FILENAME).write_text("<MasterDataSet/>")
    (tmp_path / PLEXOS_PARAM_FILENAME).write_text(_PARAM_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    spec = extract_bundle_spec(bundle)
    assert spec.step_count == 48
    assert spec.step_type == "Hour"
    assert spec.day_beginning == 3
    assert spec.bundle_date == "2026-04-22"


def test_extract_bundle_spec_ignores_bad_integers(tmp_path: Path) -> None:
    """Non-integer Step_Count / Day_Beginning fall back to defaults."""
    (tmp_path / DBSEN_FILENAME).write_text("<MasterDataSet/>")
    (tmp_path / PLEXOS_PARAM_FILENAME).write_text(_PARAM_XML_BAD_INTS)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    spec = extract_bundle_spec(bundle)
    # Defaults survive when the XML carries un-parseable integers.
    assert spec.step_count == 24
    assert spec.day_beginning == 0
    # The Step_Type string is still picked up.
    assert spec.step_type == "Hour"


def test_extract_bundle_spec_handles_parse_error(tmp_path: Path) -> None:
    """Malformed PLEXOS_Param.xml logs a warning and returns defaults."""
    (tmp_path / DBSEN_FILENAME).write_text("<MasterDataSet/>")
    (tmp_path / PLEXOS_PARAM_FILENAME).write_text("not-xml<<<")
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    spec = extract_bundle_spec(bundle)
    assert spec.step_count == 24
    assert spec.step_type == "Hour"


def test_validate_plexos_bundle_happy_path(tmp_path: Path) -> None:
    """`validate_plexos_bundle` returns True for a directory with the XML present."""
    bundle_dir = tmp_path / "ok"
    bundle_dir.mkdir()
    (bundle_dir / DBSEN_FILENAME).write_text("<MasterDataSet/>")
    assert validate_plexos_bundle({"input_bundle": str(bundle_dir)}) is True


def test_read_wide_drops_zero_only_column(tmp_path: Path) -> None:
    """``drop_zero_cols=True`` (default) strips all-zero data columns."""
    csv_path = tmp_path / "Nod_Load.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,active,quiet\n2026,1,1,1,10,0\n2026,1,1,2,11,0\n"
    )
    out = read_wide(csv_path)
    assert "quiet" not in out
    assert out["active"][0] == 10.0
