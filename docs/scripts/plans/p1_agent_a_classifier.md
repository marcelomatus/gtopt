# P1 Agent A — implementation plan: skeleton, gtopt reader, classifier, zones

> **Scope**: deliverables P1.0, P1.A, P1.C, P1.E, P1.H of the master plan
> (`docs/scripts/gtopt_marginal_units_plan.md`). Owner of the
> `scripts/gtopt_marginal_units/` package skeleton, the gtopt-output
> reader, the per-cell classifier engine, the congestion-zone partition,
> and the piecewise MC lookup. Coordinates with **Agent B** (feed reader,
> §4.7 reconstruction) and **Agent C** (writer, report, test fan-out).
>
> **Status**: planning only. No production code yet.

---

## 1. Package layout

Mirrors the layout of `scripts/gtopt_check_output/` with one
classification-specific module per concern. All filenames are relative
to `scripts/gtopt_marginal_units/`.

```
scripts/gtopt_marginal_units/
├── __init__.py            # public surface; re-exports ClassificationStatus
├── __main__.py            # `python -m gtopt_marginal_units` shim
├── main.py                # argparse + dispatcher (Agent A — P1.0/P1.A)
├── _gtopt_reader.py       # gtopt-output reader (Agent A — P1.A)
├── _feed_reader.py        # canonical-feed reader STUB (Agent B — P1.B)
├── _classify.py           # 8-status rule engine (Agent A — P1.C)
├── _zones.py              # connected-components partition (Agent A — P1.C)
├── _segments.py           # piecewise-linear MC lookup (Agent A — P1.E)
├── _io.py                 # output Parquet/CSV writer STUB (Agent C — P1.F)
├── _report.py             # Markdown digest STUB (Agent C — P1.F)
├── _types.py              # ClassificationStatus, dataclasses, tolerances
└── tests/
    ├── __init__.py
    ├── data/              # tiny synthetic fixtures
    ├── test_classify.py        # Agent C subagent C1
    ├── test_zones.py           # Agent C subagent C2
    ├── test_segments.py        # Agent C subagent C3
    ├── test_io_roundtrip.py    # Agent C subagent C4
    ├── test_cli_smoke.py       # Agent C subagent C5
    ├── test_minimum_data.py    # Agent C subagent C6
    └── test_gtopt_reader.py    # Agent A — owns this one
```

**Stub contract** for `_feed_reader.py` and `_io.py`/`_report.py`:
Agent A ships them as importable modules with the public function
signatures defined and a body of `raise NotImplementedError("owned by
Agent B/C")`. This unblocks `main.py`'s dispatch table and CLI smoke
test (P1.0) without forcing Agent A to wait on B/C.

**`_types.py`** centralises:

* `class ClassificationStatus(StrEnum)` — the 8 statuses from §4.4 of
  the master plan (`marginal`, `forced_pmin`, `capped_pmax`,
  `inframarginal`, `extramarginal_off`, `extramarginal_interior`,
  `hydro_marginal`, `profile_dispatched`) plus the synthetic
  `forced_pmin_marginal`, `__demand_fail__`, `__renewable_curtailment__`,
  `__unattributed__`.
* `@dataclass(frozen=True) class Tolerances` with the five fields from
  master-plan §4.4 (`eps`, `tol_price`, `tol_flow`, `tol_mu`, `tol_lmp`).
  Defaults match the master plan; all are CLI-overridable.
* `@dataclass class GeneratorRow` (per-row view used by the classifier:
  `uid, name, bus_uid, pmin, pmax, gcost, kind`).
* `@dataclass class CellInputs` (the in-memory shape produced by either
  reader: `cell_key, dispatch, lmp, flow, flow_dual, load, ens`).
* `class GenKind(StrEnum)`: `thermal | hydro | battery | profile`.

Per `feedback_no_nan`: every `Optional[float]` field uses
`Optional[float]` in-memory; `pd.NA` at the DataFrame boundary; NaN only
when serialising to Parquet (which natively understands it).

Per `feedback_naming_convention`: strong-typed identifiers carry the
`_uid` / `_index` suffix (`bus_uid`, `gen_uid`, `line_uid`,
`zone_index`); bare names are reserved for LP/domain objects.

---

## 2. `pyproject.toml` patch

Add the following entries to `scripts/pyproject.toml`. Diffs are minimal
— ruff/pylint/mypy versions inherit from the existing config.

```toml
# [project.scripts] — add this line in alphabetical position:
gtopt_marginal_units  = "gtopt_marginal_units.main:cli"

# [project] dependencies — already present except networkx:
#   pandas>=2.0.0,<3.0   ✓
#   pyarrow>=12.0.0      ✓
#   numpy>=1.26.0,<2.4   ✓
# add:
#   "networkx>=3.0",

# [tool.setuptools.packages.find] include — append:
#   "gtopt_marginal_units*"

# [tool.pytest.ini_options] testpaths — append:
#   "gtopt_marginal_units/tests",

# [tool.coverage.run] source — append:
#   "gtopt_marginal_units"

# [tool.isort] known_first_party — append:
#   "gtopt_marginal_units"
```

The Phase-0 peer package (`scripts/_canonical_feed/`, owned by
schema-owner) is consumed via direct import (`from _canonical_feed import
Topology, Cells, read_feed, write_feed`). It is **not** declared as a
runtime dependency — it lives in the same setuptools install.

The CLI entry point uses `cli` (not `main`) to distinguish the
argparse-driven entry point from internal `main(...)` helpers, matching
existing scripts that have a sub-command surface.

---

## 3. CLI parser (`main.py`)

### 3.1 Flag table

| Flag | Type | Default | Notes |
|---|---|---|---|
| `--input` | `{gtopt,feed}` | required | data-source kind; controls reader dispatch |
| `--mode` | `{simulated,real,real-reconstruct,compare}` | `simulated` | classification mode |
| `--planning` | `Path` | none | required when `--input gtopt`; planning JSON |
| `--output` | `Path` | none | required when `--input gtopt`; gtopt output dir |
| `--feed` | `Path` | none | required when `--input feed` |
| `--feed-against` | `Path` | none | for `--mode compare` (gtopt side) |
| `--gtopt-against` | `Path` | none | for `--mode compare` (feed side) |
| `--out` | `Path` | `./marginal_units.parquet` | output artifact |
| `--csv` | flag | `False` | also write `.csv` view alongside Parquet |
| `--scenes` | range list | all | filter, gtopt input only |
| `--stages` | range list | all | filter, gtopt input only |
| `--blocks` | range list | all | filter, gtopt input only |
| `--dates` | range list | all | filter, feed input only |
| `--hours` | range list | all | filter, feed input only |
| `--tol-price` | float | `1e-3` | LMP-vs-MC gap (relative) |
| `--tol-flow` | float | `5e-3` | fractional saturation |
| `--tol-mu` | float | `1e-2` | flow dual nonzero threshold |
| `--eps` | float | `1e-4` | dispatch-bound slack (relative to pmax) |
| `--tol-lmp` | float | `1e-2` | intra-zone LMP spread |
| `--single-bus` | flag | `False` | force copperplate (skip line/zone analysis) |
| `--zone-mode` | `{congestion,physical,both}` | `congestion` | partition policy |
| `--report` | `Path` | none | optional Markdown digest (Agent C territory) |
| `-v` / `-q` | flag | INFO/WARNING | log verbosity |
| `-V` / `--version` | flag | — | print version (uses `gtopt_config.get_version`) |

Range-list flags accept the dash-and-comma syntax used elsewhere in
`scripts/` (e.g. `--scenes 1,2`, `--stages 1-3`, `--blocks 1,4-8,12`).
Reuse `scripts/gtopt_check_lp/` helpers if a parser already exists;
otherwise ship a 10-line helper in `_types.py` (`parse_range_list`).

### 3.2 Exit-code contract (matches master plan §5)

| Code | Meaning |
|---|---|
| `0` | every cell attributed cleanly |
| `2` | at least one `__unattributed__` row emitted (`degenerate=True`) |
| `3` | input files missing / inconsistent / mode minimum (§4.8.3) not met — **no output written** |
| `1` | unexpected exception (caught at top level; logged with stack) |

### 3.3 Dispatch table

`main.cli(argv)` builds the parser, validates the flag combination,
then dispatches to a reader by `(--input, --mode)`:

```python
DISPATCH = {
    ("gtopt", "simulated"):
        lambda args: _gtopt_reader.read(args.planning, args.output, args),
    ("feed", "real"):
        lambda args: _feed_reader.read(args.feed, args, reconstruct=False),
    ("feed", "real-reconstruct"):
        lambda args: _feed_reader.read(args.feed, args, reconstruct=True),
    ("gtopt", "compare"):
        lambda args: _compare.read_paired(
            gtopt=(args.planning, args.output),
            feed=args.feed_against, args=args),
    ("feed", "compare"):
        lambda args: _compare.read_paired(
            feed=args.feed,
            gtopt=(args.planning, args.gtopt_against), args=args),
}
```

`_compare` is a thin module owned by Agent A (P1.H) that calls both
readers in turn and aligns the resulting frames; the alignment helper
(`align_simulated_to_real`) joins on
`(generator.name, datetime_from_block(planning, scene, stage, block))`
per master plan §3.3.3. Until P1.H lands, the `compare` rows raise
`NotImplementedError`.

`_validate_minimum_data(args, data)` runs **after** the reader returns
its in-memory frames and before the classifier loop; it inspects the
mode-specific minimum from §4.8.3 of the master plan and exits `3` with
a one-line `error: missing X for mode Y` message when violated.

### 3.4 Top-level flow

```text
cli(argv):
    args = make_parser().parse_args(argv)
    setup_logging(args.verbose, args.quiet)
    reader = DISPATCH[(args.input, args.mode)]
    topology, cells = reader(args)        # canonical in-memory shape
    _validate_minimum_data(args, topology, cells)
    rows = run_classifier(topology, cells, tolerances_from(args))
    _io.write(rows, args.out, csv=args.csv)
    if args.report:
        _report.write(rows, args.report, topology=topology)
    sys.exit(_exit_code_from(rows))
```

`run_classifier` lives in `main.py` and is the per-cell driver of §8.

---

## 4. gtopt reader (`_gtopt_reader.py`)

### 4.1 Public signature

```python
def read(
    planning_path: Path,
    output_dir: Path,
    args: argparse.Namespace,
) -> tuple[Topology, Cells]:
    """Load a gtopt run and return canonical Topology + Cells frames."""
```

Returns the **same dataclasses** as the Phase-0 `_canonical_feed`
package — the classifier downstream is reader-agnostic by construction.

### 4.2 Data sources & helpers

Reuse `scripts/gtopt_check_output/_reader.py:read_table` for every
output file (it transparently handles single-file Parquet,
Hive-partitioned Parquet, and CSV-shard layouts). Reuse
`load_planning(...)` from the same module.

Mapping (per master plan §3.1, §3.2):

| Source (under `output_dir`) | Stem | Required when |
|---|---|---|
| Planning JSON | `--planning` arg | always |
| Generator dispatch | `Generator/generation_sol` | always |
| Generator cost (audit) | `Generator/generation_cost` | optional |
| Bus LMP | `Bus/balance_dual` | always |
| Line flow | `Line/flowp_sol` | always unless `use_single_bus` |
| Flow capacity dual | `Line/flowp_cost` | always unless `use_single_bus` |
| Theta dual (info) | `Line/theta_dual` | optional |
| Demand load | `Demand/load_sol` | optional |
| Demand fail | `Demand/fail_sol` | optional |
| Reservoir / Battery duals | `Reservoir/<name>/*dual*`, `Battery/<name>/*dual*` | optional (warn once per missing if any hydro_marginal cell emitted) |

Column convention from the gtopt CSVs (verified against
`scripts/output/`): `scenario, stage, block, uid:<N>, uid:<M>, ...`
where each `uid:N` column is one entity. The reader pivots the wide
form into the long-form Cells layout via `pd.melt`:

```text
melt: id_vars=[scenario, stage, block]
      → var_name='entity_uid', value_name='value'
      → strip 'uid:' prefix, cast to int
```

### 4.3 Topology assembly

From the planning JSON, populate the `Topology` dataclass exactly:

* `bus_array`  → `bus(uid, name, region=None)`
* `generator_array`  → `generator(uid, name, bus_uid, pmin, pmax, declared_MC, kind, segments)`
  * `kind` is computed by checking which array the unit appears in:
    `reservoir_array` and `battery_array` references mark it
    `hydro`/`battery`; presence in `generator_profile_array` marks
    `profile`; otherwise `thermal`.
  * `declared_MC` = scalar `gcost` when scalar; `None` when scheduled
    (v1 only handles scalar — log a one-time WARNING per unit and set
    `kind` accordingly). `feedback_no_nan` says use `None`, not NaN.
  * `segments` = `commitment.heat_rate_segments` + `pmax_segments` when
    both present; else `None`. Schema follows
    `include/gtopt/commitment.hpp`.
* `line_array`  → `line(uid, bus_a_uid, bus_b_uid, tmax_ab, tmax_ba, active=True)`
  * Lines with `active=false` per stage are dropped from the static
    lookup for that stage (master plan §6.8). The stage-aware filter
    lives in `_zones.py`, not here.

### 4.4 Cells assembly

For every `(scenario, stage, block)` row in `generation_sol`, build a
`CellInputs` row. `dispatch` is a `dict[gen_uid, float]`, `lmp` is
`dict[bus_uid, float]`, `flow` and `flow_dual` are
`dict[line_uid, Optional[float]]` (None when single-bus / file absent).

Filter via `args.scenes / args.stages / args.blocks` **at this stage**
(after melt, before constructing CellInputs) — keeps memory bounded for
big runs.

### 4.5 `scale_objective` sanity check

Per master plan §4.2 and §6.1:

```text
recompute_total_cost = Σ_g Σ_cell  gcost_g · disp_{g,cell} · duration_block
assert |recompute_total_cost − solver_status.objective_value
       · planning.options.scale_objective| / total < 5%
```

Read `solver_status.json` from `output_dir`. On mismatch, log a WARNING
(not an abort — the user may have profile units or piecewise costs we
can't reconstruct). On `λ_z > 1.05 · demand_fail_cost` *during* the
classifier (not here), abort with exit `3` per master plan §6.1 — this
is the user-forgot-`--scale_objective` signal.

### 4.6 Stage-aware static lookups

`Topology` carries the full catalogues; the per-cell driver in §8 caches
*per-stage* views (`gen_at_bus[stage]`, `line_df[stage]`) once at the top
of the loop. This keeps reader cheap and lookup fast.

---

## 5. Classifier engine (`_classify.py`)

### 5.1 Public signature

```python
def classify(
    disp: float,
    pmin: float,
    pmax: float,
    mc: float | None,
    lmp: float,
    kind: GenKind,
    eps: float,
    tol_price: float,
) -> ClassificationStatus:
    """Pure function — no IO, no state. Vectorised wrapper below."""
```

Plus the vectorised entry point used in the per-cell driver:

```python
def classify_frame(
    gen_df: pd.DataFrame,
    cell: CellInputs,
    zone_lmp: dict[int, float],
    tol: Tolerances,
) -> pd.DataFrame:
    """Apply classify() row-wise across one cell. Returns a frame with
    columns: gen_uid, bus_uid, status, mc, reduced_cost, active_segment."""
```

### 5.2 Priority-ordered rule table (master plan §4.4)

```python
def classify(disp, pmin, pmax, mc, lmp, kind, eps, tol_price):
    eps_g    = eps * max(pmax, 1.0)
    tolp     = tol_price * max(abs(lmp), 1.0)
    interior = (pmin + eps_g < disp < pmax - eps_g)

    # Rule 1 — off
    if disp <= eps_g and pmin <= eps_g:
        return Status.OFF
    # Rule 2 — forced pmin
    if (pmin - eps_g) < disp < (pmin + eps_g) and pmin > eps_g:
        return Status.FORCED_PMIN
    # Rule 3 — capped pmax  (note: disp may slightly exceed pmax in LP)
    if (pmax - eps_g) < disp <= (pmax + eps_g):
        return Status.CAPPED_PMAX
    # Rule 4 — non-dispatchable profile (any positive dispatch)
    if kind == GenKind.PROFILE and disp >= eps_g:
        return Status.PROFILE_DISPATCHED
    # Rule 5 — hydro/battery interior
    if interior and kind in (GenKind.HYDRO, GenKind.BATTERY):
        return Status.HYDRO_MARGINAL
    # Rules 6–8 require a numeric MC
    if interior and mc is not None:
        gap = mc - lmp
        if abs(gap) <= tolp:
            return Status.MARGINAL
        if gap < -tolp:
            return Status.INFRAMARGINAL
        return Status.EXTRAMARGINAL_INTERIOR  # flag as degenerate
    # Fallback — interior with unknown MC
    return Status.UNATTRIBUTED
```

The textbook `extramarginal_off` collapses into rule 1 when `disp ≈ 0`
(and `mc > λ_g` is reported as a side-channel column, not as a status —
it's only visible when the user asks for one). The synthetic
`forced_pmin_marginal`, `__demand_fail__`, `__renewable_curtailment__`,
and `__unattributed__` are emitted by the **per-zone post-pass** in §8,
not by `classify` itself — `classify` is pure per-unit.

### 5.3 Tolerance defaults (mirrors master plan §4.4)

| Symbol | Default | Definition |
|---|---|---|
| `eps` | `1e-4` | absolute slack on dispatch (scaled by `pmax`) |
| `tol_price` | `1e-3` | LMP-vs-MC gap (scaled by `|λ|`) |
| `tol_flow` | `5e-3` | fractional saturation threshold |
| `tol_mu` | `1e-2` | dual nonzero threshold |
| `tol_lmp` | `1e-2` | intra-zone LMP spread (scaled by `|λ_z|`) |

All exposed via `Tolerances` dataclass (`_types.py`) and overridable
from `main.py` flags.

---

## 6. Zone partition (`_zones.py`)

### 6.1 Public signature

```python
def partition(
    topology: Topology,
    flow: dict[int, float],
    flow_dual: dict[int, float],
    tmax_ab: dict[int, float],   # may differ per stage
    tmax_ba: dict[int, float],
    tol_flow: float,
    tol_mu: float,
    *,
    single_bus: bool = False,
    zone_mode: str = "congestion",
) -> tuple[dict[int, int], list[int]]:
    """Return (bus_uid → zone_index, saturated_line_uids)."""
```

### 6.2 Algorithm

```python
def partition(...):
    if single_bus or not topology.line:
        return ({b.uid: 0 for b in topology.bus}, [])

    G = nx.Graph()
    G.add_nodes_from(b.uid for b in topology.bus)

    saturated: list[int] = []
    for line in topology.line:
        if not line.active:
            continue
        f = flow.get(line.uid, 0.0)
        mu = abs(flow_dual.get(line.uid, 0.0))
        # Two-condition saturation test (master plan §6.6)
        cap = tmax_ab[line.uid] if f >= 0 else tmax_ba[line.uid]
        is_sat = (abs(f) >= cap * (1 - tol_flow)) and (mu > tol_mu)

        # zone_mode='physical': also drop tmax==0 / inactive lines
        # zone_mode='both': callers run partition twice and emit both
        if is_sat or (zone_mode == "physical" and (cap == 0 or not line.active)):
            saturated.append(line.uid)
            continue
        G.add_edge(line.bus_a_uid, line.bus_b_uid)

    components = list(nx.connected_components(G))
    bus_to_zone = {bus: idx for idx, comp in enumerate(components) for bus in comp}
    return bus_to_zone, saturated
```

### 6.3 Intra-zone LMP-spread sanity check

After partition, the per-cell driver computes per zone:

```python
λ_z = median(lmp[b] for b in zone_buses)
spread = max(lmp[b] for b in zone_buses) - min(lmp[b] for b in zone_buses)
if spread > tol_lmp * max(abs(λ_z), 1.0):
    log.warning("zone %d LMP spread %.3f exceeds tolerance — "
                "candidates: %s", zone_idx, spread, near_saturated_lines)
    cell.degenerate = True
    cell.reason = "lmp_spread_exceeds_tol"
```

`near_saturated_lines` = lines with `|f|/cap > 0.95` not currently in
`saturated` — the master plan §6.7 hint that the partition missed
something.

`zone_mode='both'` runs `partition` twice (once with `congestion`, once
with `physical`) and the writer emits both attributions side by side
(see Agent C territory).

---

## 7. Piecewise MC lookup (`_segments.py`)

### 7.1 Public signature

```python
def active_segment_slope(
    gen: GeneratorRow,
    disp: float,
    eps: float,
) -> tuple[float | None, int, bool]:
    """Return (slope, segment_index, ambiguous).
    slope=None when no MC info is available (e.g. profile units or
    scheduled-gcost units in v1).
    ambiguous=True when disp falls exactly on a segment break (within eps)."""
```

### 7.2 Logic

* If `gen.segments is None`: return `(gen.declared_MC, -1, False)` —
  scalar `gcost` case, no piecewise table.
* If `gen.declared_MC is None and gen.segments is None`: return
  `(None, -1, False)` — v1 cannot resolve scheduled gcost; the
  classifier falls through to `UNATTRIBUTED` for interior dispatch.
* Otherwise iterate segments. The schema in
  `include/gtopt/commitment.hpp` is **cumulative pmax**:

  ```text
  pmax_segments = [pmax_seg0, pmax_seg0+pmax_seg1, ..., pmax_total]
  heat_rate_segments = [hr_0, hr_1, ..., hr_n]   # cost per segment
  ```

  `gcost` (per-segment slope) is itself the segment slope when
  `heat_rate_segments` is the cost-per-unit form (master-plan
  `feedback_generation_cost_segments`: piecewise costs use generation
  cost per segment, not marginal/incremental heat rates).

* The active segment is the first index `k` with
  `disp ≤ pmax_segments[k] + eps_g`. Set `ambiguous=True` when
  `|disp − pmax_segments[k]| ≤ eps_g` and `k+1` exists (the dispatch
  sits at a break — both segment slopes are valid optima per master
  plan §2.5).

### 7.3 Ambiguous-segment surfacing

The per-cell driver records `ambiguous=True` in the row's
`active_segment` column (e.g. `active_segment = -2` as a sentinel; a
flag column `segment_ambiguous: bool` is cleaner — Agent C's writer
chooses the schema). The classifier treats either of the two slope
candidates as marginal-equivalent; the row is emitted with
`degenerate=True, reason="segment_break"`.

---

## 8. Per-cell driver (pseudocode)

Lives in `main.py:run_classifier(...)`.

```python
def run_classifier(topology, cells, tol):
    # ---- Build static lookups once (master plan §4.1) ----
    bus_df  = topology.bus_frame()                 # uid → name
    gen_df  = topology.generator_frame()           # full attrs
    line_df = topology.line_frame()                # full attrs incl. active

    gen_at_bus: dict[int, list[int]] = {}
    for g in gen_df.itertuples():
        gen_at_bus.setdefault(g.bus_uid, []).append(g.uid)

    # Per-stage filters (line.active varies per stage in c0 etc.)
    line_per_stage = topology.lines_by_stage()
    gen_per_stage  = topology.generators_by_stage()

    rows: list[dict] = []
    for cell in cells.iter_cells():
        stage = cell.cell_key.stage   # gtopt mode; feed mode: 0
        active_lines = line_per_stage[stage]
        active_gens  = gen_per_stage[stage]

        # 4.3 — partition
        bus_to_zone, saturated = partition(
            topology=topology.with_active_lines(active_lines),
            flow=cell.flow, flow_dual=cell.flow_dual,
            tmax_ab=line_df.tmax_ab, tmax_ba=line_df.tmax_ba,
            tol_flow=tol.tol_flow, tol_mu=tol.tol_mu,
            single_bus=args.single_bus, zone_mode=args.zone_mode,
        )

        # 4.3 — per-zone LMP + spread check
        zone_lmp, degen_zones = compute_zone_lmps(
            cell.lmp, bus_to_zone, tol.tol_lmp)

        # 4.4 — per-unit classification
        per_unit = classify_frame(
            gen_df=active_gens, cell=cell, zone_lmp=zone_lmp, tol=tol)

        # 4.5 — pick marginal unit(s) per zone (post-pass)
        marginal_picks = pick_marginal_per_zone(
            per_unit, bus_to_zone, zone_lmp,
            demand_fail_cost=topology.demand_fail_cost,
            tol=tol)

        # Emit row(s) per (cell, zone, gen) — see master plan §4.6
        rows.extend(emit_rows(
            cell, per_unit, marginal_picks, bus_to_zone, zone_lmp,
            saturated, degen_zones))

    return pd.DataFrame(rows)
```

`pick_marginal_per_zone` implements master plan §4.5: marginal /
hydro_marginal candidates first; else `forced_pmin_marginal` if a
single-pmin or all-pmin zone matches load within `tol_price`; else
`__demand_fail__` when `λ_z ≈ demand_fail_cost`; else
`__unattributed__` with reason. Every zone emits ≥ 1 row per cell — no
silent dropouts (matches the master plan's "downstream consumers never
silently lose a cell" guarantee).

---

## 9. Step-by-step coding order

Each step below ships as its own commit. **Build only at the end of
each step** per `feedback_minimize_builds`. Tests run with
`pytest -n auto` per `feedback_pytest_parallel`. Pylint runs with
`--jobs=0` per `feedback_pylint_parallel`.

| # | What | Files touched | Verification |
|---|---|---|---|
| 1 | **P1.0a** — package skeleton + `pyproject.toml` patch + empty modules + `_types.py` enums/dataclasses | all of §1 + §2 | `pip install -e ./scripts` succeeds; `python -m gtopt_marginal_units --help` exits 0 |
| 2 | **P1.0b** — CLI parser (§3.1, §3.2) with `_validate_minimum_data` and dispatch table; `_feed_reader.py`, `_io.py`, `_report.py` raise `NotImplementedError` | `main.py`, stub modules | `tests/test_cli_smoke.py::test_help`, `::test_missing_args` (exit 3) — written by Agent A as a placeholder; Agent C subagent C5 expands it later |
| 3 | **P1.A** — gtopt reader (§4) + topology+cells assembly + scale_objective sanity | `_gtopt_reader.py`, `tests/test_gtopt_reader.py` | reader round-trips a tiny synthetic gtopt-output fixture under `tests/data/`; integration test placeholder |
| 4 | **P1.A integration** — wire `_gtopt_reader` into the dispatch table; classifier still a no-op | `main.py` | `gtopt_marginal_units --input gtopt --planning ... --output build/integration_test/test_output/ieee_9b_ori/` produces an empty Parquet (no crash) |
| 5 | **P1.E.1** — `_segments.py` piecewise lookup (§7) | `_segments.py`, `tests/test_segments.py` (Agent C subagent C3 owns the file; Agent A ships the function) | unit tests on 3-segment hand-constructed unit |
| 6 | **P1.C.1** — `_classify.py` engine (§5) | `_classify.py` | unit tests on 8 statuses (placeholder for Agent C subagent C1) |
| 7 | **P1.C.2** — `_zones.py` partition (§6) + intra-zone LMP-spread check | `_zones.py` | unit tests on 3-bus chain, ring, single-bus |
| 8 | **P1.C.3** — per-cell driver (§8) + per-zone marginal-unit pick + emission | `main.py:run_classifier`, `_io.py` schema agreed with Agent C | **integration: `ieee_9b_ori`** (uncongested) — every cell is one zone, marginal unit = the `gcost=35` thermals, exit 0 |
| 9 | **P1.C.4** — congested-case path | (no new files; just exercises §6 + §8 together) | **integration: `ieee_14b`** (congested) — ≥ 2 zones, LMP differs across zones by `μ_l`, exit 0 |
| 10 | **P1.E.2** — hydro/battery/profile/demand-fail flagging (full) | `main.py:pick_marginal_per_zone`, `_classify.py` Rules 4–5 polish | **integration: `ieee_9b`** (solar profile) reports `profile_dispatched`, never marginal; demand-fail synthetic test on hand-built case |
| 11 | **P1.H** — `_compare.py` joiner + delta report | `_compare.py`, `main.py` | `compare` mode pairs gtopt + feed; deltas computed; integration test reads gold fixture × `ieee_4b_ori` |

`ieee_9b_ori` (step 8) MUST pass before `ieee_14b` (step 9) — uncongested
case validates the classifier with no zone-partition surprises;
congested case adds the partition+spread machinery on top.

Build flow per step: edit → `ruff format scripts/gtopt_marginal_units/`
→ `pylint --jobs=0 gtopt_marginal_units` → `mypy gtopt_marginal_units`
→ `cd scripts && python -m pytest -n auto gtopt_marginal_units/` →
commit. **Never** build in parallel with another agent.

---

## 10. Coordination with Agents B and C

### 10.1 Stubs Agent A ships (signed-off interface)

Agent B and Agent C can start work the moment step 1 lands. The stubs
Agent A ships in step 1:

| Module | Public surface (all `raise NotImplementedError(...)` initially) |
|---|---|
| `_feed_reader.py` | `def read(feed_path: Path, args, *, reconstruct: bool) -> tuple[Topology, Cells]` |
| `_io.py` | `def write(rows: pd.DataFrame, out: Path, *, csv: bool) -> None` |
| `_report.py` | `def write(rows: pd.DataFrame, report_path: Path, *, topology: Topology) -> None` |
| `_compare.py` | `def read_paired(*, gtopt, feed, args) -> tuple[Topology, Cells]` (Agent A owns this; ships `NotImplementedError` until step 11) |

### 10.2 Stubs Agent A consumes (from Phase 0 + Agent B + Agent C)

| Owner | Module | What Agent A imports |
|---|---|---|
| Phase 0 (schema-owner) | `_canonical_feed/__init__.py` | `Topology`, `Cells`, `read_feed`, `write_feed`, `Manifest` dataclasses |
| Agent B | `_feed_reader.py` | `read(feed_path, args, reconstruct)` — frozen signature; B fills the body |
| Agent C | `_io.py` | `write(rows, out, csv)` — frozen signature; C fills the body |
| Agent C | `_report.py` | `write(rows, report_path, topology)` — frozen signature; C fills the body |

### 10.3 Seams

* **Reader → driver**: every reader returns `(Topology, Cells)`. The
  driver is reader-blind. New data sources only need a new reader.
* **Driver → writer**: the driver returns a `pd.DataFrame` of rows
  matching master plan §4.6. The writer is driver-blind.
* **Compare → align**: the `_compare.read_paired` helper joins on
  `(gen_name, datetime_from_block(...))` per master plan §3.3.3.
  Agent A owns this; the alignment helper is private.
* **Tolerances**: `Tolerances` dataclass is constructed in `main.py`
  from CLI args and threaded through every public function. No
  module-level globals.

### 10.4 Hand-off rules (per master plan §12.2)

1. No two agents build at the same time. Each agent owns its own
   scratch build dir via `tools/mk_scratch_build.sh`.
2. Schema is frozen after Phase 0; bugs require user-driven version
   bump.
3. No agent edits another agent's package. Cross-agent fixes go
   through PR comments or a synchronous hand-off.
4. Agent A signals B and C by pushing step 1 (P1.0a + P1.0b). At that
   point B/C can `pip install -e ./scripts` and import the stubs.

---

## 11. Open questions

1. **Output schema source of truth**. Master plan §4.6 lists 14
   columns plus optional `data_source` / `confidence`. Agent C owns
   the writer. Does Agent A's `run_classifier` emit exactly those 14
   columns and let Agent C add the optional two, or does it always
   emit all 16 with `data_source="simulated"` hard-wired in
   `--input gtopt` mode? Recommend the latter (single shape avoids
   conditional schemas downstream).

2. **Stage-aware `tmax`**. The planning JSON allows `tmax_ab` /
   `tmax_ba` to vary per stage (via scheduled fields). v1 of
   `_zones.py` reads the scalar form; if a benchmark case relies on
   per-stage thermal limits, we need `gtopt_field_extractor` plumbing.
   Defer to v1.1 unless `ieee_14b` requires it (it does not, per
   master plan §7.2).

3. **Hydro `kind` detection**. The planning JSON has no explicit
   `kind` field; we infer from cross-references (reservoir_array,
   battery_array, generator_profile_array). Confirm that this is
   sufficient — alternative would be to add a synthetic `kind`
   metadata pass during canonicalisation.

4. **Profile-curtailment marginal flag**. Master plan §6.4 says
   `__renewable_curtailment__` when LMP=0 and a profile unit is
   curtailed. The "curtailment shadow sets the price" framing matches
   open question 2 in master plan §13. v1 plan above treats this as a
   post-pass synthetic emission in `pick_marginal_per_zone`; confirm
   that the user accepts this (vs. promoting profiles to a
   `profile_curtailed_marginal` status).

5. **`--zone-mode both` row multiplication**. When the user requests
   both partitions, do we emit two rows per `(cell, gen)` (one per
   zone-mode) with a `zone_mode` column, or two artifacts side by
   side (`marginal_units_congestion.parquet` +
   `marginal_units_physical.parquet`)? Recommend single artifact with
   a `zone_mode: str` column — keeps comparison joins trivial.

6. **`feedback_no_alpha_fix_for_compress` parallel**. The §6.1
   "abort with exit 3 on `λ_z > 1.05 · demand_fail_cost`" is the
   exact pattern the feedback warns against — *never silently
   reinterpret a suspect dual*. We honour it: abort cleanly, do not
   downgrade. Confirm this is the intended escalation.
