# -*- coding: utf-8 -*-

"""PLP vs gtopt comparison on the plp_2_years case.

The 2-year support case ships the OFFICIAL PLP outputs of the same
inputs (``plpemb.parquet`` — per-(simulation, block) reservoir
volumes, turbined/spilled flows, inflows and filtration), so gtopt
with all irrigation couplings enabled can be validated against the
real PLP run without building the Fortran model.

Three tiers, reflecting how the two models are allowed to differ:

* **EQUAL (always on)** — policy-independent data-model identities:
  the same inputs must produce the same model constants.  Initial
  volumes, inflow passthrough, and the ReservoirSeepage piecewise
  evaluated at PLP's own operating volumes (vs PLP's ``EmbQFil``).
  A mismatch here is a conversion bug, never a dispatch difference.

* **GLOBAL similar (opt-in, ``GTOPT_PLP_COMPARE=1``)** — runs the
  full gtopt solve (first hydrology = PLP "Sim 1") and compares
  system-level aggregates with wide bands: per-reservoir turbined
  totals, total end-of-horizon storage.  PLP simulates a trained
  SDDP policy; gtopt solves with perfect foresight — levels diverge
  legitimately, structure should not.

* **IRRIGATION similar (opt-in)** — the agreements' physical
  footprint: El Toro turbinado (the anchored Laja partition draws on
  it), the 2017 Acuerdo recovery direction of the lake, Laguna del
  Maule extraction totals, and spill discipline.  Seasonal shares
  are written to the report rather than asserted (perfect-foresight
  horizon effects legitimately reshape the within-year timing).

Both opt-in classes write side-by-side CSV reports next to the solve
output for human review.

Run the opt-in tier single-worker (one conversion + one solve):

    GTOPT_PLP_COMPARE=1 GTOPT_BIN=... python -m pytest -n0 \
        plp2gtopt/tests/test_plp_gtopt_comparison.py
"""

import json
import math
import os
import subprocess
import sys
from pathlib import Path

import pytest

pd = pytest.importorskip("pandas")
pytest.importorskip("pyarrow")

_SUPPORT_DIR = Path(__file__).resolve().parents[3] / "support"
_PLP_2Y = _SUPPORT_DIR / "plp" / "2_years"
_PLP_EMB = _PLP_2Y / "plpemb.parquet"
_HIDRO_FIRST = "Sim  1"  # PLP simulation of the first hydrology

pytestmark = [
    pytest.mark.integration,
    pytest.mark.skipif(
        not _PLP_EMB.exists(),
        reason="plp_2_years official PLP outputs (plpemb.parquet) not available",
    ),
]


@pytest.fixture(scope="module", name="plp_emb")
def fixture_plp_emb():
    """Official PLP embalse output."""
    df = pd.read_parquet(_PLP_EMB)
    df["EmbNom"] = df["EmbNom"].str.strip()
    return df


def _plp_reservoir(plp_emb, name: str, hidro: str = _HIDRO_FIRST):
    sel = plp_emb[(plp_emb["EmbNom"] == name) & (plp_emb["Hidro"] == hidro)]
    return sel.sort_values("Bloque").reset_index(drop=True)


def _convert(output_dir, extra_args=()):
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "plp2gtopt.main",
            str(_PLP_2Y),
            "-o",
            str(output_dir),
            "--first-scenario",
            "-t",
            "1y",
            "-F",
            "csv",
            "--expand-water-rights",
            "--use-kirchhoff",
            "--demand-fail-cost",
            "1000",
            "--scale-objective",
            "1000",
            "--no-drop-spillway-waterway",
            *extra_args,
        ],
        capture_output=True,
        text=True,
        check=False,
        cwd=Path(__file__).resolve().parents[2],
    )
    assert result.returncode == 0, result.stderr[-2000:]
    planning_file = _planning_file(output_dir)
    return output_dir, json.loads(planning_file.read_text(encoding="utf-8"))


@pytest.fixture(scope="module", name="converted")
def fixture_converted(tmp_path_factory):
    """Convert with all irrigation couplings enabled (plp2gtopt
    defaults — including cuts-govern-terminal, so cut-covered
    reservoirs carry no efin/efin_cost)."""
    return _convert(tmp_path_factory.mktemp("plp_compare"))


@pytest.fixture(scope="module", name="converted_legacy")
def fixture_converted_legacy(tmp_path_factory):
    """Convert with ``--no-cuts-govern-terminal --soft-storage-bounds`` so
    cut-covered reservoirs retain a boundary-cut-priced ``efin_cost`` (for
    provenance checks that inspect the emitted value).

    ``--soft-storage-bounds`` is required, not incidental: ``efin_cost`` is
    the *price* of the soft slack on the ``vol_end >= efin`` row, so it is
    only emitted when that terminal bound is soft.  The default is a HARD
    ``vol_end >= efin`` bound with no price (see
    ``junction_writer._relax_storage_bounds``, which returns early unless
    ``soft_storage_bounds`` is set), which is why the reservoirs otherwise
    carry ``efin_cost = None``.  ``--no-cuts-govern-terminal`` then keeps the
    cut-covered reservoirs' ``efin_cost`` instead of dropping it in favour of
    the loaded FCF cuts as the sole terminal mechanism."""
    return _convert(
        tmp_path_factory.mktemp("plp_compare_legacy"),
        ("--no-cuts-govern-terminal", "--soft-storage-bounds"),
    )


def _planning_file(output_dir: Path) -> Path:
    return next(
        p
        for p in output_dir.glob("*.json")
        if not any(
            k in p.name for k in ("water_rights", "ror_promoted", "state", "output")
        )
    )


def _stage_block_counts(planning) -> list[int]:
    return [int(s["count_block"]) for s in planning["simulation"]["stage_array"]]


def _stage_months(planning) -> list[str]:
    return [str(s.get("month", "")) for s in planning["simulation"]["stage_array"]]


def _plp_stage_means(series, counts):
    means, pos = [], 0
    for n in counts:
        chunk = series.iloc[pos : pos + n]
        if chunk.empty:
            break
        means.append(float(chunk.mean()))
        pos += n
    return means


def _seepage_piecewise(planning, reservoir: str):
    entity = next(
        (
            e
            for e in planning["system"].get("reservoir_seepage_array", [])
            if e.get("reservoir") == reservoir
        ),
        None,
    )
    if entity is None:
        return None
    segments = entity.get("segments") or [
        {
            "volume": 0.0,
            "slope": float(entity.get("slope", 0.0)),
            "constant": float(entity.get("constant", 0.0)),
        }
    ]

    def qfil(v_hm3: float) -> float:
        active = segments[0]
        for seg in segments:
            if float(seg.get("volume", 0.0)) <= v_hm3:
                active = seg
        return (
            float(active.get("constant", 0.0)) + float(active.get("slope", 0.0)) * v_hm3
        )

    return qfil


class TestEqualTier:
    """Policy-independent identities: same inputs, same constants."""

    def test_initial_volumes_equal(self, plp_emb, converted):
        """Converted reservoirs start exactly where PLP starts
        (validates the per-reservoir volume scale factors, e.g.
        ELTORO's 1e10)."""
        _, planning = converted
        gtopt_res = {
            r["name"]: r for r in planning["system"].get("reservoir_array", [])
        }
        checked = 0
        for name in ("ELTORO", "LMAULE", "COLBUN", "CIPRESES", "RALCO"):
            plp = _plp_reservoir(plp_emb, name)
            entity = gtopt_res.get(name)
            if plp.empty or entity is None:
                continue
            eini = entity.get("eini")
            if not isinstance(eini, (int, float)):
                continue
            fac = float(plp["EmbFac"].iloc[0])
            plp_v0 = float(plp["EmbVini"].iloc[0]) * fac / 1e6
            assert eini == pytest.approx(plp_v0, rel=0.02), (
                f"{name}: gtopt eini {eini} vs PLP {plp_v0} hm3"
            )
            checked += 1
        assert checked >= 3

    def test_inflows_equal(self, plp_emb, converted):
        """The aflce inflows pass through unchanged: gtopt's Flow
        discharge per stage equals PLP's EmbAflu (first hydrology)."""
        output_dir, planning = converted
        flows = {f["name"]: f for f in planning["system"].get("flow_array", [])}
        counts = _stage_block_counts(planning)
        sched_file = output_dir / "Flow" / "discharge.csv"
        assert sched_file.exists(), "Flow discharge schedule not emitted"
        sched = pd.read_csv(sched_file)
        checked = 0
        for name in ("ELTORO", "LMAULE", "CIPRESES"):
            plp = _plp_reservoir(plp_emb, name)
            flow = flows.get(name)
            if plp.empty or flow is None:
                continue
            plp_means = _plp_stage_means(plp["EmbAflu"], counts)
            sel = sched[sched["uid"] == flow["uid"]]
            if sel.empty:
                continue
            gt_means = sel.groupby("stage")["value"].mean()
            # First 24 stages only: the `-t 1y` truncation makes the
            # tail stages' block alignment partial on the gtopt side.
            n = min(len(plp_means), len(gt_means), 24)
            assert n >= 6
            for i in range(n):
                assert float(gt_means.iloc[i]) == pytest.approx(
                    plp_means[i], rel=0.05, abs=0.05
                ), (
                    f"{name} stage {i + 1}: gtopt inflow "
                    f"{float(gt_means.iloc[i])} vs PLP {plp_means[i]}"
                )
            checked += 1
        assert checked >= 1

    def test_default_strips_cut_covered_efin(self, converted):
        """plp2gtopt replicates PLP: by DEFAULT cut-covered reservoirs
        are governed by the FCF cuts, so they carry no efin/efin_cost
        soft-slack (PLP EmbCFUE behaviour)."""
        _, planning = converted
        from plp2gtopt.planos_parser import (  # pylint: disable=import-outside-toplevel
            PlanosParser,
            find_planos_files,
        )

        files = find_planos_files(_PLP_2Y)
        assert files is not None
        planos = PlanosParser(files[0], files[1])
        planos.parse()
        cut_names = set(planos.lower_bound_water_value_by_reservoir(num_scenarios=None))
        assert cut_names
        for entity in planning["system"].get("reservoir_array", []):
            if entity["name"] in cut_names:
                assert "efin_cost" not in entity, (
                    f"{entity['name']}: cut-covered reservoir still carries "
                    "efin_cost under the default (should be cuts-governed)"
                )

    def test_efin_cost_from_boundary_cuts(self, converted_legacy):
        """Reservoir.efin_cost must come from the BOUNDARY CUTS file
        (plpplem1/2 planos, cut lower-bound water value un-discounted
        by the last stage's discount factor) — never from the
        ANCHOR x lost_pf fail-cost estimate when cuts exist for the
        reservoir (user requirement 2026-07)."""
        from plp2gtopt.planos_parser import (  # pylint: disable=import-outside-toplevel
            PlanosParser,
            find_planos_files,
        )

        _, planning = converted_legacy
        files = find_planos_files(_PLP_2Y)
        assert files is not None, "planos files missing from the case"
        planos = PlanosParser(files[0], files[1])
        planos.parse()
        cut_wv = planos.lower_bound_water_value_by_reservoir(num_scenarios=None)
        last_df = float(planning["simulation"]["stage_array"][-1]["discount_factor"])
        checked = 0
        for entity in planning["system"].get("reservoir_array", []):
            name = entity["name"]
            efin_cost = entity.get("efin_cost")
            if name not in cut_wv or not isinstance(efin_cost, (int, float)):
                continue
            expected = cut_wv[name] / last_df
            assert efin_cost == pytest.approx(expected, rel=0.01), (
                f"{name}: efin_cost {efin_cost} != boundary-cut value "
                f"{cut_wv[name]} / discount {last_df} = {expected:.1f} — "
                f"fell back to the fail-cost estimate?"
            )
            checked += 1
        assert checked >= 8, f"only {checked} reservoirs cut-priced"

    def test_seepage_piecewise_matches_plp_filtration(self, plp_emb, converted):
        """gtopt's ReservoirSeepage piecewise reproduces PLP's
        per-block EmbQFil when evaluated at PLP's own volumes — the
        filtration model is identical regardless of dispatch."""
        _, planning = converted
        checked = 0
        for name in ("ELTORO", "CIPRESES"):
            qfil = _seepage_piecewise(planning, name)
            plp = _plp_reservoir(plp_emb, name)
            if qfil is None or plp.empty:
                continue
            fac = float(plp["EmbFac"].iloc[0])
            v_hm3 = plp["EmbVini"] * fac / 1e6
            pred = v_hm3.map(qfil)
            # BINDING regime only: PLP's filtration is itself an LP
            # variable constrained by the segment envelope — during
            # extreme refill events (small reservoirs swinging from
            # empty within a stage) the solved value legitimately
            # sits off the curve.  Where the volume is stable and the
            # flow meaningful, the identity must hold.
            stable = ((plp["EmbVfin"] - plp["EmbVini"]).abs() * fac / 1e6) < 0.2 * v_hm3
            mask = (pred > 1.0) & (plp["EmbQFil"] > 1.0) & stable
            if mask.sum() < 20:
                continue
            rel = (pred[mask] - plp["EmbQFil"][mask]).abs().sum() / plp["EmbQFil"][
                mask
            ].sum()
            assert rel < 0.10, f"{name}: aggregate filtration error {rel:.1%}"
            checked += 1
        assert checked >= 1, "no seepage-bearing reservoir found to check"


_COMPARE_ENABLED = bool(os.environ.get("GTOPT_PLP_COMPARE")) and bool(
    os.environ.get("GTOPT_BIN")
)


@pytest.fixture(scope="module", name="solved")
def fixture_solved(converted):
    """Full gtopt solve of the converted case (opt-in tier only)."""
    output_dir, planning = converted
    result = subprocess.run(
        [os.environ["GTOPT_BIN"], _planning_file(output_dir).name],
        capture_output=True,
        text=True,
        check=False,
        cwd=output_dir,
    )
    assert result.returncode == 0, result.stderr[-2000:]
    return output_dir, planning


def _gtopt_solution_series(output_dir, planning, kind, name, field):
    arrays = {
        "Turbine": "turbine_array",
        "Reservoir": "reservoir_array",
        "Waterway": "waterway_array",
        "VolumeRight": "volume_right_array",
    }
    uid = next(
        (e["uid"] for e in planning["system"][arrays[kind]] if e["name"] == name),
        None,
    )
    if uid is None:
        return []
    rows: list[float] = []
    rdir = output_dir / "results" / kind
    for p in sorted(
        rdir.glob(f"{field}_s1_p*.csv.zst"),
        key=lambda x: int(x.stem.split("_p")[-1].split(".")[0]),
    ):
        df = pd.read_csv(p)
        sel = df[df["uid"] == uid]
        # One value per phase (stage mean) regardless of the element's
        # output granularity (flow-mode turbines emit a single stage
        # column, waterway-fed ones emit per block); the writers omit
        # all-zero rows, so a missing phase means zero flow.
        rows.append(float(sel["value"].mean()) if not sel.empty else 0.0)
    return rows


_IN_SEASON = {"december", "january", "february", "march", "april"}


def _seasonal_split(values, counts, months):
    pos, inseason, total = 0, 0.0, 0.0
    for i, n in enumerate(counts):
        chunk = sum(values[pos : pos + n])
        total += chunk
        if i < len(months) and months[i] in _IN_SEASON:
            inseason += chunk
        pos += n
    return inseason, total


@pytest.mark.skipif(
    not _COMPARE_ENABLED,
    reason="behavioral comparison is opt-in: set GTOPT_PLP_COMPARE=1 and GTOPT_BIN",
)
class TestGlobalSimilar:
    """System-level aggregates: similar, not number-by-number."""

    def test_turbined_totals_per_reservoir(self, plp_emb, solved):
        """Total turbined water per reservoir over the common horizon:
        the two models dispatch differently but move comparable water
        (calibrated band: observed ratios 0.76-1.58)."""
        output_dir, planning = solved
        rows = []
        for turbine in planning["system"].get("turbine_array", []):
            name = turbine["name"]
            plp = _plp_reservoir(plp_emb, name)
            if plp.empty:
                continue
            gt = _gtopt_solution_series(
                output_dir, planning, "Turbine", name, "flow_sol"
            )
            counts = _stage_block_counts(planning)
            plp_stage = _plp_stage_means(plp["EmbQgen"], counts)
            n = min(len(gt), len(plp_stage))
            if n < 12:
                continue
            plp_total = float(pd.Series(plp_stage[:n]).sum())
            gt_total = float(pd.Series(gt[:n]).sum())
            rows.append((name, plp_total, gt_total))
            # ELTORO is asserted by the irrigation-specific test with
            # its own band: its tiny turbinado trades off against the
            # 2017-Acuerdo storage recovery.
            if plp_total > 500.0 and name != "ELTORO":
                ratio = gt_total / plp_total
                assert 0.4 <= ratio <= 2.5, (
                    f"{name}: turbined-total ratio {ratio:.2f} out of band "
                    f"(gtopt {gt_total:.0f} vs PLP {plp_total:.0f})"
                )
        assert len(rows) >= 5, "too few comparable reservoirs"
        pd.DataFrame(
            rows, columns=["reservoir", "plp_turbined", "gtopt_turbined"]
        ).to_csv(output_dir / "plp_gtopt_global_comparison.csv", index=False)

    def test_total_final_storage(self, plp_emb, solved):
        """System-wide end-of-horizon storage: both models retain a
        comparable total (observed: near-equal)."""
        output_dir, planning = solved
        plp_total, gt_total = 0.0, 0.0
        for entity in planning["system"].get("reservoir_array", []):
            name = entity["name"]
            plp = _plp_reservoir(plp_emb, name)
            gt = _gtopt_solution_series(
                output_dir, planning, "Reservoir", name, "efin_sol"
            )
            if plp.empty or not gt:
                continue
            fac = float(plp["EmbFac"].iloc[0])
            # Compare at gtopt's horizon end (PLP covers 2y, gtopt 1y).
            idx = min(len(plp) - 1, max(0, len(gt) - 1))
            plp_total += float(plp["EmbVfin"].iloc[idx]) * fac / 1e6
            gt_total += float(gt[-1])
        assert plp_total > 0 and gt_total > 0
        assert gt_total == pytest.approx(plp_total, rel=0.5), (
            f"total final storage: gtopt {gt_total:.0f} vs PLP {plp_total:.0f} hm3"
        )


@pytest.mark.skipif(
    not _COMPARE_ENABLED,
    reason="behavioral comparison is opt-in: set GTOPT_PLP_COMPARE=1 and GTOPT_BIN",
)
class TestIrrigationSimilar:
    """The agreements' physical footprint (structure, not numbers)."""

    def test_eltoro_agreement_footprint(self, plp_emb, solved):
        """El Toro carries the whole Laja agreement: the anchored
        rights partition rides on its turbinado and the 2017 Acuerdo
        drives lake recovery.  Asserts totals band + recovery
        direction; writes the seasonal detail to the report."""
        output_dir, planning = solved
        plp = _plp_reservoir(plp_emb, "ELTORO")
        gt_flow = _gtopt_solution_series(
            output_dir, planning, "Turbine", "ELTORO", "flow_sol"
        )
        gt_vol = _gtopt_solution_series(
            output_dir, planning, "Reservoir", "ELTORO", "efin_sol"
        )
        # gtopt turbine/right solutions are one value per phase (stage
        # mean) — compare in stage space.
        counts_all = _stage_block_counts(planning)
        plp_stage = _plp_stage_means(plp["EmbQgen"], counts_all)
        n = min(len(gt_flow), len(plp_stage))
        assert n >= 12
        plp_total = float(pd.Series(plp_stage[:n]).sum())
        gt_total = float(pd.Series(gt_flow[:n]).sum())
        # WIDE band: El Toro's turbinado is small in absolute terms
        # and trades off against storage — with a 1-year horizon and
        # end-volume value, perfect foresight hoards (2017 Acuerdo
        # recovery) where PLP's policy releases.  The recovery-
        # direction assertion below couples the deficit to storage.
        ratio = gt_total / max(plp_total, 1.0)
        assert 0.1 <= ratio <= 4.0, f"ELTORO turbinado ratio {ratio:.2f}"

        # 2017 Acuerdo recovery: neither model may drain the lake
        # below a comparable fraction of PLP's end state.
        fac = float(plp["EmbFac"].iloc[0])
        idx = min(len(plp) - 1, sum(counts_all[:n]) - 1)
        plp_final = float(plp["EmbVfin"].iloc[idx]) * fac / 1e6
        gt_final = float(gt_vol[-1])
        assert gt_final >= 0.7 * plp_final, (
            f"ELTORO final volume {gt_final:.0f} hm3 vs PLP "
            f"{plp_final:.0f} — recovery direction violated"
        )

        # Anchor identity (EQUAL, within gtopt): the Laja partition
        # rights sum equals the anchored El Toro turbinado.  This is a
        # HARD LP row, so it must hold PER (scenario, stage, block) —
        # compare at that grain (phase-mean sums mismatch only because
        # the writers drop all-zero rows, giving each element a
        # different block denominator).
        frs = {
            fr["name"]: fr["uid"]
            for fr in planning["system"].get("flow_right_array", [])
        }
        et_uid = next(
            t["uid"]
            for t in planning["system"]["turbine_array"]
            if t["name"] == "ELTORO"
        )
        rdir = output_dir / "results" / "FlowRight"
        tdir = output_dir / "results" / "Turbine"
        part_uids = [
            frs[k]
            for k in (
                "laja_der_riego",
                "laja_der_electrico",
                "laja_der_mixto",
                "laja_gasto_anticipado",
            )
            if k in frs
        ]
        max_err = 0.0
        n_cells = 0
        for tf in sorted(tdir.glob("flow_sol_s1_p*.csv.zst")):
            ph = tf.name
            rf = rdir / ph
            if not rf.exists():
                continue
            tdf = pd.read_csv(tf)
            rdf = pd.read_csv(rf)
            tsel = tdf[tdf["uid"] == et_uid]
            for _, trow in tsel.iterrows():
                psum = float(
                    rdf[(rdf["uid"].isin(part_uids)) & (rdf["block"] == trow["block"])][
                        "value"
                    ].sum()
                )
                max_err = max(max_err, abs(float(trow["value"]) - psum))
                n_cells += 1
        assert n_cells >= 20
        assert max_err < 1e-3, (
            f"Laja partition != anchored turbinado (max |Δ| = {max_err:.6f} "
            f"over {n_cells} cells)"
        )

        # Spill discipline: the agreements must not manufacture spills.
        plp_spill_stage = _plp_stage_means(plp["EmbQver"], counts_all)
        plp_spill = float(pd.Series(plp_spill_stage[:n]).sum())
        gt_spill_series = _gtopt_solution_series(
            output_dir, planning, "Waterway", "ELTORO_ver_37_39", "flow_sol"
        ) or [0.0]
        gt_spill = float(pd.Series(gt_spill_series[:n]).sum())
        assert gt_spill <= plp_spill + 0.2 * max(plp_total, gt_total), (
            f"gtopt spills {gt_spill:.0f} vs PLP {plp_spill:.0f}"
        )

        months = _stage_months(planning)
        one = [1] * n
        plp_in, plp_tot = _seasonal_split(plp_stage[:n], one, months)
        gt_in, gt_tot = _seasonal_split(gt_flow[:n], one, months)
        pd.DataFrame(
            [
                ("turbined_total", plp_total, gt_total),
                ("final_volume_hm3", plp_final, gt_final),
                ("spill_total", plp_spill, gt_spill),
                ("inseason_share", plp_in / max(plp_tot, 1), gt_in / max(gt_tot, 1)),
            ],
            columns=["metric", "plp", "gtopt"],
        ).to_csv(output_dir / "plp_gtopt_irrigation_comparison.csv", index=False)

    def test_price_and_water_value_kpis(self, plp_emb, solved):
        """LMP / water-value / cut KPIs (the inner-issue detectors).

        * LMPs: PLP CMgBar vs gtopt bus balance duals — the spatial
          rank correlation across matched buses probes the network
          and congestion model (observed 0.975); the mean-level band
          is wide (policy differences move the marginal unit).
        * Water values: PLP EmbPsom2 vs gtopt's reservoir
          water_value duals per stage.  UNITS MATTER (user-caught
          2026-07): EmbPsom = CMg x FPhi / FactRendim is the $/MWh
          representation (divided by the downstream chain yield);
          EmbPsom2 = CMg x FPhi is the raw VOLUME shadow price —
          the counterpart of gtopt's volume-balance dual
          (plp-gdbdemb.f:115-123 vs reservoir_lp.cpp:231).  Against
          Psom2 the median ratio is ~1.2 with most reservoirs near
          1; the residual ELTORO ~5x is reported as a finding.
        * Cuts: PLP's EmbPsom IS its FCF gradient at the visited
          state; gtopt's sddp_cuts must carry Reservoir efin states
          AND the irrigation VolumeRight states (the agreements'
          buckets participate in the value function).
        Writes plp_gtopt_kpi_report.csv.
        """
        output_dir, planning = solved
        bar_file = _PLP_2Y / "plpbar.parquet"
        report_rows = []

        # ---- LMP ----
        if bar_file.exists():
            plp_bar = pd.read_parquet(bar_file)
            plp_bar = plp_bar[plp_bar["Hidro"] == _HIDRO_FIRST].copy()
            plp_bar["BarNom"] = plp_bar["BarNom"].str.strip()
            buses = {b["uid"]: b["name"] for b in planning["system"]["bus_array"]}
            frames = [
                pd.read_csv(fp)
                for fp in (output_dir / "results" / "Bus").glob(
                    "balance_dual_s1_p*.csv.zst"
                )
            ]
            gt = pd.concat(frames)
            n_blocks = int(gt["block"].max())
            gt_mean = gt.groupby("uid")["value"].mean().rename(index=buses)
            plp_mean = (
                plp_bar[plp_bar["Bloque"] <= n_blocks]
                .groupby("BarNom")["CMgBar"]
                .mean()
            )
            both = pd.concat([plp_mean, gt_mean], axis=1, join="inner")
            both.columns = ["plp", "gtopt"]
            both = both.dropna()
            assert len(both) >= 100, "too few matched buses"
            rank_corr = both["plp"].corr(both["gtopt"], method="spearman")
            level_ratio = both["gtopt"].mean() / max(both["plp"].mean(), 1.0)
            assert rank_corr >= 0.8, f"LMP spatial rank corr {rank_corr:.2f}"
            assert 0.5 <= level_ratio <= 2.0, f"LMP level ratio {level_ratio:.2f}"
            report_rows.append(("lmp_matched_buses", len(both), ""))
            report_rows.append(("lmp_rank_corr", round(rank_corr, 3), ""))
            report_rows.append(("lmp_level_ratio", round(level_ratio, 3), ""))

        # ---- water values ----
        counts = _stage_block_counts(planning)
        ratios = []
        for entity in planning["system"].get("reservoir_array", []):
            name = entity["name"]
            plp = _plp_reservoir(plp_emb, name)
            if plp.empty:
                continue
            wv = _gtopt_solution_series(
                output_dir, planning, "Reservoir", name, "water_value_dual"
            )
            if not wv:
                continue
            pos, pm = 0, []
            for nblk in counts[: len(wv)]:
                # Psom2 = raw volume shadow price (x1000: PLP writes
                # per dam3, gtopt per hm3).  Psom (no 2) is the $/MWh
                # form — NOT comparable to a volume dual.
                pm.append(float(plp["EmbPsom2"].iloc[pos : pos + nblk].mean()) * 1e3)
                pos += nblk
            n = min(len(pm), len(wv))
            if n < 6:
                continue
            s_p = pd.Series(pm[:n])
            s_g = pd.Series(wv[:n])
            ratio = s_g.mean() / max(s_p.mean(), 1.0)
            ratios.append(ratio)
            report_rows.append(
                (
                    f"water_value_ratio_{name}",
                    round(ratio, 3),
                    round(float(s_p.corr(s_g)), 3) if n > 3 else "",
                )
            )
        assert len(ratios) >= 5
        median_ratio = float(pd.Series(ratios).median())
        assert 0.3 <= median_ratio <= 3.0, (
            f"median water-value ratio {median_ratio:.2f}"
        )
        report_rows.append(("water_value_median_ratio", round(median_ratio, 3), ""))

        # ---- cuts: irrigation states participate in the FCF ----
        cuts_file = output_dir / "results" / "cuts" / "sddp_cuts.parquet"
        if cuts_file.exists():
            cuts = pd.read_parquet(cuts_file)
            n_res, n_vr = 0, 0
            for coeffs in cuts["coeffs"]:
                for e in coeffs:
                    if e["cls"] == "Reservoir":
                        n_res += 1
                    elif e["cls"] == "VolumeRight":
                        n_vr += 1
            assert n_res > 0, "no reservoir states in cuts"
            assert n_vr > 0, (
                "no VolumeRight states in cuts — the irrigation buckets "
                "must participate in the value function"
            )
            report_rows.append(("cut_reservoir_coeffs", n_res, ""))
            report_rows.append(("cut_volume_right_coeffs", n_vr, ""))

        pd.DataFrame(report_rows, columns=["kpi", "value", "extra"]).to_csv(
            output_dir / "plp_gtopt_kpi_report.csv", index=False
        )

    # PLP convenio state names (plplajam.csv / plpmaule.csv columns,
    # leelajam.f:62-83 / genpdmaule.f:1385-1399) -> gtopt VolumeRights.
    _LAJA_STATES = {
        "vdrf": "laja_vol_der_riego",
        "vdef": "laja_vol_der_electrico",
        "vdmf": "laja_vol_der_mixto",
        "vgaf": "laja_vol_gasto_anticipado",
    }
    _MAULE_STATES = {
        "vmgemf": "maule_vol_gasto_elec_mensual",
        "vmgeaf": "maule_vol_gasto_elec_anual",
        "vmgrtf": "maule_vol_gasto_riego_temp",
        "vmgoef": "maule_vol_reserva_ord_elec",
        "vmgorf": "maule_vol_reserva_ord_riego",
        "vmdcef": "maule_vol_compensacion_elec",
    }

    def _plp_convenio_csv(self, filename):
        """Load a PLP convenio CSV (plplajam.csv / plpmaule.csv).

        Format quirks handled: fixed-width F9.3 fields overflow to
        ``*********`` for values >= 1e6 (coerced to NaN); trailing
        comma yields an unnamed empty column; names carry padding.
        """
        for base in (os.environ.get("PLP_OUT_DIR"), str(_PLP_2Y)):
            if not base:
                continue
            for suffix in ("", ".xz"):
                path = Path(base) / (filename + suffix)
                if path.exists():
                    df = pd.read_csv(path, skipinitialspace=True)
                    df.columns = [c.strip() for c in df.columns]
                    for col in df.columns[9:]:
                        df[col] = pd.to_numeric(df[col], errors="coerce")
                    return df
        return None

    def _compare_states(self, solved, plp_df, mapping, report_name):
        """Unit-free convenio state comparison.

        PLP writes the eta-level states as VOLUME PER STAGE
        (state x etadur x VolScale — genpdlajam.f:986-999), so the
        raw numbers oscillate with stage duration and their unit
        bridge to hm3 is PLP-internal.  The comparison is therefore
        SHAPE-based (the user's "similar, not number-by-number"):

        * convert PLP to rates (divide by stage hours from HorasAcum),
        * normalize both series by their own max,
        * require the normalized trajectories to not anti-correlate
          (inverted semantics detector) whenever both sides move;
        * flat-vs-flat (wet-year unused buckets) counts as agreement.
        """
        output_dir, planning = solved
        hidro = [
            h for h in plp_df["Hidro"].astype(str).str.strip().unique() if h != "MEDIA"
        ][0]
        sim = plp_df[plp_df["Hidro"].astype(str).str.strip() == hidro]
        st = sim.sort_values(["Etapa", "Bloque"]).groupby("Etapa").first()
        hours = st["HorasAcum"].diff().shift(-1)
        hours = hours.fillna(hours.median()).clip(lower=1.0)
        vrs = {
            v["name"]: v["uid"]
            for v in planning["system"].get("volume_right_array", [])
        }
        rows = []
        for plp_col, gt_name in mapping.items():
            if plp_col not in st.columns or gt_name not in vrs:
                continue
            gt = _gtopt_solution_series(
                output_dir, planning, "VolumeRight", gt_name, "efin_sol"
            )
            plp_rate = (st[plp_col] / hours).reset_index(drop=True)
            n = min(len(plp_rate), len(gt))
            if n < 6:
                continue
            s_p = pd.Series(plp_rate[:n].to_numpy(), dtype=float).fillna(0.0)
            s_g = pd.Series(gt[:n], dtype=float)
            p_moves = s_p.std() > 1e-6 * max(abs(s_p.max()), 1.0)
            g_moves = s_g.std() > 1e-6 * max(abs(s_g.max()), 1.0)
            if p_moves and g_moves:
                norm_p = s_p / max(abs(s_p.max()), 1e-9)
                norm_g = s_g / max(abs(s_g.max()), 1e-9)
                corr = float(norm_p.corr(norm_g))
                verdict = "corr"
            elif not p_moves and not g_moves:
                corr = float("nan")
                verdict = "both-flat"
            else:
                corr = float("nan")
                verdict = "one-sided-movement"
            rows.append(
                (
                    plp_col,
                    gt_name,
                    verdict,
                    round(corr, 3) if not math.isnan(corr) else "",
                    round(float(s_p.mean()), 3),
                    round(float(s_g.mean()), 3),
                )
            )
        assert rows, "no comparable convenio states found"
        report = pd.DataFrame(
            rows,
            columns=[
                "plp_var",
                "gtopt_volume_right",
                "verdict",
                "norm_corr",
                "plp_mean_rate",
                "gtopt_mean_hm3",
            ],
        )
        report.to_csv(output_dir / report_name, index=False)
        # Correlation is REPORTED, not gated: gtopt tracks each right's
        # REMAINING allowance (a bucket provisioned then drawn down)
        # while several PLP columns (e.g. vdef) hold the PROVISIONED /
        # accumulated volume, so a depleting-vs-filling convention can
        # legitimately anti-correlate without any semantic error.  The
        # real correctness check — that the buckets provision on the
        # same stages — is the separate reset-timing test.  Here we
        # only require that at least one pair moves comparably (both
        # sides are actually exercising the machinery).
        moving = [v for _, _, v, _, _, _ in rows if v in ("corr", "one-sided-movement")]
        assert moving, (
            "no convenio state moved on either side — the agreement "
            f"machinery is inert:\n{report}"
        )

    @staticmethod
    def _plp_reset_stages(plp_df, marker):
        """1-based Etapa numbers whose TipoEtaGM equals `marker`."""
        st = plp_df.sort_values(["Etapa", "Bloque"]).groupby("Etapa").first()
        return [int(i) for i in st.index[st["TipoEtaGM"] == marker]]

    def test_laja_reset_timing_matches_plp(self, solved):
        """PLP stamps INICIOTEMP (TipoEtaGM=3) / INICIOANTIC (=4) on
        its stages; gtopt's reset months (december/september) must
        select the same stages — unit-free semantic alignment."""
        plp_df = self._plp_convenio_csv("plplajam.csv")
        if plp_df is None or "TipoEtaGM" not in plp_df.columns:
            pytest.skip("plplajam.csv not available")
        _, planning = solved
        months = _stage_months(planning)
        n = len(months)
        plp_temp = [e for e in self._plp_reset_stages(plp_df, 3) if e <= n]
        plp_antic = [e for e in self._plp_reset_stages(plp_df, 4) if e <= n]
        gt_temp = [
            i + 1
            for i in range(n)
            if months[i] == "december" and (i == 0 or months[i - 1] != "december")
        ]
        gt_antic = [
            i + 1
            for i in range(n)
            if months[i] == "september" and (i == 0 or months[i - 1] != "september")
        ]
        assert plp_temp == gt_temp, (
            f"INICIOTEMP stages diverge: PLP {plp_temp} vs gtopt {gt_temp}"
        )
        assert plp_antic == gt_antic, (
            f"INICIOANTIC stages diverge: PLP {plp_antic} vs gtopt {gt_antic}"
        )

    def test_laja_state_variables_vs_plp(self, solved):
        """Direct convenio state comparison: PLP's vdrf/vdef/vdmf/vgaf
        bucket volumes (plplajam.csv) vs gtopt's laja_vol_* efin.
        Skipped until a PLP run's plplajam.csv is placed in the case
        dir (or PLP_OUT_DIR)."""
        plp_df = self._plp_convenio_csv("plplajam.csv")
        if plp_df is None:
            pytest.skip(
                "plplajam.csv not available — run PLP on the case with "
                "the Laja convenio active (or set PLP_OUT_DIR)"
            )
        self._compare_states(
            solved, plp_df, self._LAJA_STATES, "plp_gtopt_laja_states.csv"
        )

    def test_maule_state_variables_vs_plp(self, solved):
        """Direct convenio state comparison: PLP's vmg*/vmdcef bucket
        volumes (plpmaule.csv) vs gtopt's maule_vol_* efin."""
        plp_df = self._plp_convenio_csv("plpmaule.csv")
        if plp_df is None:
            pytest.skip(
                "plpmaule.csv not available — run PLP on the case with "
                "the Maule convenio active (or set PLP_OUT_DIR)"
            )
        self._compare_states(
            solved, plp_df, self._MAULE_STATES, "plp_gtopt_maule_states.csv"
        )

    def test_lmaule_extraction_totals(self, plp_emb, solved):
        """Laguna del Maule: the gasto machinery bounds both models'
        extraction — totals must land in the same band."""
        output_dir, planning = solved
        plp = _plp_reservoir(plp_emb, "LMAULE")
        gt = _gtopt_solution_series(
            output_dir, planning, "Waterway", "LMAULE_gen_1_2", "flow_sol"
        )
        counts = _stage_block_counts(planning)
        plp_stage = _plp_stage_means(plp["EmbQgen"], counts)
        n = min(len(gt), len(plp_stage))
        assert n >= 12
        plp_total = float(pd.Series(plp_stage[:n]).sum())
        gt_total = float(pd.Series(gt[:n]).sum())
        ratio = gt_total / max(plp_total, 1.0)
        assert 0.3 <= ratio <= 3.0, (
            f"LMAULE extraction ratio {ratio:.2f} "
            f"(gtopt {gt_total:.0f} vs PLP {plp_total:.0f})"
        )
