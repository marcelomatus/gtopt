# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.hydro_epf` — cascade EPF walker.

The EPF of a reservoir is the total MW generated per m³/s of water
released, summed over every turbine in the cascade segment between
the reservoir and the next downstream reservoir (or the ocean / sink).

These tests cover:

1. Single-reservoir + single-turbine cascade — EPF = turbine PF
2. Multi-turbine cascade segment — sum of all PFs
3. Stop at next reservoir — downstream reservoir's segment doesn't
   get attributed to the upstream one
4. Stop at sink (no outgoing edges = ocean)
5. Branching cascade — multiple downstream paths all count
6. Two converter schemas:
     * plexos2gtopt direct-arc turbines (junction_a/junction_b on Turbine)
     * plp2gtopt turbine-on-waterway (Turbine.waterway → Waterway arc)
7. ``water_emission_value_per_cumec_hour`` formula
"""

from __future__ import annotations

import pytest

from gtopt_shared.hydro_epf import (
    _GAS_EMISSION_TCO2_PER_MWH,
    _LOSS_MULTIPLIER,
    build_hydro_graph,
    epf_per_reservoir,
    water_emission_value_per_cumec_hour,
)


# ---------------------------------------------------------------------------
# Helpers for building tiny synthetic hydro systems
# ---------------------------------------------------------------------------


def _make_system(
    reservoirs: list[dict],
    turbines: list[dict],
    waterways: list[dict] | None = None,
    junctions: list[dict] | None = None,
) -> dict:
    """Wrap arrays in a gtopt ``system`` dict.  Junctions auto-derived
    when not given (every junction mentioned on any element)."""
    if junctions is None:
        names = set()
        for r in reservoirs:
            names.add(r.get("junction", r["name"]))
        for w in waterways or []:
            for k in ("junction_a", "junction_b"):
                if w.get(k):
                    names.add(w[k])
        for t in turbines:
            for k in ("junction_a", "junction_b"):
                if t.get(k):
                    names.add(t[k])
        junctions = [{"uid": i + 1, "name": n} for i, n in enumerate(sorted(names))]
    return {
        "reservoir_array": reservoirs,
        "turbine_array": turbines,
        "waterway_array": waterways or [],
        "junction_array": junctions,
    }


# ---------------------------------------------------------------------------
# Schema 1 — plexos2gtopt-style (Turbine carries its own junction_a/_b)
# ---------------------------------------------------------------------------


class TestPlexosSchemaCascade:
    """plexos2gtopt: each Turbine has junction_a + junction_b directly
    (the built-in waterway pattern).  Waterway-only entries are plain
    arcs with PF=0."""

    def test_single_reservoir_single_turbine(self) -> None:
        """R1 → turbine_a (PF=2.5) → sink.  EPF[R1] = 2.5."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 2.5,
                    "junction_a": "R1",
                    "junction_b": "sink",
                }
            ],
        )
        epfs = epf_per_reservoir(sys)
        assert epfs["R1"] == pytest.approx(2.5)

    def test_multi_turbine_chain(self) -> None:
        """R1 → t1 (PF=3) → t2 (PF=2) → t3 (PF=1) → sink.  EPF=6."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 3.0,
                    "junction_a": "R1",
                    "junction_b": "j2",
                },
                {
                    "uid": 2,
                    "name": "t2",
                    "production_factor": 2.0,
                    "junction_a": "j2",
                    "junction_b": "j3",
                },
                {
                    "uid": 3,
                    "name": "t3",
                    "production_factor": 1.0,
                    "junction_a": "j3",
                    "junction_b": "sink",
                },
            ],
        )
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(6.0)

    def test_stop_at_next_reservoir(self) -> None:
        """R1 → t1 (PF=3) → R2 → t2 (PF=2) → sink.

        EPF[R1] = 3 (only t1; t2 belongs to R2's segment).
        EPF[R2] = 2 (only t2).
        """
        sys = _make_system(
            reservoirs=[
                {"uid": 1, "name": "R1", "junction": "R1"},
                {"uid": 2, "name": "R2", "junction": "R2"},
            ],
            turbines=[
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 3.0,
                    "junction_a": "R1",
                    "junction_b": "R2",
                },
                {
                    "uid": 2,
                    "name": "t2",
                    "production_factor": 2.0,
                    "junction_a": "R2",
                    "junction_b": "sink",
                },
            ],
        )
        epfs = epf_per_reservoir(sys)
        assert epfs["R1"] == pytest.approx(3.0)
        assert epfs["R2"] == pytest.approx(2.0)

    def test_branching_cascade(self) -> None:
        """R1 forks into two parallel turbines, both sum into R1's EPF.

                  → t_left (PF=4)  → sink
        R1 →
                  → t_right (PF=1) → sink

        EPF[R1] = 4 + 1 = 5.
        """
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "t_left",
                    "production_factor": 4.0,
                    "junction_a": "R1",
                    "junction_b": "sink_l",
                },
                {
                    "uid": 2,
                    "name": "t_right",
                    "production_factor": 1.0,
                    "junction_a": "R1",
                    "junction_b": "sink_r",
                },
            ],
        )
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(5.0)

    def test_waterway_zero_pf_pass_through(self) -> None:
        """Waterways without an attached turbine carry PF=0 — they pass
        flow through but contribute zero energy."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 5.0,
                    "junction_a": "j_mid",
                    "junction_b": "sink",
                }
            ],
            waterways=[
                {
                    "uid": 1,
                    "name": "spillway",
                    "junction_a": "R1",
                    "junction_b": "j_mid",
                    "fmax": 100.0,
                },
            ],
        )
        # R1 → spillway (PF=0) → j_mid → t1 (PF=5) → sink ⇒ EPF=5
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(5.0)

    def test_reservoir_with_no_outgoing_edges(self) -> None:
        """Reservoir at the bottom of the network (no turbines below)
        gets EPF=0 — there's no energy to extract from its water."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[],
        )
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# Schema 2 — plp2gtopt-style (Turbine references a Waterway by name)
# ---------------------------------------------------------------------------


class TestPlpSchemaCascade:
    """plp2gtopt: Turbine has ``waterway`` field referencing a Waterway
    by name; the Waterway carries the junction_a/junction_b arc.  The
    builder decorates the matching waterway edge with the turbine's PF.
    """

    def test_turbine_decorates_waterway(self) -> None:
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                # No junction_a/junction_b — references waterway by name
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 7.0,
                    "waterway": "ww1",
                },
            ],
            waterways=[
                {
                    "uid": 1,
                    "name": "ww1",
                    "junction_a": "R1",
                    "junction_b": "sink",
                    "fmax": 50.0,
                },
            ],
        )
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(7.0)

    def test_multiple_turbines_share_waterway_sum_pf(self) -> None:
        """Edge case: two turbines reference the same waterway → PFs sum
        (rare in practice but the contract is defined)."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {"uid": 1, "name": "t1", "production_factor": 4.0, "waterway": "ww1"},
                {"uid": 2, "name": "t2", "production_factor": 1.5, "waterway": "ww1"},
            ],
            waterways=[
                {
                    "uid": 1,
                    "name": "ww1",
                    "junction_a": "R1",
                    "junction_b": "sink",
                    "fmax": 100,
                }
            ],
        )
        assert epf_per_reservoir(sys)["R1"] == pytest.approx(5.5)

    def test_plp_chain_with_stop_at_next_reservoir(self) -> None:
        """R1 — ww1+turb (PF=3) → R2 — ww2+turb (PF=2) → sink.
        EPF[R1] = 3, EPF[R2] = 2."""
        sys = _make_system(
            reservoirs=[
                {"uid": 1, "name": "R1", "junction": "R1"},
                {"uid": 2, "name": "R2", "junction": "R2"},
            ],
            turbines=[
                {"uid": 1, "name": "t1", "production_factor": 3.0, "waterway": "ww1"},
                {"uid": 2, "name": "t2", "production_factor": 2.0, "waterway": "ww2"},
            ],
            waterways=[
                {
                    "uid": 1,
                    "name": "ww1",
                    "junction_a": "R1",
                    "junction_b": "R2",
                    "fmax": 50,
                },
                {
                    "uid": 2,
                    "name": "ww2",
                    "junction_a": "R2",
                    "junction_b": "sink",
                    "fmax": 50,
                },
            ],
        )
        epfs = epf_per_reservoir(sys)
        assert epfs["R1"] == pytest.approx(3.0)
        assert epfs["R2"] == pytest.approx(2.0)


# ---------------------------------------------------------------------------
# Real CEN-Maule cascade (L_Maule basin from the PLEXOS reference)
# ---------------------------------------------------------------------------


class TestCenMauleCascade:
    """Verify the L_Maule chain (real PLEXOS data) gives the expected
    9.34 MW/(m³/s) for L_Maule by summing LOS_CONDORES + LA_MINA +
    ISLA + CURILLINQUE + LOMAALTA before hitting PEHUENCHE / COLBUN.
    """

    def test_l_maule_total_epf(self) -> None:
        """Synthetic recreation of the L_Maule chain shape:
            L_Maule → LOS_CONDORES (PF=6.50) → LA_MINA
            LA_MINA → ww (PF=0.57) → B_M_Isla
            B_M_Isla → ww (PF=0)   → ISLA
            ISLA → ww (PF=0.81) → Post_Isla
            Post_Isla → ww (PF=0) → CURILLINQUE
            CURILLINQUE → ww (PF=1.01) → LOMAALTA
            LOMAALTA → ww (PF=0.45) → B_Maule
            B_Maule → ww (PF=0) → PEHUENCHE  [STOP]
            B_Maule → ww (PF=0) → COLBUN     [STOP]

        Expected: EPF[L_Maule] = 6.50 + 0.57 + 0.81 + 1.01 + 0.45 = 9.34
        """

        # Use plexos-direct-arc schema for compactness
        def _turb(name: str, pf: float, ja: str, jb: str, uid: int) -> dict:
            return {
                "uid": uid,
                "name": name,
                "production_factor": pf,
                "junction_a": ja,
                "junction_b": jb,
            }

        sys = _make_system(
            reservoirs=[
                {"uid": 1, "name": "L_Maule", "junction": "L_Maule"},
                {"uid": 2, "name": "PEHUENCHE", "junction": "PEHUENCHE"},
                {"uid": 3, "name": "COLBUN", "junction": "COLBUN"},
            ],
            turbines=[
                _turb("LOS_CONDORES", 6.50, "L_Maule", "LA_MINA", 1),
                _turb("LA_MINA", 0.57, "LA_MINA", "B_M_Isla", 2),
                _turb("ISLA", 0.81, "ISLA", "Post_Isla", 3),
                _turb("CURILLINQUE", 1.01, "CURILLINQUE", "LOMAALTA", 4),
                _turb("LOMAALTA", 0.45, "LOMAALTA", "B_Maule", 5),
            ],
            waterways=[
                {
                    "uid": 10,
                    "name": "ww_BMIsla_Isla",
                    "junction_a": "B_M_Isla",
                    "junction_b": "ISLA",
                },
                {
                    "uid": 11,
                    "name": "ww_PostIsla_Cur",
                    "junction_a": "Post_Isla",
                    "junction_b": "CURILLINQUE",
                },
                {
                    "uid": 12,
                    "name": "ww_BMaule_PEH",
                    "junction_a": "B_Maule",
                    "junction_b": "PEHUENCHE",
                },
                {
                    "uid": 13,
                    "name": "ww_BMaule_COLBUN",
                    "junction_a": "B_Maule",
                    "junction_b": "COLBUN",
                },
            ],
        )
        epfs = epf_per_reservoir(sys)
        assert epfs["L_Maule"] == pytest.approx(9.34)
        # Downstream reservoirs have empty cascade in this fixture
        assert epfs["PEHUENCHE"] == pytest.approx(0.0)
        assert epfs["COLBUN"] == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# water_emission_value_per_cumec_hour formula
# ---------------------------------------------------------------------------


class TestWaterEmissionValue:
    def test_default_constants(self) -> None:
        """Module-level anchors match the design (gas + loss multiplier)."""
        assert _GAS_EMISSION_TCO2_PER_MWH == pytest.approx(0.5)
        assert _LOSS_MULTIPLIER == pytest.approx(0.95)

    def test_zero_epf_yields_zero_value(self) -> None:
        assert water_emission_value_per_cumec_hour(0.0) == pytest.approx(0.0)

    def test_l_maule_value(self) -> None:
        """EPF=9.34 → 9.34 × 0.5 × 0.95 = 4.4365 tCO2eq/(m³/s)·h."""
        v = water_emission_value_per_cumec_hour(9.34)
        assert v == pytest.approx(4.4365, rel=1e-4)

    def test_custom_gas_em_overrides_default(self) -> None:
        """Higher gas emission rate (e.g. less efficient backup) → bigger
        emission opportunity per stored cumec."""
        v_default = water_emission_value_per_cumec_hour(5.0)
        v_higher = water_emission_value_per_cumec_hour(5.0, gas_em=1.0)
        assert v_higher > v_default
        assert v_higher == pytest.approx(5.0 * 1.0 * _LOSS_MULTIPLIER)

    def test_loss_mult_one_drops_efficiency_factor(self) -> None:
        """loss_mult=1 → no efficiency discount, raw EPF × gas_em."""
        v = water_emission_value_per_cumec_hour(5.0, loss_mult=1.0)
        assert v == pytest.approx(5.0 * _GAS_EMISSION_TCO2_PER_MWH)


# ---------------------------------------------------------------------------
# Graph builder edge cases
# ---------------------------------------------------------------------------


class TestBuildHydroGraph:
    def test_empty_system(self) -> None:
        G = build_hydro_graph({})
        assert G.number_of_nodes() == 0
        assert G.number_of_edges() == 0

    def test_turbine_with_missing_junctions_skipped(self) -> None:
        """plexos2gtopt built-in turbines without explicit junction_b
        (bus-only turbines) skip the cascade-arc creation."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "no_b_turb",
                    "production_factor": 9.0,
                    "junction_a": "R1",
                    # no junction_b — skip
                },
            ],
        )
        G = build_hydro_graph(sys)
        assert G.number_of_edges() == 0

    def test_turbine_with_waterway_takes_precedence(self) -> None:
        """When Turbine has BOTH waterway ref AND direct arc, the
        waterway-decoration path wins (the direct-arc fallback skips
        turbines that already reference a waterway)."""
        sys = _make_system(
            reservoirs=[{"uid": 1, "name": "R1", "junction": "R1"}],
            turbines=[
                {
                    "uid": 1,
                    "name": "t1",
                    "production_factor": 5.0,
                    "waterway": "ww1",
                    "junction_a": "X",  # ignored when waterway present
                    "junction_b": "Y",
                },
            ],
            waterways=[
                {
                    "uid": 1,
                    "name": "ww1",
                    "junction_a": "R1",
                    "junction_b": "sink",
                }
            ],
        )
        G = build_hydro_graph(sys)
        # Only the waterway arc R1 → sink should exist (with PF=5)
        assert G.has_edge("R1", "sink")
        assert G["R1"]["sink"]["production_factor"] == pytest.approx(5.0)
        # The fallback junction_a/junction_b "X → Y" should NOT exist
        assert not G.has_edge("X", "Y")
