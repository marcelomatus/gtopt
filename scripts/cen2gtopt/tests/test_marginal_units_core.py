# SPDX-License-Identifier: BSD-3-Clause
"""Core unit tests for cen2gtopt marginal-unit pipeline.

Locks in the five highest-leverage invariants identified during the
v3 cleanup pass:

1. ``cluster_buses_into_islands`` does NOT chain across an arbitrary
   distance just because each bus is < ``tol`` apart from the next.
   The ``ISLAND_MAX_SPREAD`` cap forces a split when the cumulative
   range within an island would exceed that bound.
2. ``identify_marginal`` picks the candidate whose CV is **absolutely
   closest** to λ_target (not the largest CV ≤ λ).
3. :func:`parse_voltage` round-trips the 3-digit voltage suffix.
4. :func:`_heuristic_bar_transf` produces the expected 17-char string
   for a clean PLEXOS node name.
5. :func:`pcp_solution.resolve_solution` with ``auto_download=False``
   never touches the network — it only scans the local cache.
"""

from __future__ import annotations

from unittest import mock

import pandas as pd
import pytest


# ---------------------------------------------------------------------------
# 1. cluster_buses_into_islands chained-tolerance pathology
# ---------------------------------------------------------------------------


def test_cluster_buses_into_islands_chained_pathology():
    """Five buses with consecutive λ-deltas of 0.4 USD/MWh — under
    pure pairwise tolerance they all chain into one island even
    though the spread is 1.6.  ``ISLAND_MAX_SPREAD = 0.8`` must
    force a split."""
    from cen2gtopt.marginal_units import cluster_buses_into_islands

    df = pd.DataFrame(
        {
            "date_utc": ["2026-04-22"] * 5,
            "quarter": [1] * 5,
            "bus_id": ["A", "B", "C", "D", "E"],
            "lmp": [70.0, 70.4, 70.8, 71.2, 71.6],
        },
    )
    out = cluster_buses_into_islands(df, tol=0.5, max_spread=0.8)

    # Without max_spread these would all chain into ONE island.
    # With max_spread=0.8 the run should split at least once
    # (range 0..1.6 > 0.8).
    n_islands = out["island_id"].nunique()
    assert n_islands >= 2, (
        f"expected ≥2 islands once cumulative spread exceeds 0.8 USD/MWh, "
        f"got {n_islands}; islands = {sorted(out['island_id'].unique())}"
    )

    # Tight cluster (all buses at the same λ) must stay as one island.
    df2 = pd.DataFrame(
        {
            "date_utc": ["2026-04-22"] * 4,
            "quarter": [1] * 4,
            "bus_id": ["A", "B", "C", "D"],
            "lmp": [70.0, 70.05, 70.1, 70.15],
        },
    )
    out2 = cluster_buses_into_islands(df2, tol=0.5, max_spread=0.8)
    assert out2["island_id"].nunique() == 1


# ---------------------------------------------------------------------------
# 2. identify_marginal closest-CV semantics
# ---------------------------------------------------------------------------


def test_identify_marginal_picks_closest_cv():
    """λ_target = 50 USD/MWh.  Candidate CVs = {30, 60, 100}.
    The picker MUST return CV=60 (Δ=10) — closer than 30 (Δ=20) or
    100 (Δ=50).  Locks in the absolute-closest-CV rule against
    accidental regression to "max CV ≤ λ" or the older inverse-MLF
    target."""
    from cen2gtopt.marginal_units import identify_marginal

    candidates = pd.DataFrame(
        {
            "id_central": [10, 20, 30],
            "central": ["UnitA", "UnitB", "UnitC"],
            "mw": [50.0, 50.0, 50.0],
            "pmin": [0.0, 0.0, 0.0],
            "pmax": [100.0, 100.0, 100.0],
            "forced": [False, False, False],
        },
    )
    fuel_by_id = {10: "biogas", 20: "gas", 30: "diesel"}
    cv_options_by_id = {10: [30.0], 20: [60.0], 30: [100.0]}

    out = identify_marginal(
        lambda_target=50.0,
        candidates=candidates,
        fuel_by_id=fuel_by_id,
        cv_options_by_id=cv_options_by_id,
    )
    assert out["marginal_id"] == 20
    assert out["marginal_central"] == "UnitB"
    assert out["marginal_cv"] == pytest.approx(60.0)
    assert out["marginal_fuel"] == "gas"

    # Edge: λ_target = 0.4 → renewable_curtailment regime.
    rc = identify_marginal(
        lambda_target=0.4,
        candidates=candidates,
        fuel_by_id=fuel_by_id,
        cv_options_by_id=cv_options_by_id,
    )
    assert rc["regime"] == "renewable_curtailment"
    assert rc["marginal_id"] is None

    # Edge: λ_target = 1500 → demand_fail regime.
    df = identify_marginal(
        lambda_target=1500.0,
        candidates=candidates,
        fuel_by_id=fuel_by_id,
        cv_options_by_id=cv_options_by_id,
    )
    assert df["regime"] == "demand_fail"
    assert df["marginal_central"] == "demand_fail_cost"


# ---------------------------------------------------------------------------
# 3. parse_voltage
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("bar_transf", "expected"),
    [
        ("CRUCERO_______220", 220),
        ("D.ALMAGRO_____110", 110),
        ("ELMAITEN______066", 66),
        ("A.JAHUEL______500", 500),
        ("PID-PID_______110", 110),
        ("", 0),
        (None, 0),
        ("NO_VOLTAGE", 0),
    ],
)
def test_parse_voltage(bar_transf, expected):
    from cen2gtopt._bus_catalogue import parse_voltage

    assert parse_voltage(bar_transf) == expected


# ---------------------------------------------------------------------------
# 4. _heuristic_bar_transf round-trip
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("plexos_name", "expected"),
    [
        # Clean cases — name ≤ 14 chars, 3-digit voltage suffix.
        ("Crucero220", "CRUCERO_______220"),
        ("ElMaiten066", "ELMAITEN______066"),
        ("Punta500", "PUNTA_________500"),
        # Exactly 14 chars in the prefix.
        ("ABCDEFGHIJKLMN500", "ABCDEFGHIJKLMN500"),
    ],
)
def test_heuristic_bar_transf_round_trip(plexos_name, expected):
    from cen2gtopt.prepopulate_cache import _heuristic_bar_transf

    assert _heuristic_bar_transf(plexos_name) == expected


def test_heuristic_bar_transf_returns_none_when_unparseable():
    from cen2gtopt.prepopulate_cache import _heuristic_bar_transf

    # No 3-digit voltage suffix.
    assert _heuristic_bar_transf("NoVoltage") is None
    # Name part > 14 chars (needs hand-curation).
    assert _heuristic_bar_transf("ThisNameIsWayTooLong220") is None


# ---------------------------------------------------------------------------
# 6. _to_float_es multi-fuel capacity parser
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        # Plain number forms
        ("145.334", 145.334),
        ("145,334", 145.334),
        ("145", 145.0),
        (145.5, 145.5),
        (200, 200.0),
        # Spanish-locale thousands separator
        ("1.234,56", 1234.56),
        # Multi-fuel CCGT format from CEN unidades_generadoras
        # ("primary (fuel) / fallback (fuel)" — return primary)
        ("214,99 (GN) / 203,74 (Diésel)", 214.99),
        ("117,72", 117.72),
        # Bracketed annotation only
        ("250 (GN)", 250.0),
        # Single fallback fuel only
        ("203,74 (Diésel)", 203.74),
    ],
)
def test_to_float_es_handles_multifuel(raw, expected):
    """``_to_float_es`` must accept the multi-fuel CCGT capacity format
    used in the CEN unidades_generadoras catalogue, returning the
    primary-fuel value.  Regression for the per-block pmax-lumping bug
    that mis-flagged ~96 % of NEHUENCO / COLBUN / ATACAMA quarters as
    ``matched_capped_pmax`` (PR follow-up to white-paper v1)."""
    from cen2gtopt.marginal_units import _to_float_es

    assert _to_float_es(raw) == pytest.approx(expected)


@pytest.mark.parametrize(
    "raw",
    [None, "", "   ", "no number here", float("nan")],
)
def test_to_float_es_returns_nan_on_unparseable(raw):
    import math

    from cen2gtopt.marginal_units import _to_float_es

    out = _to_float_es(raw)
    assert math.isnan(out)


# ---------------------------------------------------------------------------
# 7. fetch_cmg_bulk — sequential / adaptive throttle
# ---------------------------------------------------------------------------


def test_fetch_cmg_bulk_is_sequential_with_adaptive_throttle(monkeypatch):
    """``fetch_cmg_bulk`` must (a) call ``fetch_by_name`` ONE bus at a
    time in input order — never in parallel — and (b) widen the
    inter-request delay on a simulated 429 / failure cascade."""
    from cen2gtopt import marginal_units as mu

    call_order: list[str] = []
    sleep_log: list[float] = []

    # Intercept fetch_by_name: succeed for 'A', fail for 'B', succeed for 'C'.
    def fake_fetch_by_name(_client, _endpoint, *, start, extra_params):  # noqa: ARG001
        bt = extra_params["bar_transf"]
        call_order.append(bt)
        if bt == "B":
            raise RuntimeError("simulated 429 cascade")
        return pd.DataFrame(
            [
                {
                    "id_info": 1,
                    "barra_transf": bt,
                    "barra_info": "X",
                    "fecha": "2026-04-22",
                    "hra": 0,
                    "min": 0,
                    "cmg_usd_mwh_": 50.0,
                }
            ]
        )

    monkeypatch.setattr(mu, "fetch_by_name", fake_fetch_by_name)
    monkeypatch.setattr(mu.time, "sleep", sleep_log.append)

    out = mu.fetch_cmg_bulk(
        client=None,  # unused by the fake
        date="2026-04-22",
        endpoint="cmg_real",
        bar_transfs=["A", "B", "C"],
        verbose=False,
        base_delay_ms=10.0,
        max_delay_ms=80.0,
        retry_failed=True,
    )

    # Sequential ordering: A, B, C, then a retry of B.
    assert call_order == ["A", "B", "C", "B"]

    # Adaptive throttle: after the failure on B, the next sleep
    # (between B and C) must be larger than the initial base delay.
    assert sleep_log[0] == pytest.approx(0.010)  # after A success
    assert sleep_log[1] > sleep_log[0]  # after B failure → bumped
    # The retry-pass sleep is at the ceiling delay.
    assert sleep_log[-1] == pytest.approx(0.080)

    # Result frame has the 2 successful buses (A and C).
    assert sorted(out["bar_transf"].unique()) == ["A", "C"]


# ---------------------------------------------------------------------------
# 8. build_unit_metadata — binding pmin = max of 3 MT fields
# ---------------------------------------------------------------------------


def test_build_unit_metadata_pmin_uses_max_of_three_mts():
    """pmin per id_central must be the SUM (across blocks) of the
    per-block ``max(pot_min_tecnica, min_tecnico_normativa_ambiental,
    min_tecnico_control_frecuencia)``.  Captures the binding minimum
    given the unit's operational mode — e.g. CCGT GT pinned at its
    environmental NOx-control floor, or coal at its AGC-roster MT."""
    from cen2gtopt.marginal_units import build_unit_metadata

    # Two id_centrales: one CCGT (env-MT binds), one steam unit
    # (freq-MT binds), one PMG-eolic (no MT data — skipped).
    units = pd.DataFrame(
        [
            # CCGT id=197 with 2 blocks (TG / TV)
            {
                "id_central": 197,
                "central": "TER X",
                "tipo_tecnologia_unidad": "CCGT-TG",
                "pot_min_tecnica": "14",
                "min_tecnico_normativa_ambiental": "145",  # binds
                "min_tecnico_control_frecuencia": "14",
                "pot_neta_efectiva": "214,99 (GN) / 203,74 (Diésel)",
                "pot_max_bruta": "217,65",
            },
            {
                "id_central": 197,
                "central": "TER X",
                "tipo_tecnologia_unidad": "CCGT-TV",
                "pot_min_tecnica": "70",
                "min_tecnico_normativa_ambiental": "80",  # binds
                "min_tecnico_control_frecuencia": "70",
                "pot_neta_efectiva": "111,98",
                "pot_max_bruta": "117,72",
            },
            # Steam id=300 with freq-MT binding
            {
                "id_central": 300,
                "central": "TER COCHRANE",
                "tipo_tecnologia_unidad": "Steam",
                "pot_min_tecnica": "60",
                "min_tecnico_normativa_ambiental": "60",
                "min_tecnico_control_frecuencia": "90",  # binds
                "pot_neta_efectiva": "250,622",
                "pot_max_bruta": "260",
            },
            # PMG eolic id=400 with no parseable MT data
            {
                "id_central": 400,
                "central": "PE PMG",
                "tipo_tecnologia_unidad": "Wind",
                "pot_min_tecnica": "no aplica",
                "min_tecnico_normativa_ambiental": "0,1",
                "min_tecnico_control_frecuencia": "0,1",
                "pot_neta_efectiva": "1,9998",
                "pot_max_bruta": "2",
            },
        ]
    )

    # Default 'technical' pmin uses only pot_min_tecnica.
    pmin_by_id, pmax_by_id, tipo_by_id = build_unit_metadata(units)
    # CCGT 197: per-block tech_min sum = 14 + 70 = 84 MW
    assert pmin_by_id[197] == pytest.approx(84.0)
    # CCGT 197 pmax: 214.99 + 111.98 = 326.97
    assert pmax_by_id[197] == pytest.approx(326.97)
    assert tipo_by_id[197] == "CCGT-TG"
    # Steam 300: tech_min only = 60
    assert pmin_by_id[300] == pytest.approx(60.0)
    # PMG 400: tech_min unparseable → 0
    assert pmin_by_id[400] == pytest.approx(0.0)

    # Opt-in 'binding' pmin = MAX of the 3 MT fields per block.
    pmin_b, _pmax_b, _ = build_unit_metadata(units, pmin_mode="binding")
    # CCGT 197: per-block max = (145, 80) → sum 225 MW
    assert pmin_b[197] == pytest.approx(225.0)
    # Steam 300: max(60, 60, 90) = 90
    assert pmin_b[300] == pytest.approx(90.0)
    # PMG 400: only env / frq parseable → max(0.1, 0.1) = 0.1
    assert pmin_b[400] == pytest.approx(0.1)


def test_build_unit_metadata_handles_empty_input():
    from cen2gtopt.marginal_units import build_unit_metadata

    pmin, pmax, tipo = build_unit_metadata(pd.DataFrame())
    assert not pmin
    assert not pmax
    assert not tipo


# ---------------------------------------------------------------------------
# 9. Instrucciones lookup helpers — surfacing zona_desaclope, motivo,
# control_tension that were previously unused
# ---------------------------------------------------------------------------


def test_build_instrucciones_lookup_surfaces_all_fields():
    from cen2gtopt.marginal_units import build_instrucciones_lookup

    instr = pd.DataFrame(
        [
            {
                "id_central": 100,
                "hour": 6,
                "consigna": "MT",
                "estado": "RO",
                "motivo": "Activación CTF (+)",
                "zona_desaclope": "1",
                "control_tension": "S",
            },
            {
                "id_central": 100,
                "hour": 7,
                "consigna": "N",
                "estado": "N",
                "motivo": "",
                "zona_desaclope": "1",
                "control_tension": "",
            },
            {
                "id_central": 200,
                "hour": 6,
                "consigna": "PP",
                "estado": "N",
                "motivo": "Con SSCC",
                "zona_desaclope": "2",
                "control_tension": "S",
            },
        ]
    )
    look = build_instrucciones_lookup(instr)
    assert look[(6, 100)]["consigna"] == "MT"
    assert look[(6, 100)]["motivo"] == "Activación CTF (+)"
    assert look[(6, 100)]["zona_desaclope"] == "1"
    assert look[(7, 100)]["consigna"] == "N"
    assert look[(6, 200)]["motivo"] == "Con SSCC"
    assert look[(6, 200)]["zona_desaclope"] == "2"


def test_system_zonas_desaclope_per_hour():
    """Per-hour count of distinct operator-published zones — when > 1,
    the SEN was split, independent confirmation for our heuristic
    island clustering."""
    from cen2gtopt.marginal_units import system_zonas_desaclope_per_hour

    instr = pd.DataFrame(
        [
            {"id_central": 1, "hour": 0, "zona_desaclope": "1"},
            {"id_central": 2, "hour": 0, "zona_desaclope": "1"},
            {"id_central": 3, "hour": 6, "zona_desaclope": "1"},
            {"id_central": 4, "hour": 6, "zona_desaclope": "2"},
            {"id_central": 5, "hour": 6, "zona_desaclope": "3"},
            # Empty / NaN zonas should be skipped, not counted.
            {"id_central": 6, "hour": 12, "zona_desaclope": ""},
            {"id_central": 7, "hour": 12, "zona_desaclope": "1"},
        ]
    )
    out = system_zonas_desaclope_per_hour(instr)
    assert out[0] == 1  # one synchronous merit clearing at midnight
    assert out[6] == 3  # 3-way split during morning ramp
    assert out[12] == 1  # blank rows ignored


def test_system_zonas_desaclope_per_hour_empty_input():
    from cen2gtopt.marginal_units import system_zonas_desaclope_per_hour

    assert system_zonas_desaclope_per_hour(pd.DataFrame()) == {}


# ---------------------------------------------------------------------------
# 10. overlay_plexos_capacities — PLEXOS-published overlay
# ---------------------------------------------------------------------------


def test_overlay_plexos_capacities_no_xml_returns_inputs_unchanged():
    """When ``plexos_xml_path=None`` the overlay must be a no-op."""
    from cen2gtopt.marginal_units import overlay_plexos_capacities

    pmin_in = {1: 50.0, 2: 100.0}
    pmax_in = {1: 150.0, 2: 250.0}
    pmin_out, pmax_out, audit = overlay_plexos_capacities(
        pmin_in, pmax_in, plexos_xml_path=None, units_df=pd.DataFrame()
    )
    assert pmin_out == pmin_in
    assert pmax_out == pmax_in
    assert audit == {}


def test_overlay_plexos_capacities_no_units_df_returns_inputs_unchanged():
    from cen2gtopt.marginal_units import overlay_plexos_capacities

    out_pmin, out_pmax, audit = overlay_plexos_capacities(
        {1: 50.0}, {1: 150.0}, plexos_xml_path="/nonexistent.xml", units_df=None
    )
    assert out_pmin == {1: 50.0}
    assert out_pmax == {1: 150.0}
    assert audit == {}


def test_overlay_plexos_capacities_sums_blocks_per_id_central(monkeypatch, tmp_path):
    """Synthetic test: PLEXOS publishes 2 blocks (TER X_1, TER X_2) for
    a CEN id_central=42 with sum-of-blocks pmax=400, pmin=200; the
    overlay must return those sums (NOT the heuristic CEN value).
    Dropouts: a 0.01 MW fuel-mix variant must be skipped."""
    from cen2gtopt import marginal_units as mu

    def fake_parse(_xml_path):
        return {
            "TER_X_1": {"Max Capacity": 200.0, "Min Stable Level": 100.0},
            "TER_X_2": {"Max Capacity": 200.0, "Min Stable Level": 100.0},
            "TER_X_1_FUELMIX": {
                "Max Capacity": 0.01,
            },  # placeholder
            "OTHER_PLANT": {"Max Capacity": 100.0, "Min Stable Level": 20.0},
        }

    monkeypatch.setattr(
        "cen2gtopt._plexos_xml.parse_generator_input_properties", fake_parse
    )
    units_df = pd.DataFrame(
        [
            {"id_central": 42, "central": "TER X", "nombre_central": "TER X"},
            {"id_central": 99, "central": "OTHER PLANT", "nombre_central": ""},
        ]
    )
    pmin_cen = {42: 50.0, 99: 30.0, 100: 40.0}  # 100 has no PLEXOS match
    pmax_cen = {42: 350.0, 99: 95.0, 100: 90.0}

    pmin_out, pmax_out, audit = mu.overlay_plexos_capacities(
        pmin_cen, pmax_cen, plexos_xml_path=str(tmp_path / "any.xml"), units_df=units_df
    )

    # id 42: PLEXOS sum overrides CEN
    assert pmax_out[42] == pytest.approx(400.0)
    assert pmin_out[42] == pytest.approx(200.0)
    # id 99: PLEXOS overrides CEN with single-block value
    assert pmax_out[99] == pytest.approx(100.0)
    assert pmin_out[99] == pytest.approx(20.0)
    # id 100: no match, CEN fallback retained
    assert pmax_out[100] == 90.0
    assert pmin_out[100] == 40.0

    assert audit[42] == "plexos"
    assert audit[99] == "plexos"
    assert audit[100] == "cen_fallback"


# ---------------------------------------------------------------------------
# 5. resolve_solution(auto_download=False) does not consult the network
# ---------------------------------------------------------------------------


def test_resolve_solution_no_network_pcp_branch(tmp_path):
    """PCP branch: when the unpacked Solution folder exists locally
    AND ``auto_download=False``, ``resolve_solution`` must return the
    accdb without calling ``fetch_index``."""
    from cen2gtopt import pcp_solution

    download_root = tmp_path / "cache"
    extract_root = download_root / "_unpacked"

    # PCP layout:
    #   extract_root/PCP_RES_<ymd>/Model PRGdia_Full_Definitivo Solution/*.accdb
    sol_dir = (
        extract_root / "PCP_RES_20260422" / "Model PRGdia_Full_Definitivo Solution"
    )
    sol_dir.mkdir(parents=True)
    accdb = sol_dir / "Model PRGdia_Full_Definitivo Solution.accdb"
    accdb.write_bytes(b"")

    def boom(*_a, **_k):
        raise AssertionError(
            "resolve_solution(auto_download=False) consulted fetch_index — "
            "this is the regression we are guarding against."
        )

    with mock.patch.object(pcp_solution, "fetch_index", side_effect=boom):
        out = pcp_solution.resolve_solution(
            "2026-04-22",
            source="pcp",
            auto_download=False,
            download_root=download_root,
            extract_root=extract_root,
        )

    assert out.source == "PCP"
    assert out.accdb_path == accdb


def test_resolve_solution_no_network_pid_branch(tmp_path):
    """PID branch of the same regression."""
    from cen2gtopt import pcp_solution

    download_root = tmp_path / "cache"
    extract_root = download_root / "_unpacked"

    pid_outer = extract_root / "PID_20260422_07"
    pid_outer.mkdir(parents=True)
    pid_inner = pid_outer / "PID_20260422_07"
    pid_inner.mkdir()
    sol_dir = pid_inner / "Modelos" / "Model PRGdia_Full_Definitivo_PID Solution"
    sol_dir.mkdir(parents=True)
    accdb = sol_dir / "Model PRGdia_Full_Definitivo_PID Solution.accdb"
    accdb.write_bytes(b"")

    def boom(*_a, **_k):
        raise AssertionError("fetch_index consulted on auto_download=False")

    with mock.patch.object(pcp_solution, "fetch_index", side_effect=boom):
        out = pcp_solution.resolve_solution(
            "2026-04-22",
            source="pid",
            auto_download=False,
            download_root=download_root,
            extract_root=extract_root,
        )

    assert out.source == "PID"
    assert out.period == 7
    assert out.accdb_path == accdb


def test_resolve_solution_raises_when_local_cache_empty_no_network(tmp_path):
    """When the local cache is empty AND auto_download=False, the
    function must raise FileNotFoundError without ever touching the
    network."""
    from cen2gtopt import pcp_solution

    download_root = tmp_path / "empty_cache"
    download_root.mkdir()

    def boom(*_a, **_k):
        raise AssertionError("fetch_index consulted on auto_download=False")

    with mock.patch.object(pcp_solution, "fetch_index", side_effect=boom):
        with pytest.raises(FileNotFoundError):
            pcp_solution.resolve_solution(
                "2026-04-22",
                source="auto",
                auto_download=False,
                download_root=download_root,
                extract_root=download_root / "_unpacked",
            )
