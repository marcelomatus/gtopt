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
    """Write a minimal RoR-equivalence CSV and return its path.

    Each row in *rows* is of the form ``"Name,vmax"`` and is expanded
    to ``"Name,vmax,1.0"`` (production_factor defaulted to 1.0) so the
    older fixture contract is preserved.  To exercise a non-default
    production_factor use :func:`_write_csv_full`.
    """
    csv = tmp_path / "ror_equiv.csv"
    body_rows = [f"{r},1.0" if r.count(",") == 1 else r for r in rows]
    csv.write_text(
        "name,vmax_hm3,production_factor\n" + "\n".join(body_rows) + "\n",
        encoding="utf-8",
    )
    return csv


def _write_csv_full(tmp_path: Path, rows: List[str]) -> Path:
    """Write a RoR-equivalence CSV where each row is ``Name,vmax,prod``."""
    csv = tmp_path / "ror_equiv_full.csv"
    csv.write_text(
        "name,vmax_hm3,production_factor\n" + "\n".join(rows) + "\n",
        encoding="utf-8",
    )
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


def test_selection_without_csv_falls_back_to_packaged_default():
    """Omitting ``ror_as_reservoirs_file`` falls back to the packaged CSV.

    With the packaged default active, ``ror_as_reservoirs='all'`` tries to
    promote every whitelist entry shipped in ``templates/ror_equivalence.csv``
    (LA_HIGUERA, LA_CONFLUENCIA, ANGOSTURA, MACHICURA).  None of those are in
    this synthetic case, so the resolver fails with a ``not present in the
    current PLP case`` error — proving the fallback fired.
    """
    with pytest.raises(ValueError, match="not present in the current PLP case"):
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


def test_csv_production_factor_overrides_plp_efficiency(tmp_path: Path):
    """The CSV ``production_factor`` must override the PLP ``efficiency``.

    This is the whole point of requiring the column: many pasada centrals
    carry a placeholder ``efficiency=1.0`` in plpcnfce.dat that is not
    physically meaningful, so the CSV value is authoritative when the
    central is promoted to a daily-cycle reservoir.
    """
    csv = _write_csv_full(tmp_path, ["CentA,1.5,0.42", "CentB,2.0,1.77"])
    # Both centrals ship with the PLP placeholder efficiency=1.0.
    system = _run(
        [
            _make_serie("CentA", 1, efficiency=1.0),
            _make_serie("CentB", 2, efficiency=1.0),
        ],
        options={
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": csv,
        },
    )
    turbines = {t["name"]: t for t in system["turbine_array"]}
    assert turbines["CentA"]["production_factor"] == 0.42
    assert turbines["CentB"]["production_factor"] == 1.77


def test_non_promoted_central_keeps_plp_efficiency(tmp_path: Path):
    """A central NOT in the CSV selection must keep its PLP efficiency."""
    csv = _write_csv_full(tmp_path, ["CentA,1.5,0.42"])
    system = _run(
        [
            _make_serie("CentA", 1, efficiency=1.0),
            _make_serie("CentB", 2, efficiency=0.73),
        ],
        options={
            "ror_as_reservoirs": "CentA",
            "ror_as_reservoirs_file": csv,
        },
    )
    turbines = {t["name"]: t for t in system["turbine_array"]}
    # Promoted central: overridden by CSV.
    assert turbines["CentA"]["production_factor"] == 0.42
    # Non-promoted central: PLP value is preserved.
    assert turbines["CentB"]["production_factor"] == 0.73


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


# ─── Reservoir spill path via drain=True on the upper junction ──────────────
#
# When a pasada/serie central is promoted to a daily-cycle reservoir and the
# PLP case has no spillway (``ser_ver == 0`` → ``ver_waterway is None``), the
# physically correct spill path is simply ``drain=True`` on the central's
# upper junction.  The turbine's generation waterway is the only waterway
# attached to that junction, so the drain sink acts as the pondage's
# overflow outlet — no synthetic bypass is needed.  These tests pin that
# behavior against regressions.


def test_ror_promotion_drain_on_upper_junction_when_no_spillway(tmp_path: Path):
    """Promoted central with ser_ver=0 → ``drain=True`` on upper junction.

    The reservoir sits on the central's own junction (= gen_waterway
    upstream endpoint).  Because the PLP case has no spillway waterway,
    excess inflow leaves via the junction drain sink — the turbine is the
    only physical waterway connected to that upper junction.
    """
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    system = _run(
        [_make_serie("CentA", 1)],  # ser_hid=0, ser_ver=0 by default
        options={
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": csv,
        },
    )

    # Reservoir exists on the central's own junction (= gen_waterway.junction_a).
    ror = _find_ror(system, "CentA")
    assert ror["junction"] == "CentA"

    # No synthetic bypass waterway is emitted.
    assert not [
        w for w in system["waterway_array"] if w["name"].endswith("_ror_bypass")
    ]

    # Exactly one waterway (the generation waterway) is attached to the
    # central's upper junction.  The turbine is its only physical outlet.
    upper_outlets = [w for w in system["waterway_array"] if w["junction_a"] == "CentA"]
    assert len(upper_outlets) == 1
    assert upper_outlets[0]["name"].startswith("CentA_gen_")

    # Post-86616b80 rule (junction_writer.py:979):
    #   drain = gen_waterway is None AND ver_waterway is None
    # Here gen_waterway exists (CentA → ocean), so the source junction is
    # NOT a drain.  Overflow handling goes through the daily-cycle
    # reservoir's emin/emax envelope rather than a free escape valve on
    # the source junction.
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["CentA"].get("drain", False) is False


def test_ror_promotion_no_bypass_when_plp_spillway_exists(tmp_path: Path):
    """PLP ``ser_ver != 0`` → real spillway, no synthetic bypass.

    When the PLP case already has a spillway waterway, the reservoir has
    two outlets from its upper junction (gen + ver), so no bypass is
    needed and no extra drain flag is added by the RoR promotion.
    """
    csv = _write_csv(tmp_path, ["CentA,1.5"])
    cent = _make_serie("CentA", 1)
    cent["ser_ver"] = 2  # points at a downstream sink
    sink = _make_serie("Sink", 2)
    system = _run(
        [cent, sink],
        options={
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": csv,
        },
    )

    # Real PLP spillway waterway is present.
    ver = [w for w in system["waterway_array"] if w["name"].startswith("CentA_ver_")]
    assert len(ver) == 1

    # No synthetic bypass — the PLP spillway is enough.
    assert not [
        w for w in system["waterway_array"] if w["name"].endswith("_ror_bypass")
    ]

    # Junction drain remains OFF because the real spillway provides the
    # physical overflow route.
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["CentA"].get("drain", False) is False


def test_ror_promotion_does_not_affect_non_promoted_central(tmp_path: Path):
    """A non-promoted central keeps the old junction behavior unchanged."""
    csv = _write_csv(tmp_path, ["Other,1.5"])
    system = _run(
        [_make_serie("CentA", 1), _make_serie("Other", 2)],
        options={
            "ror_as_reservoirs": "Other",
            "ror_as_reservoirs_file": csv,
        },
    )
    # No synthetic bypass waterway anywhere.
    assert not [
        w for w in system["waterway_array"] if w["name"].endswith("_ror_bypass")
    ]
    # CentA was NOT promoted.  Post-86616b80 the source junction is a
    # drain only when BOTH gen and ver waterways are absent (see the rule
    # at junction_writer.py:979).  CentA has a gen waterway (to its
    # synthetic ocean drain), so drain=False — overflow handling goes
    # through the explicit `_gen` arc and the synthetic ocean junction.
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["CentA"].get("drain", False) is False


def test_ror_promotion_serie_with_upstream_inflow_uses_drain(tmp_path: Path):
    """Serie central receiving upstream inflow: drain-on-junction, no bypass.

    Mirrors ANGOSTURA/LA_CONFLUENCIA topology: upstream generation waterway
    delivers water to the central, the central's own gen waterway drains
    downstream through the turbine, and ``drain=True`` on the central's
    upper junction provides the overflow spill path.
    """
    csv = _write_csv(tmp_path, ["Mid,1.5"])
    upstream = _make_serie("Up", 1)
    upstream["ser_hid"] = 2  # Up → Mid
    mid = _make_serie("Mid", 2)
    mid["ser_hid"] = 3  # Mid → Down
    down = _make_serie("Down", 3)
    system = _run(
        [upstream, mid, down],
        options={
            "ror_as_reservoirs": "Mid",
            "ror_as_reservoirs_file": csv,
        },
    )

    # No synthetic bypass waterway.
    assert not [
        w for w in system["waterway_array"] if w["name"].endswith("_ror_bypass")
    ]

    # Mid has gen waterway → Down (real downstream) and ser_ver=0 with
    # vert_max=0 (no spillway path needed).  The gen arc is the only
    # outlet from Mid's upper junction.
    mid_outlets = [w for w in system["waterway_array"] if w["junction_a"] == "Mid"]
    assert len(mid_outlets) == 1
    assert mid_outlets[0]["name"].startswith("Mid_gen_")
    assert mid_outlets[0]["junction_b"] == "Down"

    # Post-86616b80 rule (junction_writer.py:979): drain is True only
    # when BOTH gen and ver are absent.  Mid has a gen waterway, so
    # drain=False — overflow handling is via the daily-cycle reservoir's
    # emin/emax envelope after RoR promotion, not a free escape valve.
    junctions = {j["name"]: j for j in system["junction_array"]}
    assert junctions["Mid"].get("drain", False) is False
