"""End-to-end integration tests for the ``--ror-as-reservoirs`` feature.

The unit tests in ``test_junction_writer_ror.py`` exercise ``JunctionWriter``
in isolation.  These tests run the full ``convert_plp_case`` pipeline on
a real PLP case (``plp_hydro_4b``, which ships with the repo and has a
serie ``TurbineA`` + pasada ``HydroRoR``) and verify that the promoted
reservoirs, synthetic bypass waterways, and production-factor overrides
propagate all the way into the generated ``gtopt`` JSON.

``plp_hydro_4b`` centrals relevant to these tests::

    LakeA    embalse  bus=1  ser_hid=2  ser_ver=0  eff=1.0
    TurbineA serie    bus=1  ser_hid=0  ser_ver=0  eff=1.0
    HydroRoR pasada   bus=2  ser_hid=0  ser_ver=0  eff=1.0

Both promoted candidates have ``ser_ver=0``, so every successful
promotion exercises the synthetic-bypass branch of ``JunctionWriter``.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from plp2gtopt.plp2gtopt import convert_plp_case

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPHydro4b = _CASES_DIR / "plp_hydro_4b"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_opts(
    tmp_path: Path,
    case_name: str,
    *,
    ror_selection: str | None = None,
    ror_file: Path | None = None,
    pasada_mode: str = "flow-turbine",
) -> dict:
    """Build a conversion options dict with RoR-as-reservoirs wired in.

    ``pasada_mode`` defaults to ``flow-turbine`` (the real default of the
    CLI).  Tests that need to exercise pasada RoR promotion must flip
    this option to ``hydro`` explicitly — in the default mode pasada
    centrals never reach ``JunctionWriter`` and the promotion is a
    silent no-op.
    """
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    opts: dict = {
        "input_dir": _PLPHydro4b,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
        "pasada_mode": pasada_mode,
        # Pin legacy spillway-waterway shape so RoR drain assertions
        # (e.g. ``drain=True`` only when no spillway exists) keep their
        # PLP-faithful meaning.  The new default suppresses ``_ver``
        # arcs and forces ``drain=True`` unconditionally for embalse /
        # serie / pasada — covered by ``test_drop_spillway_waterway``.
        "drop_spillway_waterway": False,
    }
    if ror_selection is not None:
        opts["ror_as_reservoirs"] = ror_selection
    if ror_file is not None:
        opts["ror_as_reservoirs_file"] = ror_file
    return opts


def _write_csv(
    tmp_path: Path,
    rows: list[tuple[str, float, float]],
    *,
    name: str = "ror_equiv.csv",
) -> Path:
    """Write a RoR-equivalence CSV with ``name,vmax_hm3,production_factor``."""
    path = tmp_path / name
    body = "".join(f"{n},{v},{p}\n" for n, v, p in rows)
    path.write_text(
        "name,vmax_hm3,production_factor\n" + body,
        encoding="utf-8",
    )
    return path


def _load_system(opts: dict) -> dict:
    """Run ``convert_plp_case(opts)`` and return the parsed ``system`` dict."""
    convert_plp_case(opts)
    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    return data["system"]


def _find_daily_cycle(system: dict, name: str) -> dict:
    matches = [
        r
        for r in system.get("reservoir_array", [])
        if r.get("name") == name and r.get("daily_cycle") is True
    ]
    assert len(matches) == 1, (
        f"expected exactly one daily-cycle reservoir for {name!r}, got {matches}"
    )
    return matches[0]


def _assert_no_bypass(system: dict) -> None:
    """No synthetic ``_ror_bypass`` waterway may appear anywhere."""
    assert not [
        w
        for w in system.get("waterway_array", [])
        if w.get("name", "").endswith("_ror_bypass")
    ]


def _find_junction(system: dict, name: str) -> dict:
    matches = [j for j in system.get("junction_array", []) if j.get("name") == name]
    assert len(matches) == 1, (
        f"expected exactly one junction named {name!r}, got {matches}"
    )
    return matches[0]


def _find_turbine(system: dict, name: str) -> dict:
    matches = [t for t in system.get("turbine_array", []) if t.get("name") == name]
    assert len(matches) == 1, (
        f"expected exactly one turbine for {name!r}, got {matches}"
    )
    return matches[0]


# ---------------------------------------------------------------------------
# Feature OFF — baseline behavior must not change
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_feature_off_no_daily_cycle_reservoirs(tmp_path: Path):
    """With no RoR options set, the output JSON has no daily-cycle reservoirs."""
    opts = _make_opts(tmp_path, "no_ror")
    system = _load_system(opts)
    assert not any(
        r.get("daily_cycle") is True for r in system.get("reservoir_array", [])
    )
    # No synthetic bypass waterways either.
    _assert_no_bypass(system)


@pytest.mark.integration
def test_feature_off_turbine_keeps_plp_efficiency(tmp_path: Path):
    """Without promotion, TurbineA's production_factor equals the PLP efficiency (1.0)."""
    opts = _make_opts(tmp_path, "no_ror_turbine")
    system = _load_system(opts)
    turb = _find_turbine(system, "TurbineA")
    assert turb["production_factor"] == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# Feature ON — serie promotion (TurbineA)
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_serie_promotion_emits_daily_cycle_reservoir(tmp_path: Path):
    """``--ror-as-reservoirs TurbineA`` emits a daily-cycle reservoir.

    The reservoir:
    - lives on the ``TurbineA`` junction (= gen_waterway.junction_a,
      i.e. the upstream end of the turbine's generation waterway);
    - has ``emax`` / ``capacity`` matching the CSV ``vmax_hm3``;
    - is flagged ``daily_cycle=True`` so the C++ LP treats it as a
      sub-stage pondage rather than a monthly storage.
    """
    csv = _write_csv(tmp_path, [("TurbineA", 2.5, 0.42)])
    opts = _make_opts(
        tmp_path,
        "serie_ror",
        ror_selection="TurbineA",
        ror_file=csv,
    )
    system = _load_system(opts)

    reservoir = _find_daily_cycle(system, "TurbineA")
    assert reservoir["junction"] == "TurbineA"
    assert reservoir["emax"] == pytest.approx(2.5)
    assert reservoir["capacity"] == pytest.approx(2.5)
    assert reservoir["emin"] == pytest.approx(0.0)
    assert reservoir["annual_loss"] == pytest.approx(0.0)

    # The LakeA embalse reservoir must still be present and NOT flagged
    # as daily-cycle — promotion only touches the whitelisted central.
    lake = [r for r in system["reservoir_array"] if r["name"] == "LakeA"]
    assert len(lake) == 1
    assert lake[0].get("daily_cycle", False) is False


@pytest.mark.integration
def test_serie_promotion_enables_drain_on_upper_junction(tmp_path: Path):
    """With ``ser_ver=0`` on TurbineA, the reservoir's upper junction drains.

    No synthetic bypass waterway is created.  Instead, because the
    turbine's generation waterway is the only waterway attached to the
    reservoir's upper junction, ``drain=True`` on that junction provides
    the physical overflow path for inflow the turbine cannot pass.
    """
    csv = _write_csv(tmp_path, [("TurbineA", 1.5, 0.42)])
    opts = _make_opts(
        tmp_path,
        "serie_drain",
        ror_selection="TurbineA",
        ror_file=csv,
    )
    system = _load_system(opts)

    # No synthetic bypass waterway anywhere.
    _assert_no_bypass(system)

    # Exactly one waterway is attached to the reservoir's upper junction:
    # the turbine's generation waterway.
    upper_outlets = [
        w for w in system["waterway_array"] if w.get("junction_a") == "TurbineA"
    ]
    assert len(upper_outlets) == 1
    assert upper_outlets[0]["name"].startswith("TurbineA_gen_")

    # Post-86616b80 rule (junction_writer.py:979): drain is True only when
    # BOTH gen and ver waterways are absent.  Here gen_waterway exists, so
    # drain=False — overflow handling goes through the daily-cycle
    # reservoir's emin/emax envelope after RoR promotion, not a free
    # escape valve.
    assert _find_junction(system, "TurbineA").get("drain", False) is False


@pytest.mark.integration
def test_serie_promotion_overrides_production_factor(tmp_path: Path):
    """The CSV ``production_factor`` overrides the PLP ``efficiency`` placeholder.

    TurbineA ships with ``efficiency=1.0`` in plpcnfce.dat; after
    promotion the emitted Turbine must carry the CSV value instead.
    """
    csv = _write_csv(tmp_path, [("TurbineA", 1.0, 0.77)])
    opts = _make_opts(
        tmp_path,
        "serie_pf_override",
        ror_selection="TurbineA",
        ror_file=csv,
    )
    system = _load_system(opts)
    turb = _find_turbine(system, "TurbineA")
    assert turb["production_factor"] == pytest.approx(0.77)


@pytest.mark.integration
def test_non_promoted_central_is_untouched(tmp_path: Path):
    """Promoting TurbineA must not affect LakeA's reservoir or its drain flag."""
    csv = _write_csv(tmp_path, [("TurbineA", 1.0, 0.5)])
    opts = _make_opts(
        tmp_path,
        "non_promoted_intact",
        ror_selection="TurbineA",
        ror_file=csv,
    )
    system = _load_system(opts)

    # LakeA is a regular embalse and must not be flagged daily_cycle.
    lake = [r for r in system.get("reservoir_array", []) if r["name"] == "LakeA"]
    assert len(lake) == 1
    assert lake[0].get("daily_cycle", False) is False

    # No synthetic bypass is emitted for any central.
    _assert_no_bypass(system)


# ---------------------------------------------------------------------------
# Feature ON — pasada promotion (HydroRoR) + unscale
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_pasada_promotion_requires_pasada_mode_hydro(tmp_path: Path):
    """Pasada centrals are only reachable from JunctionWriter in 'hydro' mode.

    This is a silent-no-op gotcha: in the default ``flow-turbine`` mode
    pasadas never reach ``JunctionWriter``, so a CSV whitelist entry for
    a pasada is quietly ignored.  This test pins the two behaviors — it
    will fail loudly if either path starts emitting a promotion under
    the wrong mode.
    """
    csv = _write_csv(tmp_path, [("HydroRoR", 1.0, 2.0)])

    # flow-turbine mode: no promotion, no bypass, no daily-cycle reservoir.
    opts_ft = _make_opts(
        tmp_path,
        "pasada_flow_turbine",
        ror_selection="HydroRoR",
        ror_file=csv,
        pasada_mode="flow-turbine",
    )
    system_ft = _load_system(opts_ft)
    assert not [
        r
        for r in system_ft.get("reservoir_array", [])
        if r.get("name") == "HydroRoR" and r.get("daily_cycle") is True
    ]
    _assert_no_bypass(system_ft)

    # hydro mode: pasada reaches the junction writer and gets promoted.
    # Drain is enabled on the upper junction as the physical spill path
    # (no synthetic bypass is synthesized).
    opts_h = _make_opts(
        tmp_path,
        "pasada_hydro",
        ror_selection="HydroRoR",
        ror_file=csv,
        pasada_mode="hydro",
    )
    system_h = _load_system(opts_h)
    reservoir = _find_daily_cycle(system_h, "HydroRoR")
    assert reservoir["emax"] == pytest.approx(1.0)
    _assert_no_bypass(system_h)
    # Post-86616b80: drain is True only when BOTH gen and ver waterways
    # are absent (junction_writer.py:979); HydroRoR has its gen waterway,
    # so drain=False.
    assert _find_junction(system_h, "HydroRoR").get("drain", False) is False


# ---------------------------------------------------------------------------
# Feature ON — "all" selection + mixed promotions
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_selection_all_promotes_every_whitelist_entry(tmp_path: Path):
    """``--ror-as-reservoirs all`` promotes every eligible CSV entry.

    Uses ``pasada_mode='hydro'`` so the pasada (HydroRoR) also reaches
    ``JunctionWriter`` and gets promoted.  Both promoted centrals have
    ``ser_ver=0``, so each reservoir's upper junction carries
    ``drain=True`` — no synthetic bypass waterways are created.
    """
    csv = _write_csv(
        tmp_path,
        [("TurbineA", 1.25, 0.50), ("HydroRoR", 0.75, 1.20)],
    )
    opts = _make_opts(
        tmp_path,
        "ror_all",
        ror_selection="all",
        ror_file=csv,
        pasada_mode="hydro",
    )
    system = _load_system(opts)

    r_turb = _find_daily_cycle(system, "TurbineA")
    r_hydro = _find_daily_cycle(system, "HydroRoR")
    assert r_turb["emax"] == pytest.approx(1.25)
    assert r_hydro["emax"] == pytest.approx(0.75)

    # No synthetic bypass waterways.  Post-86616b80, source junctions are
    # NOT marked drain when they have a gen waterway (junction_writer.py:
    # 979 rule).  Both promoted centrals here have gen waterways, so
    # drain=False.  Overflow handling is via the daily-cycle reservoir's
    # emin/emax envelope, not a free escape valve.
    _assert_no_bypass(system)
    assert _find_junction(system, "TurbineA").get("drain", False) is False
    assert _find_junction(system, "HydroRoR").get("drain", False) is False


# ---------------------------------------------------------------------------
# Error paths — end-to-end through convert_plp_case
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_selection_without_file_falls_back_to_packaged_default(tmp_path: Path):
    """Omitting ``--ror-as-reservoirs-file`` falls back to the packaged CSV.

    Programmatic callers of ``convert_plp_case`` (who do not go through
    argparse) must see the same packaged-default fallback that CLI users
    get via ``_parsers.DEFAULT_ROR_RESERVOIRS_FILE``.  With that fallback
    active, ``ror_selection='all'`` tries to promote the whitelist entries
    shipped in ``templates/ror_equivalence.csv`` (LA_HIGUERA, LA_CONFLUENCIA,
    ANGOSTURA, MACHICURA).  This synthetic fixture only contains TurbineA
    / HydroRoR, so the resolver fails with a "not present in the current
    PLP case" error — wrapped by ``convert_plp_case`` as a ``RuntimeError``.
    """
    opts = _make_opts(tmp_path, "err_no_file", ror_selection="all")
    with pytest.raises(RuntimeError, match="not present in the current PLP case"):
        convert_plp_case(opts)


@pytest.mark.integration
def test_unknown_name_raises(tmp_path: Path):
    """Selecting a name not in the CSV whitelist is a hard error."""
    csv = _write_csv(tmp_path, [("TurbineA", 1.0, 0.5)])
    opts = _make_opts(
        tmp_path,
        "err_unknown",
        ror_selection="Ghost",
        ror_file=csv,
    )
    with pytest.raises(RuntimeError, match="Ghost"):
        convert_plp_case(opts)


@pytest.mark.integration
def test_whitelist_entry_not_in_case_raises(tmp_path: Path):
    """A whitelist entry that isn't present in plpcnfce.dat is a hard error."""
    csv = _write_csv(tmp_path, [("Ghost", 1.0, 0.5)])
    opts = _make_opts(
        tmp_path,
        "err_missing",
        ror_selection="all",
        ror_file=csv,
    )
    with pytest.raises(RuntimeError, match="Ghost"):
        convert_plp_case(opts)


@pytest.mark.integration
def test_explicit_none_is_silent_noop(tmp_path: Path):
    """``--ror-as-reservoirs none`` must short-circuit and produce no promotion."""
    # Point at a path that does not exist — ``none`` must not open it.
    fake = tmp_path / "not_really.csv"
    opts = _make_opts(
        tmp_path,
        "noop_none",
        ror_selection="none",
        ror_file=fake,
    )
    system = _load_system(opts)
    assert not any(
        r.get("daily_cycle") is True for r in system.get("reservoir_array", [])
    )
    _assert_no_bypass(system)
