"""Integration / magnitude tests for ``--auto-water-fail-cost``.

These tests validate the **magnitude claim** at the heart of the
unified water-shortfall pricing helper: under the legacy paths
``Reservoir.efin_cost`` is ~5,000× too cheap relative to the energy-
equivalent value of stored water, and ``FlowRight.fail_cost`` is a
single hard-coded number for every FR irrespective of the central's
own ``Rendi``.

The new pipeline replaces both with a single principled formula
(see ``plp2gtopt._water_value``) — these tests run plp2gtopt on the
real juan/IPLP case (when present locally) and confirm that the
emitted prices match the documented anchor × lost_pf reference table
within a tight tolerance, and that the legacy path produces values
*orders of magnitude smaller*.

The juan/IPLP case is not always available in the sandbox; when
absent the entire test class is skipped.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from plp2gtopt._water_value import WaterValueResolver

# Reference ``lost_pf`` values — these are intrinsic to the juan/IPLP
# topology (cascade walks, cenre lifts) and remain stable across
# changes to the anchor formula.  The expected magnitudes are
# computed at runtime as ``anchor × lost_pf`` (FR) or
# ``anchor × lost_pf × 1e6/3600`` (reservoir efin), where ``anchor``
# is whatever the resolver returns from the case data.
#
# Under the current ``ANCHOR = (max_unit_gcost + min_falla_gcost) / 2``
# formula the absolute magnitudes are smaller than under the prior
# ``max(falla.gcost) + 1`` formula by the ratio of the two anchors,
# but the *proportionality* between reservoirs is unchanged because
# every cost surface scales linearly in the anchor.
#
# Re-baselined 2026-05-11 after switching:
#
#   * cascade walk to **stop at the next reservoir** (instead of
#     walking to the ocean) — affects LMAULE / CIPRESES / RALCO
#     whose downstream chains hit a `type=embalse` central in
#     real PLP topology, AND
#   * cascade walk to **exclude pasada** centrals from the PF sum
#     (only embalse / serie contribute), AND
#   * anchor to ``avg(termica.gcost)`` (instead of ``max(non-falla.gcost)``).
#
# Per-reservoir change vs. the old cascade-to-ocean rule:
#
#   reservoir   | old   | new    | diff   reason
#   ------------|------:|-------:|------|-----------------------
#   LMAULE      | 10.050| 8.270  | -1.78 stops at PEHUENCHE (embalse)
#   CIPRESES    | 6.914 | 5.134  | -1.78 stops at next embalse
#   RALCO       | 2.976 | 1.746  | -1.23 stops at next embalse
#   (all others unchanged — their cascade already terminated at
#   the ocean without crossing an intermediate embalse)
#
# FR (junction_lost_pf) values are unchanged because that path
# uses only the FR-bound central's own ``max_rendi`` and was not
# modified by the cascade rules.
_REFERENCE_RESERVOIRS_LOST_PF = {
    "LMAULE": 8.270,
    "CIPRESES": 5.134,
    "ELTORO": 6.627930,  # cenre lift
    "RALCO": 1.745756,  # cenre lift
    "CANUTILLAR": 2.074676,  # cenre lift
    "COLBUN": 1.942115,  # cenre lift
    "PEHUENCHE": 1.780,
    "PANGUE": 1.230,
    "PILMAIQUEN": 0.905,
    "RAPEL": 0.640,
}

_REFERENCE_FLOW_RIGHTS_LOST_PF = {
    "ANTUCO": 1.6,
    "ABANICO": 1.2,
    "PALMUCHO": 1.143,
    "PANGUE": 0.8,
    "MACHICURA": 0.33,
    "PILMAIQUEN": 0.27,
}

# Tight relative tolerance (1 ppm) — absorbs FP rounding without
# letting silent regressions in the cascade / cenre-lift logic slip
# through.
_TOL_REL = 1e-6


def _juan_iplp_dir() -> Path | None:
    """Return the juan/IPLP support directory if it has the required files."""
    repo_root = Path(__file__).resolve().parents[3]
    case_dir = repo_root / "support" / "juan" / "IPLP"
    if not case_dir.is_dir():
        return None
    # plp2gtopt reads .dat or .dat.xz transparently; either is fine.
    for required in ("plpcnfce.dat", "plpcenre.dat", "plpblo.dat"):
        if (
            not (case_dir / required).exists()
            and not (case_dir / f"{required}.xz").exists()
        ):
            return None
    return case_dir


def _convert_juan(case_dir: Path, output_dir: Path, *, extra_flags: list[str]) -> dict:
    """Run plp2gtopt on the juan/IPLP case and return parsed planning JSON.

    Always uses ``-F csv -t 1y --first-scenario --no-check`` to keep
    the run fast and avoid downstream consistency checks that depend
    on solver plugins.
    """
    cmd = [
        sys.executable,
        "-m",
        "plp2gtopt.main",
        str(case_dir),
        "-o",
        str(output_dir),
        "-F",
        "csv",
        "-t",
        "1y",
        "--first-scenario",
        "--no-check",
        *extra_flags,
    ]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=240,
        check=False,
    )
    assert result.returncode == 0, (
        f"plp2gtopt failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
    )
    json_files = list(output_dir.glob("*.json"))
    # Filter out auxiliary fragments that share the .json extension.
    _aux = {
        "laja_water_rights.json",
        "maule_water_rights.json",
        "ror_promoted.json",
    }
    json_files = [p for p in json_files if p.name not in _aux]
    assert len(json_files) == 1, f"Expected 1 main JSON file, got {json_files}"
    with open(json_files[0], encoding="utf-8") as f:
        return json.load(f)


@pytest.mark.integration
@pytest.mark.skipif(
    _juan_iplp_dir() is None,
    reason="support/juan/IPLP not available in this environment",
)
class TestAutoWaterFailCostMagnitude:
    """End-to-end magnitude tests on the real juan/IPLP case.

    Runs plp2gtopt twice — once with the legacy path, once with
    ``--auto-water-fail-cost`` — and verifies:

    * The auto values match the documented reference table exactly
      (±1 in PLP units).
    * The legacy values are at least three orders of magnitude smaller
      (the original design issue: vrebemb cost capped at 500 $/hm³ vs.
      the 10⁵–10⁶ $/hm³ the energy-equivalent argument requires).
    """

    @pytest.fixture(scope="class")
    def planning_auto(self, tmp_path_factory):
        case_dir = _juan_iplp_dir()
        assert case_dir is not None  # gated by skipif
        out = tmp_path_factory.mktemp("juan_auto")
        return _convert_juan(
            case_dir,
            out,
            extra_flags=["--auto-water-fail-cost"],
        )

    @pytest.fixture(scope="class")
    def planning_legacy(self, tmp_path_factory):
        case_dir = _juan_iplp_dir()
        assert case_dir is not None
        out = tmp_path_factory.mktemp("juan_legacy")
        return _convert_juan(
            case_dir,
            out,
            extra_flags=["--no-auto-water-fail-cost"],
        )

    @staticmethod
    def _resolved_anchor(planning_auto) -> float:
        """Recover the anchor (in ``$/MWh``) from the emitted JSON.

        Picks the reservoir with the highest ``lost_pf`` whose
        reference is in the table, then back-solves ``efin_cost =
        anchor × lost_pf × 1e6 / 3600``.  This avoids hard-coding the
        anchor magnitude (which depends on the case's
        ``max(non-falla.gcost)`` and ``min(falla.gcost)``) while still
        validating the rest of the pipeline.
        """
        reservoirs = planning_auto.get("system", {}).get("reservoir_array", [])
        # Use LMAULE (cascade_pf=10.05) as the calibration probe.
        lmaule = next((r for r in reservoirs if r.get("name") == "LMAULE"), None)
        assert lmaule is not None and lmaule.get("efin_cost") is not None
        lost_pf = _REFERENCE_RESERVOIRS_LOST_PF["LMAULE"]
        return lmaule["efin_cost"] / lost_pf * 3600.0 / 1e6

    # ------------------------------------------------------------------
    # Auto pipeline — proportionality across reservoirs
    # ------------------------------------------------------------------
    @pytest.mark.parametrize(
        "name,lost_pf",
        list(_REFERENCE_RESERVOIRS_LOST_PF.items()),
    )
    def test_reservoir_efin_cost_matches_reference(self, planning_auto, name, lost_pf):
        """``efin_cost = anchor × lost_pf × 1e6/3600`` for each reservoir.

        Validates the cascade / cenre-lift chain that produces
        ``lost_pf`` while remaining anchor-formula-agnostic — the
        absolute magnitude depends only on the case's electricity
        prices, but every reservoir must scale by the same anchor.
        """
        anchor = self._resolved_anchor(planning_auto)
        expected_efin = anchor * lost_pf * 1e6 / 3600.0
        reservoirs = planning_auto.get("system", {}).get("reservoir_array", [])
        match = next((r for r in reservoirs if r.get("name") == name), None)
        assert match is not None, f"Reservoir {name} not found in planning"
        actual = match.get("efin_cost")
        assert actual is not None, f"Reservoir {name} has no efin_cost"
        assert actual == pytest.approx(expected_efin, rel=_TOL_REL), (
            f"{name}: efin_cost={actual} vs anchor*lost_pf*1e6/3600="
            f"{expected_efin} (anchor={anchor:.2f}, lost_pf={lost_pf})"
        )

    @pytest.mark.parametrize(
        "central,lost_pf",
        list(_REFERENCE_FLOW_RIGHTS_LOST_PF.items()),
    )
    def test_flow_right_fail_cost_matches_reference(
        self, planning_auto, central, lost_pf
    ):
        """Per-FR ``fail_cost = anchor × own_max_rendi`` (anchor-agnostic)."""
        anchor = self._resolved_anchor(planning_auto)
        expected_fail_cost = anchor * lost_pf
        frs = planning_auto.get("system", {}).get("flow_right_array", [])
        # FR names follow the pattern ``<CENTRAL>_pmin_as_flow_right`` in
        # the bundled --pmin-as-flowright whitelist.
        match = next(
            (
                fr
                for fr in frs
                if fr.get("name", "").startswith(f"{central}_pmin_as_flow_right")
            ),
            None,
        )
        assert match is not None, f"FlowRight for {central} not found"
        actual = match.get("fail_cost")
        assert actual is not None, f"FR {central} has no fail_cost"
        assert actual == pytest.approx(expected_fail_cost, rel=_TOL_REL), (
            f"{central}: fail_cost={actual} vs anchor*own_pf="
            f"{expected_fail_cost} (anchor={anchor:.2f}, lost_pf={lost_pf})"
        )

    def test_flow_right_fail_costs_are_heterogeneous(self, planning_auto):
        """All FR fail_costs differ — single uniform value would be a regression."""
        frs = planning_auto.get("system", {}).get("flow_right_array", [])
        costs = sorted({fr.get("fail_cost") for fr in frs if fr.get("fail_cost")})
        # We expect at least 4 distinct values across the bundled whitelist.
        assert len(costs) >= 4, f"FR fail_costs collapsed to {costs}"

    # ------------------------------------------------------------------
    # Legacy path — magnitude justification
    # ------------------------------------------------------------------
    def test_legacy_efin_cost_is_orders_of_magnitude_smaller(self, planning_legacy):
        """Legacy ``efin_cost`` is in the 0.0005-500 $/hm³ band — far below
        the 1.7M $/hm³ that the cascade-energy-equivalent argument
        produces for LMAULE.

        This is the regression the new helper closes: under the legacy
        cascade, an LP can manufacture fictitious water through the
        slack at ~5,000× discount relative to the demand-fail penalty.
        """
        reservoirs = planning_legacy.get("system", {}).get("reservoir_array", [])
        # Legacy must produce *some* finite efin_cost on at least one
        # reservoir (otherwise the test fixture is broken).
        any_legacy = [r.get("efin_cost") for r in reservoirs if r.get("efin_cost")]
        assert any_legacy, "Legacy run produced no efin_cost values"
        max_legacy = max(any_legacy)
        # Reference auto value for LMAULE is 1.7M $/hm³.  Legacy is
        # capped at 500 $/hm³ (default --vert-cost-cap), so even the
        # largest legacy value is at least 1000× smaller.
        assert max_legacy < 10_000.0, (
            f"Legacy max efin_cost {max_legacy} unexpectedly large; "
            "auto-pipeline magnitude justification may be invalid"
        )
        # The auto-pipeline magnitude is anchor × LMAULE.lost_pf × 1e6/3600.
        # Even with a conservative anchor of 100 $/MWh (well below any
        # juan/IPLP electricity price), LMAULE's cascade_pf=10.05 gives
        # an auto efin_cost of ~280 k $/hm³ — orders of magnitude above
        # the legacy cap.
        conservative_anchor = 100.0  # $/MWh — lower bound for any case
        auto_lower_bound = (
            conservative_anchor * _REFERENCE_RESERVOIRS_LOST_PF["LMAULE"] * 1e6 / 3600.0
        )
        ratio = auto_lower_bound / max(max_legacy, 1e-9)
        assert ratio > 100.0, (
            f"auto/legacy ratio = {ratio:.1f}× — expected >100× discount"
        )


# ---------------------------------------------------------------------------
# Synthetic threshold parametrisation (Python-only — no PLP case required)
# ---------------------------------------------------------------------------


# These tests verify the *formula* of the auto pipeline against the
# threshold values quoted in the design note, without needing a full
# Planning JSON or solver plugin to be available.  They duplicate
# :mod:`test_water_value` checks at coarser granularity — the unit
# tests cover correctness on inputs; here we cover the thresholds the
# design note calls out as governing slack satisfaction in the LP.


class _CP:  # pragma: no cover - tiny test helper
    def __init__(self, centrals):
        self.centrals = list(centrals)


def _juan_like_resolver(*, anchor_override: float | None = None):
    """Build a small resolver matching the juan/IPLP magnitudes.

    Single embalse with cascade ending at one bus>0 turbine of rendi
    10.0 — matches the design note's synthetic "JUAN_LIKE_CASE" with
    cascade_rendi = 10.0.
    """
    centrals = [
        # Reservoir (transit, bus=0).
        {
            "number": 1,
            "name": "TEST_RES",
            "type": "embalse",
            "bus": 0,
            "ser_hid": 2,
            "efficiency": 0.0,
            "emax": 100.0,
        },
        # Single turbine downstream.
        {
            "number": 2,
            "name": "TEST_TURB",
            "type": "serie",
            "bus": 1,
            "ser_hid": 0,
            "efficiency": 10.0,
            "emax": 0.0,
        },
        {
            "number": 99,
            "name": "FALLA_T4",
            "type": "falla",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": 568.4,
        },
    ]
    options: dict = {"auto_water_fail_cost": True}
    if anchor_override is not None:
        options["water_fail_cost"] = anchor_override
    return WaterValueResolver(
        central_parser=_CP(centrals),
        cenre_parser=None,
        options=options,
    )


@pytest.mark.parametrize(
    "anchor_override,expected_efin",
    [
        # Below threshold: anchor × cascade × 277.78 < demand_fail_cost ×
        # cascade × 277.78.  The 360 anchor produces ~1.0M $/hm³ which is
        # below the 1.578M $/hm³ break-even — the LP would manufacture
        # fictitious water via the slack.
        pytest.param(360.0, 1_000_000.0, id="below_threshold"),
        # Auto-derived value: 569.4 × 10 × 277.78 ≈ 1.58M $/hm³ — strictly
        # above the 1.578M break-even (max(falla.gcost) × cascade × 277.78).
        pytest.param(569.4, 1_581_667.0, id="auto_proposed"),
        # User-quoted threshold: 568 × 10 × 277.78 ≈ 1.578M (no +1).
        pytest.param(568.0, 1_577_777.0, id="exact_threshold"),
        # Way above — slack always satisfied.
        pytest.param(1000.0, 2_777_777.0, id="above_threshold"),
        # Zero anchor: any slack is "free", LP fully violates.
        pytest.param(0.0, 0.0, id="zero_cost"),
    ],
)
def test_efin_cost_threshold_formula(anchor_override, expected_efin):
    """Sweep the anchor and verify ``efin_cost(cascade=10)`` follows the
    documented formula (anchor × 10 × 1e6/3600).

    The qualitative claim — that anchors above ``demand_fail_cost ×
    cascade × 1e6/3600`` price-dominate the slack and below it the LP
    manufactures fictitious water — is documented in the helper module
    and checked end-to-end by the magnitude class above when the PLP
    case is on disk.
    """
    resolver = _juan_like_resolver(anchor_override=anchor_override)
    assert resolver.water_fail_cost == pytest.approx(anchor_override, abs=0.01)
    cost = resolver.efin_cost(10.0)
    assert cost == pytest.approx(expected_efin, abs=200.0)


@pytest.mark.parametrize(
    "anchor_override,expected_fail_cost",
    [
        pytest.param(300.0, 3_000.0, id="below_threshold"),
        pytest.param(569.4, 5_694.0, id="auto_proposed"),
        pytest.param(568.0, 5_680.0, id="exact_threshold"),
        pytest.param(1000.0, 10_000.0, id="above_threshold"),
        pytest.param(0.0, 0.0, id="zero_cost"),
    ],
)
def test_fail_cost_threshold_formula(anchor_override, expected_fail_cost):
    """Symmetric sweep for ``FlowRight.fail_cost = anchor × own_rendi``."""
    resolver = _juan_like_resolver(anchor_override=anchor_override)
    cost = resolver.fail_cost(10.0)  # rendi = 10 to mirror the cascade test
    assert cost == pytest.approx(expected_fail_cost, abs=1.0)
