# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plexos2gtopt's --emissions integration with gtopt_shared.

The shared engine itself is exercised by ``plp2gtopt/tests/test_emissions.py``.
These tests check the plexos2gtopt-specific surface:

* CLI exposes --emissions / --emissions-file / --emissions-report and
  options-dict threads them through.
* When --emissions is off, no emission_factors / emission_array are
  synthesized that the converter wouldn't already produce.
* When --emissions is on, IPCC defaults fill in CO2 factors on Fuel
  entries that PLEXOS XML left empty.
* PLEXOS-shipped factors win over IPCC defaults (delegated to the
  shared engine — checked end-to-end here).
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from plexos2gtopt.main import make_parser


# ---------------------------------------------------------------------------
# CLI surface
# ---------------------------------------------------------------------------


def test_cli_emissions_flags_default_off() -> None:
    args = make_parser().parse_args(["--input-bundle", "x"])
    assert args.emissions is False
    assert args.emissions_file is None
    assert args.emissions_report is None


def test_cli_emissions_master_switch_on() -> None:
    args = make_parser().parse_args(["--input-bundle", "x", "--emissions"])
    assert args.emissions is True


def test_cli_emissions_custom_file_and_report(tmp_path: Path) -> None:
    custom = tmp_path / "custom.json"
    report = tmp_path / "report.json"
    args = make_parser().parse_args(
        [
            "--input-bundle",
            "x",
            "--emissions",
            "--emissions-file",
            str(custom),
            "--emissions-report",
            str(report),
        ]
    )
    assert args.emissions is True
    assert args.emissions_file == custom
    assert args.emissions_report == report


# ---------------------------------------------------------------------------
# Shared-engine wiring (end-to-end on a planning fixture)
# ---------------------------------------------------------------------------


def _planning(fuels: list[dict]) -> dict:
    return {
        "options": {},
        "simulation": {},
        "system": {"name": "test", "fuel_array": fuels},
    }


def test_apply_via_shared_engine_fills_missing_factor() -> None:
    """When a fuel has no emission rows, IPCC defaults inject one per
    non-zero pollutant (CO2 + CH4 + N2O for diesel).
    """
    from gtopt_shared.emissions import apply_emission_defaults_from_file

    planning = _planning(fuels=[{"uid": 1, "name": "diesel", "price": 80.0}])
    report = apply_emission_defaults_from_file(planning, None)

    diesel = planning["system"]["fuel_array"][0]
    assert "emission_factors" in diesel
    rows_by_pollutant = {row["emission"]: row for row in diesel["emission_factors"]}
    # All three IPCC pollutants land for diesel (Table 1.4 + 2.2):
    assert rows_by_pollutant["co2"]["combustion"] == pytest.approx(0.0741)
    assert rows_by_pollutant["ch4"]["combustion"] == pytest.approx(3e-6)
    assert rows_by_pollutant["n2o"]["combustion"] == pytest.approx(6e-7)
    assert report.fuels_factor_added == ("diesel",)
    assert report.emission_array_created is True

    # emission_array now carries one row per pollutant.
    pollutants = {p["name"] for p in planning["system"]["emission_array"]}
    assert pollutants == {"co2", "ch4", "n2o"}


def test_apply_via_shared_engine_preserves_plexos_factor() -> None:
    """A factor already shipped by PLEXOS XML (via build_fuel_array)
    must NOT be clobbered by the IPCC defaults — the source value is
    preserved exactly, while the IPCC fills in pollutants the source
    did not ship (CH4 / N2O).
    """
    from gtopt_shared.emissions import apply_emission_defaults_from_file

    planning = _planning(
        fuels=[
            {
                "uid": 1,
                "name": "natural_gas",
                "emission_factors": [
                    {"emission": "co2", "combustion": 0.0540},  # custom value
                ],
            },
        ]
    )
    report = apply_emission_defaults_from_file(planning, None)

    gas = planning["system"]["fuel_array"][0]
    rows_by_pollutant = {row["emission"]: row for row in gas["emission_factors"]}
    # CO2 preserved at the custom 0.0540 (NOT clobbered by IPCC 0.0561).
    assert rows_by_pollutant["co2"]["combustion"] == pytest.approx(0.0540)
    # CH4 + N2O filled in from IPCC defaults (Table 2.2: 1 kg/TJ, 0.1 kg/TJ).
    assert rows_by_pollutant["ch4"]["combustion"] == pytest.approx(1e-6)
    assert rows_by_pollutant["n2o"]["combustion"] == pytest.approx(1e-7)
    # The fuel WAS touched (CH4 / N2O added), so it lands in
    # fuels_factor_added — NOT fuels_factor_preserved (the latter
    # means "every pollutant the defaults knew was already present").
    assert report.fuels_factor_added == ("natural_gas",)
    assert not report.fuels_factor_preserved


def test_report_written_to_disk(tmp_path: Path) -> None:
    from gtopt_shared.emissions import apply_emission_defaults_from_file

    planning = _planning(fuels=[{"uid": 1, "name": "carbon"}])  # alias → coal
    report_path = tmp_path / "out" / "emissions_report.json"
    apply_emission_defaults_from_file(planning, None, report_path=report_path)

    assert report_path.exists()
    loaded = json.loads(report_path.read_text())
    assert loaded["summary"]["factor_added"] == 1
    assert loaded["fuels_factor_added"] == ["carbon"]
