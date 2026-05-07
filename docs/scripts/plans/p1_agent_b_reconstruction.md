# Phase-1 Agent B — Implementation plan

> **Scope**: `scripts/gtopt_marginal_units/_feed_reader.py` and
> `scripts/gtopt_marginal_units/_reconstruct.py`. Implements deliverables
> **P1.B** (canonical-feed reader) and **P1.D** (§4.7 R1–R5 LMP
> reconstruction + PTDF builder) of the master plan
> [`gtopt_marginal_units_plan.md`](../gtopt_marginal_units_plan.md).
>
> **Status**: planning. No production code in this commit.
>
> **Peers** (do not edit):
> * `scripts/_canonical_feed/` — Phase-0 schema-owner. Exposes
>   `Topology`, `Cells`, `read_feed(path)`, `Manifest`. Frozen schema
>   per master plan §3.3.3.
> * `scripts/gtopt_marginal_units/_gtopt_reader.py`,
>   `_classifier.py`, `_zones.py`, `__main__.py`, `main.py` — owned by
>   Phase-1 Agent A. We import `_zones.partition(...)` and
>   `_classifier.classify(...)` from there.
>
> **Memory feedback applied**:
> `feedback_no_nan` (use `pd.NA` / `Optional[float]`, never `NaN` in
> in-memory dataframes), `feedback_no_views_iota` (avoid `range`-style
> antipatterns; same principle: prefer named index helpers when they
> exist), `feedback_proactive_tests` (every public surface ships unit +
> integration tests), `feedback_minimize_builds` (Python only, no C++
> rebuild needed).

---

## 1. Module layout

```
scripts/gtopt_marginal_units/
├── _feed_reader.py        # P1.B — this plan
├── _reconstruct.py        # P1.D — this plan
└── tests/
    ├── data/
    │   ├── gold_feed_with_lmp.parquet/      # symlink/copy of P0.3 fixture
    │   └── gold_feed_no_lmp.parquet/        # P0.3 fixture w/ lmp dropped
    ├── test_feed_reader.py
    ├── test_reconstruct.py
    └── test_ptdf.py
```

`_feed_reader.py` and `_reconstruct.py` are private modules
(underscore prefix); they are imported by `main.py` (Agent A) and by
the tests above. No re-export.

---

## 2. `_feed_reader.py` — canonical-feed reader

### 2.1 Public surface

```python
def read_feed_into_frames(
    feed_path: Path,
    *,
    drop_lmp: bool = False,
) -> tuple[Topology, CellTable]: ...
```

* `feed_path`: absolute path to a `canonical_feed.parquet/` directory
  (the layout in master-plan §3.3.3).
* `drop_lmp`: when `True`, the `cells/lmp.parquet` file is **not**
  loaded even if present — used by `mode=real-reconstruct` to enforce
  that the algorithm cannot accidentally cheat by reading the LMP it
  is supposed to reconstruct.
* Returns a `(Topology, CellTable)` pair where the in-memory shape is
  byte-identical to what Agent A's `_gtopt_reader.read_gtopt(...)`
  produces. This is the seam that lets the rest of the package be
  mode-agnostic.

`Topology` and `CellTable` come from `_canonical_feed`:

```python
from _canonical_feed import Topology, CellTable, read_feed, Manifest
```

`CellTable` is the dataframe-of-frames object (one frame per
quantity: `dispatch`, `lmp`, `flow`, `flow_dual`, `load`, `ens`),
each indexed by `cell_key` and the relevant uid. We rely on the
schema-owner's pyarrow→pandas/numpy conversion; we do not roll our
own parquet reader.

### 2.2 Concrete steps

1. **Locate the feed directory**.
   * Accept either `feed_path` pointing at the directory itself, or at
     the `manifest.json` inside it (resolve to parent).
   * Reject if the path does not exist or is not a directory; raise
     `FileNotFoundError` with a clear "expected canonical_feed/
     directory at X" message.
2. **Validate `manifest.json`**.
   * Load via `Manifest.read(feed_path / "manifest.json")`.
   * Assert `manifest.schema_version == _canonical_feed.SCHEMA_VERSION`;
     on mismatch, raise `SchemaVersionError(found, expected)` —
     **never** silently downgrade. Per master-plan §12.2 rule 2 the
     schema is frozen after Phase 0; a mismatch means the producer is
     stale.
   * Verify each parquet file's hash against `manifest.files[*].sha256`
     (the schema-owner provides the helper). On mismatch, raise
     `FeedCorruptError(path, expected, actual)`.
3. **Delegate the bulk read**.
   * `topology, cells = _canonical_feed.read_feed(feed_path)`.
   * The helper handles parquet → typed in-memory frames; we only
     post-process.
4. **Handle missing `cells/lmp.parquet`**.
   * If the file is absent **or** `drop_lmp=True`, the
     `cells.lmp` frame is left as `None`. This is the
     `real-reconstruct` happy path.
   * If present and `drop_lmp=False`, `cells.lmp` is populated and the
     downstream classifier uses it directly (`mode=real`).
   * Crucially: never substitute a default value or NaN for missing
     LMP — the contract is "absent ⇒ caller reconstructs" per master
     plan §3.3.1 and §4.8.1.
5. **Optional column shape check**.
   * Assert each cell-frame's index columns match the canonical names
     from §3.3.3 (`cell_key`, `gen_uid` / `bus_uid` / `line_uid`).
   * Assert no `NaN` floats in `dispatch` or `load` (these are
     required §4.8.1 fields); convert any `NaN` to a clear error per
     `feedback_no_nan`. Genuine missing data must arrive as `pd.NA`
     and is allowed only on the optional columns (`flow`, `flow_dual`,
     `ens`, `lmp`).
6. **Return**. The caller (`main.py`) decides which mode applies and
   passes the frames into the classifier.

### 2.3 Error taxonomy

* `FileNotFoundError` — feed dir / manifest missing.
* `SchemaVersionError` — manifest schema_version ≠ frozen value.
* `FeedCorruptError` — sha256 mismatch on any tracked parquet file.
* `MissingRequiredColumnError` — required §4.8.1 column is absent.
* All inherit from a package-level `FeedReaderError` so `main.py` can
  exit-3 with one `except FeedReaderError as e: log.error(str(e));
  sys.exit(3)` block.

### 2.4 Tests (`tests/test_feed_reader.py`)

* **gold-fixture round-trip**: read the Phase-0 gold feed (with LMP),
  assert frame shapes and a couple of hand-checked values from the
  fixture spec.
* **lmp-absent path**: read the gold feed with `drop_lmp=True`; assert
  `cells.lmp is None` and the rest of the frames are populated.
* **lmp-truly-missing**: read a copy of the gold fixture with
  `cells/lmp.parquet` deleted from disk; same assertion as above
  (proves it works when the producer never wrote it, not just when
  the caller asked us to drop it).
* **schema-version mismatch**: edit the fixture's `manifest.json` to
  bump `schema_version`, expect `SchemaVersionError`.
* **hash mismatch**: corrupt one byte of `cells/dispatch.parquet`,
  expect `FeedCorruptError`.
* **missing required column**: write a malformed feed with no
  `dispatch` frame, expect `MissingRequiredColumnError`.

---

## 3. `_reconstruct.py` — §4.7 R1–R5 LMP reconstruction

### 3.1 File-level docstring

The first ~40 lines of the module are a docstring restating
master-plan §4.7 R1–R5 in implementation terms — what each step does,
which inputs it reads, what it writes, and which edge cases it
defers to which numerical guard. Direct quote-and-cite from the
master plan, no paraphrase. This makes the module self-documenting
for reviewers who haven't read the master plan.

### 3.2 Public entry

```python
def reconstruct_lmp(
    topology: Topology,
    cells_for_block: CellSlice,
    *,
    mode: Literal["real-reconstruct"] = "real-reconstruct",
    feed_lmp: Mapping[int, float] | None = None,  # for R5 delta only
    tol_flow: float = 5e-3,
    eps_dispatch: float = 1e-4,
    demand_fail_cost: float,
) -> ReconstructResult: ...
```

`CellSlice` is the per-(scene, stage, block) view of `CellTable` —
one cell's worth of `dispatch`, `load`, `flow|None`, etc. Agent A's
main loop builds these slices and calls us once per cell.

`ReconstructResult` is a small dataclass:

```python
@dataclass(frozen=True, slots=True)
class ReconstructResult:
    lmp_by_bus: dict[int, float]            # bus_uid -> λ_z
    zone_of_bus: dict[int, int]             # bus_uid -> zone_id
    saturated_lines: frozenset[int]         # line_uid set
    marginal_unit_per_zone: dict[int, MarginalChoice]
    confidence: Literal["merit_order", "fallback"]
    degenerate_zones: frozenset[int]
    notes: list[str]                        # human-readable warnings
```

`MarginalChoice` carries `gen_uid`, `declared_MC`, and a
`reason: Literal["interior", "pmax_pinned_cheapest",
"forced_pmin_only", "demand_fail"]` so the writer can render the
audit trail without re-deriving anything.

### 3.3 R1 — saturation pattern recovery

```python
def _saturated_lines(
    topology: Topology, cell: CellSlice, tol_flow: float,
) -> tuple[frozenset[int], dict[int, float]]: ...
```

* **If `cell.flow is not None`** (feed published flows):
  * For each line `l`, mark saturated iff
    `abs(cell.flow[l]) >= topology.line.tmax_for(l, sign(flow)) *
    (1.0 - tol_flow)`. The `tmax_for(...)` helper picks `tmax_ab`
    when `flow >= 0`, `tmax_ba` otherwise.
  * Note: this is the **flow-only** test (master plan §4.7 R1),
    weaker than §4.3's flow-AND-dual test. We accept this weakening
    because there are no duals in real-mode data.
* **If `cell.flow is None`**:
  * Estimate flows by `flow = PTDF @ netload(cell)` where
    `netload[bus] = sum(dispatch where bus(g)=bus) - load[bus]`.
  * Apply the same flow-only saturation test on the estimate.
  * Return both the saturation set **and** the estimated flow vector
    (downstream debugging / audit; never persisted to the feed).

### 3.4 R1 — PTDF builder

* Build PTDF **once per topology**, cache on `Topology` via
  `functools.lru_cache(maxsize=1)` keyed on `id(topology)` (the
  topology object is immutable for a run).

* Formula (master plan §8 of this doc, restated):

  > PTDF = B_x · (B_θ)⁻¹

  * **B_x**: `n_lines × n_buses` line-bus incidence matrix weighted
    by `1 / x_l`, with the slack column **kept** (it gets multiplied
    by zero anyway when we right-multiply).
  * **B_θ**: `n_buses × n_buses` bus susceptance (Laplacian) matrix
    `B_θ[i,i] = Σ_{l ∋ i} 1/x_l`, `B_θ[i,j] = -1/x_l` for line `l`
    between `i,j`, **with the slack row+column removed** (yielding
    `(n_buses - 1) × (n_buses - 1)`).
  * The slack/reference bus is the bus with the **lowest `bus_uid`**
    in the topology (deterministic, no API for the user to pick a
    different one in v1 — open question §6 below).
  * After inversion, pad B_θ⁻¹ back to `n_buses × n_buses` by
    inserting a zero row+column at the slack index, then compute
    `PTDF = B_x @ B_θ_inv_padded`.

* **Reactance source**: `topology.line.reactance` (column added by
  the schema-owner per Phase 0). When a line's reactance is `pd.NA`,
  fall back to a uniform `x = 1.0 p.u.` per line and emit a one-time
  warning per line listing the affected `line_uid`s. This is the
  graceful-degradation path master-plan §8 requires; `cen2gtopt`
  Phase 2 will populate `reactance` from the SIP `lineas` endpoint.

* **Implementation choice for v1**: dense numpy
  (`numpy.linalg.solve(B_theta_reduced, I)` to invert via LU rather
  than computing an explicit `inv`). Threshold to switch to sparse
  (`scipy.sparse.linalg.spsolve` on a CSR factorisation): when
  `n_buses > 500`. Document the threshold in the module docstring;
  v1 ships dense-only and falls back to sparse in v1.1 when SEN-scale
  topologies (~600 buses) exercise the path. For Phase-1 acceptance
  the ≤14-bus IEEE benchmarks plus the 3-bus gold fixture are well
  within dense regime.

* **Islanded topologies**: B_θ_reduced is singular when the
  underlying graph is disconnected. We detect this **before**
  inverting via `scipy.sparse.csgraph.connected_components` on the
  unweighted line graph (or equivalent numpy code). For each
  connected component we independently build and invert a reduced
  Laplacian with that component's lowest-uid bus as slack, then
  block-diagonal-assemble the PTDF. Inter-component PTDF entries are
  zero (no flow can cross). Open question §6 below.

### 3.5 R2 — zone partition

Reuse Agent A's `_zones.partition(topology, saturated_lines)`
verbatim. The seam is:

```python
from gtopt_marginal_units import _zones
zone_of_bus, zone_count = _zones.partition(topology, saturated_lines)
```

`_zones.partition` returns the same `(dict[bus_uid, int], int)` shape
in both modes — Agent A's docstring guarantees it. If Agent A's API
shifts (parameter name, return type), Phase-1 coordinator (the user)
adjudicates per master-plan §12.2 rule 3.

### 3.6 R3 — merit-order λ recovery

For each zone `z`:

```python
zone_buses = {b for b in zone_of_bus if zone_of_bus[b] == z}
zone_gens = topology.generators_at_buses(zone_buses)
zone_load = sum(load[b] for b in zone_buses)
```

Then apply the rule cascade in this exact order (first match wins):

1. **Demand-fail check first** (master plan §4.7 R3 last bullet,
   master plan §6 edge case 13):
   * If `zone_load > sum(g.pmax for g in zone_gens) + eps_pmax`,
     emit `λ_z = demand_fail_cost`, `MarginalChoice(reason="demand_fail")`,
     `confidence="fallback"`. This is the load-shedding zone case.
2. **Interior-dispatch unit set**:
   * `interior = [g for g in zone_gens
        if g.declared_MC is not None
        and g.pmin + eps_g < cell.dispatch[g.uid] < g.pmax - eps_g]`
   * `eps_g = max(eps_dispatch * g.pmax, eps_dispatch)` — protects
     against `pmax = 0` units (numerical guard §3.8).
3. **Interior, ≥1 candidate**:
   * `λ_z = max(g.declared_MC for g in interior)`.
   * **Tie-breaking**: when multiple `interior` units share the same
     `declared_MC` within `1e-9`, pick the one with the **lowest
     `gen_uid`** (deterministic, reproducible across runs). Record
     all tied gen_uids in `MarginalChoice.tied_with` so the report
     can show ambiguity without picking a different "real" one each
     run.
4. **No interior, but at least one pmax-pinned unit with a declared
   MC**:
   * `pmax_pinned = [g for g in zone_gens
        if g.declared_MC is not None
        and cell.dispatch[g.uid] >= g.pmax - eps_g]`
   * `λ_z = min(g.declared_MC for g in pmax_pinned)` — the cheapest
     pmax-pinned unit, master plan §4.7 R3 second bullet.
   * Mark zone `degenerate=True`. Confidence stays
     `"merit_order"` (we did use the merit order; we just had no
     interior witness).
5. **No interior, no pmax-pinned with MC, but ≥1 forced-pmin unit**:
   * `forced_pmin = [g for g in zone_gens
        if g.pmin > eps_g
        and abs(cell.dispatch[g.uid] - g.pmin) < eps_g]`
   * If zone load `≈ sum(g.pmin for g in forced_pmin)` within
     `eps_load`, emit `λ_z = max(g.declared_MC for g in forced_pmin
     if g.declared_MC is not None)` — the must-run setter case
     (master plan §4.5 forced_pmin_marginal). Confidence
     `"merit_order"`, `reason="forced_pmin_only"`,
     `degenerate=False`.
6. **None of the above**:
   * `λ_z = 0.0`, `MarginalChoice(reason="unattributed")`,
     `confidence="fallback"`, `degenerate=True`. Add a `notes`
     entry naming the zone and listing its gen states.
7. **Reconstructed λ_z exceeds rationing cap** (master plan §6 edge
   case 13):
   * After steps 1–6, clamp `λ_z = min(λ_z, demand_fail_cost)`,
     mark `confidence="fallback"`, surface offending gen_uid in
     `notes`.

### 3.7 R4 — feed λ_z into the classifier

`_reconstruct.py` does **not** call `_classifier.classify(...)`
directly; that is `main.py`'s responsibility. We return the
`lmp_by_bus` map and `main.py` constructs the synthetic
`cell.lmp[bus_uid] = lmp_by_bus[bus_uid]` frame, then runs the §4.4
classifier with `confidence="merit_order"` stamped on every emitted
row. Keeping the seam thin avoids circular imports between
`_reconstruct` and `_classifier`.

### 3.8 R5 — comparison-mode delta

R5 lives **outside** `_reconstruct.py` proper; it runs at *summary*
time in Agent C's writer module (`_writer.py`). The reason: R5 needs
the full cells × buses panel, not one cell at a time, to compute
percentile bands. We expose only the per-cell delta computation here
as a convenience helper:

```python
def lmp_delta(
    reconstructed: Mapping[int, float],
    feed: Mapping[int, float],
) -> dict[int, float]: ...
```

Agent C's writer accumulates the per-cell deltas into the audit
columns:

* `cells_within(±0.5)`, `cells_within(±2)` — count per (scene, stage)
  partition.
* `topology_mismatch` — set when the zone partition we derived in R2
  disagrees with the bus-grouping CEN's CSV implies (CEN groups buses
  with equal published LMP into a *subsistema desacoplado*). Computed
  by checking if buses with identical `feed_lmp` fall into the same
  reconstructed zone.
* `must_run_override` — set when `feed_lmp_b < min(declared_MC for g
  in interior(zone(b)))`, i.e. CEN priced below the cheapest interior
  unit, suggesting CEN counted a forced-pmin unit (master plan §4.7
  R5 last bullet).
* `cells_with_demand_fail_clamp` — count of cells where step 7 of R3
  fired.

Agent B owns only `lmp_delta`; the aggregation lives with the writer.

### 3.9 PTDF construction details (worked out)

Numbered to match master plan §8 of this brief.

1. Bus list: `B = sorted(b.uid for b in topology.bus)`. `n = |B|`.
2. Slack: `s = B[0]` (lowest uid). `B_red = B \ {s}`.
3. `B_θ` build:
   ```
   for line l in topology.line where l.active:
       i, j = bus_index[l.bus_a_uid], bus_index[l.bus_b_uid]
       y = 1.0 / l.reactance   # fallback 1.0 if NA
       B_theta[i,i] += y
       B_theta[j,j] += y
       B_theta[i,j] -= y
       B_theta[j,i] -= y
   ```
4. Drop slack row+col: `B_red = B_theta[~s, ~s]`.
5. Solve `B_red @ X = I_{n-1}` via `numpy.linalg.solve` (NOT inv).
6. Pad `X` back to `n × n` by inserting a zero row + zero col at
   index `s`.
7. `B_x` build:
   ```
   for line l in topology.line where l.active:
       i, j = bus_index[l.bus_a_uid], bus_index[l.bus_b_uid]
       y = 1.0 / l.reactance
       B_x[line_index[l.uid], i] = +y
       B_x[line_index[l.uid], j] = -y
   ```
8. `PTDF = B_x @ X_padded`. Shape `(n_active_lines, n_buses)`.
9. Cache on `topology` via `_PTDF_CACHE: WeakValueDictionary[id, np.ndarray]`.

### 3.10 Numerical guards

| Edge case | Guard |
|---|---|
| `g.pmin == g.pmax` (forced) | Skip from R3 interior set; treat as forced-pmin if dispatch ≈ pmin else as capped-pmax. Never marginal in R3. |
| `g.pmax == 0` (inactive) | Skip from R3 entirely. Mark inactive in `_zones.partition` already, so it never appears here, but the guard is defensive. |
| `g.declared_MC is None` (hydro/profile) | Excluded from R3 interior set (the `is not None` filter in step 2). Never sets λ_z. Hydro/profile units get classified by Agent A's `_classifier.py` independently using their LP-side dual when available; in feed mode they end up `hydro_marginal` with `marginal_cost = pd.NA` per master-plan §6 edge case 3. |
| `g.pmax - g.pmin < 2 * eps_g` (effectively forced) | Treat as forced (collapses to pmin or pmax depending on dispatch). |
| `cell.dispatch[g.uid]` is `pd.NA` | This is a `feedback_no_nan` violation upstream. We assert non-NA on §4.8.1-required columns in `_feed_reader.py` step 5, so by the time we reach R3 every dispatch is a finite float. If somehow not, raise `MissingRequiredColumnError`. |
| `cell.load[bus]` is `pd.NA` | Same — assert in feed reader. |
| `topology.line.reactance` is `pd.NA` | Fallback `x = 1.0`, one-time warning per line uid (§3.4 above). |
| `n_buses == 1` (single-bus mode) | Skip PTDF build entirely; PTDF is the empty matrix, R1 returns empty saturation set, R2 yields one zone, R3 runs over that one zone. |
| Disconnected topology | Per-component PTDF (§3.4 islands paragraph). |

---

## 4. Test plan

Per `feedback_proactive_tests`. All tests are pytest, run via the
project's `cd scripts && python -m pytest -n auto -q` command per
`feedback_pytest_parallel`.

### 4.1 `tests/test_feed_reader.py`

Six tests as listed in §2.4 above. Fixtures live in
`tests/data/gold_feed_with_lmp.parquet/` (copied from Phase 0) and
`tests/data/gold_feed_no_lmp.parquet/` (built once at session scope
by deleting `cells/lmp.parquet` from the with-lmp copy).

### 4.2 `tests/test_reconstruct.py`

Hand-built 3-bus / 3-unit case:

```
Bus 1 — load 100,  gen A: pmin=20, pmax=80, declared_MC=30
Bus 2 — load  50,  gen B: pmin=0,  pmax=120, declared_MC=50
Bus 3 — load  30,  gen C: pmin=0,  pmax= 60, declared_MC=80
Lines: 1-2 (tmax=200), 2-3 (tmax=200), 1-3 (tmax=200) — uncongested
```

* **Regime 1: uncongested**. Total load 180, dispatched as
  `(80, 70, 30)`. Gen A pinned at pmax (80=80), Gen C pinned at
  pmax… wait, `30 < 60`, so Gen C is interior. Gen B
  (`0 < 70 < 120`) is interior. Expected
  `λ = max(50, 80) = 80`. One zone covers all three buses.
* **Regime 2: line 1-2 saturated** (force tmax=50, flow=50). Two
  zones: `{1}` and `{2,3}`.
  * Zone `{1}`: load=100, only gen A (pmax=80), so `load > Σ pmax`
    → λ_z = `demand_fail_cost`, R3 step 1 fires.
  * Zone `{2,3}`: load=80, gens B (pmax=120) and C (pmax=60). Need
    80 from these; with B=80 (interior), C=0 (off): C is *not*
    interior (`0 < 0+eps` so off, not interior); B is interior
    (`0 < 80 < 120`). λ = max(B.MC) = 50.
* **Regime 3: load > Σ pmax everywhere**. Drop one generator;
  remaining capacity < total load → demand-fail attribution at the
  global zone level.
* **Tie-break test**: two interior units with identical `declared_MC`;
  assert reconstructed λ uses the lower `gen_uid` and `tied_with`
  lists both.
* **No interior, pmax-pinned cheapest path**: force every unit to
  pmax; assert R3 step 4 fires and `λ` = min(pmax_MCs),
  `degenerate=True`.

### 4.3 `tests/test_ptdf.py`

PTDF round-trip on the IEEE 9-bus topology. Reference PTDF computed
analytically from Anderson & Fouad's textbook values for that case
(or from MATPOWER's `case9` reactances if simpler — we ship the
expected matrix as a numpy `.npy` in `tests/data/ieee9_ptdf_ref.npy`
so the test compares numerically rather than re-deriving).

* **Test A**: build PTDF on a tiny 3-bus ring, assert closed-form
  values (a 3-bus ring with all `x = 1` has PTDF entries `±1/3`).
* **Test B**: build PTDF on IEEE 9-bus, compare to the cached
  reference within `1e-6`.
* **Test C**: islanded topology (two disjoint 2-bus subnetworks),
  assert block-diagonal structure of PTDF (each block has only one
  internal column).
* **Test D**: missing reactance column → fallback to `x=1`, assert
  one warning emitted, assert PTDF still has the expected unit-x
  values.
* **Test E**: cache hit — call PTDF builder twice on the same
  topology, assert the second call returns the cached `np.ndarray`
  (`is` identity).

---

## 5. Step-by-step coding order

1. **Wire in `_canonical_feed`** — confirm import path, run
   `python -c "from _canonical_feed import Topology, Cells,
   read_feed, Manifest, SCHEMA_VERSION"` to fail fast if Phase 0
   isn't deployed.
2. **`_feed_reader.py` skeleton + error taxonomy** — define
   exception classes and `read_feed_into_frames` signature, no body
   yet. Land lint+type CI green.
3. **`_feed_reader.py` body** — implement steps §2.2 1–6 in order.
   Run tests in §4.1 (each one as it lands).
4. **`_reconstruct.py` PTDF builder** (§3.4 + §3.9) — pure linear
   algebra, no cell dependency. Run tests in §4.3.
5. **`_reconstruct.py` R1 — saturation pattern** — implement
   `_saturated_lines` for both flow-known and flow-estimated paths.
   Test against gold-feed regime 1 (uncongested → empty saturation
   set) and regime 2 (forced saturation).
6. **`_reconstruct.py` R3 — merit-order recovery** — implement the
   rule cascade §3.6 (steps 1–7) **first**, then the entry function
   `reconstruct_lmp` that wires R1 → `_zones.partition` → R3 → return.
   Run tests in §4.2.
7. **`lmp_delta` helper** for R5 — trivial, but write the test for
   it before merging (Agent C will integrate it).
8. **Tighten and document** — add the file-level docstrings, run
   `ruff format`, `pylint --jobs=0`, `mypy --ignore-missing-imports`.
   Do not run pylint with anything other than `--jobs=0`
   (`feedback_pylint_parallel`).

Each numbered step compiles + passes its own test before the next
begins. No batch-and-pray.

---

## 6. Open questions

These are flagged for the user to adjudicate before P1.D ships. None
of them block writing the code; v1 picks the documented default and
notes the override path.

1. **PTDF behaviour on islanded topologies**. v1 plan: per-component
   PTDF, slack = lowest-uid bus per component (§3.4). Alternative:
   raise `IslandedTopologyError` and require the caller to pass
   `--component-slacks` explicitly. The per-component path is
   silently friendlier to messy CEN data but hides a real
   modelling issue — open for the user. **Recommendation**: ship
   per-component, log one warning naming each island.

2. **Battery-as-load convention in netload computation**. PTDF
   inputs are `netload[bus] = Σ_g dispatch_g - load_b`. For batteries,
   gtopt's `dispatch` column is signed (`+ = discharge, − = charge`),
   so a charging battery already shows up as negative net injection —
   no special handling needed. v1 plan: trust the sign convention in
   `cells.dispatch`. CEN's *Generación Real* never goes negative (it
   reports BESS discharge only); CEN's *Demanda Real* must include
   the charging side. `cen2gtopt` is responsible for that
   normalisation per master-plan §9.4. **Recommendation**: add a
   sanity assertion in the feed reader (§2.2 step 5) that no
   `dispatch` value is `< -tol_negative` *unless* the unit's `kind`
   is `battery`; flag mismatches as `MissingRequiredColumnError`.

3. **DC HVDC links in v1**. The DC-OPF / PTDF derivation assumes AC
   lines with reactance. SEN's HVDC trunk (Kimal-Lo Aguirre, in
   construction at the time of writing) does not fit this model. v1
   plan: treat HVDC lines as normal lines with whatever `reactance`
   the schema-owner records (likely a conventionally-large number
   that lets them be modelled as controllable injections in gtopt
   itself). `cen2gtopt` will need to decide how to publish HVDC in
   the feed; this is master-plan Phase-2 / future work, not P1.D's
   problem. **Recommendation**: P1.D documents the limitation in
   the `_reconstruct.py` docstring and ships the AC-only path; v1.1
   adds an `hvdc_link` typed column in `topology.line` and a
   matching PTDF treatment.

4. **Slack bus selection**. v1 hard-codes "lowest bus_uid". CEN's
   convention is to use a regional reference bus (typically Polpaico
   or Charrúa for the SEN). For the IEEE benchmarks the chosen
   slack must match whatever the reference PTDF was computed with
   (Anderson-Fouad uses bus 1 for case9, which conveniently
   coincides with lowest-uid). **Recommendation**: keep
   "lowest-uid" as the default; expose `--slack-bus` on the CLI in
   v1.1 only if the IEEE 9-bus reference PTDF doesn't agree, in
   which case we re-root.

5. **Per-component slack for islanded topologies — does
   `_zones.partition` return a *physical-island* partition in the
   uncongested cell?** If yes, we can pre-call it once on the
   topology to discover islands and feed them into the PTDF
   builder. If no (Agent A defines `_zones.partition` to ignore
   physical disconnections), we run our own
   `connected_components` and do not share state. Coordinate with
   Agent A. **Recommendation**: own our own connected-components
   call inside `_reconstruct.py`; do not couple to `_zones`.

---

## 7. Acceptance checklist (P1.B + P1.D)

* [ ] `read_feed_into_frames(gold_feed_path)` returns the same
      `(Topology, CellTable)` shape Agent A's gtopt reader produces
      (verified by a frame-equality test on the gold fixture).
* [ ] `read_feed_into_frames(gold_feed_path, drop_lmp=True)` drops
      `cells.lmp` and leaves the rest intact.
* [ ] Schema-version mismatch and hash mismatch raise typed errors
      with actionable messages.
* [ ] PTDF builder reproduces the IEEE 9-bus reference within 1e-6.
* [ ] PTDF cache returns identical `np.ndarray` instance on repeat
      call.
* [ ] `reconstruct_lmp` on the 3-bus / 3-unit gold feed regimes
      matches the hand-computed λ_z exactly.
* [ ] Demand-fail and pmax-pinned-cheapest fallbacks fire on the
      designed regimes.
* [ ] All tests pass under `pytest -n auto -q`.
* [ ] `ruff format`, `pylint --jobs=0`, `mypy
      --ignore-missing-imports` clean per master-plan pre-commit
      checklist.
* [ ] No `NaN` floats in any in-memory dataframe; all missing data
      is `pd.NA` (`feedback_no_nan`).
* [ ] No `--no-verify`, no schema-version bumps without coordinator
      approval (`feedback_minimize_builds`, master-plan §12.2).
