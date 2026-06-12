# Phase-3 Agent F — End-to-End CEN Back-Test Integration Plan

> Status: planning only (no production code in this PR).
> Owner: Agent F (Phase 3, serialised after P1 + P2 ship v1).
> Master plan: [`gtopt_marginal_units_plan.md`](./../gtopt_marginal_units_plan.md)
> §1.5, §4.7, §7.5, §11 (P3.A/P3.B/P3.C), §12.1 Agent F brief.
> Memory feedback in scope: `feedback_proactive_tests`,
> `feedback_minimize_builds`, `feedback_no_parallel_builds`.

This plan describes the integration glue that joins
`scripts/cen2gtopt/` (Phase 2) and `scripts/gtopt_marginal_units/`
(Phase 1) end-to-end through the canonical operation feed (master
plan §3.3.3). Agent F writes **no new algorithms** — only orchestration,
fixtures, the back-test, the Markdown diagnostic report, the workflow
user-guide, and a small fixture-refresh tool.

---

## 1. One-week SEN snapshot fixture

### 1.1 Location and layout

```
scripts/gtopt_marginal_units/tests/data/cen_sample/
  manifest.json                          # producer + provenance; see §1.5
  topology/
    bus.parquet
    generator.parquet
    line.parquet
  cells/
    dispatch.csv.zst                     # Generación Real, hourly
    load.csv.zst                         # Demanda Real, hourly
    lmp.csv.zst                          # Costo Marginal Real (reference)
    flow.csv.zst                         # Flujos Reales (optional, may be NA)
  reference/
    cen_zone_partition.csv.zst           # CEN-published "subsistemas
                                         # desacoplados" hourly partition;
                                         # may be absent — see §3.4 fallback
  README.md                              # how to refresh; pointers to URLs
```

CSV-with-zstd is used for the cell tables (highly repetitive long-form
data, compresses well, diff-friendly in PR review). Topology is small
and benefits from Parquet's column types; we keep it as native Parquet.

### 1.2 Historical week pick

Fixture must encode a **known north-south congestion event** so the
back-test exercises the §4.3 zone partition (not just copperplate).
Candidates in priority order:

1. **2022-04-25 → 2022-05-01** — Kimal-Lo Aguirre HVDC outage; daily
   LMP differentials > 30 USD/MWh between SING-legacy zone and SIC
   core. Ref: CEN, *Reporte Anual Art. 72-15 Año 2022* (analogue to
   the 2023 reference in the master plan).
2. **2023-08-14 → 2023-08-20** — winter peak, Cardones-Polpaico 500 kV
   congestion; multi-hour two-zone operation in
   [`cmgreal.coordinador.cl`][cen-cmgreal] history.
3. **2024-09-09 → 2024-09-15** — spring valley, south-to-north reverse
   flow saturation; sign-flip regression case.

**Default pick: candidate #1** — documented, large differential
(non-trivial ±2 USD/MWh bar), CEN report names the saturated corridor
giving a reference partition for §3. User must confirm (§10).

### 1.3 Size budget

Target: **< 5 MB compressed total** committed in-tree.

| Path | Approx | Notes |
|---|---|---|
| topology/ | ~150 kB | ~700 bus × 300 gen × 1.2k lines |
| cells/dispatch.csv.zst | ~1.5 MB | 300 units × 168 h |
| cells/load.csv.zst | ~0.6 MB | 700 buses × 168 h |
| cells/lmp.csv.zst | ~0.6 MB | reference |
| cells/flow.csv.zst | ~1.2 MB | often absent / smaller |
| reference/ | ~0.1 MB | |

If the blob exceeds 5 MB, trim to a 72-hour window covering the peak
and document in the manifest.

### 1.4 Storage policy

**Default: committed in-tree** — fixture is immutable historical
data, guarantees offline CI reproducibility, fits the project's
binary-blob tolerance (compare `scripts/cases/`).

**Fallback: lazy-pull** if the blob grows past ~10 MB. A pytest
fixture in `scripts/conftest.py` would (a) check the local path,
(b) if absent and `GTOPT_RUN_CEN_BACKTEST=1`, fetch a pinned
GitHub-release tarball whose sha256 lives in `manifest.json`,
(c) if absent and env-var unset, skip silently (§9). Lazy-pull
deferred to v1.1 (§10).

### 1.5 manifest.json shape

Standard `cen2gtopt` manifest (§3.3.3) plus back-test extras:
`producer`, `producer_version`, `schema_version`, `fetched_at`,
`from`/`to`, `source` (`sip|csv|mixed`), `endpoints[]`, `files`
(path → sha256), and the back-test-only fields `event` (e.g.
`"Kimal-Lo Aguirre HVDC outage"`) and `cen_report_ref` (e.g.
`"CEN Reporte Art.72-15 2022 §3.4"`). The back-test additionally
asserts `producer == "cen2gtopt"` and that file checksums match.

---

## 2. End-to-end pipeline test

### 2.1 File and gating

* Path: `scripts/gtopt_marginal_units/tests/integration/test_e2e_cen_backtest.py`
* Markers: `@pytest.mark.integration`, `@pytest.mark.cen_backtest`
* Env-var gate: skip unless `GTOPT_RUN_CEN_BACKTEST=1`
* Skip reasons (never fail — see §9): env-var unset; fixture missing;
  `manifest.json` checksum mismatch. Each skip emits a one-line
  recovery hint (e.g. `tools/refresh_cen_fixture.py --allow-network`).

### 2.2 Pseudocode

```python
FIXTURE = Path(__file__).parent.parent / "data" / "cen_sample"

pytestmark = [
    pytest.mark.integration, pytest.mark.cen_backtest,
    pytest.mark.skipif(os.environ.get("GTOPT_RUN_CEN_BACKTEST") != "1",
                       reason="opt-in; set GTOPT_RUN_CEN_BACKTEST=1"),
]

def test_e2e_cen_backtest(tmp_path):
    _check_fixture(FIXTURE)            # skip on missing / checksum

    # 1. Hermetic cen2gtopt CLI smoke (no network).
    run(["cen2gtopt", "--from", "2022-04-25", "--to", "2022-05-01",
         "--out", str(tmp_path / "stub.parquet"), "--manifest-only"])

    # 2. Reconstruct LMP from the committed feed.
    out_recon = tmp_path / "marginals_recon.parquet"
    run(["gtopt-marginal-units", "--input", "feed",
         "--feed", str(FIXTURE), "--mode", "real-reconstruct",
         "--out", str(out_recon),
         "--report", str(tmp_path / "report_recon.md")])

    # 3. Compare reconstruction against CEN's published number.
    out_cmp = tmp_path / "marginals_compare.parquet"
    report_md = tmp_path / "cen_backtest_report.md"
    run(["gtopt-marginal-units", "--input", "feed",
         "--feed", str(FIXTURE), "--mode", "compare",
         "--against", "real",
         "--out", str(out_cmp), "--report", str(report_md)])

    # 4. Assert §7.5 quality bars (see §3 below).
    df = pd.read_parquet(out_cmp)
    assert_quality_bars(df, FIXTURE / "reference" /
                            "cen_zone_partition.csv.zst")

    # 5. Publish the diagnostic report next to IEEE outputs.
    target = Path("build/integration_test/test_output/"
                  "cen_backtest_report.md")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(report_md.read_text())
```

### 2.3 What the test does *not* do

No network call (the fixture **is** the cen2gtopt output;
`--manifest-only` is a hermetic CLI smoke check). No byte-equality on
the reconstructed LMP — only statistical agreement per §3. No gtopt
simulator invocation; feed-only path.

---

## 3. Quality bars (master plan §7.5 verbatim)

> *"≥ 95 % of `(date, hour, bus)` cells with `|λ_reconstructed −*
> *λ_published_CEN| ≤ 2 USD/MWh; zone partition matches CEN's*
> *subsistemas desacoplados report for the same hour (when published);*
> *every cell with a delta > 5 USD/MWh appears in the topology_mismatch*
> *or must_run_override audit list."*

### 3.1 Bar #1 — distributional agreement

```python
def assert_distribution(df):
    assert "delta_lmp" in df.columns
    finite = df["delta_lmp"].abs().dropna()
    within_2 = (finite <= 2.0).mean()
    assert within_2 >= 0.95, (
        f"Bar 1: only {within_2:.1%} within ±2 USD/MWh (need ≥95%); "
        f"med={finite.median():.2f}, p95={finite.quantile(0.95):.2f}, "
        f"max={finite.max():.2f}")
```

### 3.2 Bar #2 — zone partition

```python
def assert_zone_partition(df, ref_path):
    if not ref_path.exists():               # see §3.4 fallback
        _assert_partition_is_a_partition(df); return
    ref = pd.read_csv(ref_path, compression="zstd")
    joined = df.merge(ref, on=["date", "hour", "bus_uid"], how="inner")
    agree = _partition_agreement_rate(joined, "zone_id", "cen_zone_id")
    assert agree >= 0.98, f"Bar 2: partition agreement {agree:.1%}"
```

`_partition_agreement_rate` averages the Rand index of
`(zone_id, cen_zone_id)` cross-tabs per `(date, hour)`.

### 3.3 Bar #3 — every large delta is explained

```python
def assert_large_deltas_explained(df):
    big = df[df["delta_lmp"].abs() > 5.0]
    if big.empty: return
    flag_re = "topology_mismatch|must_run_override"
    explained = big["audit_flags"].str.contains(flag_re, regex=True,
                                                na=False)
    unexplained = big[~explained]
    assert unexplained.empty, (
        f"Bar 3: {len(unexplained)} cells with |delta|>5 unexplained\n"
        f"{unexplained.head(10)}")
```

Requires Phase-1's `mode=compare` to expose `audit_flags` carrying the
§4.7 step-R5 categories. Agent F must confirm with Agent A before P3.B
starts (open question §10.5).

### 3.4 Fallback when CEN reference partition is unpublished

CEN does not publish the *subsistemas desacoplados* table for every
hour. When `reference/cen_zone_partition.csv.zst` is absent we
**downgrade Bar #2** to a structural sanity check:

* every bus is in exactly one zone (partition property);
* the number of zones is `≥ 1` and `≤ n_buses`;
* if any line in the topology has `|flow|/tmax > 0.95`, then the
  number of zones is `≥ 2` (saturation must induce a split).

Fallback never strengthens the bar — it only verifies the partition is
self-consistent. The test logs a clear `"Bar 2 downgraded to
structural-sanity (no CEN reference)"` line so a human reviewing the
report knows the assertion was relaxed.

---

## 4. Diagnostic report

Phase-1's `--report` already writes a digest. The back-test adds a
higher-level rollup focused on reconstruction-vs-CEN delta, written to
`build/integration_test/test_output/cen_backtest_report.md` (lives
next to existing IEEE integration outputs).

### 4.1 Skeleton

```markdown
# CEN Reconstruction Back-Test — <fixture week>

_Generated: <UTC timestamp>_
_Fixture: scripts/gtopt_marginal_units/tests/data/cen_sample/_
_Event: <manifest.event>_

## 1. Executive summary

| Metric | Value | Bar |
|---|---|---|
| Cells analysed | <N> | — |
| Cells within ±2 USD/MWh | <pct>% | ≥ 95 % |
| Median \|Δλ\| | <x.xx> USD/MWh | — |
| p95 \|Δλ\| | <x.xx> USD/MWh | — |
| Zone-partition agreement | <pct>% | ≥ 98 % (when ref present) |
| Large deltas (>5) explained | <m>/<m> | 100 % |

**Overall verdict**: PASS / DOWNGRADED / FAIL.

## 2. Distribution of `delta_lmp`

ASCII histogram of |Δλ| in 0.5 USD/MWh bins, plus the JSON-dumped
quantile table. (We do not embed PNG plots — keeps the report
diff-friendly in PR review.)

## 3. Top-20 outlier cells

Table with columns `date | hour | bus_name | λ_recon | λ_CEN |
Δλ | audit_flags | likely cause`. The "likely cause" column is
generated by a small explainer that reads the §4.7 step-R5
categories.

## 4. Zone-partition diff (when reference available)

Per-hour rows where the partitions disagree, listing the bus pairs
on which agreement breaks (i.e. buses that share a zone in one and
not the other).

## 5. Must-run overrides observed

Cells where `audit_flags` includes `must_run_override`, with the
forced unit's name and declared MC. Cross-check against §2.3 of the
methodology doc.

## 6. Reproducibility footer

* `cen2gtopt` version: <from manifest>
* `gtopt_marginal_units` version: <from `--version`>
* fixture checksum: <sha256 of manifest.json>
* commands used: <copy-paste block matching §2.2>
```

### 4.2 Generator location

`scripts/gtopt_marginal_units/_report_backtest.py` — small new module
in Agent C's package; Agent F edits via a hand-off PR comment per
§12.2 rule 3 of the master plan.

---

## 5. User-facing workflow doc (`docs/scripts/marginal_units_workflow.md`)

### 5.1 Section outline

The doc has six sections:

1. **Quick start (3 commands)** — `cen2gtopt --from … --to … --out
   feed.parquet --include lmp,flows`; `gtopt-marginal-units --input
   feed --feed feed.parquet --mode real-reconstruct --report
   report.md`; optional `--mode compare --against real`.
2. **When to use which mode** — table mapping inputs to modes:
   simulator dir → `simulated`; CEN feed with LMP → `real`; CEN feed
   without LMP / audit → `real-reconstruct`; both → `compare`.
3. **Reading the report** — verdict (PASS / DOWNGRADED / FAIL); "likely
   cause" heuristics; interpretation of `audit_flags`
   (`topology_mismatch`, `must_run_override`, `merit_order_only`,
   `demand_fail_clamped`) and the `confidence` ladder
   (`published > lp_dual > merit_order > fallback`).
4. **Troubleshooting** — table covering:
   missing `$CEN_API_KEY` (exit 3); CEN network errors (exit 4, retry
   with cache); unmapped-units warning (catalogue rename — rerun
   `--source sip`); time-zone gotchas (pre-2015 SING data → `--source-tz`);
   Bar #1 fails at ~85 % (stale declared-cost catalogue → refresh
   fixture); manifest checksum mismatch (rerun
   `tools/refresh_cen_fixture.py`).
5. **See also / cross-links** — methodology, canonical-feed schema,
   Phase-1 CLI, Phase-2 CLI.

### 5.2 Cross-link insertions

Two reciprocal links: `docs/scripts-guide.md` index entry; and a
"See also: workflow doc" footer in
`docs/methods/marginal_unit_classification.md`.

---

## 6. Fixture maintenance — `tools/refresh_cen_fixture.py`

Manual maintenance script (not CI) for when CEN URLs, the SIP API
schema, or the catalogue change.

### 6.1 Outline

```python
# tools/refresh_cen_fixture.py — gated behind --allow-network.
# Args: --allow-network (required), --from, --to, --source, --dry-run.
# Steps:
#   1. exit if --allow-network not set
#   2. cen2gtopt --from … --to … --out FIXTURE --source … \
#                --include dispatch,demand,lmp,flows
#   3. recompute sha256 of every fixture file → rewrite manifest.json
#   4. warn if total size > 5 MB
#   5. (optional) regenerate reference/cen_zone_partition.csv.zst by
#      scraping CEN's "subsistemas desacoplados" page when available
```

### 6.2 Failure modes

CEN endpoint changed → `cen2gtopt` exits 4; the script reports it and
refuses to overwrite. Schema-version bump → manifest mismatch; script
writes `manifest.json.new` and asks for review.

Never invoked by CI. Lives next to `tools/get_gtopt_binary.py`
("human-run helpers live in `tools/`").

---

## 7. CI policy

The back-test is **not** in default CI. Gate:
`GTOPT_RUN_CEN_BACKTEST=1`. Pytest marker `cen_backtest`; default
`pytest -q` deselects via `addopts = -m "not cen_backtest"` in
`scripts/gtopt_marginal_units/pyproject.toml`. Default GitHub Actions
workflows do not set the env-var, so the test always skips silently in
CI.

A nightly-or-manual schedule is **deferred** to the scheduling-wrapper
spec (master plan §10) — the same wrapper that schedules `cen2gtopt`.
When that lands, it should: (a) fetch a fresh seven-day window via
`cen2gtopt`; (b) run `gtopt-marginal-units --mode compare`; (c) post
the resulting `cen_backtest_report.md` to Slack/Gmail; (d) fail loudly
only on Bar #3 (Bars #1/#2 advisory in scheduled mode, since CEN data
quality drifts). This plan only declares the test exists and is wired
so a future scheduler can call it.

---

## 8. Step-by-step coding order

Mostly orchestration. Seven small commits:

1. **Fixture skeleton** — create
   `scripts/gtopt_marginal_units/tests/data/cen_sample/` with empty
   subdirs + placeholder `manifest.json`. No data yet.
2. **Pytest marker** — wire `addopts = -m "not cen_backtest"` in
   `scripts/gtopt_marginal_units/pyproject.toml`; register the marker
   in `scripts/conftest.py`. Confirm `pytest -q` collects zero new
   tests.
3. **`tools/refresh_cen_fixture.py`** (network-gated). Local
   `--allow-network --dry-run` smoke-check only; do not populate.
4. **Populate fixture** — run the refresh script for real, commit the
   `< 5 MB` blob and updated manifest. PR description names the
   chosen week and CEN report.
5. **`test_e2e_cen_backtest.py`** per §2 pseudocode. Verify via
   `GTOPT_RUN_CEN_BACKTEST=1 pytest -m cen_backtest -q`. Per
   `feedback_minimize_builds`, batch all edits before pytest; never
   run two pytest instances in parallel
   (`feedback_no_parallel_builds`).
6. **Diagnostic-report module**
   (`_report_backtest.py`). Re-run the test, review
   `build/integration_test/test_output/cen_backtest_report.md`.
7. **Workflow doc** at `docs/scripts/marginal_units_workflow.md`;
   reciprocal cross-links from `docs/scripts-guide.md` and
   `docs/methods/marginal_unit_classification.md`; link-check before
   PR.

Zero C++ rebuilds (Phase-3 is pure Python). Pytest invoked at most
twice across the milestone (steps 5 and 6).

---

## 9. Failure-to-run-back-test policy

The back-test must **never spuriously fail**. Three categories of
non-success:

| Situation | Action | pytest outcome |
|---|---|---|
| `GTOPT_RUN_CEN_BACKTEST` unset | skip with explanatory reason | `SKIPPED [opt-in]` |
| Fixture absent / empty | skip with refresh instructions | `SKIPPED [missing fixture]` |
| `manifest.json` checksum mismatch | skip with refresh instructions | `SKIPPED [stale fixture]` |
| Bar #2 reference partition missing | downgrade to structural sanity, log clearly | continue |
| Bar #1 below 95 % | **fail** | `FAILED` |
| Bar #2 below 98 % (when reference present) | **fail** | `FAILED` |
| Bar #3 has any unexplained large delta | **fail** | `FAILED` |
| `gtopt-marginal-units` exit code 3 (missing input) | **error** (real bug) | `ERROR` |
| `cen2gtopt --manifest-only` non-zero | **error** (CLI broken) | `ERROR` |

Skips emit a one-line CI-readable cause + recovery command. Failures
emit the full quality-bar diagnostic so a human can act.

Skips never silently drop coverage: integration tests don't count
toward the `scripts/ ≥ 83 %` threshold. Existence of the back-test is
verified by a cheap collection-only test (`test_backtest_exists`)
asserting the file is collected under the `cen_backtest` marker.

---

## 10. Open questions

1. **Historical week pick** — confirm candidate #1 (2022-04-25 → 2022-05-01,
   Kimal-Lo Aguirre HVDC outage) or pick from #2/#3. Decision affects the
   fixture's commit and the back-test's signal-to-noise.
2. **Fixture storage policy** — commit in-tree (default; ≤ 5 MB) vs
   lazy-pull from a GitHub release (better if the blob grows). Default
   is in-tree; the lazy-pull path is documented in §1.4 but not yet
   wired.
3. **What counts as "CEN's published zone-partition reference"?** —
   CEN does not publish a stable hourly *subsistemas desacoplados* CSV.
   Candidates: (a) the `Costo Marginal Real` page's "subsistemas"
   filter, scraped per-hour; (b) the CEN Art. 72-15 annual report
   appendix (event-level, not hourly); (c) a hand-curated reference
   built once for the chosen week. Fallback (§3.4) lets us proceed
   without a definitive answer; the user should confirm which option
   becomes the v1.1 target.
4. **Schedule wrapper** — explicitly out of scope here (master plan
   §10), but the back-test is the first user of it. Confirming the
   wrapper choice (cron, systemd, `loop` skill, GitHub Actions
   schedule) early would let us pre-stub the CI hook.
5. **Phase-1 contract for `audit_flags` column** — Bar #3 assumes
   `mode=compare` emits an `audit_flags` string with the §4.7 step-R5
   categories. The master plan §4.7 says the categories are tracked
   "in the report"; Agent F must confirm with Agent A that they are
   also exposed in the parquet. If not, Agent F adds a tiny helper
   that re-derives them in the back-test.

---

[cen-cmgreal]: https://cmgreal.coordinador.cl/
