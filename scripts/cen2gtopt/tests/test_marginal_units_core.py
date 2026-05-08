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
