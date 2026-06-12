# Phase-0 Schema Lock-in — Implementation Plan

> Owner: schema-owner agent (Phase 0).
> Output of this plan: a frozen contract between `gtopt_marginal_units`
> (Phase 1) and `cen2gtopt` (Phase 2), implemented as the shared
> `scripts/_canonical_feed/` helper package.
> See `docs/scripts/gtopt_marginal_units_plan.md` §1.5, §3.3, §4.8,
> §11 (P0.1–P0.3), §12.1.

This document is **planning only**. No production code is written
here. Everything below is a recipe for the next agent to execute
mechanically.

---

## 0. Decisions taken up-front

| Decision | Choice | Rationale |
|---|---|---|
| Dataclass library | `dataclasses` (stdlib) | `grep` of `scripts/` shows zero `pydantic` / `attrs` imports; `sddp2gtopt/entities.py` already uses `@dataclass`. Stdlib only — no new dep on `scripts/pyproject.toml`. |
| Validation | Hand-rolled `__post_init__` checks + a `validate(self)` method that returns `list[str]` of errors | Mirrors `gtopt_check_json/_topology_checks.py`. Pydantic-style without the dep. |
| On-disk format | Parquet (one file per long-form table), `manifest.json` JSON | §3.3.3 prescribes Parquet; matches `scripts/gtopt_check_output/_reader.py:read_table`. |
| Compression | `zstd` for parquet (matches `feedback_no_alpha_fix_for_compress` / project default `output_compression=zstd`) | One codec across the project; `lz4` is for in-memory snapshots only. |
| Hash algo | `sha256`, hex-encoded | Stdlib `hashlib`; matches `gtopt_check_fingerprint`. |
| Python version | 3.10+ (`from __future__ import annotations`) | `scripts/pyproject.toml` `requires-python = ">=3.8"`, mypy `python_version = "3.10"`. |
| Schema version | `"1.0.0"` semver string | Frozen at end of Phase 0; bump rules in §8 below. |

---

## 1. Package layout

All paths absolute from repo root.

```
/home/marce/git/gtopt-hygiene/scripts/_canonical_feed/
  __init__.py        # re-exports public API: read_feed, write_feed,
                     # verify_feed, Topology, Cells, Manifest,
                     # SCHEMA_VERSION
  topology.py        # Bus, Generator, Line dataclasses + Topology
                     # container; to_frames() / from_frames() helpers
  cells.py           # Cells dataclass holding the long-form pandas
                     # frames (dispatch, lmp, flow, flow_dual, load,
                     # ens); column-name and dtype constants live here
  manifest.py        # Manifest dataclass + JSON read/write +
                     # sha256 helpers + schema-version check
  feed_io.py         # read_feed(path) / write_feed(path, ...) /
                     # verify_feed(path); orchestrates topology +
                     # cells + manifest
  tests/
    __init__.py
    conftest.py      # path to tests/data/gold_feed/, fixture loader
    test_topology.py # Bus/Generator/Line dataclass invariants,
                     # validate() failure modes
    test_cells.py    # column dtype enforcement, NA handling for
                     # optional lmp/flow_dual columns
    test_manifest.py # JSON round-trip, hash mismatch, version
                     # mismatch
    test_roundtrip.py# §7 here: write_feed → read_feed → assert
                     # equal; missing-LMP path; hash-mismatch path
    data/
      gold_feed/     # the fixture from §5; pre-built parquet+JSON
        topology/
          bus.parquet
          generator.parquet
          line.parquet
        cells/
          dispatch.parquet
          lmp.parquet
          flow.parquet
          load.parquet
        manifest.json
      gold_feed_no_lmp/   # same case but with cells/lmp.parquet absent
        topology/
          bus.parquet
          generator.parquet
          line.parquet
        cells/
          dispatch.parquet
          flow.parquet
          load.parquet
        manifest.json
```

A leading `_` prefixes the package name to mark it private — neither
`gtopt_marginal_units` nor `cen2gtopt` is required to expose its
internals to users; they consume `_canonical_feed` directly.

Add to `scripts/pyproject.toml`:

* under `[tool.setuptools.packages.find].include`: `"_canonical_feed*"`
* under `[tool.pytest.ini_options].testpaths`: `"_canonical_feed/tests"`
* under `[tool.coverage.run].source`: `"_canonical_feed"`
* under `[tool.isort].known_first_party`: `"_canonical_feed"`

No `[project.scripts]` entry — this package has no CLI.

---

## 2. Dataclass shapes

All shapes verbatim from master plan §3.3.3. Stdlib `dataclasses`,
`from __future__ import annotations`, frozen where reasonable.

### 2.1 `topology.py`

```python
from dataclasses import dataclass, field
from typing import Optional

@dataclass(frozen=True, slots=True)
class Bus:
    uid: int                   # stable, user-controlled
    name: str                  # unique within the feed
    region: Optional[str] = None  # CEN: SEN sub-zone label; None for synthetic

@dataclass(frozen=True, slots=True)
class Generator:
    uid: int
    name: str
    bus_uid: int               # FK → Bus.uid
    pmin: float                # MW
    pmax: float                # MW; > pmin
    declared_MC: Optional[float] = None  # USD/MWh; None when piecewise-only
    kind: str = "thermal"      # one of {thermal, hydro, battery, profile}
    segments: Optional[list[tuple[float, float]]] = None
                               # piecewise: list of (q_MW, slope_USD_per_MWh)

@dataclass(frozen=True, slots=True)
class Line:
    uid: int
    bus_a_uid: int             # FK → Bus.uid
    bus_b_uid: int             # FK → Bus.uid
    tmax_ab: float             # MW; ≥ 0
    tmax_ba: float             # MW; ≥ 0
    active: bool = True

@dataclass
class Topology:
    buses: list[Bus]
    generators: list[Generator]
    lines: list[Line]

    def validate(self) -> list[str]:
        """Return a list of human-readable error strings; empty if OK."""
        # checks: unique uids per table, FK integrity gen.bus_uid →
        # Bus, line.bus_*.uid → Bus, pmin <= pmax, tmax_ab >= 0,
        # kind ∈ allowed set, segments monotone-nondecreasing slopes.

    def to_frames(self) -> dict[str, "pd.DataFrame"]: ...
    @classmethod
    def from_frames(cls, frames: dict[str, "pd.DataFrame"]) -> "Topology": ...
```

### 2.2 `cells.py`

`Cells` is **not** a list of dataclasses — it is a container of
*long-form* `pandas.DataFrame`s, exactly what §3.3.3 specifies. The
class is just a typed wrapper.

```python
from dataclasses import dataclass
from typing import Optional
import pandas as pd

# Column-name constants — single source of truth, no magic strings
# anywhere else (`feedback_no_magic_strings`).
COL_SCENE       = "scene"
COL_STAGE       = "stage"
COL_BLOCK       = "block"
COL_DATE_UTC    = "date_utc"     # alternative cell-key for real mode
COL_HOUR        = "hour"
COL_DATA_SOURCE = "data_source"  # "simulated" | "real"
COL_GEN_UID     = "gen_uid"
COL_BUS_UID     = "bus_uid"
COL_LINE_UID    = "line_uid"
COL_VALUE       = "value"

CELL_KEY_SIM    = (COL_SCENE, COL_STAGE, COL_BLOCK)
CELL_KEY_REAL   = (COL_DATE_UTC, COL_HOUR)

@dataclass
class Cells:
    """Long-form per-cell tables. Each frame has a cell-key prefix
    (either CELL_KEY_SIM or CELL_KEY_REAL), a `data_source` column,
    one *_uid column, and a `value` column (float64, NA-allowed).
    """
    dispatch:  pd.DataFrame                  # required
    load:      pd.DataFrame                  # required
    lmp:       Optional[pd.DataFrame] = None # optional (R3 path)
    flow:      Optional[pd.DataFrame] = None # optional
    flow_dual: Optional[pd.DataFrame] = None # simulated-only
    ens:       Optional[pd.DataFrame] = None # optional

    def cell_key_kind(self) -> str:
        """Returns "sim" or "real" based on which key columns are
        present in dispatch. Used by the schema check to enforce that
        all frames in a feed share one key kind."""

    def validate(self, topology: Topology) -> list[str]: ...
```

#### Long-form column contracts

Every cells table uses one of two key-column triples:

| Mode | Cell-key columns | Per-row columns | dtype |
|---|---|---|---|
| simulated | `scene`, `stage`, `block` | `data_source`, one of `{gen_uid, bus_uid, line_uid}`, `value` | `int64`, `int64`, `int64`, `string`, `int64`, `float64` |
| real | `date_utc`, `hour` | same | `datetime64[ns, UTC]`, `int64` (0..23), `string`, `int64`, `float64` |

* `value` is `float64` and **may be NA** for `lmp`, `flow_dual`,
  `ens` columns (NA → reconstructed by §4.7 R3 or absent).
* `data_source ∈ {"simulated", "real"}` per row (allows feeds that
  splice both — Phase-3 mode `compare`).
* No mixed key kinds within a single feed; `Cells.cell_key_kind()`
  enforces this.

### 2.3 `manifest.py`

```python
@dataclass
class Manifest:
    schema_version: str          # SemVer; gates compatibility (see §8)
    producer: str                # "gtopt_marginal_units" | "cen2gtopt" | "gold_fixture" | …
    producer_version: str        # version string from the producer's pyproject
    fetched_at: str              # ISO-8601 UTC, e.g. "2026-05-05T12:34:56Z"
    source_urls: list[str]       # CEN endpoints / file paths consulted
    file_hashes: dict[str, str]  # relative path → sha256 hex
    row_counts: dict[str, int]   # relative path → row count
    notes: str = ""              # free-form, audit-only
```

Helpers in `manifest.py`:

* `compute_sha256(path: Path) -> str`
* `Manifest.from_disk(path: Path) -> Manifest` (reads `manifest.json`)
* `Manifest.write(self, path: Path) -> None`
* `Manifest.verify(self, root: Path) -> list[str]` — recomputes file
  hashes, recomputes row counts, returns mismatches.

---

## 3. Parquet on-disk layout

Top-level directory the producer writes:

```
<feed_root>/
  topology/
    bus.parquet
    generator.parquet
    line.parquet
  cells/
    dispatch.parquet
    load.parquet
    lmp.parquet         # OPTIONAL — absent triggers §4.7 R3
    flow.parquet        # OPTIONAL
    flow_dual.parquet   # OPTIONAL — simulated-only
    ens.parquet         # OPTIONAL
  manifest.json
```

`<feed_root>` is a *directory*, not a single `.parquet` file. The
master-plan layout `canonical_feed.parquet/topology/bus.parquet`
(§3.3.3) is treated as a hint, not a literal extension; the directory
*may* end in `.parquet` for cosmetic reasons but the readers/writers
do not require it.

### 3.1 Per-file column / dtype tables

#### `topology/bus.parquet`

| col | dtype | nullable | index | notes |
|---|---|---|---|---|
| `uid` | `int64` | no | yes (unique) | |
| `name` | `string` (`pa.string()`) | no | unique | |
| `region` | `string` | yes | — | |

#### `topology/generator.parquet`

| col | dtype | nullable | index |
|---|---|---|---|
| `uid` | `int64` | no | unique |
| `name` | `string` | no | unique |
| `bus_uid` | `int64` | no | FK → bus.uid |
| `pmin` | `float64` | no | |
| `pmax` | `float64` | no | |
| `declared_MC` | `float64` | yes | |
| `kind` | `string` | no | enum {thermal,hydro,battery,profile} |
| `segments` | `list<struct<q:float64, slope:float64>>` | yes | piecewise |

#### `topology/line.parquet`

| col | dtype | nullable |
|---|---|---|
| `uid` | `int64` | no (unique) |
| `bus_a_uid` | `int64` | no (FK) |
| `bus_b_uid` | `int64` | no (FK) |
| `tmax_ab` | `float64` | no |
| `tmax_ba` | `float64` | no |
| `active` | `bool` | no |

#### `cells/<table>.parquet` (dispatch, load, lmp, flow, flow_dual, ens)

Long-form, one schema reused six times (only the `_uid` column name
varies — gen for dispatch, bus for lmp/load/ens, line for flow/flow_dual):

| col | dtype | nullable | notes |
|---|---|---|---|
| `scene` | `int64` | no (sim only) | omitted in real-mode feed |
| `stage` | `int64` | no (sim only) | |
| `block` | `int64` | no (sim only) | |
| `date_utc` | `timestamp[ns, UTC]` | no (real only) | omitted in sim feed |
| `hour` | `int64` | no (real only) | 0..23 |
| `data_source` | `string` | no | "simulated" \| "real" |
| `gen_uid` / `bus_uid` / `line_uid` | `int64` | no | FK → topology |
| `value` | `float64` | yes | NA permitted for lmp/flow_dual/ens |

Compression: `zstd` (level 3, default).
Sort order at write time: `(*cell_key, *_uid)` ascending — makes
verify_feed bytewise-comparable across producers.

### 3.2 `manifest.json`

UTF-8, JSON-encoded, two-space indent, keys sorted. See §4.

---

## 4. `manifest.json` schema and example

### 4.1 Field-by-field

| field | type | required | constraint |
|---|---|---|---|
| `schema_version` | string (SemVer) | yes | matches `_canonical_feed.SCHEMA_VERSION` major |
| `producer` | string | yes | free-form name |
| `producer_version` | string | yes | typically `pyproject.version` |
| `fetched_at` | string | yes | ISO-8601 with `Z` suffix |
| `source_urls` | list[string] | yes (may be `[]`) | absolute URLs / file paths |
| `file_hashes` | object[string→string] | yes | keys: paths relative to feed root, posix-slashes; values: hex sha256 |
| `row_counts` | object[string→int] | yes | same keys as `file_hashes` |
| `notes` | string | no | default `""` |

### 4.2 Example

```json
{
  "schema_version": "1.0.0",
  "producer": "cen2gtopt",
  "producer_version": "0.1.0",
  "fetched_at": "2026-05-05T12:34:56Z",
  "source_urls": [
    "https://www.coordinador.cl/operacion/graficos/operacion-real/costo-marginal-real/",
    "https://sipub.api.coordinador.cl/sipubv2/v1/operacion/generacion-real"
  ],
  "file_hashes": {
    "topology/bus.parquet":         "f4ca…",
    "topology/generator.parquet":   "9ae1…",
    "topology/line.parquet":        "31cd…",
    "cells/dispatch.parquet":       "a012…",
    "cells/load.parquet":           "bb77…",
    "cells/lmp.parquet":            "5e23…"
  },
  "row_counts": {
    "topology/bus.parquet":          3,
    "topology/generator.parquet":    3,
    "topology/line.parquet":         3,
    "cells/dispatch.parquet":       72,
    "cells/load.parquet":           72,
    "cells/lmp.parquet":            72
  },
  "notes": "Phase-0 gold fixture; 3-bus 3-unit 24-hour synthetic case."
}
```

---

## 5. Gold fixture spec (3-bus, 3-unit, 24-hour)

The fixture is purely synthetic — its expected LMPs are computed by
hand below so the round-trip test, the §4.7 reconstruction test, and
the merit-order test all assert on a *closed-form* answer.

### 5.1 Topology

```
bus.parquet
| uid | name | region |
|-----|------|--------|
|  1  | b_n  | NORTH  |
|  2  | b_c  | CENTER |
|  3  | b_s  | SOUTH  |

generator.parquet
| uid | name | bus_uid | pmin | pmax | declared_MC | kind    |
|-----|------|---------|------|------|-------------|---------|
| 10  | g_cheap  | 1 |   0  | 80   |  10.0       | thermal |  ← coal-ish
| 11  | g_mid    | 2 |   0  | 60   |  30.0       | thermal |  ← gas-ish
| 12  | g_peak   | 3 |   0  | 50   |  80.0       | thermal |  ← diesel

line.parquet
| uid | bus_a | bus_b | tmax_ab | tmax_ba | active |
|-----|-------|-------|---------|---------|--------|
|  20 |   1   |   2   |   50    |   50    |  true  |
|  21 |   2   |   3   |  200    |  200    |  true  |
|  22 |   1   |   3   |  200    |  200    |  true  |  ← effectively unbounded
```

The first line (uid 20) is the only constraint that ever binds; the
other two are sized so they are non-binding for every hour. We use
single-bus PTDF intuition (lossless DC-OPF, all reactances equal): a
uniform demand at b_s draws roughly evenly from b_n/b_c when prices
are similar.

### 5.2 Demand profile (per bus, per hour)

24-hour load shape in MW. Three phases: off-peak (h 0–7), shoulder
(h 8–15), peak (h 16–23):

```
hour  | load[b_n] | load[b_c] | load[b_s] | total
------+-----------+-----------+-----------+-------
0..7  |    20     |    20     |    30     |   70   (off-peak)
8..15 |    30     |    30     |    60     |  120   (shoulder)
16..23|    40     |    40     |    80     |  160   (peak)
```

### 5.3 Expected dispatch (hand-computed)

We dispatch by merit order subject to the 50 MW cap on line 20
(b_n→b_c). For the fixture we pin generation **at each unit's bus**;
load travels over the network. With three roughly equal-resistance
lines, the saturation pattern of line 20 is the only one that ever
flips a zone partition.

| Hours | g_cheap (b_n) | g_mid (b_c) | g_peak (b_s) | line 20 flow | sat? |
|-------|---------------|-------------|--------------|--------------|------|
| 0–7   |  70 MW        |    0 MW     |    0 MW      |    35 MW     | no   |
| 8–15  |  80 MW (cap)  |   40 MW     |    0 MW      |    50 MW     | YES (cap-pinned) |
| 16–23 |  80 MW (cap)  |   60 MW (cap) | 20 MW       |    50 MW     | YES |

* In hours 0–7, `g_cheap` alone covers the 70 MW load via the network;
  no line saturated; **single zone**.
* In hours 8–15, `g_cheap` is at `pmax=80`, line 20 saturated at 50
  MW; the system splits into two zones: `{b_n}` (cheap-pinned) and
  `{b_c, b_s}` (mid is interior). The marginal unit in zone-N is
  `capped_pmax g_cheap` (degenerate); in zone-CS it is `marginal
  g_mid`.
* In hours 16–23, both `g_cheap` and `g_mid` capped, `g_peak`
  interior. Zones: `{b_n}`, `{b_c}` (because line 20 is saturated and
  b_c→b_s is non-binding so b_c stays with b_s)…

Working through R2 + R3 of §4.7 carefully:

| Hours | Zone | members | interior unit (R3) | λ_z (USD/MWh) | marginal status |
|-------|------|---------|--------------------|---------------|-----------------|
| 0–7   | Z1   | {b_n,b_c,b_s} | g_cheap (interior)  | 10.0  | g_cheap = marginal |
| 8–15  | Z1n  | {b_n}         | (none — pmax pinned)| 10.0  | g_cheap = capped_pmax → degenerate, R3 fallback rule |
| 8–15  | Z1cs | {b_c, b_s}    | g_mid (interior)    | 30.0  | g_mid = marginal |
| 16–23 | Z1n  | {b_n}         | (none — pmax pinned)| 10.0  | g_cheap = capped_pmax → degenerate |
| 16–23 | Z1cs | {b_c, b_s}    | g_peak (interior, in b_s but same zone) | 80.0 | g_peak = marginal |

These are the hand-computed expected LMPs the round-trip test asserts
on (within `tol_price = 1e-3 · max(|λ|, 1)`).

### 5.4 Files in `tests/data/gold_feed/`

* `topology/bus.parquet`        — 3 rows
* `topology/generator.parquet`  — 3 rows
* `topology/line.parquet`       — 3 rows
* `cells/dispatch.parquet`      — 24 hours × 3 gens = 72 rows
* `cells/load.parquet`          — 24 × 3 = 72 rows
* `cells/lmp.parquet`           — 24 × 3 = 72 rows
* `cells/flow.parquet`          — 24 × 3 = 72 rows  (we publish all
  three lines for every hour for completeness)
* `manifest.json`

`gold_feed_no_lmp/` is identical except `cells/lmp.parquet` and its
manifest entries are absent — used to test the §4.7 R3 path.

A small helper, `tests/_build_gold.py`, generates both fixtures from
the tables above in one pass; the fixtures are checked into the
repo (so CI doesn't regenerate, keeping the hashes stable), but the
helper stays around so the schema-owner can rebuild on a version
bump.

The fixture is built once by the schema-owner agent and committed.
Hashes in `manifest.json` are computed at commit time and never
recomputed by tests — `verify_feed` *checks* them, it does not
update them.

---

## 6. Public API

Imported as `from _canonical_feed import …`.

```python
SCHEMA_VERSION: str = "1.0.0"

def read_feed(
    feed_root: Path,
    *,
    require_lmp: bool = False,
    verify_hashes: bool = True,
) -> tuple[Topology, Cells, Manifest]:
    """Load topology + cells + manifest from a feed directory.

    Raises:
        FileNotFoundError: feed_root does not exist or topology files missing.
        SchemaVersionError: manifest.schema_version major != SCHEMA_VERSION major.
        FeedHashError: verify_hashes=True and any sha256 disagrees.
        FeedSchemaError: column dtypes / FKs invalid.
    """

def write_feed(
    feed_root: Path,
    topology: Topology,
    cells: Cells,
    *,
    producer: str,
    producer_version: str,
    source_urls: list[str] = (),
    fetched_at: Optional[str] = None,  # default: now-UTC
    notes: str = "",
) -> Manifest:
    """Write all parquet files + manifest.json. Returns the
    populated Manifest (with hashes + row_counts filled in)."""

def verify_feed(feed_root: Path) -> list[str]:
    """Re-hash every parquet file and compare against
    manifest.json:file_hashes. Returns a list of mismatch
    descriptions; empty list means OK. Does not raise."""
```

Three custom exceptions in `_canonical_feed.exceptions` (or top-level
`__init__.py`):

* `SchemaVersionError(Exception)` — major-version mismatch
* `FeedHashError(Exception)` — sha256 mismatch on at least one file
* `FeedSchemaError(Exception)` — dtype / FK / required-column failure

---

## 7. Round-trip test outline (`tests/test_roundtrip.py`)

Pseudocode — actual implementation goes in P0.3. Pytest-only, no
new deps beyond `pyproject.toml` (`pandas`, `pyarrow`, `pytest`,
`pytest-xdist`). All seven tests read fixtures from
`tests/data/gold_feed[_no_lmp]/`.

```python
def test_roundtrip_writes_and_reads_back(tmp_path):
    # read_feed(GOLD) → write_feed(tmp_path) → read_feed back;
    # assert Topology equality; pd.testing.assert_frame_equal on
    # every cells frame; assert lmp/flow_dual/ens None-ness preserved.

def test_missing_lmp_round_trips(tmp_path):
    # gold_feed_no_lmp: cells.lmp is None;
    # write_feed must NOT emit cells/lmp.parquet;
    # round-trip preserves None.

def test_require_lmp_raises_when_absent():
    # read_feed(GOLD_NO_LMP, require_lmp=True) → FeedSchemaError.

def test_schema_version_mismatch_raises(tmp_path):
    # write feed; mutate manifest.schema_version to "99.0.0";
    # read_feed → SchemaVersionError.

def test_hash_mismatch_raises(tmp_path):
    # write feed; append a byte to cells/dispatch.parquet;
    # read_feed(verify_hashes=True) → FeedHashError.

def test_verify_feed_returns_mismatches_without_raising(tmp_path):
    # verify_feed returns list, never raises.
    # Corrupt cells/load.parquet → mismatches non-empty,
    # contains "load.parquet" substring.

def test_gold_fixture_is_pinned():
    # verify_feed(GOLD) == [] AND verify_feed(GOLD_NO_LMP) == [].
    # Catches accidental fixture drift in review.
```

---

## 8. Schema versioning rules

`SCHEMA_VERSION` is a semver string. Compatibility is checked **on
major** at `read_feed` time:

| Change | Bump |
|---|---|
| Add a new optional column to an existing cells frame | minor |
| Add a brand-new optional cells frame (e.g. `reservoir_dual`) | minor |
| Add a new optional field on a topology dataclass | minor |
| Tighten validation rules (reject inputs that previously passed) | major |
| Rename / remove an existing column | major |
| Change a column's dtype | major |
| Add a new **required** column or required cells frame | major |
| Change `cell_key` semantics or names | major |

**Back-compat policy.** Phase 1 and Phase 2 both depend on
`_canonical_feed`; the major must match in lockstep. Minor and patch
bumps are forward-compatible — older readers ignore unknown columns
and treat missing optional frames as `None`. Each major bump requires
a coordinator (the user) to authorise it; agents never silently bump.

The `manifest.json:schema_version` is checked as follows:

```python
fmaj, _, _ = manifest.schema_version.split(".")
cmaj, _, _ = SCHEMA_VERSION.split(".")
if int(fmaj) != int(cmaj):
    raise SchemaVersionError(...)
```

A minor mismatch is logged at WARNING level but accepted.

---

## 9. Step-by-step coding order

Each step is mechanical — no design decisions remain.

1. **Skeleton + pyproject wiring.**
   Create `scripts/_canonical_feed/{__init__.py, topology.py,
   cells.py, manifest.py, feed_io.py, exceptions.py}` as empty
   stubs. Edit `scripts/pyproject.toml` to add the package to
   `find.include`, `pytest.testpaths`, `coverage.source`, and
   `isort.known_first_party`. Run `ruff format` and `pylint` on
   the empty files to confirm CI is green.

2. **Topology dataclasses.**
   Implement `Bus`, `Generator`, `Line`, `Topology` per §2.1 in
   `topology.py`. Add `Topology.validate`, `to_frames`,
   `from_frames`. Land the unit test `tests/test_topology.py`
   covering: unique-uid enforcement, FK integrity, pmin>pmax
   rejection, kind-enum check.

3. **Cells dataclass + column constants.**
   Implement `Cells` and the `COL_*` / `CELL_KEY_*` constants per
   §2.2 in `cells.py`. Add `Cells.cell_key_kind` and
   `Cells.validate(topology)`. Land `tests/test_cells.py`:
   sim-vs-real key kinds, dtype enforcement, FK to topology,
   NA-allowed columns.

4. **Manifest + hash helpers.**
   Implement `Manifest`, `compute_sha256`, `Manifest.from_disk`,
   `Manifest.write`, `Manifest.verify` per §2.3 / §4. Define
   `SCHEMA_VERSION = "1.0.0"`. Land `tests/test_manifest.py`:
   JSON round-trip, hex-sha256 stability, missing-field error
   messages.

5. **Feed I/O orchestrator.**
   Implement `read_feed`, `write_feed`, `verify_feed` in
   `feed_io.py`. Wire the three exceptions in `exceptions.py`.
   Re-export the public API from `__init__.py`.

6. **Gold fixture builder.**
   Write `tests/_build_gold.py` implementing the §5 tables (numbers
   hard-coded). Run it once locally to materialise
   `tests/data/gold_feed/` and `tests/data/gold_feed_no_lmp/`.
   `git add` the resulting parquet + JSON files. Do **not** add the
   builder to CI; it is a one-shot tool.

7. **Round-trip tests.**
   Land `tests/test_roundtrip.py` per §7. Run `pytest -n auto` from
   `scripts/` and confirm everything passes.

8. **Lint + type pass.**
   `ruff format scripts/_canonical_feed/`, then
   `cd scripts && ruff check _canonical_feed && pylint --jobs=0
   _canonical_feed && mypy _canonical_feed --ignore-missing-imports`.
   Fix any messages until exit code 0 (per
   `feedback_pylint_parallel` and the CRITICAL note in CLAUDE.md
   about pylint exit codes). Commit with message
   `feat(_canonical_feed): freeze Phase-0 canonical operation feed
   schema`.

After step 8 the schema is **frozen**. Phase 1 and Phase 2 agents
depend on this commit's hash.

---

## 10. Open questions for the user

Each item affects the on-disk bytes and cannot change after the
freeze. If the user does not respond, the schema-owner proceeds with
the **Recommended** default and records the choice in the freeze
commit message.

1. **Mixed cell-key kinds.** Plan assumes one feed = one kind
   (sim *or* real). Phase-3 `compare` splices at the DataFrame level,
   not on disk. **Recommended: single-kind per feed.**
2. **`Bus.region` taxonomy.** Free-form string vs CEN-pinned enum.
   **Recommended: free-form; document expected values in the P0.1
   companion doc.**
3. **Piecewise cost shape.** `list[(q_MW, slope_USD_per_MWh)]`
   vs cumulative-cost curves (CEN's native form). **Recommended:
   slope form per `feedback_generation_cost_segments`; Phase 2
   converts at write time.**
4. **`fetched_at` timezone.** UTC-only with `Z` suffix vs accept
   local-with-offset. **Recommended: UTC only; Phase 2 records source
   TZ in `manifest.notes`.**
5. **Reservoir/battery dual frame.** Reserved by §3.3.1 but not
   allocated in v1.0.0. **Recommended: defer to v1.1 minor bump.**
6. **Strict semver bumps.** Any rename = major (consumers rebuild).
   **Recommended: yes — that is what semver means.**

---

## 11. Acceptance checklist

Phase 0 is done when:

* [ ] `scripts/_canonical_feed/` is importable; all 8 steps in §9 are committed.
* [ ] `pytest -n auto scripts/_canonical_feed/tests` passes.
* [ ] `pylint --jobs=0 _canonical_feed` and `mypy _canonical_feed` exit 0.
* [ ] `tests/data/gold_feed/manifest.json:schema_version == "1.0.0"`.
* [ ] `verify_feed(gold_feed)` and `verify_feed(gold_feed_no_lmp)` return `[]`.
* [ ] Companion doc `docs/scripts/canonical_operation_feed.md` (P0.1) cites this plan.
* [ ] §10 questions resolved or recommended defaults recorded in the freeze commit.
