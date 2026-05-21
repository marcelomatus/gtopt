"""Integration test against the real CEN PCP daily bundle.

The fixture lives under ``support/plexos/DATOS20260422.zip.xz`` and is
~3 MB compressed; if it isn't present the test is skipped so CI on a
sparse checkout stays green.  When present, the test asserts:

* the bundle locator handles ``.zip.xz`` without extracting in place
  (the file lands in a tempdir owned by the bundle handle);
* every P1 extractor returns a non-trivial count;
* the JSON written to ``tmp_path`` round-trips through the writer and
  has the required ``options.model_options`` / ``simulation`` shape;
* when a ``gtopt`` binary is available (env ``GTOPT_BIN`` or under
  ``build/standalone/``), ``gtopt --lp-only`` accepts the produced
  planning in both single-bus and DC-OPF modes.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from plexos2gtopt.parsers import extract_case
from plexos2gtopt.plexos2gtopt import convert_plexos_bundle
from plexos2gtopt.plexos_loader import locate_bundle


REAL_BUNDLE = (
    Path(__file__).resolve().parents[3]
    / "support"
    / "plexos_pcp_2026-04-22"
    / "DATOS20260422.zip.xz"
)


def _gtopt_binary() -> Path | None:
    """Return a usable ``gtopt`` executable path or ``None``."""
    env = os.environ.get("GTOPT_BIN")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return Path(env)
    repo_root = Path(__file__).resolve().parents[3]
    candidate = repo_root / "build" / "standalone" / "gtopt"
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return candidate
    on_path = shutil.which("gtopt")
    return Path(on_path) if on_path else None


_skip_if_missing = pytest.mark.skipif(
    not REAL_BUNDLE.is_file(),
    reason=f"CEN PCP bundle not vendored: {REAL_BUNDLE}",
)
_skip_if_no_gtopt = pytest.mark.skipif(
    _gtopt_binary() is None,
    reason="gtopt binary not available (set GTOPT_BIN or build standalone)",
)


@_skip_if_missing
def test_real_bundle_locate_and_extract() -> None:
    """The .zip.xz extracts to a tempdir; original file is untouched."""
    mtime_before = REAL_BUNDLE.stat().st_mtime
    with locate_bundle(REAL_BUNDLE) as bundle:
        assert bundle.xml_path.is_file()
        # The tempdir is somewhere under /tmp, NOT next to the archive.
        assert str(bundle.root).startswith("/tmp/")
        assert bundle.root != REAL_BUNDLE.parent
    # Archive on disk is unchanged.
    assert REAL_BUNDLE.stat().st_mtime == mtime_before


@_skip_if_missing
def test_real_bundle_extractors_nonempty() -> None:
    """Every P1 extractor returns at least one element."""
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert len(case.nodes) > 100, "expected hundreds of buses in PCP bundle"
    assert len(case.generators) > 500, "expected hundreds of generators"
    assert len(case.lines) > 100
    assert len(case.demands) > 50
    assert len(case.batteries) > 0
    assert len(case.fuels) > 0


@_skip_if_missing
def test_real_bundle_decision_variables() -> None:
    """PLEXOS Decision Variable catalogue (43 objects) flows end-to-end.

    Each DV object becomes a :class:`DecisionVariableSpec` with bounds
    + cost pulled from the System→Decision Variables ``Lower Bound`` /
    ``Upper Bound`` / ``Objective Function Coefficient`` t_data rows.
    The Value Coefficient on DV→Constraint memberships routes into
    ``decision_variable("X").value`` user constraint LHS terms.
    """
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert len(case.decision_variables) >= 40, (
        f"expected ≥40 PLEXOS Decision Variables, got {len(case.decision_variables)}"
    )
    # At least some DVs carry bounds.
    bounded = sum(
        1
        for d in case.decision_variables
        if d.lower_bound is not None or d.upper_bound is not None
    )
    assert bounded >= 30
    # Constraints reference them via decision_variable("X").value.
    dv_uses = [c for c in case.user_constraints if "decision_variable(" in c.expression]
    assert len(dv_uses) >= 30


@_skip_if_missing
def test_real_bundle_user_constraints() -> None:
    """PLEXOS Constraint catalogue translates into a non-trivial UserConstraint set.

    The CEN PCP bundle ships 1218 Constraint objects; we emit the
    subset whose LHS uses gtopt-supported coefficient kinds (Generator
    Generation, Line Flow, Battery Charge/Discharge, Generator
    Commitment binaries, ReserveProvision up/dn, Fuel Offtake expansion)
    AND whose referenced elements survive the active-block filter.
    """
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert len(case.user_constraints) >= 600
    # Every expression carries an operator + a right-hand-side number.
    for c in case.user_constraints[:20]:
        assert (
            (" <= " in c.expression)
            or (" >= " in c.expression)
            or (" = " in c.expression)
        )
    # Corridor (SD_*) constraints with line.flow terms must be present.
    sd_constraints = [c for c in case.user_constraints if "line(" in c.expression]
    assert sd_constraints, "expected at least one Line-based UserConstraint"
    # Commitment binaries must be present (gtopt's
    # ``commitment_lp.cpp`` registers status/startup/shutdown via
    # ``add_ampl_variable`` on chronological stages).
    commit_constraints = [
        c for c in case.user_constraints if "commitment(" in c.expression
    ]
    assert len(commit_constraints) >= 50, (
        "expected commitment-binary references in user constraints"
    )
    # Reserve-provision constraints (urmax/drmax + provision_factor
    # defaults ensure gtopt allocates the up/dn columns).
    rp_constraints = [
        c for c in case.user_constraints if "reserve_provision(" in c.expression
    ]
    assert len(rp_constraints) >= 20


@_skip_if_missing
def test_real_bundle_p7_flow_rights() -> None:
    """P7: ``Hydro_AntucoBounds.csv`` no longer emits FlowRights.

    The legacy interpretation of that CSV as junction-level irrigation
    rights was incorrect — PLEXOS encodes those rows as per-generator
    discharge limits (Generator→FlowConstraint, collection_id=32), not
    Junction→Right.  Emitting them as FlowRights caused phantom
    infeasibilities on the upstream reservoir balance.

    ``extract_flow_rights`` now returns ``()`` unconditionally for
    this CSV; junction-level irrigation rights (when needed) will
    come from a different overlay.  This test pins the disabled
    behaviour so any future re-enable is intentional.
    """
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert case.flow_rights == ()


@_skip_if_missing
def test_real_bundle_p5_p6_reserves_and_commitment() -> None:
    """P5/P6: reserves carry ur/dr profiles; commitments cover thermals.

    Sanity-check the CEN PCP bundle exposes:
      - 12 Reserve zones (CPF/CSF/CTF × LW/RS × 2)
      - At least 4 reserves with a non-empty requirement profile
      - Several hundred ReserveProvision rows (one per eligible Gen)
      - A few hundred Commitment rows for thermal units
    """
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert len(case.reserves) >= 6
    with_req = sum(1 for r in case.reserves if r.ur_requirement or r.dr_requirement)
    assert with_req >= 4
    assert len(case.reserve_provisions) >= 100
    assert len(case.commitments) >= 100
    # Sample commitment: thermal AGUAS_BLANCAS has a non-zero startup cost
    # or initial-hours signal on the daily PCP run.
    nonzero = sum(
        1 for c in case.commitments if c.startup_cost > 0 or c.initial_hours != 0.0
    )
    assert nonzero >= 50


@_skip_if_missing
def test_real_bundle_p4_hydro_topology() -> None:
    """P4: hydro extractors populate reservoir/waterway/junction/turbine/flow.

    Sanity-check the CEN PCP bundle exposes the expected hydro shapes:
    ~28 storage-bearing Reservoirs (PLEXOS pass-through Storages and
    synthetic ``*_sink`` / ``*_ocean`` drain endpoints are NOT emitted
    as Reservoirs — they live in ``junction_array`` only with
    ``drain = True``), ~70 Junctions (one per Reservoir plus the
    extra sink/ocean/pass-through nodes), ~28 Waterways, 77 hydro
    turbines, ~23 inflow time-series, and every Flow's junction
    matching a real Junction (no orphans).
    """
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    assert len(case.reservoirs) >= 25
    # Junction count >= reservoir count: every Reservoir gets a co-located
    # Junction, plus extra Junctions for PLEXOS pass-through Storage and
    # synthetic ``*_sink`` / ``*_ocean`` drain endpoints.
    assert len(case.junctions) >= len(case.reservoirs)
    # Sink / ocean drain Junctions must exist but NOT be Reservoirs.
    reservoir_names = {r.name for r in case.reservoirs}
    sink_junctions = [
        j
        for j in case.junctions
        if j.name.endswith("_sink") or j.name.endswith("_ocean")
    ]
    assert sink_junctions, "expected synthetic sink/ocean junctions"
    for j in sink_junctions:
        assert j.drain, f"sink junction {j.name} must have drain=True"
        assert j.name not in reservoir_names, (
            f"sink junction {j.name} must not be emitted as a Reservoir"
        )
    assert len(case.waterways) >= 20
    assert len(case.turbines) >= 50
    assert len(case.flows) >= 10
    # Every Flow's junction matches a real Junction (orphans dropped).
    junction_names = {j.name for j in case.junctions}
    for flow in case.flows:
        assert flow.junction_name in junction_names, (
            f"orphan Flow junction: {flow.junction_name}"
        )
    # Every Turbine's reservoir matches a real Reservoir.  ``*_GNL_INF``
    # reservoirs are dropped upstream (they are LNG gas-import artifacts,
    # not water reservoirs — see e763f39d1) so the turbines that still
    # reference them are expected orphans; skip them here.
    reservoir_names = {r.name for r in case.reservoirs}
    for turbine in case.turbines:
        if turbine.reservoir_name and turbine.reservoir_name.endswith("_GNL_INF"):
            continue
        assert turbine.reservoir_name in reservoir_names
    # Every Waterway's endpoints map to junctions.
    for ww in case.waterways:
        assert ww.storage_from in junction_names
        assert ww.storage_to in junction_names


@_skip_if_missing
def test_real_bundle_p3_thermal_costs() -> None:
    """P3: thermals priced from Fuel_Price × HeatRate + VOM; renewables = 0."""
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    # Most fuels in the daily PCP bundle ship a non-zero price (a
    # handful of carry-over fuels like biogas are intentionally 0).
    priced = sum(1 for f in case.fuels if f.price > 0.0)
    assert priced >= 0.8 * len(case.fuels), (
        f"expected ≥80% of fuels priced, got {priced}/{len(case.fuels)}"
    )
    thermals = [g for g in case.generators if g.fuel_names]
    renewables = [g for g in case.generators if not g.fuel_names]
    assert thermals, "expected at least some thermal units"
    assert renewables, "expected at least some renewable units (no Fuel link)"
    # Thermal heat rates land in PLEXOS-units (MMBtu/MWh-ish); the
    # 2026-04-22 bundle has roughly half its thermals with non-zero
    # heat rate (the rest are derated/maintenance units).
    with_hr = sum(1 for g in thermals if g.heat_rate > 0.0)
    assert with_hr >= 0.4 * len(thermals)


@_skip_if_missing
def test_real_bundle_p2_static_properties() -> None:
    """P2: line reactance and battery power should populate from t_data."""
    with locate_bundle(REAL_BUNDLE) as bundle:
        case = extract_case(bundle)
    # At least 90% of lines should expose a non-zero reactance — the
    # CEN PCP bundle declares Reactance for every transmission element.
    lines_with_x = sum(1 for line in case.lines if line.reactance > 0.0)
    assert lines_with_x >= 0.9 * len(case.lines), (
        f"expected ≥90% of lines to carry reactance, got "
        f"{lines_with_x}/{len(case.lines)}"
    )
    # Every battery must expose a non-zero Max Power (Capacity may be
    # zero for some sites still in construction, but Max Power is the
    # nameplate the unit was registered with).
    powered = sum(1 for b in case.batteries if b.pmax_discharge > 0.0)
    assert powered >= 0.9 * len(case.batteries)
    # Round-trip efficiency: ≥50% means the percent→fraction conversion
    # in the parser fired (CEN values are 95–98%).  Without the
    # conversion the value would still be ≈97 (raw percent), well
    # above 1.0 and trivially distinguishable.
    for batt in case.batteries:
        assert 0.5 <= batt.input_efficiency <= 1.0
        assert 0.5 <= batt.output_efficiency <= 1.0


@_skip_if_missing
def test_real_bundle_convert_emits_valid_planning(tmp_path: Path) -> None:
    """`convert_plexos_bundle` produces a gtopt-shaped planning JSON."""
    out_file = tmp_path / "DATOS20260422.json"
    convert_plexos_bundle(
        {
            "input_bundle": REAL_BUNDLE,
            "output_dir": tmp_path,
            "output_file": out_file,
            "use_single_bus": True,
            # Pin the legacy 24-block layout: the default is now
            # ``--horizon-mode plexos`` which would auto-discover the
            # adjacent RES bundle and emit the 111-block PLEXOS layout.
            # This test asserts the shape of the simulation block; keep
            # the single-day baseline for it and add a separate test
            # for the multi-day PLEXOS-native layout below.
            "horizon_mode": "hourly",
            "horizon_days": 1,
        }
    )
    assert out_file.is_file()
    planning = json.loads(out_file.read_text())
    # Schema shape required by gtopt's planning parser.
    assert set(planning) == {"options", "simulation", "system"}
    model_opts = planning["options"]["model_options"]
    assert model_opts["use_single_bus"] is True
    assert model_opts["use_kirchhoff"] is False
    sim = planning["simulation"]
    assert {"block_array", "stage_array", "scenario_array"} <= set(sim)
    assert "scene_array" not in sim
    assert "phase_array" not in sim
    assert len(sim["block_array"]) == 24
    system = planning["system"]
    assert len(system["bus_array"]) > 100
    assert len(system["generator_array"]) > 500
    assert len(system["demand_array"]) > 50
    # Generator/bus references match by name.
    bus_names = {b["name"] for b in system["bus_array"]}
    for gen in system["generator_array"][:50]:
        assert gen["bus"] in bus_names
    # P3: at least a few hundred generators ship a non-zero gcost
    # (thermals with priced fuel + heat rate × fuel-price > 0).
    nonzero_gcost = sum(1 for g in system["generator_array"] if g.get("gcost", 0) > 0)
    assert nonzero_gcost >= 100, f"expected ≥100 gens with gcost>0, got {nonzero_gcost}"


@_skip_if_missing
@_skip_if_no_gtopt
def test_real_bundle_single_bus_solves(tmp_path: Path) -> None:
    """`gtopt --lp-only` accepts the single-bus planning end-to-end."""
    out_file = tmp_path / "single_bus.json"
    convert_plexos_bundle(
        {
            "input_bundle": REAL_BUNDLE,
            "output_dir": tmp_path,
            "output_file": out_file,
            "use_single_bus": True,
            # Pin the legacy 24-block layout for this LP-only smoke test.
            "horizon_mode": "hourly",
            "horizon_days": 1,
        }
    )
    gtopt = _gtopt_binary()
    assert gtopt is not None  # _skip_if_no_gtopt ensures this
    proc = subprocess.run(
        [str(gtopt), "--lp-only", "-s", out_file.name],
        cwd=tmp_path,
        capture_output=True,
        timeout=180,
        check=False,
    )
    assert proc.returncode == 0, (
        f"gtopt --lp-only failed (rc={proc.returncode}): {proc.stderr.decode()[:500]}"
    )


@_skip_if_missing
@_skip_if_no_gtopt
def test_real_bundle_dc_opf_solves(tmp_path: Path) -> None:
    """`gtopt --lp-only` accepts the DC-OPF (multi-bus, Kirchhoff) planning."""
    out_file = tmp_path / "dc_opf.json"
    convert_plexos_bundle(
        {
            "input_bundle": REAL_BUNDLE,
            "output_dir": tmp_path,
            "output_file": out_file,
            # No use_single_bus override → multi-bus, use_kirchhoff=true.
            "horizon_mode": "hourly",
            "horizon_days": 1,
        }
    )
    planning = json.loads(out_file.read_text())
    assert planning["options"]["model_options"]["use_kirchhoff"] is True
    gtopt = _gtopt_binary()
    assert gtopt is not None
    proc = subprocess.run(
        [str(gtopt), "--lp-only", "-s", out_file.name],
        cwd=tmp_path,
        capture_output=True,
        timeout=180,
        check=False,
    )
    assert proc.returncode == 0, (
        f"gtopt --lp-only (DC OPF) failed (rc={proc.returncode}): "
        f"{proc.stderr.decode()[:500]}"
    )
