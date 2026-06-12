# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``_select_marginal_candidates`` filter cascade.

Verifies that cogens, batteries, and reservoir hydro are treated as
price-takers and excluded from the LP-marginal candidate pool, with a
graceful fallback so price-taker-only zones still resolve to *some*
dispatched gen (the recipe layer's consequential walk-up takes over
from there).
"""

from __future__ import annotations

from gtopt_canonical_feed import Generator, Topology

from gtopt_marginal_units.constants import Tolerances
from gtopt_marginal_units.main import _select_marginal_candidates


def _gen(
    uid: int,
    *,
    bus_uid: int = 1,
    kind: str = "thermal",
    is_cogen: bool = False,
    mc: float = 50.0,
    pmax: float = 100.0,
    name: str | None = None,
) -> Generator:
    return Generator(
        uid=uid,
        name=name if name is not None else f"g{uid}",
        bus_uid=bus_uid,
        pmin=0.0,
        pmax=pmax,
        declared_MC=mc,
        kind=kind,
        emission_rate=400.0,
        is_cogen=is_cogen,
    )


def _topo(*gens: Generator) -> Topology:
    return Topology(buses=[], generators=list(gens), lines=[])


def _select(
    topology: Topology,
    *,
    zone_bus_uids: set[int],
    dispatch_by_uid: dict[int, float],
    rc_by_uid: dict[int, float] | None = None,
    merit_eligible: dict[int, bool] | None = None,
    lam: float = 50.0,
):
    if merit_eligible is None:
        # Default: only profile-kind generators are merit-ineligible.
        merit_eligible = {g.uid: g.kind != "profile" for g in topology.generators}
    return _select_marginal_candidates(
        topology=topology,
        zone_bus_uids=zone_bus_uids,
        lam=lam,
        dispatch_by_uid=dispatch_by_uid,
        rc_by_uid=rc_by_uid or {},
        srmc_by_uid={},
        tol=Tolerances.default(),
        merit_eligible=merit_eligible,
    )


# ---------------------------------------------------------------------------
# Tier-1: real setters — thermal AND not cogen.
# ---------------------------------------------------------------------------


def test_thermal_noncogen_preferred_over_cogen():
    """When a thermal non-cogen and a cogen both dispatch, only the
    non-cogen is in the candidate pool."""
    t = _gen(10, kind="thermal", is_cogen=False)
    c = _gen(11, kind="thermal", is_cogen=True)
    topo = _topo(t, c)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={10: 50.0, 11: 50.0},
        rc_by_uid={10: 0.0, 11: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {10}, f"expected only non-cogen thermal, got {uids}"


def test_thermal_noncogen_preferred_over_battery():
    """Battery is a storage price-taker — excluded when a thermal
    non-cogen is dispatching."""
    t = _gen(20, kind="thermal", is_cogen=False)
    b = _gen(21, kind="battery", is_cogen=False)
    topo = _topo(t, b)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={20: 50.0, 21: 30.0},
        rc_by_uid={20: 0.0, 21: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {20}, f"expected only thermal, got {uids}"


def test_thermal_noncogen_preferred_over_hydro_reservoir():
    """Reservoir hydro is a water-value price-taker — excluded when a
    thermal non-cogen is dispatching."""
    t = _gen(30, kind="thermal", is_cogen=False)
    h = _gen(31, kind="hydro", is_cogen=False)
    topo = _topo(t, h)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={30: 50.0, 31: 40.0},
        rc_by_uid={30: 0.0, 31: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {30}, f"expected only thermal, got {uids}"


def test_thermal_noncogen_preferred_over_all_price_takers():
    """The four price-taker kinds (cogen, battery, hydro, profile) are
    all excluded when a real thermal setter is dispatching."""
    t = _gen(40, kind="thermal", is_cogen=False)
    c = _gen(41, kind="thermal", is_cogen=True)  # cogen
    b = _gen(42, kind="battery", is_cogen=False)
    h = _gen(43, kind="hydro", is_cogen=False)
    p = _gen(44, kind="profile", is_cogen=False)
    topo = _topo(t, c, b, h, p)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={40: 50.0, 41: 50.0, 42: 30.0, 43: 40.0, 44: 20.0},
        rc_by_uid={k: 0.0 for k in (40, 41, 42, 43, 44)},
    )
    uids = {g.uid for g in cands}
    assert uids == {40}, f"expected only thermal non-cogen, got {uids}"


# ---------------------------------------------------------------------------
# Tier-2..5: graceful fallback when no real setter is dispatching.
# ---------------------------------------------------------------------------


def test_storage_only_zone_falls_back_to_dispatched():
    """A zone with only batteries dispatching (e.g. islanded BESS pocket)
    must still return *some* candidate — the recipe layer's
    consequential walk-up takes it from there."""
    b1 = _gen(50, kind="battery", is_cogen=False)
    b2 = _gen(51, kind="battery", is_cogen=False)
    topo = _topo(b1, b2)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={50: 30.0, 51: 30.0},
        rc_by_uid={50: 0.0, 51: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {50, 51}, f"expected both batteries (fallback), got {uids}"


def test_hydro_only_zone_falls_back_to_dispatched():
    """A zone with only reservoir hydro dispatching falls back to
    those hydros (recipe walks up consequentially)."""
    h1 = _gen(60, kind="hydro", is_cogen=False)
    topo = _topo(h1)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={60: 40.0},
        rc_by_uid={60: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {60}, f"expected lone hydro (fallback), got {uids}"


def test_cogen_only_zone_falls_back_to_cogen():
    """A zone with only cogens dispatching (refinery node) falls back
    to the cogen — better than returning empty."""
    c1 = _gen(70, kind="thermal", is_cogen=True)
    topo = _topo(c1)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={70: 30.0},
        rc_by_uid={70: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {70}, f"expected lone cogen (fallback), got {uids}"


def test_profile_only_zone_falls_back_to_profile():
    """A zone with only solar/wind dispatching falls back to that
    profile — recipe will downstream-classify as renewable_curtailment
    when LMP ≈ 0."""
    p1 = _gen(80, kind="profile", is_cogen=False)
    topo = _topo(p1)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={80: 50.0},
        rc_by_uid={80: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {80}, f"expected lone profile (fallback), got {uids}"


def test_profile_plus_battery_falls_back_to_profile_via_nc_nostorage():
    """When only profile + battery dispatch (no thermal, no cogen),
    cascade tier 2 (¬cogen ∧ ¬storage) picks profile over battery."""
    b = _gen(90, kind="battery", is_cogen=False)
    p = _gen(91, kind="profile", is_cogen=False)
    topo = _topo(b, p)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={90: 30.0, 91: 50.0},
        rc_by_uid={90: 0.0, 91: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {91}, f"expected profile via nc_nostorage cascade tier, got {uids}"


# ---------------------------------------------------------------------------
# Empty / out-of-zone behavior.
# ---------------------------------------------------------------------------


def test_no_dispatched_returns_empty():
    """No dispatching gens → empty candidate list (caller's
    responsibility to classify as unattributed / empty island)."""
    t = _gen(100, kind="thermal", is_cogen=False)
    topo = _topo(t)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={},  # nothing dispatching
        rc_by_uid={},
    )
    assert cands == [], f"expected empty pool, got {cands}"


# ---------------------------------------------------------------------------
# Synthetic-gen prefilter — CEN-PCP placeholders + _INF markers.
# ---------------------------------------------------------------------------


def test_synthetic_pmax_below_threshold_skipped():
    """A real-capacity thermal beats a 0.01 MW synthetic placeholder."""
    from gtopt_marginal_units.main import _is_synthetic_gen

    real = _gen(120, kind="thermal", pmax=100.0, name="ANGAMOS_1")
    synth = _gen(121, kind="thermal", pmax=0.01, name="ATA_CC1_TGA_DIE")
    assert _is_synthetic_gen(synth) is True
    assert _is_synthetic_gen(real) is False
    topo = _topo(real, synth)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={120: 50.0, 121: 0.005},
        rc_by_uid={120: 0.0, 121: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {120}, f"expected real-capacity only, got {uids}"


def test_synthetic_inf_suffix_skipped():
    """A ``_INF`` placeholder is skipped even if pmax > threshold."""
    from gtopt_marginal_units.main import _is_synthetic_gen

    real = _gen(130, kind="thermal", pmax=100.0, name="ANDINA")
    inf = _gen(131, kind="thermal", pmax=999.0, name="TAMAKAYA_NOGNL_INF")
    assert _is_synthetic_gen(inf) is True
    assert _is_synthetic_gen(real) is False
    topo = _topo(real, inf)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={130: 50.0, 131: 50.0},
        rc_by_uid={130: 0.0, 131: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {130}, f"expected real gen only (_INF skipped), got {uids}"


def test_synthetic_only_zone_falls_back():
    """When every dispatched gen in a zone is synthetic, the cascade
    proceeds on the unfiltered set so the recipe still has *something*
    for the consequential walk-up."""
    s1 = _gen(140, kind="thermal", pmax=0.01, name="ATA_CC1_TGA_DIE")
    s2 = _gen(141, kind="thermal", pmax=0.01, name="ATA_CC1_TGA_GNL")
    topo = _topo(s1, s2)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={140: 0.005, 141: 0.005},
        rc_by_uid={140: 0.0, 141: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {140, 141}, (
        f"expected fallback to synthetic set when no real gens, got {uids}"
    )


def test_synthetic_filter_layered_with_cogen_skip():
    """Cogen-skip + synthetic-skip compose: prefer non-cogen real
    over a cogen real over a non-cogen synthetic over a cogen synthetic."""
    real_noncogen = _gen(150, kind="thermal", pmax=50.0, name="REAL_THERMAL")
    real_cogen = _gen(151, kind="thermal", pmax=50.0, name="REAL_COGEN", is_cogen=True)
    synth_noncogen = _gen(152, kind="thermal", pmax=0.01, name="SYN_THERMAL")
    synth_cogen = _gen(153, kind="thermal", pmax=0.01, name="SYN_COGEN", is_cogen=True)
    topo = _topo(real_noncogen, real_cogen, synth_noncogen, synth_cogen)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},
        dispatch_by_uid={150: 50, 151: 50, 152: 0.005, 153: 0.005},
        rc_by_uid={150: 0.0, 151: 0.0, 152: 0.0, 153: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {150}, f"expected real non-cogen as the canonical setter, got {uids}"


def test_other_zone_gens_ignored():
    """Gens on buses outside the zone are not considered, even if they
    would otherwise qualify as real setters."""
    in_zone = _gen(110, bus_uid=1, kind="thermal", is_cogen=True)
    out_of_zone = _gen(111, bus_uid=2, kind="thermal", is_cogen=False)
    topo = _topo(in_zone, out_of_zone)
    cands, _ = _select(
        topo,
        zone_bus_uids={1},  # only bus 1
        dispatch_by_uid={110: 50.0, 111: 50.0},
        rc_by_uid={110: 0.0, 111: 0.0},
    )
    uids = {g.uid for g in cands}
    assert uids == {110}, f"expected only in-zone cogen (fallback), got {uids}"
