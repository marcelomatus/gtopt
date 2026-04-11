"""Unit tests for the --ror-as-reservoirs feature of JunctionWriter."""

from pathlib import Path
from typing import Any, Dict, List

import pytest

from ..junction_writer import JunctionWriter
from .test_junction_writer import MockCentralParser


def _make_serie(
    name: str,
    number: int,
    *,
    bus: int = 1,
    efficiency: float = 1.0,
    ctype: str = "serie",
) -> Dict[str, Any]:
    """Build a minimal serie/pasada central dict accepted by _process_central."""
    return {
        "number": number,
        "name": name,
        "type": ctype,
        "bus": bus,
        "pmin": 0,
        "pmax": 10,
        "vert_min": 0.0,
        "vert_max": 0.0,
        "efficiency": efficiency,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
    }


def _make_embalse(name: str, number: int) -> Dict[str, Any]:
    """Build a minimal embalse central that produces an embalse reservoir entry."""
    return {
        "number": number,
        "name": name,
        "type": "embalse",
        "bus": 0,
        "pmin": 0,
        "pmax": 100.0,
        "vert_min": 0.0,
        "vert_max": 1000.0,
        "efficiency": 0.85,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
        "vol_ini": 50.0,
        "vol_fin": 50.0,
        "emin": 0.0,
        "emax": 500.0,
    }


def _write_csv(tmp_path: Path, rows: List[str]) -> Path:
    """Write a minimal RoR-equivalence CSV and return its path."""
    csv = tmp_path / "ror_equiv.csv"
    csv.write_text("name,vmax_hm3\n" + "\n".join(rows) + "\n", encoding="utf-8")
    return csv


def _run(
    centrals: List[Dict[str, Any]],
    options: Dict[str, Any],
) -> Dict[str, Any]:
    """Run JunctionWriter on *centrals* with *options* and return the system dict."""
    jw = JunctionWriter(
        central_parser=MockCentralParser(centrals),
        options=options,
    )
    result = jw.to_json_array()
    assert result, "expected a non-empty system output"
    return result[0]


# ─── Disabled / no-op cases ─────────────────────────────────────────────────


def test_disabled_by_default():
    """Without any RoR options, no daily-cycle reservoir is emitted."""
    system = _run([_make_serie("CentA", 1)], options={})
    assert all(not r.get("daily_cycle", False) for r in system["reservoir_array"])
    # Pure serie input means no embalse reservoir either.
    assert system["reservoir_array"] == []


def test_disabled_explicit_none(tmp_path: Path):
    """``ror_as_reservoirs='none'`` short-circuits before reading the CSV."""
    # Point to a non-existent CSV — it must not be opened.
    fake_csv = tmp_path / "does_not_exist.csv"
    system = _run(
        [_make_serie("CentA", 1)],
        options={
            "ror_as_reservoirs": "none",
            "ror_as_reservoirs_file": fake_csv,
        },
    )
    assert system["reservoir_array"] == []


def test_file_without_selection_is_noop(tmp_path: Path, caplog):
    """Passing only the CSV path (no selection) must be a silent no-op."""
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    import logging

    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.junction_writer"):
        system = _run(
            [_make_serie("CentA", 1)],
            options={"ror_as_reservoirs_file": csv},
        )
    assert system["reservoir_array"] == []


# ─── Error paths ─────────────────────────────────────────────────────────────


def test_selection_without_csv_raises():
    """``ror_as_reservoirs='all'`` without a CSV file is a hard error."""
    with pytest.raises(ValueError, match="--ror-as-reservoirs-file"):
        _run(
            [_make_serie("CentA", 1)],
            options={"ror_as_reservoirs": "all"},
        )


def test_unknown_name_in_selection(tmp_path: Path):
    """A selected name that is not in the CSV whitelist is an error."""
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    with pytest.raises(ValueError, match="Ghost"):
        _run(
            [_make_serie("CentA", 1), _make_serie("Ghost", 2)],
            options={
                "ror_as_reservoirs": "Ghost",
                "ror_as_reservoirs_file": csv,
            },
        )


def test_whitelisted_central_missing_from_case(tmp_path: Path):
    """Whitelist entry not present in the current case is an error."""
    csv = _write_csv(tmp_path, ["Ghost,1.5"])
    with pytest.raises(ValueError, match="Ghost"):
        _run(
            [_make_serie("CentA", 1)],
            options={
                "ror_as_reservoirs": "all",
                "ror_as_reservoirs_file": csv,
            },
        )


def test_wrong_type_rejected(tmp_path: Path):
    """Whitelisting an embalse central is rejected with a type-mismatch error."""
    csv = _write_csv(tmp_path, ["Dam1,1.5"])
    centrals = [_make_embalse("Dam1", 1)]
    with pytest.raises(ValueError, match="type"):
        _run(
            centrals,
            options={
                "ror_as_reservoirs": "all",
                "ror_as_reservoirs_file": csv,
            },
        )


def test_bus_nonpositive_rejected(tmp_path: Path):
    """A whitelisted serie with bus<=0 is rejected (no turbine to drain it)."""
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    # bus=0 serie with no downstream is normally skipped, but the whitelist
    # check runs first — so we still get the specific bus<=0 error.
    # Give it ser_hid>0 so it is referenced and kept around.
    cent = _make_serie("CentA", 1, bus=0)
    cent["ser_hid"] = 2
    drain = _make_serie("Sink", 2, bus=1)
    with pytest.raises(ValueError, match="bus"):
        _run(
            [cent, drain],
            options={
                "ror_as_reservoirs": "all",
                "ror_as_reservoirs_file": csv,
            },
        )


def test_efficiency_nonpositive_rejected(tmp_path: Path):
    """A whitelisted serie with efficiency<=0 is rejected."""
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    with pytest.raises(ValueError, match="efficiency"):
        _run(
            [_make_serie("CentA", 1, efficiency=0.0)],
            options={
                "ror_as_reservoirs": "all",
                "ror_as_reservoirs_file": csv,
            },
        )


# ─── Happy paths ─────────────────────────────────────────────────────────────


def _find_ror(system: Dict[str, Any], name: str) -> Dict[str, Any]:
    matches = [
        r
        for r in system["reservoir_array"]
        if r.get("name") == name and r.get("daily_cycle") is True
    ]
    assert len(matches) == 1, (
        f"expected exactly one daily-cycle reservoir for {name!r}, got {matches}"
    )
    return matches[0]


def test_happy_path_explicit_list(tmp_path: Path):
    """Explicit 'A,B' selection promotes exactly those centrals."""
    csv = _write_csv(tmp_path, ["CentA,1.25", "CentB,2.5", "CentC,3.75"])
    system = _run(
        [
            _make_serie("CentA", 1),
            _make_serie("CentB", 2),
            _make_serie("CentC", 3),
        ],
        options={
            "ror_as_reservoirs": "CentA,CentB",
            "ror_as_reservoirs_file": csv,
        },
    )
    ra = _find_ror(system, "CentA")
    assert ra["uid"] == 1
    assert ra["junction"] == "CentA"
    assert ra["emin"] == 0.0
    assert ra["emax"] == 1.25
    assert ra["capacity"] == 1.25
    assert ra["annual_loss"] == 0.0
    assert ra["daily_cycle"] is True

    rb = _find_ror(system, "CentB")
    assert rb["uid"] == 2
    assert rb["emax"] == 2.5
    assert rb["capacity"] == 2.5

    # CentC was in the CSV but not selected.
    assert not any(
        r.get("name") == "CentC" and r.get("daily_cycle")
        for r in system["reservoir_array"]
    )


def test_happy_path_all_selection(tmp_path: Path):
    """Selection ``'all'`` promotes every whitelist entry."""
    csv = _write_csv(tmp_path, ["CentA,1.0", "CentB,2.0"])
    system = _run(
        [_make_serie("CentA", 1), _make_serie("CentB", 2)],
        options={
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": csv,
        },
    )
    promoted = {
        r["name"] for r in system["reservoir_array"] if r.get("daily_cycle") is True
    }
    assert promoted == {"CentA", "CentB"}
    assert _find_ror(system, "CentA")["emax"] == 1.0
    assert _find_ror(system, "CentB")["emax"] == 2.0


def test_coexists_with_embalse_reservoir(tmp_path: Path):
    """An embalse reservoir and a promoted serie coexist in reservoir_array."""
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    system = _run(
        [_make_embalse("Dam1", 10), _make_serie("CentA", 1)],
        options={
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": csv,
        },
    )

    dam_entries = [r for r in system["reservoir_array"] if r["name"] == "Dam1"]
    assert len(dam_entries) == 1
    # The embalse reservoir must not be flagged as daily-cycle.
    assert dam_entries[0].get("daily_cycle", False) is False

    ror = _find_ror(system, "CentA")
    assert ror["emax"] == 1.5
    assert ror["capacity"] == 1.5
