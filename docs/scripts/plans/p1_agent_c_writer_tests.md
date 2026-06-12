# Phase 1 — Agent C: Writer, Report, and Test Fan-out Plan

> Scope: P1.F (output writer + summary view + Markdown report) and P1.G
> (unit-test fan-out across 6+ sub-agents) of the master plan
> `docs/scripts/gtopt_marginal_units_plan.md`.
> Owner: Agent C, parallel with Agents A and B once P1.0 lands.
> Read-only references: master plan §4.6, §4.7, §6, §7.1, §7.2, §7.3,
> §11 P1.F/P1.G, §12.1.

---

## 1. Output writer — `_io.py`

### 1.1 Module location

`scripts/gtopt_marginal_units/_io.py` — sibling of Agent A's
`_classify.py`, `_zones.py` and Agent B's `_reconstruct.py`,
`_feed.py`. Exposes a single public function plus two private helpers:

```python
def write_marginal_units(
    out_path: pathlib.Path,
    rows: pandas.DataFrame,
    summary: pandas.DataFrame,
    *,
    csv: bool = False,
    overwrite: bool = True,
) -> dict[str, pathlib.Path]:
    ...
```

`rows` is the per-`(cell, generator)` long frame; `summary` is the
per-`(cell, zone)` digest produced by §2 below. The returned dict maps
artifact-name → final path so the CLI can log them.

### 1.2 Column schema (frozen, from master plan §4.6)

| Column | In-memory | On-disk | Notes |
|---|---|---|---|
| `scenario`, `stage`, `block` | `int32` | `int32` | from cell_key |
| `zone_id` | `int32` | `int32` | connected-component id, §4.3 |
| `zone_lmp` | `float64` | `float64` | median λ over zone buses |
| `bus_uid`, `bus_name` | `int32`/`string` | same | host bus + denormalised name |
| `gen_uid`, `gen_name` | `int32`/`string` | same | synthetic rows: `gen_uid=-1`, name in `{"__demand_fail__","__unattributed__","__renewable_curtailment__"}` |
| `status` | `string` (categorical) | `dictionary<string>` | one of the 8 labels from §4.4 |
| `dispatch`, `pmin`, `pmax` | `float64` | `float64` | static + dynamic |
| `marginal_cost`, `reduced_cost` | `Float64` (nullable) | `float64` (nullable) | `pd.NA` for hydro w/o water value (§6 row 3) and outside simulated mode |
| `active_segment` | `Int32` (nullable) | `int32` (nullable) | `-1` for scalar gcost |
| `is_marginal` | `bool` | `bool` | True for `marginal`, `hydro_marginal`, `forced_pmin_marginal` |
| `degenerate` | `bool` | `bool` | True for `__unattributed__` and intra-zone LMP spread > `tol_lmp` |
| `reason` | `string` (nullable) | `string` (nullable) | populated only when degenerate / unattributed |
| `data_source` | `string` (categorical) | `dictionary<string>` | `simulated`/`real`/`real-reconstruct` |
| `confidence` | `string` (categorical) | `dictionary<string>` | `lp_dual`/`merit_order`/`fallback` |

Per `feedback_no_nan`: in the in-memory frame use `pd.NA` for missing
floats (pandas nullable `Float64`, `Int32`); the **only** place NaN
surfaces is the parquet boundary — pyarrow handles the cast
transparently when columns are declared as nullable in the schema.

### 1.3 Partitioning (decision: **none** for v1)

* Output is a **single Parquet file** (`marginal_units.parquet`), not
  a Hive-partitioned dataset. Rationale: a one-week SEN run is
  ≈168 cells × ~250 generators × ~30 cols ≈ 1.3 M rows ≈ ~25 MB
  zstd-compressed. Single file keeps `pd.read_parquet` one-liner usage
  for downstream consumers and matches the `gtopt_results_summary`
  precedent.
* Parquet writer args: `compression="zstd"`, `compression_level=3`,
  `row_group_size=64*1024`, `use_dictionary=True` for the categorical
  columns.
* If a future scale demands it, partitioning by `scenario` is a
  one-line change (kept as a dormant `--partition-by` flag in the open
  questions, §10).

### 1.4 CSV mode (`--csv`)

`--csv` writes **two** files alongside the parquet (it does **not**
replace it — Parquet is the canonical artifact, CSV is for
spreadsheets):

* `marginal_units.csv` — same long frame, `pd.NA` → empty string,
  utf-8 BOM-less.
* `marginal_units_summary.csv` — the §2 summary view.

Field separator `,`, quoting `csv.QUOTE_MINIMAL`. Float format
`%.6g` (matches PLP convention in `scripts/plp2gtopt/`).

### 1.5 Summary file always written

The summary parquet is written next to the row-level parquet,
regardless of `--csv`:
`marginal_units_summary.parquet`. Two artifacts always, four when
`--csv` is passed.

---

## 2. Summary view — one row per `(cell, zone)`

### 2.1 Schema

| Column | Type | Source |
|---|---|---|
| `scenario`,`stage`,`block` (or `date_utc`,`hour`) | `int32` | cell_key; names switch on `data_source` |
| `zone_id`, `zone_lmp` | `int32`/`float64` | from rows frame, `groupby.first()` |
| `marginal_uids`, `marginal_names` | `string` | `,`-joined sorted lists where `is_marginal=True`; empty for unattributed |
| `n_marginal` | `int32` | length of the list (0 → degenerate or demand-fail) |
| `ens` | `float64` | sum of `Demand/fail_sol` over zone buses (NA when not loaded) |
| `saturated_lines`, `n_saturated` | `string`/`int32` | crossing-edge `line_uid` list of this zone partition |
| `data_source`, `confidence` | `string` | inherited zone-uniform; confidence collapses to weakest member |
| `degenerate`, `reason` | `bool`/`string?` | any-row degeneracy + first non-null reason |

### 2.2 Transform

`build_summary(rows, cell_keys)` does a single
`groupby(cell_keys + ['zone_id'], sort=False, observed=True)` with
aggs: `zone_lmp=first`, `marginal_uids/names=_join_marginal`,
`n_marginal=is_marginal.sum`, `degenerate=any`, `reason=first_non_null`,
`data_source=first`, `confidence=_worst_confidence` (fallback <
merit_order < lp_dual). Then `_attach_ens` and
`_attach_saturated_lines` join on the zone-of-bus map exposed by Agent
A's `_zones.py` — writer **never** recomputes zones, only consumes.
Exposed publicly so `_report.py` reuses the same frame. Mirrors
`gtopt_results_summary/summary.py`; common helper extraction tracked
as open question §10 #3.

---

## 3. `--report` Markdown digest — `_report.py`

### 3.1 Module location and entry point

`scripts/gtopt_marginal_units/_report.py`:

```python
def write_report(
    out_path: pathlib.Path,
    rows: pd.DataFrame,
    summary: pd.DataFrame,
    *,
    cli_args: dict,
    run_meta: dict,
) -> pathlib.Path:
    ...
```

`run_meta` carries: input mode (`gtopt`/`feed`), classification mode
(`simulated`/`real`/`real-reconstruct`/`compare`), planning JSON path
or feed manifest hash, scenario/stage/block filters, tolerances, and
the script version. Rendered via `jinja2` (already a `pyproject.toml`
dependency) from a template `_report_template.md.j2` shipped in the
package.

### 3.2 Section skeleton

```markdown
# gtopt-marginal-units report
**Run:** {{ run_meta.script_version }} · **mode:** {{ run_meta.mode }}
· **input:** {{ run_meta.input_kind }} · generated {{ run_meta.generated_at_utc }}

## 1. Executive summary
Table: cells analysed, max zones, #congested cells (%), #unattributed cells (%),
#demand_fail cells, distinct marginal units, confidence breakdown
(lp_dual / merit_order / fallback counts).

## 2. Per-scene breakdown
Table per scenario: #cells, %congested, #marginal units seen, #unattributed.

## 3. Top-10 marginal units by frequency
Rank | gen_uid | gen_name | bus_name | times_marginal | share_of_cells.

## 4. Saturated-line frequency
Rank | line_uid | bus_a → bus_b | times_saturated | share_of_cells.

## 5. Degeneracy / unattributed log
Either "_No degenerate or unattributed cells._" or first N rows
(cell | zone | reason | suggested-investigation), with pointer to
filter `marginal_units.parquet` by `degenerate=True` for the full list.

## 6. Run parameters
JSON dump of `cli_args` + `run_meta`.
```

The actual jinja template lives in
`scripts/gtopt_marginal_units/_report_template.md.j2` and renders
this skeleton with full table syntax (`{% for %}` loops over each
table's pre-aggregated frame).

### 3.3 Heuristic hints in §5

`reason` is mapped to a one-line investigation hint via a static dict
(also test-covered):

| reason regex | hint |
|---|---|
| `intra-zone LMP spread.*` | "candidate saturated line missed: see `lines_above_95pct` audit column" |
| `no marginal candidate.*forced_pmin sum.*` | "all units must-run; verify pmin sum matches load" |
| `λ_z = demand_fail_cost` | "load shedding active; check for missing reserve capacity" |
| `mc > λ_g + tol_price` | "LP degeneracy or stale segment data; rerun with finer `--tol-price`" |
| _default_ | "see master plan §6 edge cases" |

### 3.4 No HTML / no plots in v1

Plain Markdown only; pandoc-renderable. Plotting is an explicit
non-goal (open question §10 #4).

---

## 4. Unit-test fan-out plan

### 4.1 Ownership table (Agent C drives, sub-agents implement)

Per `feedback_parallel_agents_for_tests` and §12.2 rule 5 (cap = 6
sub-agents), Agent C fans out C1–C6 in parallel. The two extra files
beyond the 6-cap (§7.1 lists 9 files) are owned **directly by Agent C**
because they exercise code Agent C wrote (writer, report) and don't
deserve a sub-agent each:

| Sub-agent | File | Owner of code-under-test | Phase ordering |
|---|---|---|---|
| C1 | `tests/test_classify.py` | Agent A (`_classify.py`) | starts after P1.A |
| C2 | `tests/test_zones.py` | Agent A (`_zones.py`) | starts after P1.A |
| C3 | `tests/test_segments.py` | Agent A (`_classify.py` segment lookup) | starts after P1.E |
| C4 | `tests/test_io_roundtrip.py` | Agent C (`_io.py`) | starts in parallel with P1.F |
| C5 | `tests/test_cli_smoke.py` | Agent A (`__main__.py`, `main.py`) | starts after P1.0 |
| C6 | `tests/test_minimum_data.py` | Agent A (mode dispatcher) | starts after P1.A + P1.B |
| Agent C (direct) | `tests/test_tolerances.py` | Agent A (`_classify.py`) — but data-driven, no fan-out | after P1.E |
| Agent C (direct) | `tests/test_cen_reader.py` | Agent B (`_feed.py`) — Phase 1 reads only the *canonical* feed; this file lives in `cen2gtopt` Phase 2 instead. **Removed from Phase 1 scope**, see §10 #1. |
| Agent C (direct) | `tests/test_reconstruct.py` | Agent B (`_reconstruct.py`) | after P1.D |

C7 (test_reconstruct) and C8 (test_tolerances) are folded into Agent C
to stay at the 6-sub-agent cap. If they grow beyond ~250 lines each,
fan them out in a follow-up PR.

### 4.2 Sub-agent briefs (each is self-contained, copy-pasteable)

#### Brief C1 — `test_classify.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_classify.py`.
> **Code under test:** `gtopt_marginal_units._classify.classify(disp, pmin, pmax, mc, lmp, kind, eps, tol_price)` returning a `Status` enum value.
> **Cases to cover (one parametrize row each, exercising the rule table in master plan §4.4 in priority order):**
> (a) off — `disp=0, pmin=0` → `off`;
> (b) forced_pmin — `disp=pmin=50, pmax=200, mc=10, lmp=80` → `forced_pmin`;
> (c) capped_pmax — `disp=pmax=200, mc=10, lmp=80` → `capped_pmax`;
> (d) hydro_marginal — `kind="hydro", disp=120, pmin=20, pmax=200, mc=NA, lmp=80` → `hydro_marginal`;
> (e) marginal — `kind="thermal", disp=120, pmin=20, pmax=200, mc=80.0001, lmp=80, tol_price=0.001` → `marginal`;
> (f) inframarginal — `mc=10, lmp=80` → `inframarginal`;
> (g) extramarginal_interior — `mc=200, lmp=80` → `extramarginal_interior` (must also check the row triggers a `degenerate=True` flag downstream);
> (h) profile_dispatched — `kind="profile", disp=50, pmax=100` → `profile_dispatched`;
> (i) priority — `disp=pmin=pmax` (single-output unit) returns `forced_pmin` not `capped_pmax` (priority order);
> (j) `eps` boundary — `disp = pmin + 0.5*eps` resolves as `forced_pmin` (within slack);
> (k) `tol_price` boundary — `mc = lmp + tol_price` is exactly `marginal`, `mc = lmp + 1.001*tol_price` is `extramarginal_interior`.
> **Fixtures:** none — pure unit test, scalar inputs.
> **Acceptance:** 100 % branch coverage of `_classify.classify` (run `pytest --cov=gtopt_marginal_units._classify`); every test parametrized; uses `pytest.approx` not `==` on floats; `pylint --jobs=0` and `mypy` clean.

#### Brief C2 — `test_zones.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_zones.py`.
> **Code under test:** `gtopt_marginal_units._zones.partition(bus_df, line_df, saturated_uids, single_bus)` returning `(zone_of_bus: dict[int,int], zone_buses: dict[int,list[int]], crossing_edges: dict[int,list[int]])`.
> **Cases to cover:**
> (a) 3-bus chain `1—2—3`, no saturated lines → 1 zone covering `{1,2,3}`;
> (b) 3-bus chain with line `2—3` saturated → 2 zones `{1,2}` and `{3}`, `crossing_edges` for both zones names that line;
> (c) 3-bus ring `1—2—3—1`, all lines saturated → 3 zones, each a singleton;
> (d) 3-bus ring, two of three lines saturated → 2 zones, the unsaturated edge stays inside one zone;
> (e) `single_bus=True` → exactly one zone over every bus regardless of line list;
> (f) empty `line_df` → one zone per bus; assert determinism of `zone_id` numbering (lowest bus_uid first);
> (g) `active=False` line is ignored even if listed in `saturated_uids`;
> (h) duplicate parallel lines (same `bus_a_uid, bus_b_uid`): saturating one but not the other leaves the buses in the same zone (the alternate path is unsaturated).
> **Fixtures:** small fixture factories `_chain(n)`, `_ring(n)`, `_parallel_pair()` defined inline.
> **Acceptance:** 100 % branch coverage of `_zones.partition`; properties asserted: every bus in exactly one zone (partition invariant); `crossing_edges` reciprocal between zone pairs.

#### Brief C3 — `test_segments.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_segments.py`.
> **Code under test:** `gtopt_marginal_units._classify.active_segment_slope(gen, dispatch)` and the related `_classify.piecewise_lookup(gen, dispatch)` returning `(segment_idx, slope)`.
> **Cases:** Build a 3-segment generator (segments=[(0,50,10), (50,100,20), (100,200,30)] — `(pmin,pmax,slope_per_MWh)`); test:
> (a) dispatch in segment 0 (`disp=25`) → `(0, 10.0)`;
> (b) dispatch in segment 1 (`disp=75`) → `(1, 20.0)`;
> (c) dispatch in segment 2 (`disp=150`) → `(2, 30.0)`;
> (d) dispatch at break `disp=50` exactly → returns *two* candidates `[(0,10.0),(1,20.0)]` (the master plan §7.1 `test_segments` row mandates this); the `marginal` classification must therefore accept either MC, and `degenerate=False` is permitted only if `lmp` matches one of them within `tol_price`;
> (e) scalar gcost generator (no segments) → `(-1, gcost)`;
> (f) dispatch above last segment's pmax → raises `ValueError` (caller responsibility);
> (g) dispatch < 0 → raises.
> **Fixtures:** `make_pwl_gen(*segments)` factory.
> **Acceptance:** branch coverage 100 % on the lookup; the dispatch-at-break dual-candidate behaviour is explicitly asserted.

#### Brief C4 — `test_io_roundtrip.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_io_roundtrip.py`.
> **Code under test:** `gtopt_marginal_units._io.write_marginal_units(...)` and the matching reader (a thin `read_marginal_units(path)` helper added to `_io.py`).
> **Cases:**
> (a) write → read parquet, dataframe equality (column order, dtypes, categoricals preserved);
> (b) `--csv` mode produces both `.parquet` and `.csv` and they agree row-for-row after type coercion;
> (c) summary parquet round-trip identical;
> (d) `pd.NA` in `marginal_cost`/`reduced_cost` survives parquet → reader as `pd.NA` (not `nan`);
> (e) zstd compression at level 3 produces a smaller file than uncompressed (sanity: ≥ 30 % savings on a 10k-row synthetic);
> (f) overwriting an existing path succeeds when `overwrite=True` and raises `FileExistsError` when `overwrite=False`;
> (g) writing to a directory that does not exist creates the parent dir;
> (h) round-trip preserves the `data_source`/`confidence` categorical levels.
> **Fixtures:** `tmp_path` (pytest builtin); `synthetic_rows()` factory in `tests/conftest.py` returning a 100-row DF covering all 8 statuses.
> **Acceptance:** `pytest -n auto` passes; `pytest --cov=gtopt_marginal_units._io` ≥ 95 %; no warning about pandas FutureWarning (use nullable dtypes from the start).

#### Brief C5 — `test_cli_smoke.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_cli_smoke.py`.
> **Code under test:** `gtopt_marginal_units.__main__.main` and `gtopt_marginal_units.main.main` (CLI entry point).
> **Cases:**
> (a) `--help` exits 0 and the help text mentions `--input`, `--mode`, `--report`, `--csv`;
> (b) missing `--input` exits 3 with a single-line "missing X for mode Y" message on stderr (master plan §4.8.3 contract);
> (c) `--input gtopt --mode simulated --planning <tiny.json> --output <tiny_dir>` runs end-to-end on a 1-cell synthetic case shipped under `tests/data/synthetic_1cell/`, exits 0, writes `marginal_units.parquet` and `marginal_units_summary.parquet`;
> (d) `--report report.md` produces a file with the §1, §2, §3, §4, §5, §6 headings;
> (e) `--csv` produces both formats;
> (f) unknown flag exits 2 (argparse default);
> (g) `--scenes 1 --stages 1 --blocks 1` filter narrows the output rows.
> **Fixtures:** `tests/data/synthetic_1cell/` — directory containing a hand-written 3-bus, 3-unit planning JSON and the matching `Generator/generation_sol.parquet`, `Bus/balance_dual.parquet`, `Line/flowp_sol.parquet`, `Line/flowp_cost.parquet` files (single-cell, hand-computed expected output). Built once via a `conftest.py` `pytest.fixture(scope="session")` that calls `_io.write_marginal_units` on a frozen DataFrame.
> **Acceptance:** subprocess invocation via `subprocess.run([sys.executable, "-m", "gtopt_marginal_units", ...], check=False)` (per `scripts/conftest.py` thread-cap pattern); reads exit code from the subprocess; tests run under `-n auto` without flakes.

#### Brief C6 — `test_minimum_data.py`

> **File:** `/home/marce/git/gtopt-hygiene/scripts/gtopt_marginal_units/tests/test_minimum_data.py`.
> **Code under test:** `gtopt_marginal_units.main._validate_minimum(args, loaded_artifacts) -> None | sys.exit(3)` (Agent A's mode dispatcher).
> **Cases (the 4-row matrix in master plan §4.8.3):**
> (a) `--input gtopt --mode simulated` missing `Bus/balance_dual` → exit 3, message contains `"balance_dual"` and `"simulated"`;
> (b) same, missing `Line/flowp_sol` AND `--single-bus` not set → exit 3 mentioning `flowp_sol`;
> (c) same with `--single-bus` and missing `Line/flowp_sol` → succeeds (single-bus exempt);
> (d) `--input feed --mode real` missing `cells/lmp.parquet` → exit 3, message names `lmp`;
> (e) `--input feed --mode real-reconstruct` missing `cells/dispatch.parquet` → exit 3, message names `dispatch`;
> (f) `--input feed --mode real-reconstruct` missing `cells/lmp.parquet` → succeeds (LMP is reconstructed, master plan §4.8.1);
> (g) `--mode compare` missing the second-side artifact → exit 3 with message `"compare requires --feed-against or --gtopt-against"`;
> (h) topology `line.parquet` empty + `--single-bus` not set → exit 3 ("topology has no lines: pass --single-bus or supply lines").
> **Fixtures:** small canonical-feed builders `_make_feed(missing=...)` returning a `tmp_path` directory with selected artifacts removed.
> **Acceptance:** every row in §4.8.3 is exercised; the script never silently downgrades; messages match a stable regex (test asserts the regex so future renames are intentional).

### 4.3 Tests owned directly by Agent C (no sub-agent fan-out)

#### `tests/test_tolerances.py` (Agent C direct)

> Parametrized over `(flow, mu, tmax, tol_flow, tol_mu)` matrix
> producing the truth table for "is line saturated?". Cases:
> `flow = 0.999*tmax, mu = 1.1*tol_mu` → saturated; `flow = tmax,
> mu = 0.5*tol_mu` → **not** saturated (master plan §6 row 6: both
> conditions required); `flow = 1.0001*tmax, mu = 1.1*tol_mu` →
> saturated. Plus an LMP-vs-MC gap matrix at exactly `tol_price`.

#### `tests/test_reconstruct.py` (Agent C direct)

> Hand-built 3-bus / 3-unit case from master plan §4.7 step R3:
> realised dispatch + declared MC → analytical λ_z; saturated-line
> override → 2 zones with distinct λ_z; load > Σ pmax → demand-fail
> attribution. Reuses Phase-0 gold fixture as the input shape but
> with hand-computed expected values.

### 4.4 `test_cen_reader.py` is **out of Phase-1 scope**

Master plan §7.1 lists `test_cen_reader.py` but the CEN CSV/SIP
reader lives in Phase-2's `cen2gtopt` package. Phase-1 reads only the
**canonical feed** produced by Phase-2; the equivalent Phase-1 test is
already covered by C4 (`test_io_roundtrip.py`) for write and by Agent
B's own feed-reader tests for read. Track in §10 open questions for
the user to confirm.

---

## 5. Integration tests — §7.2

### 5.1 Location decision: `scripts/gtopt_marginal_units/tests/integration/`

Reasons:
* The integration tests are pure-Python; they consume already-emitted
  `build/integration_test/test_output/<case>/` directories.
* `integration_test/` is C++ CTest territory and adding pytest suites
  there would force a CMake plumbing change.
* `pyproject.toml [tool.pytest.ini_options] testpaths` already
  discovers `<pkg>/tests/*` recursively; we just add the
  `gtopt_marginal_units/tests` testpath and pytest picks
  `tests/integration/test_*.py` automatically.
* Marker `@pytest.mark.integration` (already declared in the
  `pyproject.toml` markers list) gates them so `pytest -m "not
  integration"` still runs unit tests fast.

### 5.2 Per-case plan

| File | Input dir | Expected zones | Expected marginal-unit set | Edge case exercised |
|---|---|---|---|---|
| `test_int_4b_ori.py` | `build/integration_test/test_output/ieee_4b_ori/` | exactly 1 zone in every cell | merit-order match — assert the cheapest interior dispatched generator's MC equals zone_lmp within `tol_price` | simplest OPF, no congestion, baseline sanity |
| `test_int_9b_ori.py` | `build/integration_test/test_output/ieee_9b_ori/` | 1 zone in every cell | thermal generators with `gcost=35` | uncongested classic 9-bus; LMP must equal 35 everywhere |
| `test_int_9b.py` | `build/integration_test/test_output/ieee_9b/` | 1 zone in every cell | mix of thermal marginals and `profile_dispatched` solar (never marginal) | profile-generator handling: assert *no* solar UID appears in the marginal list across 24 hours |
| `test_int_14b.py` | `build/integration_test/test_output/ieee_14b/` | ≥ 2 zones in cells where `flowp_cost` is non-zero | exactly the units whose MC equals each zone's λ_z | **congested** case: the dataset has a deliberately constrained line; assert `λ_zone_A − λ_zone_B = ±μ_l` (KKT consistency) |
| `test_int_c0.py` | `build/integration_test/test_output/c0/` | per-stage attribution can change | same generator may be `inframarginal` in stage 1 and `marginal` in stage 3 (capacity expansion shifts the merit order) | multi-stage capacity expansion: assert at least one generator changes status across stages |

### 5.3 Discovery and skip logic

A `tests/integration/conftest.py` provides a session-scoped
`@pytest.fixture` `case_output_dir(case_name)` that:
1. Looks for `build/integration_test/test_output/<case_name>/` relative
   to repo root.
2. If absent, `pytest.skip("integration output for <case_name> not
   built — run ctest first")`.
3. Returns `pathlib.Path` to the directory.

This avoids hard CI failures when a developer runs `pytest -n auto`
without first building the C++ side, while keeping the tests
reproducible in CI (which always builds first).

### 5.4 Build-once policy

Integration tests **never** invoke `cmake` / `ctest`. They only read
existing output. Per `feedback_minimize_builds`, the C++ build runs
once in CI before the Python suite.

---

## 6. Property tests — §7.3 (Hypothesis)

### 6.1 Location

`scripts/gtopt_marginal_units/tests/test_properties.py`. Uses
`hypothesis` (declared as a new dev dependency in §7).

### 6.2 Strategies

`gen_triple` builds `(pmin, pmax, mc, lmp, disp)` with `pmin ≤ pmax`
and `disp ∈ [pmin, pmax]` via `pmin + frac·span`. `graph_strategy`
builds `(n_buses, edges, saturated_idx)` with `n ∈ [1,10]`, edges as
`tuples(integers(0, n-1))`, saturated subset of edge indices. Both
constructed with `hypothesis.strategies.builds` and `lists` from
bounded float/int strategies (`allow_nan=False`).

### 6.3 Properties

1. **Single-status invariant.** `@given(gen_triple)` →
   `classify(...)` returns exactly one `Status` value. The rule table
   is total and disjoint.
2. **Partition invariant.** `@given(graph_strategy)` →
   `_zones.partition(bus_df, line_df, sat).zone_of_bus` is a
   well-formed partition: every bus in exactly one zone; the union of
   `zone_buses[z]` equals the bus set; intersection is empty.
3. **UID-permutation invariance.** `@given(gen_triple, st.permutations(...))`
   → permuting the order of `gen_uid`s in `gen_df` and re-running the
   pipeline produces the *same* `is_marginal` set (modulo uid order).
   Tests that no rule depends on iteration order.
4. **Idempotence of zone partition.** Running `partition` twice on
   the same input yields equal `zone_of_bus` dicts.

`settings(max_examples=200, deadline=2000)` per property; gated under
`pytest-xdist -n auto` (Hypothesis is xdist-friendly since 6.0).

---

## 7. CI integration

### 7.1 `scripts/pyproject.toml` edits

Add to `[project.scripts]`:

```toml
gtopt_marginal_units = "gtopt_marginal_units.main:main"
```

Add to `[tool.setuptools.packages.find].include`:

```toml
"gtopt_marginal_units*"
```

Add to `[tool.pytest.ini_options].testpaths`:

```toml
"gtopt_marginal_units/tests",
```

Add to `[tool.coverage.run].source`:

```toml
"gtopt_marginal_units"
```

Add to `[project.optional-dependencies].dev`:

```toml
"hypothesis>=6.100",
```

### 7.2 `scripts/conftest.py`

No edits needed. The thread-cap conftest is package-agnostic; the new
test tree picks it up automatically because `pyproject.toml`
`pythonpath = ["."]` already covers it.

### 7.3 Pylint / mypy targets in CLAUDE.md pre-commit script

The pre-commit recipe in `CLAUDE.md` "Python files" step lists every
package by name. Append `gtopt_marginal_units` to the `ruff check`,
`pylint`, and `mypy` invocation lines. (Out-of-band edit owned by
Agent A in P1.0; we just verify it's there.)

### 7.4 Parallel pytest

The existing `addopts = "-v --import-mode=importlib -n auto"` makes
`python -m pytest -n auto` the default invocation per
`feedback_pytest_parallel`. New tests must therefore avoid:
* shared `tmp_path` collisions (always use the fixture, never a
  hard-coded `/tmp/...`);
* shared module-level state (every classifier call is pure);
* file-name collisions across packages (per the
  `test_parquet_2.cpp` fix on 2026-03-02 in `MEMORY.md`) — the writer
  test uses `cname="mu_test"` to avoid clashing with
  `gtopt_check_output/test_parquet*.cpp` `test_data` directories.

---

## 8. Coverage targets

* **100 % branch coverage on `_classify.py` and `_zones.py`.** These
  are the critical correctness paths; each branch is reachable via a
  parametrize row in C1/C2/C3.
* **≥ 95 % on `_io.py`.** Roundtrip + CSV mode + error paths.
* **≥ 90 % on `_report.py`.** Skeleton render + each section's
  branches + the heuristic-hint dict.
* **≥ 85 % overall** (above the project-wide 83 % gate in
  `[tool.coverage.report].fail_under`).

Measure:

```bash
cd scripts
python -m pytest -n auto --cov=gtopt_marginal_units \
    --cov-report=term-missing --cov-report=xml \
    --cov-branch \
    --cov-fail-under=85 \
    gtopt_marginal_units/tests
```

The `--cov-fail-under=85` flag overrides the package-wide 83 in this
local invocation; CI uses the project default plus a per-package
gate added to a `pytest --cov` step in `.github/workflows/`.

---

## 9. Step-by-step coding order (Agent C own work)

Sequential, per `feedback_minimize_builds` and
`feedback_no_parallel_builds`:

1. **Wait for P1.0 from Agent A.** Confirm `_io.py` and `_report.py`
   stubs (or empty placeholders) exist and the package imports cleanly.
2. **Implement `_io.py:write_marginal_units` + `read_marginal_units`.**
   Frame schema, parquet round-trip, CSV mode, summary writer.
3. **Implement `_io.py:build_summary`.** Pulls in zone-of-bus map from
   Agent A's `_zones.py` (must have shipped P1.A by now).
4. **Implement `_report.py:write_report`** + the `.md.j2` template.
   Use jinja2; render against a synthetic 100-row DF first.
5. **Wire `--report`, `--csv`, `--out` into `main.py`.** Coordinate
   with Agent A on the CLI parser layout; do not edit `_classify.py`
   or `_zones.py`.
6. **Author own tests** (`test_io_roundtrip`, `test_tolerances`,
   `test_reconstruct`) in parallel with the writer code (TDD-friendly
   on the `_io` round-trip).
7. **Fan out C1–C6 sub-agents** (one parallel `Skill: Agent`-style
   delegation each) with the briefs in §4.2 above, after Agent A
   signals P1.A and P1.E and Agent B signals P1.D so the
   code-under-test exists.
8. **Final lint + test pass.** Run the full pre-commit recipe from
   `CLAUDE.md` (ruff format, ruff check, pylint, mypy, pytest -n auto
   --cov). One build, one test cycle. Any failure → fix locally and
   re-run, never re-trigger sub-agents.

---

## 10. Open questions

1. **Drop `test_cen_reader.py` from §7.1?** The CEN reader belongs to
   Phase 2 (`cen2gtopt`). Phase 1 only round-trips the canonical
   feed. Recommend striking the row from the master plan §7.1 table
   when Agent C ships P1.G; pending user confirmation.
2. **Hypothesis as a new dev dependency.** Adds ~3 MB to the dev
   wheel set. Acceptable per `feedback_proactive_tests`?
3. **Share helpers with `gtopt_results_summary/summary.py`?** The
   summary view (§2) duplicates the "groupby cell × zone, list
   marginal UIDs" aggregation. Worth extracting to
   `scripts/_common/summary_helpers.py` in a follow-up PR? Out of
   scope for Phase 1.
4. **Plot output in the report?** v1 is plain Markdown only. Future
   `--report-html` flag could embed matplotlib SVGs of the zone graph
   and top-units bar chart. Defer to v1.1.
5. **Coverage delta for dataframe-heavy code.** Pandas one-liners
   often hit 100 % line coverage but skip branch coverage in
   conditional `df.assign(...)` chains. We rely on `--cov-branch` to
   catch this; if a branch is unreachable due to a guard upstream,
   document it inline rather than `# pragma: no cover`.
6. **Determinism of `marginal_uids` ordering in the summary.** We
   sort numerically by `gen_uid`; the alternative (sort by descending
   dispatch) is more meaningful but breaks UID-permutation invariance
   (§6.3 property #3). Recommend keeping uid-sort and adding a
   secondary `marginal_uids_by_dispatch` column if downstream wants it.
7. **Where do `data_source = "real-reconstruct"` rows pin their
   `confidence`?** Master plan §4.6 says `merit_order`, but a R3
   step that hits the demand-fail clamp must land as `fallback`.
   Need explicit confirmation when reviewing Agent B's reconstruct
   output before C tests pin the value.
