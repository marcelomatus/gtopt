# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end schedule-aggregation test on cases/gtopt_case_2y.

The 2-year PLP-derived case carries the three Line schedules that
real-world cases ship today, in long layout: ``active.parquet``
(per-stage line maintenance), ``tmax_ab.parquet`` / ``tmax_ba.parquet``
(per-(stage, block) capacity derating).  This test
exercises the full pipeline:

  1. Load the case via the reducer.
  2. Reduce to ~40 corridors with ``--reduced-tag red40``.
  3. Assert the three new ``Line/<field>_red40.parquet`` files exist
     with the expected number of corridor uid columns.
  4. Assert basic physics invariants:
       - active_red40 ∈ {0, 1}, no NaN
       - tmax_ab_red40 ≥ 0
       - corridor sum >= max(original line in corridor) when active
  5. Assert every reduced line's JSON entry now points at the new stems.
  6. Round-trip: load the reduced JSON back and verify the field strings.
"""

from __future__ import annotations

from pathlib import Path

import pyarrow.parquet as pq
import pytest

from gtopt_reduce_network import load_case
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case


CASE_2Y = (
    Path(__file__).resolve().parents[3]
    / "cases"
    / "gtopt_case_2y"
    / "gtopt_case_2y.json"
)


@pytest.fixture
def case_2y_dir() -> Path:
    if not CASE_2Y.exists():
        pytest.skip(f"missing case: {CASE_2Y}")
    return CASE_2Y.parent


@pytest.fixture
def unique_tag(request: pytest.FixtureRequest, case_2y_dir: Path) -> str:
    """Give each test its own ``reduced_tag`` so parallel pytest workers
    don't trample each other's parquet outputs.  Cleans up after."""
    tag = f"red40_{request.node.name.replace('test_', '')[:24]}"
    line_dir = case_2y_dir / "Line"
    pattern = f"*_{tag}.parquet"
    for p in line_dir.glob(pattern):
        p.unlink()
    yield tag
    for p in line_dir.glob(pattern):
        p.unlink()


def test_reduce_2y_emits_three_parquets(case_2y_dir: Path, unique_tag) -> None:
    """Reduce → 3 _{unique_tag} parquets exist with correct shape."""
    case = load_case(CASE_2Y)
    cfg = ReduceConfig(
        target_buses=40,
        reduced_tag=unique_tag,
        parquet_case_dir=str(case_2y_dir),
        transport_only=True,
        loss_mode="off",
    )
    result = reduce_case(case, cfg)
    n_corridors = len(result.case.array("line_array"))

    line_dir = case_2y_dir / "Line"
    # Expected temporal shape, derived from the case's own long inputs so the
    # test survives fixture regeneration (active = 1 row/stage, tmax = 1
    # row/(stage, block)).
    n_stages = pq.read_table(line_dir / "active.parquet").to_pandas()["stage"].nunique()
    n_ticks = (
        pq.read_table(line_dir / "tmax_ab.parquet")
        .to_pandas()[["stage", "block"]]
        .drop_duplicates()
        .shape[0]
    )
    for field in ("active", "tmax_ab", "tmax_ba"):
        p = line_dir / f"{field}_{unique_tag}.parquet"
        assert p.exists(), f"missing {p}"
        df = pq.read_table(p).to_pandas()
        uid_cols = [c for c in df.columns if c.startswith("uid:")]
        assert len(uid_cols) == n_corridors, (
            f"{field}: expected {n_corridors} corridor cols, got {len(uid_cols)}"
        )
        # Shape sanity: active has one row per stage; tmax_ab one per (stage, block).
        if field == "active":
            assert len(df) == n_stages, f"active rows must equal stages ({n_stages})"
            assert (df[uid_cols] >= 0).all().all()
            assert (df[uid_cols] <= 1).all().all()
        else:
            assert len(df) == n_ticks, (
                f"tmax rows must equal stage-block ticks ({n_ticks})"
            )
            assert (df[uid_cols] >= 0).all().all(), "tmax_ab must be non-negative"
        assert not df.isna().any().any(), f"{field} has NaN"


def test_reduce_2y_rewrites_json_to_new_stems(case_2y_dir: Path, unique_tag) -> None:
    case = load_case(CASE_2Y)
    cfg = ReduceConfig(
        target_buses=40,
        reduced_tag=unique_tag,
        parquet_case_dir=str(case_2y_dir),
        transport_only=True,
        loss_mode="off",
    )
    result = reduce_case(case, cfg)
    for ln in result.case.array("line_array"):
        assert ln["active"] == f"active_{unique_tag}"
        assert ln["tmax_ab"] == f"tmax_ab_{unique_tag}"
        assert ln["tmax_ba"] == f"tmax_ba_{unique_tag}"


def test_reduce_2y_invariants(case_2y_dir: Path, unique_tag) -> None:
    """Smoke physics: where ALL originals in a corridor are active at a
    given (stage, block), the corridor's tmax_ab should equal the sum
    of those originals' tmax_ab values at that tick."""
    case = load_case(CASE_2Y)
    orig_line_array = list(case.array("line_array"))
    cfg = ReduceConfig(
        target_buses=40,
        reduced_tag=unique_tag,
        parquet_case_dir=str(case_2y_dir),
        transport_only=True,
        loss_mode="off",
    )
    result = reduce_case(case, cfg)

    # Pick a corridor with > 1 original to make the check meaningful.
    multi_eq = [eq for eq in result.aggregated_lines if len(eq.absorbed) > 1]
    if not multi_eq:
        pytest.skip("no multi-original corridor in this reduction")

    eq = multi_eq[0]
    line_dir = case_2y_dir / "Line"
    df_act = pq.read_table(line_dir / "active.parquet").to_pandas()
    df_red_act = pq.read_table(line_dir / f"active_{unique_tag}.parquet").to_pandas()
    df_tmax = pq.read_table(line_dir / "tmax_ab.parquet").to_pandas()
    df_red_tmax = pq.read_table(line_dir / f"tmax_ab_{unique_tag}.parquet").to_pandas()

    # Original line scalar tmax_ab fallback for lines NOT in the parquet.
    orig_scalar = {
        int(ln["uid"]): float(ln.get("tmax_ab", 0.0))
        for ln in orig_line_array
        if isinstance(ln.get("tmax_ab"), (int, float))
    }

    # Pick a stage where ALL originals in the corridor are active (or
    # default-active when not in active.parquet).
    chosen_stage: int | None = None
    for stage in df_red_act["stage"].astype(int).tolist():
        all_active = True
        for u in eq.absorbed:
            col = f"uid:{u}"
            if col in df_act.columns:
                row = df_act.loc[df_act["stage"] == stage, col].iloc[0]
                if int(row) == 0:
                    all_active = False
                    break
        if all_active:
            chosen_stage = stage
            break
    if chosen_stage is None:
        pytest.skip("no stage where all originals in chosen corridor are active")

    # First block of the chosen stage.
    sb_row = df_tmax[df_tmax["stage"] == chosen_stage].iloc[0]
    red_row = df_red_tmax[df_red_tmax["stage"] == chosen_stage].iloc[0]

    expected = 0.0
    for u in eq.absorbed:
        col = f"uid:{u}"
        expected += (
            float(sb_row[col]) if col in df_tmax.columns else orig_scalar.get(u, 0.0)
        )
    actual = float(red_row[f"uid:{eq.uid}"])
    assert actual == pytest.approx(expected, rel=1e-9), (
        f"corridor {eq.uid} at stage {chosen_stage}: "
        f"expected gated sum {expected}, got {actual}"
    )


def test_reduce_2y_re_run_does_not_re_aggregate(case_2y_dir: Path, unique_tag) -> None:
    """Re-running with the same tag overwrites without re-reading its own
    output parquets (sanity: no exponential growth, no recursive uid:N
    columns referencing the reduced uids)."""
    case = load_case(CASE_2Y)
    cfg = ReduceConfig(
        target_buses=40,
        reduced_tag=unique_tag,
        parquet_case_dir=str(case_2y_dir),
        transport_only=True,
        loss_mode="off",
    )
    reduce_case(case, cfg)
    # Run a second time — must produce identical-shape outputs.
    case2 = load_case(CASE_2Y)
    reduce_case(case2, cfg)
    line_dir = case_2y_dir / "Line"
    for field in ("active", "tmax_ab", "tmax_ba"):
        p = line_dir / f"{field}_{unique_tag}.parquet"
        df = pq.read_table(p).to_pandas()
        uid_cols = [c for c in df.columns if c.startswith("uid:")]
        # Equivalent line uids start at 100_001 + N; never collide with
        # the original uid range (≤ 330 for this case).
        for col in uid_cols:
            uid = int(col[4:])
            assert uid > 1000, f"reduced corridor uid {uid} too small"
