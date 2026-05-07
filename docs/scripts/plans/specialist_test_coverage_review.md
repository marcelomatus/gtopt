# Specialist Test Coverage Review
# `gtopt_marginal_units` (Phase 1) + `cen2gtopt` (Phase 2)

> Audit date: 2026-05-05. Plan source:
> `docs/scripts/gtopt_marginal_units_plan.md`.
> No implementation exists yet — this audit evaluates the **planned**
> test suite against the algorithm specification.

---

## P0 — Blocking gaps (must land before v1 release)

### P0-1. §4.4 rule table — row 4 (`hydro_marginal`) not covered by `test_classify.py` as described

**Plan ref**: §4.4, §7.1 `test_classify.py`.

The plan description says the file covers "all 8 statuses", but only lists
`pmin=0`, `pmin>0`, segment breaks, and demand-fail cap as explicit cases.
Row 4 of the table requires `kind ∈ {hydro, battery}` AND dispatch strictly
interior; neither the kind dimension nor the distinction between
`hydro_marginal` (interior + hydro/battery kind) and `marginal` (interior +
thermal kind) is enumerated in the plan's test description.

**Why critical**: a thermal unit dispatched in the interior at `mc ≈ λ` and a
hydro unit dispatched at the same point must reach *different* status codes.
If `classify()` tests this case with any `kind=thermal` fixture and uses it to
cover the hydro branch, the rule-priority bug (hydro check before mc-comparison
check) would be silently missed.

**Proposed test spec**:

```python
def test_classify_hydro_interior_takes_priority_over_marginal():
    """Row 4 fires before row 5: kind=hydro + interior → hydro_marginal
    even when mc ≈ λ (which would match row 5 for a thermal unit)."""
    assert classify(disp=50, pmin=0, pmax=100,
                    mc=30.0, lmp=30.0, kind="hydro") == "hydro_marginal"
    assert classify(disp=50, pmin=0, pmax=100,
                    mc=30.0, lmp=30.0, kind="thermal") == "marginal"
    assert classify(disp=50, pmin=0, pmax=100,
                    mc=30.0, lmp=30.0, kind="battery") == "hydro_marginal"
```

**Criticality**: CRITICAL — wrong classification propagates directly into the
`is_marginal` flag and the zone marginal-unit list.

---

### P0-2. §4.4 rule table — row 7 (`extramarginal_interior`) not explicitly tested

**Plan ref**: §4.4, §7.1 `test_classify.py`.

The plan marks this row with "should not occur — flagged as LP-degeneracy /
segment-mismatch". Because it *can* occur (particularly with time-series gcost
or when a segment edge is misread), and because its output is a warning flag
rather than an error exit, it is easy to accidentally route to `inframarginal`
or `marginal` instead.

**Proposed test spec**:

```python
def test_classify_extramarginal_interior_is_flagged():
    """Interior dispatch with mc > λ + tol_price produces
    extramarginal_interior, never marginal or inframarginal."""
    result = classify(disp=50, pmin=0, pmax=100,
                      mc=80.0, lmp=30.0, kind="thermal")
    assert result == "extramarginal_interior"
```

**Criticality**: CRITICAL — mis-routing this case to `marginal` would corrupt
the zone marginal-unit list and the audit columns.

---

### P0-3. §4.5 — `forced_pmin_marginal` promotion path not covered

**Plan ref**: §4.5 (pick marginal units per zone), §7.1 `test_classify.py`.

The plan does not name a test that exercises the "no interior candidates →
fall back to forced_pmin_marginal" branch. This branch fires when every unit in
a zone is pinned at a bound, but the zone load equals the sum of forced-pmin
units' pmin within `tol_price`. Getting this wrong causes the zone to emit
`__unattributed__` (exit code 2) instead of a valid attribution.

**Proposed test spec** (in `test_classify.py` or a new
`test_zone_picker.py`):

```python
def test_pick_marginal_forced_pmin_fallback():
    """Zone with no interior units but load = sum(pmin) → forced_pmin_marginal.
    Zone with load > sum(pmax) → __demand_fail__."""
    zone_gens = [
        Gen(uid=1, pmin=40, pmax=100, disp=40, mc=20, kind="thermal"),
        Gen(uid=2, pmin=60, pmax=150, disp=60, mc=25, kind="thermal"),
    ]
    zone_load = 100.0  # == pmin_1 + pmin_2
    zone_lmp = 25.0
    result = pick_marginal(zone_gens, zone_load, zone_lmp,
                           demand_fail_cost=1000.0)
    assert result == [Attribution(uid=1, status="forced_pmin_marginal"),
                      Attribution(uid=2, status="forced_pmin_marginal")]
```

**Criticality**: CRITICAL — all-pmin-pinned zones are common in hydro-heavy
systems (technical minimum dispatch) and in must-run scenarios; wrong
attribution here gives wrong prices in the output artifact.

---

### P0-4. §4.7 R3 — `demand_fail_cost` clamp not tested

**Plan ref**: §4.7 step R3 last bullet, §6 item 13, §7.1 `test_reconstruct.py`.

The plan's description of `test_reconstruct.py` covers three scenarios
(normal, saturated line → 2 zones, load > Σ pmax). It does **not** enumerate
a case where R3 produces `λ_z > demand_fail_cost` (the "declared MC above
rationing cap" data-error case described in §6 item 13). This clamp is a
critical correctness invariant: if the clamp is absent, the output artifact
can contain prices above `demand_fail_cost`, which is undefined behaviour in
the downstream `compare` mode.

**Proposed test spec** (in `test_reconstruct.py`):

```python
def test_r3_clamps_lmp_at_demand_fail_cost():
    """When the only interior unit has declared_MC > demand_fail_cost,
    R3 must clamp λ_z to demand_fail_cost and set confidence=fallback."""
    result = reconstruct_zone(
        gens=[Gen(uid=1, pmin=0, pmax=100, disp=50,
                  declared_MC=1500.0, kind="thermal")],
        demand=50.0, demand_fail_cost=1000.0,
    )
    assert result.lmp == pytest.approx(1000.0)
    assert result.confidence == "fallback"
    assert result.offending_gen_uid == 1
```

**Criticality**: CRITICAL — §6 item 13 is an explicit edge case.

---

### P0-5. §4.3 — zone-LMP spread warning (§6 item 7) not in `test_zones.py`

**Plan ref**: §4.3 last paragraph, §6 item 7, §7.1 `test_zones.py`.

The plan's `test_zones.py` description tests partition formation but does not
test the "max(λ_b) − min(λ_b) > tol_lmp inside a zone → degenerate=True"
warning path. This path is the primary correctness signal when a saturated line
is missed.

**Proposed test spec** (in `test_zones.py`):

```python
def test_intra_zone_lmp_spread_triggers_degenerate():
    """When buses in the same zone have LMPs spread > tol_lmp,
    the cell is flagged degenerate=True and the suspect lines are logged."""
    # 2 buses in zone 0, lmp[0]=30, lmp[1]=35 → spread = 5 >> tol_lmp
    cell = build_cell(zones={0: [bus_a, bus_b]},
                      lmps={bus_a: 30.0, bus_b: 35.0},
                      tol_lmp=0.01)
    assert cell.degenerate is True
    assert "intra_zone_spread" in cell.reason
```

**Criticality**: CRITICAL — this is the script's self-check invariant.

---

### P0-6. Exit-code branches — only code 3 is tested in `test_cli_smoke.py`

**Plan ref**: §5 exit codes, §7.1 `test_cli_smoke.py`.

The plan's smoke-test description says "runs end-to-end on a 1-cell synthetic
case" (exit 0) and "missing flags exit 3". Exit code 2 (unattributed cells
emitted) is not mentioned. This means the `__unattributed__` escape hatch can
be tested at the unit level (`test_zones.py`, `test_classify.py`) but the
end-to-end exit-code signal is never verified.

**Proposed test spec** (in `test_cli_smoke.py`):

```python
def test_exit_code_2_on_unattributed_cell(tmp_path):
    """When a synthetic 1-cell case produces __unattributed__ output,
    the script exits with code 2 and still writes the artifact."""
    # Construct case: single zone, no interior unit, load != sum(pmin)
    feed = build_minimal_feed(tmp_path, dispatch_all_at_pmax=True,
                               zone_load_exceeds_pmax=False)
    result = subprocess.run(
        ["gtopt-marginal-units", "--input", "feed",
         "--feed", str(feed), "--mode", "real"],
        capture_output=True,
    )
    assert result.returncode == 2
    assert (tmp_path / "marginal_units.parquet").exists()
```

**Criticality**: CRITICAL — exit codes are the contract between this script and
any pipeline that calls it; exit 2 must not be confused with exit 0.

---

## P1 — Important gaps (should land before v1 release)

### P1-1. §4.4 + §7.1 — `profile_dispatched` rule not separated from `off` rule

**Plan ref**: §4.4 row 8, §6 item 4, §7.1 `test_classify.py`.

Row 8 fires on `disp ≥ ε AND kind == profile`. The `off` rule (row 1) fires
on `disp ≤ ε AND pmin ≤ ε`. A profile unit at `disp=0` matches row 1 (`off`)
before row 8 (`profile_dispatched`). The plan does not test the zero-dispatch
profile case. More importantly, §6 item 4 introduces a new status
`__renewable_curtailment__` that the rule table in §4.4 does not contain — it
is emitted from §4.5 (zone picker) when `λ_z = 0` and profile units are
curtailed. Neither `test_classify.py` nor `test_zones.py` is described as
testing this path.

**Proposed test spec** (new `SUBCASE` in `test_classify.py` + `test_zones.py`):

```python
def test_classify_zero_disp_profile_is_off():
    assert classify(disp=0, pmin=0, pmax=100, mc=0, lmp=0,
                    kind="profile") == "off"

def test_zone_picker_renewable_curtailment_at_zero_lmp():
    """Zone LMP=0 + profile units curtailed → __renewable_curtailment__."""
    result = pick_marginal(
        zone_gens=[Gen(uid=1, pmin=0, pmax=80, disp=60,
                       mc=0, kind="profile", curtailed=20)],
        zone_load=60.0, zone_lmp=0.0, demand_fail_cost=1000.0,
    )
    assert result[0].status == "__renewable_curtailment__"
```

**Criticality**: IMPORTANT — renewable curtailment is a common SEN operating
state; misclassifying it as `__unattributed__` produces spurious exit code 2
and confuses the report.

---

### P1-2. §4.7 R1 — PTDF fallback path not tested in `test_reconstruct.py`

**Plan ref**: §4.7 step R1, §7.1 `test_reconstruct.py`.

The plan's description of `test_reconstruct.py` has the saturated-line case
"forced via flow override" — meaning real flows are provided. The PTDF
estimation path (when `Line/flowp_sol` is absent) is a separate code branch
that builds PTDF from the topology and solves `PTDF · netload`. This path
is exercised only if the test explicitly omits the flow column.

**Proposed test spec**:

```python
def test_r1_ptdf_fallback_matches_exact_flow():
    """With identical injections, PTDF-estimated flows must agree with
    the exact flows to within tol_flow on the 3-bus ring fixture."""
    # Run reconstruction twice: once with flows, once without
    result_exact = reconstruct(feed=feed_with_flows, zone_mode="congestion")
    result_ptdf  = reconstruct(feed=feed_without_flows, zone_mode="congestion")
    assert result_exact.zones == result_ptdf.zones
```

**Criticality**: IMPORTANT — the PTDF path is the only path available in
real-operation mode; an untested PTDF build is a silent correctness risk.

---

### P1-3. §7.2 — no benchmark exercises hydro water-value or battery

**Plan ref**: §7.2 integration tests.

The five listed benchmarks (`ieee_4b_ori`, `ieee_9b_ori`, `ieee_9b`,
`ieee_14b`, `c0`) cover:
- (a) multi-stage status change: `c0` (capacity expansion, stage-varying
  availability) — covered.
- (b) hydro/water-value path: **not covered** — none of the five IEEE cases
  has a `Reservoir` or `Battery` in the planning JSON.
- (c) batteries (charge/discharge): **not covered**.
- (d) renewable curtailment / zero LMP: `ieee_9b` has solar, but the test
  only asserts `profile_dispatched`; zero-LMP / curtailment condition is not
  asserted.
- (e) demand-fail / rationing: **not covered** — no existing IEEE case forces
  load shedding.

**Proposed additions**:
- Extend `c0` (the only multi-stage case) with a `Reservoir` object to hit
  the hydro/battery classification branch in integration, OR add a new minimal
  3-bus synthetic JSON that has one hydro and one thermal (runnable in <1s).
- Add a `--single-bus` synthetic case where `demand_fail_cost` is saturated
  (load > Σ pmax) to cover exit 2 + `__demand_fail__` attribution end-to-end.

---

### P1-4. §7.3 Hypothesis — three missing property strategies

**Plan ref**: §7.3.

The three strategies listed (`classify` returns exactly one status; zone
partition is a partition; `is_marginal` is permutation-invariant) are
structural. Three additional strategies address numerical and topological
correctness:

**Strategy A — R3 reconstruction is monotone in declared cost**:

```python
@given(
    st.lists(st.floats(min_value=0, max_value=500), min_size=1, max_size=10),
    st.floats(min_value=0, max_value=1000),  # zone_load
)
def test_r3_lmp_is_max_interior_mc(mcs, load):
    """λ_z from R3 equals the maximum MC of interior units.
    After sorting, removing the highest-MC unit raises λ_z to the next — never lower."""
```

**Strategy B — zone partition is stable under line-uid permutation**:

```python
@given(st.permutations(line_uids))
def test_zone_partition_invariant_to_line_ordering(perm):
    """Connected-components result must not depend on the order lines
    are added to the graph."""
```

**Strategy C — reconstructed λ_z is always in `[0, demand_fail_cost]`**:

```python
@given(random_topology_and_dispatch())
def test_reconstructed_lmp_bounded(topology_dispatch):
    topology, dispatch, demand_fail_cost = topology_dispatch
    result = reconstruct_zone(topology, dispatch, demand_fail_cost)
    assert 0.0 <= result.lmp <= demand_fail_cost
```

**Strategy D — `classify` is total (every valid input maps to exactly one row)**:

The existing strategy covers random triples but the plan does not specify
that `kind` is sampled from the full set `{thermal, hydro, battery, profile}`.
Add `kind` to the strategy to ensure `hydro_marginal` and `profile_dispatched`
branches are reached under fuzz.

**Strategy E — `tol_price` boundary is symmetric**:

```python
@given(st.floats(min_value=1e-6, max_value=1.0))  # tol_price
def test_classify_tol_price_symmetry(tol):
    """mc = λ + tol → marginal; mc = λ + tol + ε → not marginal.
    The boundary must be consistent from both sides."""
```

---

### P1-5. §9.6 `cen2gtopt` — DST hour-25/hour-23 test not exercised

**Plan ref**: §9.6 `tests/test_csv_parsers.py`.

The plan lists "hour-25 (DST historical)" in the description but DST handling
was discontinued in Chile/Continental in 2015. The real concern is:

1. Pre-2015 historical files *do* have 23-hour and 25-hour days.
2. Post-2015 files use a 24-hour flat calendar but legacy exporters still
   occasionally mis-label columns.

The test should use a synthetic CSV fixture with explicit `hora 25` / `hora 0`
ambiguity to confirm `Chile/Continental` localisation via `zoneinfo` resolves
correctly and does not produce `NaT` or a duplicate UTC hour.

**Proposed test spec** (in `test_csv_parsers.py`):

```python
def test_dst_hour25_historical_resolves_correctly():
    """A CEN historical CSV with 25 rows for the spring-forward day
    must produce exactly 25 UTC timestamps, none NaT."""
    csv = load_fixture("cen_sample/2010-10-10_cmgreal_dst25.csv")
    df = parse_cmgreal(csv, source_tz="Chile/Continental")
    assert df["datetime_utc"].notna().all()
    assert len(df) == 25
    assert df["datetime_utc"].is_monotonic_increasing
```

**Criticality**: IMPORTANT — a NaT row would produce a cell_key=NaT entry in
the canonical feed that `gtopt_marginal_units` would flag as
`degenerate=True, reason="missing_realised_data"` — wrong attribution for an
entire hour.

---

### P1-6. §9.6 `cen2gtopt` — idempotency not tested (§9.5.1 requirement)

**Plan ref**: §9.5.1 last bullet, §9.6.

The plan states "invoking it twice with the same arguments produces
byte-identical output (modulo `manifest.json` fetched_at timestamp)". None of
the five listed test files asserts this. The `test_writer.py` round-trip only
checks that the feed is readable after write; it does not run the CLI twice
and diff the outputs.

**Proposed test spec** (in `test_cli_smoke.py` or a new
`test_idempotency.py`):

```python
def test_cen2gtopt_idempotency(tmp_path, cen_csv_fixtures):
    """Running cen2gtopt twice on the same offline fixtures produces
    byte-identical Parquet files (manifest fetched_at excluded)."""
    out1 = tmp_path / "feed1.parquet"
    out2 = tmp_path / "feed2.parquet"
    run_cen2gtopt(fixtures=cen_csv_fixtures, out=out1)
    run_cen2gtopt(fixtures=cen_csv_fixtures, out=out2)
    assert parquet_equal_except_manifest(out1, out2)
```

**Criticality**: IMPORTANT — idempotency is a stated acceptance criterion
(§11 Phase-2 acceptance condition 2).

---

### P1-7. §9.6 — CI guard `tests/test_no_scheduling.py` not described in the test table

**Plan ref**: §11 Phase-2 acceptance condition 3, §9.6.

The acceptance criterion says "CI verifies this by grep-ing the package for
`cron|systemd|schedule|daemon`". This test file is mentioned in §12 ("CI
guard") but is absent from the §9.6 test-file table. Without it, a future
agent can silently add scheduling code and only discover the CI failure at
release time.

**Proposed test spec** (new file `tests/test_no_scheduling.py`):

```python
import subprocess, pathlib

def test_no_scheduling_code_in_package():
    pkg = pathlib.Path(__file__).parent.parent  # scripts/cen2gtopt/
    result = subprocess.run(
        ["grep", "-r", "--include=*.py", "-l",
         r"cron\|systemd\|schedule\|daemon", str(pkg)],
        capture_output=True, text=True,
    )
    assert result.stdout.strip() == "", (
        f"Scheduling code found: {result.stdout}")
```

**Criticality**: IMPORTANT — directly enforces the §9.1.1 scope constraint.

---

### P1-8. §9.6 — exit code 4 (network error + `.partial` file) not tested

**Plan ref**: §9.5.2 exit code 4.

`test_cli_smoke.py` is described as testing exit 0 (`--manifest-only`) and
exit 3 (`--from > --to`, missing API key). Exit code 4 is absent. The `.partial`
file convention is the only way a caller can inspect what was fetched before
the network failure.

**Proposed test spec** (in `test_cli_smoke.py`):

```python
def test_exit_code_4_on_network_error_writes_partial(tmp_path, monkeypatch):
    """Simulate a network timeout after the first page is fetched.
    Script must exit 4 and write <out>.partial."""
    monkeypatch.setattr("cen2gtopt._http.fetch_page",
                        fail_after_first_page)
    out = tmp_path / "feed.parquet"
    result = subprocess.run(
        ["cen2gtopt", "--from", "2026-01-01", "--to", "2026-01-07",
         "--out", str(out), "--source", "csv"],
        capture_output=True,
    )
    assert result.returncode == 4
    assert (tmp_path / "feed.parquet.partial").exists()
    assert not out.exists()  # incomplete output never written to final path
```

**Criticality**: IMPORTANT — callers rely on exit code 4 to distinguish
"network failure (retry)" from "argument error (fix the call)".

---

## P2 — Plumbing gaps (nice-to-have before v1, required by v1.1)

### P2-1. §3.3 — Phase-0 gold fixture round-trip is described but not mapped to a test file

The plan mentions the gold fixture (3 buses, 3 units, 24 hours) as a Phase-0
deliverable (P0.3) and says both Phase-1 and Phase-2 round-trip tests load it.
There is no test file explicitly named for `_canonical_feed/tests/`. The §7.1
`test_io_roundtrip.py` is described as testing the three shard layouts but
does not call out the gold fixture by name. Add an explicit
`test_io_roundtrip.py` SUBCASE that reads `tests/data/gold_feed.parquet` and
asserts byte-equality after re-serialisation — this is the schema-lock-in
test described in §9.6 `test_writer.py`.

### P2-2. §4.2 — `scale_objective` sanity-check assertion not tested in unit suite

The plan states (§4.2): "The script asserts `λ_z ≤ 1.05 · demand_fail_cost`;
values above mean the user forgot `--scale_objective` consistency and the
script aborts." There is no test file in §7.1 that exercises this assertion
path. A SUBCASE in `test_cli_smoke.py` should pass a synthetic output where
`Bus/balance_dual` values are 1000× the expected LMP and verify exit 3 with
a "LMP exceeds 1.05 × demand_fail_cost" message.

### P2-3. §3.1 — time-series `gcost` (`OptTRealFieldSched`) silently falls back; no test

The plan says "v1 only handles the scalar form; v1.1 will resolve the
scheduled form". There should be at least a `pytest.warns` or log-level
assertion in `test_segments.py` that confirms the script emits a one-time
warning (not a silent skip) when it encounters a scheduled gcost reference.
Without this, the warning-path test coverage is zero on a real-world case
where most generators have time-varying variable costs.

### P2-4. §4.1 — `active=false` per-stage generator / line filtering (§6 item 8) not tested

The plan mentions this as edge case 8 ("a line that is `active=false` in stage
p is dropped from the static lookup for that stage"). Neither `test_zones.py`
nor `test_io_roundtrip.py` is described as testing multi-stage topology
changes. An `ieee_14b` fixture with one line disabled in stage 2 would cover
this cheaply.

---

## 5. Integration-test state-space coverage (§7.2 analysis)

| Axis | Benchmark | Status |
|---|---|---|
| (a) Multi-stage status change | `c0` | Covered |
| (b) Hydro water-value | None | **MISSING** (P1-3 above) |
| (c) Battery charge/discharge | None | **MISSING** (P1-3 above) |
| (d) Renewable curtailment / zero LMP | `ieee_9b` (partial) | Partially covered — `profile_dispatched` asserted, curtailment/zero-LMP not asserted |
| (e) Demand-fail / rationing | None | **MISSING** (P1-3 above) |
| (f) Congestion → multi-zone | `ieee_14b` | Covered |
| (g) Single-bus mode | Implicit in `ieee_9b_ori` + `--single-bus` | Covered only if `use_single_bus` is set in that case |
| (h) Physical-island topology | None | Not in v1 scope (§10) but `--zone-mode physical` has no integration test |

---

## 6. CI integration check

`scripts/conftest.py` sets `OMP_NUM_THREADS` and `MKL_NUM_THREADS` only. It
does not add the new packages to `sys.path` — pytest-xdist workers will find
them only if `scripts/gtopt_marginal_units/` and `scripts/cen2gtopt/` each
have a `pyproject.toml` with `packages = [...]` and are installed editable
(`pip install -e`). Verify the CI job calls
`pip install -e scripts/gtopt_marginal_units scripts/cen2gtopt` before
running `pytest -n auto`.

Opt-in tests (§7.4, §7.5, §9.6 integration) must be decorated with both a
custom marker **and** an `importorskip`-style guard:

```python
pytestmark = pytest.mark.skipif(
    not os.getenv("GTOPT_RUN_CEN_BACKTEST"),
    reason="set GTOPT_RUN_CEN_BACKTEST=1 to run",
)
```

Without the env guard, `pytest -n auto` will attempt network access in CI
workers and fail non-deterministically.

---

## 7. Coverage measurement proposal

```bash
pytest scripts/gtopt_marginal_units/tests \
       scripts/cen2gtopt/tests \
       scripts/_canonical_feed/tests \
       --cov=gtopt_marginal_units \
       --cov=cen2gtopt \
       --cov=_canonical_feed \
       --cov-report=term-missing \
       --cov-fail-under=85 \
       -n auto
```

Per-module critical-path targets (100% required, enforced via
`--cov-fail-under` on individual module reports):

| Module | Minimum |
|---|---|
| `gtopt_marginal_units/_classify.py` | 100% |
| `gtopt_marginal_units/_zones.py` | 100% |
| `gtopt_marginal_units/_reconstruct.py` | 100% |
| `cen2gtopt/_csv_parsers.py` | 100% |

Enforce per-module via:
```bash
pytest --cov=gtopt_marginal_units._classify \
       --cov-fail-under=100 \
       scripts/gtopt_marginal_units/tests/test_classify.py
```

---

## 8. Open questions → test implications (§13)

| # | Open question | Test needed to validate the chosen answer |
|---|---|---|
| Q1 | Hydro water-value source: read dual vs reconstruct from cuts? | `test_reconstruct.py` SUBCASE: run with `dual_present=False`, assert script warns and sets `confidence=fallback`; run with `dual_present=True`, assert `confidence=lp_dual`. |
| Q2 | Profile curtailed at zero LMP: `profile_curtailed_marginal` or non-marginal? | `test_classify.py` SUBCASE: `kind=profile, disp < pmax, lmp=0` → assert exact status string matches the decision; the test is the spec. |
| Q3 | `--cen-format` exporter: needed? | If implemented: round-trip test in `test_io_roundtrip.py`; if not: note in `test_cli_smoke.py` that the flag is absent and exits 3 with "not implemented". |
| Q4 | Cache location: `~/.cache/gtopt/cen/` vs project-relative? | `test_cli_smoke.py` SUBCASE: `--cache /tmp/pytest-cen-cache` writes to the given dir; default writes to `~/.cache/gtopt/cen/`; `--no-cache` with a populated cache still issues a network fetch (assert HTTP called via mock). |

---

## Verdict

`COVERAGE VERDICT: BLOCKING`

The planned test suite has five critical gaps (P0-1 through P0-6): the
`hydro_marginal` priority rule, `extramarginal_interior` flagging,
`forced_pmin_marginal` zone-picker fallback, R3 demand-fail-cost clamp, and
exit-code 2 end-to-end signal are each named in the algorithm but absent from
the described tests. None of these can be considered covered by the currently
described test fixtures; each one, if wrong, would produce silently corrupt
output or wrong exit codes in production use.
