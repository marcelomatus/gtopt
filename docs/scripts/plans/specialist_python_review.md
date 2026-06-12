# Python Architecture Review — `gtopt_marginal_units` + `cen2gtopt` + `_canonical_feed`

> Reviewer: python-reviewer agent  
> Source: `docs/scripts/gtopt_marginal_units_plan.md` (all sections)  
> Conventions baseline: `scripts/pyproject.toml`, `gtopt_check_output/_reader.py`,
> `gtopt_results_summary/`, `gtopt_compare/main.py`, `scripts/conftest.py`  
> Date: 2026-05-05

---

## 1. Scope

| Artefact | Status | Lines reviewed |
|---|---|---|
| `docs/scripts/gtopt_marginal_units_plan.md` | design doc, 1300+ lines | full |
| `scripts/gtopt_check_output/_reader.py` | existing, reference | 164 |
| `scripts/gtopt_results_summary/main.py` + `summary.py` | existing, reference | 67 + 364 |
| `scripts/gtopt_compare/main.py` | existing, reference | 1535 |
| `scripts/pyproject.toml` | existing | 224 |
| `scripts/conftest.py` | existing | 23 |

No production code exists yet for the new packages; this is a pre-implementation
architecture review. All findings cite master-plan sections.

---

## 2. Findings — Priority P0 (must fix before implementation starts)

### P0-1 — `_canonical_feed` as a peer package breaks `setuptools.packages.find`

**Plan section:** §1.5 (Phase 0), §11 (P0.2)  
**Problem:** `pyproject.toml:L83` uses `include = ["gtopt_compare*", ...]` with explicit
package prefixes. A leading-underscore package `_canonical_feed` will be silently
omitted from the wheel because none of the existing glob patterns match it. It is also
invisible to `mypy` and `pylint` when they scan the tree (both tools receive an explicit
package list in CI commands — see `CLAUDE.md` pre-commit section).

**Proposed fix:**

```toml
# pyproject.toml — add in three places
[tool.setuptools.packages.find]
include = [..., "_canonical_feed*"]

[tool.pytest.ini_options]
testpaths = [..., "_canonical_feed/tests"]

# CLAUDE.md pre-commit lint line — add _canonical_feed to pylint and mypy invocations
```

The package name must also appear in the `pylint --jobs=0` and `mypy` invocations in
`CLAUDE.md`. Missing it means CI never lints or type-checks Phase-0 code.

**Why:** setuptools `find` with an explicit `include` list will not discover
`_canonical_feed`; the package ships empty wheels and CI is blind to its quality.

---

### P0-2 — `main()` returns `None` in the plan's CLI sketch; CI requires `int`

**Plan section:** §5 (Public CLI), §9.5 (Phase-2 CLI), §11 (P1.0, P2.0)  
**Problem:** The plan describes exit-code semantics (exit 0/2/3/4) but never states
that `main()` must return `int` and be wrapped by `sys.exit(main())`. The existing
`gtopt_results_summary/main.py:L13` follows the `-> int` / `sys.exit()` pattern; the
plan's prose is silent on this. If the skeleton is written with `-> None` it will
compile but the `__main__.py` caller must `sys.exit()` independently, which is routinely
forgotten.

**Proposed skeleton (consistent with existing packages):**

```python
# main.py
def main(argv: list[str] | None = None) -> int:
    ...
    return 0  # or 2, 3, 4 per §5/§9.5

# __main__.py
import sys
from .main import main
sys.exit(main())
```

**Why:** Exit code 3 for "missing input" is a documented contract; if `main()` returns
`None` the process always exits 0 regardless of what the code does internally.

---

### P0-3 — `assert` for LMP-scale sanity check (§4.2 / §6.1) will be stripped under `-O`

**Plan section:** §4.2 ("sanity-check by recomputing"), §6 edge-case 1  
The plan says "the script **asserts** `λ_z ≤ 1.05 · demand_fail_cost`". Using bare
`assert` for this check is wrong: Python strips assertions under `python -O` (which
some deployment wrappers activate). The project's own style (see `_reader.py`) raises
explicit exceptions for runtime validation.

**Proposed fix:**

```python
# _classify.py or wherever the check lives
if abs(lmp_z) > 1.05 * demand_fail_cost:
    raise ValueError(
        f"Zone LMP {lmp_z:.2f} > 1.05 × demand_fail_cost "
        f"({demand_fail_cost:.2f}); check --scale-objective consistency."
    )
```

**Why:** `assert` in production code is stripped under `-O`; the check silently
disappears and the script produces nonsense classifications without warning.

---

### P0-4 — `requests.get` in `cen2gtopt` needs `timeout=`

**Plan section:** §9.2.1 (pure-requests HTTPS GETs)  
The plan specifies plain `requests.get` calls against CEN static CSV endpoints. The
project style guide (and the reviewer taxonomy) require `timeout=` on all `requests`
calls. Without it a hanging CEN server will stall the CI job indefinitely.

**Proposed fix:**

```python
# _csv_downloader.py
DEFAULT_TIMEOUT = 30  # seconds
resp = requests.get(url, timeout=DEFAULT_TIMEOUT)
resp.raise_for_status()
```

Expose this as a `--timeout SECONDS` CLI flag (default 30) so the user can override
for slow networks — also improves `--help` per ergonomics conventions.

**Why:** Network hang without timeout is an unrecoverable stall in CI and in scripts
running unattended.

---

## 3. Findings — Priority P1 (should fix before merging the first PR)

### P1-1 — Package layout: `_canonical_feed` as a peer is the right abstraction but needs
a public name

**Plan section:** §1.5, §11 P0.2  
The plan uses `_canonical_feed` (leading underscore). In Python the leading underscore
marks a *private* package — importable but conventionally not part of the public API.
Both `gtopt_marginal_units` and `cen2gtopt` **import** it as a public dependency, which
is a contradiction. Options:

| Option | Trade-off |
|---|---|
| Rename to `gtopt_canonical_feed` | Consistent with `gtopt_*` naming convention; visible to setuptools, pylint, mypy without special-casing; clear public status. |
| Keep `_canonical_feed` but document it as project-internal | Requires the `_` be stripped from every place it appears in `pyproject.toml`, `CLAUDE.md`, and `known_first_party` isort list. |

**Recommendation:** Rename to `gtopt_canonical_feed`. One small submodule per Phase-0
deliverable: `schema.py` (dataclasses + `Literal` types), `io.py` (parquet read/write),
`manifest.py` (JSON read/write + hash check). The three-file split maps directly onto
P0.2 and is testable in isolation.

---

### P1-2 — File-name conventions: `_classify.py` diverges from existing package layout

**Plan section:** §3.3.3, §11 (P1.A/C)  
Existing packages use `<verb>_or_noun.py` without leading underscore for module files
(e.g. `_reader.py` in `gtopt_check_output` is an internal helper, but the top-level
`main.py` / `summary.py` are unprefixed). The plan introduces seven `_*.py` private
modules. That is fine for helpers not exported to other packages, but the *names* must
be stable within the package.

Suggested rename for clarity and consistency:

| Plan name | Suggested name | Why |
|---|---|---|
| `_classify.py` | `_classify.py` | fine, pure function, fully internal |
| `_zones.py` | `_zones.py` | fine |
| `_segments.py` | `_segments.py` | fine |
| `_io.py` | `_reader.py` | matches `gtopt_check_output` convention |
| `_report.py` | `_report.py` | fine |
| `_gtopt_reader.py` | `_gtopt_reader.py` | fine |
| `_feed_reader.py` | `_feed_reader.py` | fine |
| `_reconstruct.py` | `_reconstruct.py` | fine |

No action required unless the team wants `_io.py` → `_reader.py` for cross-package
grep consistency.

---

### P1-3 — Dataclass / typing strategy: stay with stdlib `dataclasses`, not pydantic

**Plan section:** §3.3.3 ("pydantic-style dataclass shapes")  
The plan uses the phrase "pydantic-style dataclass shapes". The codebase has zero
pydantic usage anywhere (confirmed by grep — `pydantic` is absent from
`pyproject.toml`). Introducing it for `Topology` and `Cells` would add a new
heavy dependency (pydantic v2 ≈ 500 kB compiled extension) for two dataclasses.

**Recommendation:** Use stdlib `@dataclass(slots=True)` + `typing.Annotated` for the
canonical schema types. The "runtime validation" that pydantic provides is only needed
at the feed's I/O boundary (one write, one read); validate there with explicit checks
rather than a full schema library.

```python
# gtopt_canonical_feed/schema.py
from __future__ import annotations
from dataclasses import dataclass, field

@dataclass(slots=True, frozen=True)
class GeneratorRecord:
    uid: int
    name: str
    bus_uid: int
    pmin: float
    pmax: float
    declared_mc: float | None
    kind: str  # Literal["thermal","hydro","battery","profile"] is ideal here
    segments: list[tuple[float, float]] | None = field(default=None)
```

If the team later needs stronger validation (e.g. for a REST endpoint), pydantic can be
introduced at that boundary only, without coupling the core schema to it.

---

### P1-4 — Pandas is the correct choice for the hot path at 10–100k rows per cell

**Plan section:** §4.2 (per-cell tables), §4.3 (zone partition)  
At 10–100k rows per cell, `pd.read_parquet` with `columns=` projection is the right
call. Polars or PyArrow-direct would be marginally faster at loading but would add a
new dependency and diverge from every other script in the tree. The existing
`_reader.py:L32` already uses `pd.read_parquet` unconditionally.

**One anti-pattern to avoid in the per-cell loop (§4.2):**
The plan describes iterating over cells. If implemented naively as:

```python
for (s, p, b), group in gen_sol.groupby(["scene", "stage", "block"]):
    ...  # classify each group
```

this is fine for moderate cell counts. The risk is if the inner step does a
`df.iterrows()` over generators per cell — that creates an O(cells × generators)
Python loop. The classifier should operate on vectorised per-cell DataFrames merged
with the static lookup, not row-by-row.

**Proposed hot-path shape:**

```python
# merge generation + static gen attributes once per run, classify vectorised
merged = gen_sol.merge(gen_df, on="gen_uid")
merged["status"] = _classify_vectorised(merged, lmp_by_bus)
```

See §5 (Pandas hot-path notes) for elaboration.

---

### P1-5 — `--mode` dispatch: use a dispatch dict, not a strategy class hierarchy

**Plan section:** §3 (mode matrix), §5 (CLI)  
The plan describes four modes: `simulated`, `real`, `real-reconstruct`, `compare`. A
strategy-class hierarchy (`class SimulatedMode(BaseMode): ...`) for four short code
paths is over-engineered and harder to grep. `gtopt_compare/main.py:L1369` already
demonstrates the correct pattern for this codebase:

```python
_CASES: dict[str, _CaseFn] = {"simulated": _run_simulated, "real": _run_real, ...}

def main(argv=None) -> int:
    ...
    fn = _MODES.get(args.mode)
    return fn(args)
```

Use a `dict[str, Callable[[argparse.Namespace], int]]` keyed on `--mode` string.
Each callable does the reader selection + algorithm invocation. This is flat, pylint-
clean, and directly matches the 4-mode space.

---

### P1-6 — CLI library: keep argparse; no click/typer

**Plan section:** §5, §9.5  
Every existing script uses argparse. `gtopt_compare/main.py` is the closest analogue
(complex multi-path CLI, 1500 lines, all argparse). Introducing click or typer for the
two new scripts would create an inconsistent ecosystem and a new dependency for
marginal benefit. Stay with argparse.

One specific recommendation for `gtopt_marginal_units`: the `--input {gtopt,feed}` ×
`--mode {simulated,real,real-reconstruct,compare}` matrix has invalid combinations
(e.g. `--input gtopt --mode real`). Enforce this with `argparse` post-parse validation
(after `parse_args`, check `args.input` × `args.mode` and call `parser.error(...)`)
rather than letting it fail deep in a reader.

---

### P1-7 — HTTP client for `cen2gtopt`: `requests` is correct; no `httpx`

**Plan section:** §9.1.1 (one-shot CLI), §9.2.1 (pure-requests GETs)  
The plan confirms v1 is one-shot per invocation, no async needed. `requests` is already
in the Python ecosystem on every CI runner and is simpler to test with `responses` or
`unittest.mock`. `httpx` buys nothing for synchronous one-shot downloads and adds a
dependency. Stay with `requests`.

---

### P1-8 — Error handling: one consistent pattern for all four exit codes

**Plan section:** §5 (exit codes), §9.5.2  
The plan defines exit codes 0/2/3/4. The recommended pattern (consistent with
`gtopt_compare/main.py:L1518`):

```python
# main.py
def main(argv=None) -> int:
    args = _parse_args(argv)
    try:
        rc = _run(args)
    except MissingInputError as exc:      # exit 3 — no output written
        log.error("%s", exc)
        return 3
    except PartialOutputError as exc:     # exit 4 — partial file exists
        log.error("%s", exc)
        return 4
    return rc   # 0 = clean, 2 = unattributed cells present

class MissingInputError(ValueError): ...
class PartialOutputError(RuntimeError): ...
```

Key rules:
- Raise specific subclasses, not bare `RuntimeError` / `Exception`.
- Never catch `Exception` and discard it.
- Use `raise ... from exc` when wrapping a lower-level exception.
- Exit 4 with `<out>.partial` written before the `PartialOutputError` is raised (write
  what you have, then signal the failure).

---

### P1-9 — Naming convention: `bus_uid`, `gen_uid`, `line_uid` are correct

**Plan section:** §3.3.3 (schema), §4.1 (static lookups)  
Per `feedback_naming_convention`, strong types use `_uid` suffixes. The plan already
uses `bus_uid`, `gen_uid`, `line_uid`, `bus_a_uid`, `bus_b_uid` consistently in the
schema definition. The dataclass fields must preserve these names exactly — do not
abbreviate to `bus`, `gen`, `line` in the DataFrame columns that cross module
boundaries, since `_reader.py`'s existing `get_generator_info()` returns a frame with
column `"bus"` (a raw string reference, not a UID). The new frames should use
`"bus_uid"` to avoid silent joins on the wrong column type.

One specific flag: `_reader.py:L84` stores `bus = g.get("bus", "")` as a string (name
or raw UID), and `get_line_info()` stores `bus_a` / `bus_b` similarly. When
`_gtopt_reader.py` builds `gen_df` it should normalise these to `int` and rename them
`bus_uid`, `bus_a_uid`, `bus_b_uid` at the reader boundary.

---

### P1-10 — Logging: `gtopt_<package>.<module>` naming is the right convention

**Plan section:** §7 (tests), §8 (docs)  
The plan does not specify a logger-name convention. The existing pattern in
`_reader.py:L12` is `log = logging.getLogger(__name__)`, which with packages becomes
`gtopt_check_output._reader`, etc. Use the same pattern — `__name__` will resolve
to `gtopt_marginal_units._classify`, `gtopt_canonical_feed.io`, etc. Do **not**
hard-code a logger name string. Never call `logging.basicConfig(...)` inside a library
module; only `main()` should configure logging (follow `gtopt_compare/main.py:L1480`).

---

### P1-11 — Dependencies: one new dep (`networkx`) needs scrutiny

**Plan section:** §4.3 (connected components for zone partition)  
The plan uses connected-components over the line graph. For the SEN topology (a few
hundred buses), this is a tiny graph. `networkx` is ~5 MB of pure Python and pulls in
`scipy` on some platforms. The existing codebase does not depend on it.

**Options, ordered by preference:**

1. **Hand-roll BFS/DFS** (≤ 20 lines): the graph is tiny, the operation is standard.
   Zero new dependency. Example:

   ```python
   def _connected_components(edges: list[tuple[int, int]], nodes: set[int]):
       adj: dict[int, set[int]] = {n: set() for n in nodes}
       for a, b in edges:
           adj[a].add(b); adj[b].add(a)
       visited: set[int] = set()
       components = []
       for n in nodes:
           if n not in visited:
               comp, stack = set(), [n]
               while stack:
                   v = stack.pop()
                   if v not in visited:
                       visited.add(v); comp.add(v)
                       stack.extend(adj[v] - visited)
               components.append(comp)
       return components
   ```

2. **Use `scipy.sparse.csgraph.connected_components`**: scipy is already pulled in by
   numpy's transitive deps on many installations. Slightly opaque but very fast.

3. **Add networkx**: only if other Phase-2 graph work (PTDF estimation via `nx.linalg`)
   is also needed. PTDF construction from scratch requires a spanning tree and matrix
   inversions — that is the one case where scipy + networkx together pay for themselves.

**Recommendation:** Option 1 for Phase 1 (zone partition only). Revisit for Phase 2
if the PTDF estimator (§4.7 step R1) needs graph algebra.

---

### P1-12 — Testability of `_gtopt_reader.py`: fully unit-testable, no C++ needed

**Plan section:** §3.2, §7.1 (`test_io_roundtrip.py`)  
`_gtopt_reader.py` reads parquet files from a directory and a JSON planning file. Both
are pure Python / pandas operations. The C++ gtopt binary is only needed to *produce*
the parquet files, not to *read* them. Therefore:

- Unit tests should ship synthetic parquet fixtures in `tests/data/` (small, committed,
  no network, no gtopt binary required).
- Integration tests that check the full round-trip (run gtopt → read output) should be
  gated on `@pytest.mark.integration` with `GTOPT_BIN` available.

This is the same split already used by `gtopt_check_output` and `gtopt_results_summary`.

One design-time trap: the plan says `_gtopt_reader.py` calls
`_reader.load_planning(json_path)` from `gtopt_check_output`. That is a cross-package
import from `gtopt_check_output._reader`. Per the project's convention of not importing
from private helpers of other packages, either:
- expose `load_planning` as a public symbol from `gtopt_check_output.__init__`, or
- copy the three-line JSON-load helper into `gtopt_canonical_feed/io.py` (no real
  duplication — it is a one-liner wrapping `json.load`).

---

## 4. Findings — Priority P2 (nice to have)

### P2-1 — `typing.Tuple`/`typing.List`/`typing.Optional` in the schema block

**Plan section:** §3.3.3  
The schema pseudo-code uses `list[tuple[float, float]] | None` (correct modern form).
Make sure the actual dataclass fields use `list[...]`, `tuple[...]`, `T | None` rather
than `typing.List`, `typing.Tuple`, `typing.Optional`. Python ≥ 3.10 is the target and
`disallow_untyped_defs = false` is already set, but new code in these packages should
set the standard.

### P2-2 — `NaN` vs `pd.NA` boundary is documented but not enforced

**Plan section:** §6 edge-case 3 ("use `pd.NA` / `Optional` in-memory, NaN only at
Parquet boundary")  
Per `feedback_no_nan`, NaN must not appear in intermediate logic. Add a one-line comment
in `_classify.py` (and the schema doc) stating: "Use `pd.NA` for missing floats in
DataFrames; cast to `np.nan` only in the Parquet writer." This prevents the common
mistake of comparing `mc == pd.NA` (always False) vs `mc is pd.NA` or `pd.isna(mc)`.

### P2-3 — `pyproject.toml` `known_first_party` list needs updating

**Plan section:** §11 (P0.2, P1.0, P2.0)  
`pyproject.toml:L150` has a hardcoded `known_first_party` list for isort. Add
`gtopt_canonical_feed` (or `_canonical_feed`), `gtopt_marginal_units`, and `cen2gtopt`
to this list before the first PR so isort groups imports correctly.

### P2-4 — `manifest.json` schema version field should be a `Literal` type

**Plan section:** §3.3.3 (manifest.json), §11 P0.2  
The manifest tracks a `schema_version` string. Type it as `Literal["1.0"]` in the
dataclass so mypy flags any code that tries to write a different version without updating
the type. This makes schema-version bumps a compile-time error rather than a runtime
surprise.

### P2-5 — Coverage threshold will need updating for new packages

**Plan section:** §11 (test coverage)  
`pyproject.toml:L133` lists the packages measured for coverage. Add
`gtopt_canonical_feed`, `gtopt_marginal_units`, `cen2gtopt` to `[tool.coverage.run]
source` and `[tool.coverage.report]`. The 83% threshold is currently applied only to the
listed packages; new packages start at 0% until added.

### P2-6 — Public functions in the plan lack `-> None` return annotations

**Plan section:** throughout  
The plan's prose sketches use `def classify(...)` without return types. New code in these
packages should annotate all public functions. `mypy` with `disallow_untyped_defs =
false` will not enforce this, but `pylint` may emit `W0107` or design violations if
functions grow large. A minimum: annotate `classify`, `_connected_components`,
`_run_simulated`, `_run_real`, `_run_reconstruct`, and the reader entry points.

---

## 5. Pandas / NumPy hot-path notes

The classifier's critical path is §4.2–4.5: for each cell `(s, p, b)`, classify every
generator and zone. With 10k cells × 100 generators = 1M classification rows, the
pandas implementation must stay vectorised.

**Anti-pattern to avoid (O(cells × generators) Python loop):**

```python
# BAD — do not do this
for (s, p, b), cell_gen in gen_sol.groupby(["scene","stage","block"]):
    for _, row in cell_gen.iterrows():
        status = _classify_one(row["dispatch"], ...)
```

**Recommended pattern (fully vectorised):**

```python
# GOOD — merge then classify in bulk
gen_sol = gen_sol.merge(gen_df[["gen_uid","pmin","pmax","gcost","kind"]], on="gen_uid")
lmp_long = lmp_df.melt(id_vars=["scene","stage","block"], var_name="bus_uid", ...)
gen_sol = gen_sol.merge(lmp_long, on=["scene","stage","block","bus_uid"])
gen_sol["status"] = _classify_series(gen_sol)  # numpy boolean masks
```

`_classify_series` uses `np.select` or `pd.Categorical` + boolean series for the
priority-ordered rule table in §4.4 — all 8 rules map cleanly to column comparisons.

**Zone partition** (§4.3) is graph work on a small graph (hundreds of nodes) — not a
pandas hot path. Call it once per cell on the saturated-line subset; Python-loop cost
is negligible.

**Measurement recommendation:** before optimising, add a `--profile` flag that dumps
`cProfile` output. The per-cell `groupby` pattern above is fast at 10k cells; only
profile-driven changes are warranted beyond the vectorised merge.

---

## 6. CLI ergonomics

Walking `gtopt-marginal-units` from a first-time user's perspective (§5):

1. **`--input` + `--mode` matrix is too wide for naive `--help`**: 4 × 4 = 16
   combinations, only 4 valid. Recommend grouping into two argparse argument groups
   (`"gtopt input options"` and `"feed input options"`) and using a mutually exclusive
   group for `--input`. Invalid combinations should be caught with `parser.error()` after
   `parse_args()` with a message like:
   `"--mode real requires --input feed; got --input gtopt"`.

2. **Numeric tolerance flags need units in `metavar`:**

   ```python
   parser.add_argument("--tol-price", type=float, default=1e-3,
       metavar="$/MWh", help="LMP vs MC gap tolerance (default 1e-3 $/MWh).")
   ```

3. **`--scenes`, `--stages`, `--blocks` need a `type=` callable** that parses
   `"1,2"` and `"1-3"` (range notation). Define a small `_parse_intlist(s: str) ->
   list[int]` helper in `main.py` and pass it as `type=_parse_intlist`.

4. **`cen2gtopt --include` is a multi-value flag** (plan: `"dispatch,demand,lmp,flows"`).
   Use `nargs="+"` + `choices=["dispatch","demand","lmp","flows"]` rather than a single
   comma-joined string, so the shell can tab-complete each item.

5. **Exit codes should appear in `--help` / `epilog`** (as in `gtopt_compare/main.py:L51`
   which documents exit codes in the module docstring). The plan's exit-code table in §5
   should be reproduced verbatim in `argparse`'s `epilog`.

6. **`--api-key` for `cen2gtopt`** should document that it reads from `$CEN_API_KEY`
   by default: `default=os.environ.get("CEN_API_KEY")`. Never hard-code a credential
   default.

---

## 7. Testability opportunities

1. **`_classify` is a pure function and is trivially parametrized.** Write
   `test_classify.py` as `@pytest.mark.parametrize` over the 8-row rule table in §4.4
   plus boundary and degenerate cases. No fixtures needed — just floats.

2. **`_connected_components` (whichever implementation is chosen) is also pure** and
   should have its own parametrized test in `test_zones.py` covering the three cases
   the plan specifies (§7.1): chain + saturated mid-line, ring all-saturated, single-bus.

3. **`_gtopt_reader.py` integration tests** should use `pytest.fixtures` with
   `tmp_path` + synthetic parquet files. Do **not** use `@pytest.mark.integration` for
   the parquet round-trip — that test should run in the default suite since it needs no
   binary.

4. **`cen2gtopt` HTTP tests** should use `unittest.mock.patch("requests.get")` or the
   `responses` library (zero-dependency mock for `requests`). The SIP client test in
   `tests/test_sip_client.py` (§9.6) should work entirely offline against JSON fixtures.

5. **Schema round-trip test (P0.3 gold fixture)** is the most important test in the
   whole plan. It should live in `gtopt_canonical_feed/tests/test_roundtrip.py` and
   import both the writer (from `gtopt_canonical_feed.io`) and the reader (from
   `gtopt_marginal_units._feed_reader`) — making it a cross-package integration test
   that is still CI-stable (no network, no binary). This test locks the schema contract
   between Phase 1 and Phase 2.

6. **Property test with Hypothesis** (§7.3): `_classify` is an ideal Hypothesis target.
   Write the `@given(st.floats(...))` test early — it will surface boundary bugs in the
   priority-ordered rule table (e.g. what happens when `pmin == pmax`).

---

## 8. Already good

- **Clean phase boundary** (§1.5): the schema-only contract between Phase 1 and
  Phase 2 via an on-disk parquet means neither script imports the other. This is the
  single most important architectural decision and it is correct.
- **Exit-code semantics** (§5, §9.5.2) are well-specified and distinguish "partial
  output" (exit 4) from "no output" (exit 3) — consistent with the project's convention
  of never silently swallowing failure.
- **`feedback_no_nan` followed** (§6 edge-case 3): the plan explicitly uses `pd.NA` in
  memory and `NaN` only at the Parquet boundary.
- **`feedback_naming_convention` followed** for `bus_uid` / `gen_uid` / `line_uid` in
  the canonical schema.
- **`feedback_proactive_tests`** is fully addressed: §7 specifies nine test files with
  explicit assertions, a Hypothesis suite, and integration tests against all five IEEE
  benchmark cases.
- **`feedback_pylint_parallel`** / **`feedback_pytest_parallel`**: the plan explicitly
  references both feedback items and instructs the implementation agent to fan out
  tests via parallel agents.
- **One-shot CLI design for `cen2gtopt`** (§9.1.1): explicitly defers scheduling to
  a future wrapper — correct scoping that avoids daemon complexity in v1.
- **Mode-agnostic classifier** (§3): the `_classify` function only sees the canonical
  schema, not the data source. This is the key enabler for unit-testability.

---

## 9. Verdict

`PYTHON VERDICT: MINOR`

No correctness-breaking P0 issues in the *existing* code (none exists yet). P0 findings
are pre-implementation architecture defects in the plan that must be resolved before the
first skeleton PR lands: `_canonical_feed` package visibility (P0-1), `main()` return
type (P0-2), `assert` for runtime validation (P0-3), and `requests` timeout (P0-4).
P1 findings are straightforward modernisation items (dispatch dict vs strategy classes,
stdlib dataclasses vs pydantic, BFS vs networkx). All are fixable in the skeleton phase
without rework.
