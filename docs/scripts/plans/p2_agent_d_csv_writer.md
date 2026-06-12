# Phase-2 Agent D — `cen2gtopt` skeleton, CSV downloaders, topology, UIDs, writer

> **Status**: planning. Implementation not started.
>
> **Scope**. Phase-2 deliverables P2.0, P2.A, P2.C, P2.D, P2.F from the
> master plan (`docs/scripts/gtopt_marginal_units_plan.md`). Owns the
> `scripts/cen2gtopt/` package skeleton, the four public-website CSV
> downloaders + parsers, the topology snapshot loader, integer-UID
> assignment + persistence, and the canonical-feed writer.
>
> **Out of scope here**. SIP API client (Agent E, P2.B), SDDP-style
> rename history (SIP-only), test fan-out for `test_sip_client.py`
> (Agent E), Phase-3 e2e (Agent F), any kind of scheduling or daemon
> (master plan §10).
>
> **Contract**. Output is a canonical-feed Parquet directory + manifest
> conforming to master plan §3.3.3. Both Agent D's CSV path and Agent
> E's SIP path populate the **same** in-memory `Topology` + `Cells`
> dataclasses defined in the Phase-0 `scripts/_canonical_feed/` peer
> package; the writer module is shared.

---

## 1. Package layout

`scripts/cen2gtopt/` — mirrors the existing `scripts/<name>/{__init__,
__main__, main, ...}` convention used by `gtopt_check_output/`,
`plp2gtopt/`, `cen_demanda/`.

```text
scripts/cen2gtopt/
├── __init__.py            # version, public re-exports (cli, write_feed)
├── __main__.py            # `python -m cen2gtopt` → main.cli()
├── main.py                # argparse, --source dispatcher, exit codes
├── _csv_endpoints.py      # endpoint catalogue (URLs, headers, columns)
├── _csv_parsers.py        # one pandas parser per endpoint
├── _topology.py           # CSV-mode topology JSON loader + schema
├── _normalize.py          # tz, NFC names, UID assignment + persistence
├── _writer.py             # wraps _canonical_feed.write_feed; manifest
├── _cache.py              # ~/.cache/gtopt/cen/<endpoint>/<date>.csv.zst
├── _http.py               # thin requests wrapper (timeout, retry, UA)
├── tests/
│   ├── __init__.py
│   ├── conftest.py            # offline fixture loaders (Agent D owns)
│   ├── test_csv_parsers.py    # Agent D
│   ├── test_topology.py       # Agent D
│   ├── test_writer.py         # Agent D
│   ├── test_cli_smoke.py      # Agent D
│   ├── test_no_scheduling.py  # Agent D — CI guard, master §11 P2 acc.3
│   ├── test_sip_client.py     # Agent E (NOT Agent D)
│   ├── data/
│   │   ├── cen_csv_fixtures/  # one CSV per endpoint, hand-trimmed
│   │   │   ├── cmgreal_2024-04-01.csv
│   │   │   ├── cmgonline_2024-04-01.csv
│   │   │   ├── gen_real_2024-04-01.csv
│   │   │   └── dem_real_2024-04-01.csv
│   │   ├── topology_csv_mode.json   # user-supplied topology fixture
│   │   └── system_planning_fragment.json  # gtopt JSON fallback fixture
│   └── integration/
│       ├── __init__.py
│       └── test_one_week.py   # opt-in, GTOPT_RUN_CEN_BACKTEST=1
```

The package is a flat module set (no nested sub-package) — same depth
as `plp2gtopt/`. Underscore-prefixed modules are private; the only
public entry point is `main.cli`.

---

## 2. `pyproject.toml` patch (in `scripts/pyproject.toml`)

Add to `[project.scripts]` (alphabetical neighbours: `cen_demanda`,
`cvs2parquet`):

```toml
cen2gtopt             = "cen2gtopt.main:cli"
```

Add to `[project.dependencies]` if absent (most are already pinned):

```toml
"requests>=2.31",       # CSV downloads + later SIP HTTP
"pandas>=2.0,<3.0",     # already pinned
"pyarrow>=12.0",        # already pinned
"zstandard>=0.22",      # already pinned (cache .csv.zst)
```

Add to `[tool.setuptools.packages.find].include`:

```toml
"cen2gtopt*"
```

The new `_canonical_feed*` peer package (Phase-0 deliverable, owned by
schema-owner) is added to the `include` list by Phase 0, **not** here.
Agent D imports `from _canonical_feed import write_feed, Topology,
Cells` once it lands.

> **Pylint / mypy registration**: append `cen2gtopt` to the long
> module-list in `CLAUDE.md`'s pre-commit recipe and to any local
> `Makefile` / `tox.ini` mirror. Per `feedback_pylint_parallel`, pylint
> always runs with `--jobs=0`.

---

## 3. CLI parser (`main.py`)

Single sub-command, no daemon (master §9.1.1). All flags from §9.5
of the master plan, no additions.

| Flag | Type | Default | Required? | Notes |
|---|---|---|---|---|
| `--from` | `YYYY-MM-DD` | — | yes | first date inclusive, civil time in `--source-tz` |
| `--to` | `YYYY-MM-DD` | — | yes | last date inclusive |
| `--out` | path | — | yes | output Parquet directory; overwritten if exists with one-line warning |
| `--bus` | comma-list or `@file` | all | no | restrict to listed bus names/UIDs (NFC-folded match) |
| `--unit` | comma-list or `@file` | all | no | restrict to listed generators (NFC-folded match) |
| `--source` | `{sip,csv,auto}` | `auto` | no | see §3.1 dispatch table |
| `--api-key` | str | `$CEN_API_KEY` | conditional | required when `--source sip`; absent ⇒ exit 3 |
| `--cache` | path | `~/.cache/gtopt/cen` | no | see §9 cache module |
| `--no-cache` | flag | off | no | bypass cache; force re-fetch |
| `--include` | repeatable enum | `dispatch,demand` | no | one of `{dispatch,demand,lmp,flows}` per master §9.5 |
| `--source-tz` | tz name | `Chile/Continental` | no | e.g. `America/Santiago` for legacy SING |
| `--manifest-only` | flag | off | no | write `manifest.json` + topology only, no cells |
| `--topology` | path | none | conditional | required when `--source csv` and SIP catalogues unavailable; see §6 |
| `-v` / `-q` | flag | INFO | no | DEBUG / WARNING log level |

### 3.1 `--source` dispatch (auto-mode rules)

```text
if --source == sip          → SIP path  (Agent E)   ; require api-key
elif --source == csv        → CSV path  (Agent D)   ; --topology required
                                                      unless system_*.json
                                                      auto-detected
elif --source == auto:
    if api-key present      → SIP path  (Agent E)
    else                    → CSV path  (Agent D) + warn
```

Agent D implements the CSV path and the dispatch shim. Agent E
implements the SIP path; the shim calls `from cen2gtopt._sip_client
import fetch_all` (a module Agent E owns) when the SIP branch is
selected. Until Agent E lands, the `auto` and `sip` branches `raise
NotImplementedError("SIP path: Agent E (P2.B)")` — fail loudly so the
two halves never silently shadow each other.

### 3.2 Exit codes (master §9.5.2)

| Code | Meaning | Agent D triggers |
|---|---|---|
| `0` | feed written successfully, every cell present | success path |
| `2` | feed written, but ≥1 cell flagged `missing_realised_data` | metering hole detected by `_csv_parsers` |
| `3` | required arguments missing / inconsistent; **no output written** | argparse fail, `--from > --to`, missing `--api-key` for `--source sip`, missing `--topology` for CSV-only with no SIP catalogue, unknown `--include` value |
| `4` | network error after retries; `<out>.partial` may exist | `_http.fetch` exhausts retries |

`main.cli()` returns the exit code; `__main__.py` calls
`sys.exit(cli())`. All error paths log a single-line "missing X for
mode Y" message before returning, never silently downgrade
(`feedback_no_alpha_fix_for_compress` discipline).

---

## 4. CSV endpoint catalogue (`_csv_endpoints.py`)

One frozen dataclass per endpoint plus a registry dict. The four
endpoints listed in master §9.2.1:

```python
@dataclass(frozen=True)
class CsvEndpoint:
    name: str                       # registry key
    url_template: str               # parameterised by date
    method: str = "GET"             # all four are GET
    headers: dict[str, str] = ...   # User-Agent + Accept
    form_params: dict[str, str] | None = None  # None → query-string only
    expected_columns: tuple[str, ...]   # Spanish names verbatim
    cell_key_columns: tuple[str, ...]   # which columns form the cell key
    canonical_target: str           # "lmp" | "dispatch" | "load" | "lmp_online"
```

### 4.1 Per-endpoint specs

All four entries carry `headers={"User-Agent": "cen2gtopt/1.0",
"Accept": "text/csv"}` and a `TODO(verify-at-integration)` comment on
the URL template — the Coordinador front-end uses a JSON+session-cookie
flow and the static `.csv` redirect must be confirmed by inspecting
live network traffic before we commit a literal URL.

| `name` | `url_template` (TODO verify) | `expected_columns` (Spanish, verbatim) | `cell_key_columns` | `canonical_target` |
|---|---|---|---|---|
| `costo_marginal_real`   | `cmgreal.coordinador.cl/api/.../{date}.csv` | `Fecha`, `Hora`, `Barra`, `CMg [USD/MWh]` | `(Fecha, Hora, Barra)` | `lmp` |
| `costo_marginal_online` | `coordinador.cl/.../cmgonline-{date}.csv`   | `Fecha`, `Hora`, `Cuarto` (1..4), `Barra`, `CMg Online [USD/MWh]` | `(Fecha, Hora, Barra)` | `lmp_online` |
| `generacion_real`       | `coordinador.cl/.../generacion-real-{date}.csv` | `Fecha`, `Hora`, `Central`, `Generación [MWh]` | `(Fecha, Hora, Central)` | `dispatch` |
| `demanda_real`          | `coordinador.cl/.../demanda-real-{date}.csv`    | `Fecha`, `Hora`, `Barra`, `Demanda [MWh]` | `(Fecha, Hora, Barra)` | `load` |

`Hora` is 1..24 (or 1..25 on the historical DST fall-back, §5.1.5).
Decimal separator (`,` vs `.`) is auto-detected per-file (§5.1.3).
`Cuarto` rows are collapsed to hourly by §5.1.6.

### 4.2 Registry

```python
ENDPOINTS: dict[str, CsvEndpoint] = {
    "costo_marginal_real":   ...,
    "costo_marginal_online": ...,
    "generacion_real":       ...,
    "demanda_real":          ...,
}

REQUIRED_FOR_MIN_FEED = ("generacion_real", "demanda_real")  # master §9.3
OPTIONAL_ENRICHMENT = ("costo_marginal_real", "costo_marginal_online")
```

The `--include` flag of §3 maps to a subset of `ENDPOINTS.keys()`:

| `--include` token | Endpoints invoked |
|---|---|
| `dispatch` | `generacion_real` |
| `demand` | `demanda_real` |
| `lmp` | `costo_marginal_real` (preferred) + `costo_marginal_online` (fallback) |
| `flows` | not in CSV path — TODO when CEN exposes a flows CSV |

A `--include flows` with `--source csv` exits 3 with
"flows endpoint not available via CSV; use --source sip" until the
flows page is verified at integration time.

---

## 5. CSV parsers (`_csv_parsers.py`)

One function per endpoint. All are pandas-based, return a long-form
DataFrame keyed by `(datetime_utc, name)` with a single value column
named after `canonical_target`.

```python
def parse_costo_marginal_real(
    csv_path: Path, *, source_tz: str = "Chile/Continental",
) -> pd.DataFrame:
    """CMR daily CSV → long frame with columns
    (datetime_utc, bus_name, lmp).
    """
```

### 5.1 Common normalisation rules (applied by every parser)

1. **Encoding probe**. Read with `encoding="utf-8"`; on
   `UnicodeDecodeError` retry with `encoding="latin-1"` and emit a
   one-time `WARNING` per file. Output strings are always NFC.
2. **Glyph fixes**. `N°` (U+00B0) is preserved; `Nº` (U+00BA) is
   rewritten to `N°`. Other punctuation ASCII-folded only when used
   as the **match key** for UID assignment (see §7) — the canonical
   `name` column keeps the original Spanish text.
3. **Decimal separator**. Detected from the first numeric column
   (`,` vs `.`); applied to all numeric columns. Logged at DEBUG.
4. **Time zone**.
   ```python
   ts_local = pd.to_datetime(df["Fecha"]) + pd.to_timedelta(df["Hora"]-1, unit="h")
   ts_local = ts_local.dt.tz_localize(source_tz, ambiguous="infer", nonexistent="shift_forward")
   df["datetime_utc"] = ts_local.dt.tz_convert("UTC")
   ```
   Every parser does the same; the helper is named
   `_localise_chile_civil` in `_normalize.py` (§7).
5. **Hour-25 (DST historical fall-back)**. Pre-2015 CEN files contain
   25 rows on the autumn DST transition. `tz_localize(ambiguous="infer")`
   resolves the duplicate; if it raises, the parser falls back to
   `ambiguous=np.array([True]*1 + [False]*24)` matching CEN's
   convention (first occurrence = pre-transition). Documented inline
   with a regression-fixture pointer.
6. **15-min collapse** (CMg Online only). After tz convert, group by
   `(datetime_utc.floor("h"), bus_name)` and take the
   `time-weighted mean` (each quarter-hour contributes 0.25); a
   missing quarter is filled with the within-hour mean and flagged.
7. **Missing rows**. Any `(datetime, key)` cell missing from the CSV
   is written to the output frame as `pd.NA` (per `feedback_no_nan` —
   never NaN; pd.NA in DataFrames; the writer converts to
   `parquet null` at the boundary). Each missing cell produces one
   `WARNING` log line and increments a counter that maps to exit
   code 2.
8. **Column renaming**. The Spanish column names listed in
   `expected_columns` are mapped to a stable canonical name set
   (`Fecha`→`date_local`, `Hora`→`hour_local_1based`, `Barra`→`bus_name`,
   `Central`→`unit_name`, the value column → `canonical_target`).
9. **Schema validation**. After load, the parser asserts the column
   set equals `expected_columns` (modulo whitespace/case); any drift
   raises `CsvSchemaDriftError` and exits 3 with "endpoint X column
   layout changed; refusing to silently coerce" — the `--no-alpha-fix`
   discipline applied to data plumbing.

### 5.2 Public API of the module

```python
def parse(endpoint: str, csv_path: Path, *, source_tz: str) -> pd.DataFrame: ...
def parse_all(
    files: dict[str, list[Path]], *, source_tz: str,
) -> dict[str, pd.DataFrame]: ...
```

`parse_all` is the loop driver invoked by `main.cli()` once the
downloader has populated the cache. Returns one frame per requested
endpoint, all keyed on `datetime_utc`.

---

## 6. Topology snapshot (`_topology.py`)

CSV path **cannot** discover the full topology — CEN's public CSV
exports list bus names and unit names only, not the line catalogue or
declared variable costs. The user must therefore supply a topology
JSON when `--source csv`, **or** the script falls back to a gtopt
planning JSON when one is present in the working directory.

### 6.1 Loader resolution order (CSV path only)

```text
1. --topology <path>           (explicit; required to be valid)
2. ./topology.json             (CEN-format minimal schema, §6.2)
3. ./system_*.json             (gtopt planning JSON, fallback per §6.3)
4. exit 3 with "no topology available; supply --topology"
```

When `--source sip` (Agent E), this module is bypassed: the SIP
`/centrales`, `/barras`, `/lineas` endpoints supply the topology
directly.

### 6.2 Minimal user-facing topology JSON schema

Hand-edit-friendly; one file per snapshot date. Schema frozen once
Phase 0 lands.

```json
{
  "schema_version": 1,
  "snapshot_date": "2026-04-01",
  "timezone": "Chile/Continental",
  "buses":      [{"name": "ALTO_JAHUEL_220", "region": "RM", "uid?": 12345}],
  "generators": [{"name": "BOCAMINA II", "bus_name": "BOCAMINA_220",
                  "pmin": 0.0, "pmax": 350.0, "declared_mc": 78.4,
                  "kind": "thermal", "uid?": 67890}],
  "lines":      [{"name": "...", "bus_a_name": "...", "bus_b_name": "...",
                  "tmax_ab": 1500.0, "tmax_ba": 1500.0,
                  "active": true, "uid?": 11111}]
}
```

`uid` is optional; absent → hashed name (§7), persisted to
`manifest.json`. Explicit-but-conflicting UIDs across runs raise
`UidConflictError` → exit 3. `kind ∈ {thermal, hydro, battery, profile}`
matches the canonical schema.

### 6.3 gtopt planning JSON fallback

For users who already have a gtopt case and want to reuse its
topology rather than hand-write one. The module reads
`system.bus_array`, `system.generator_array`, `system.line_array`
(the same fields master §3.1 lists), and rewrites them into the §6.2
shape in memory. UIDs from the gtopt JSON are honoured.

A user opting into this fallback gets a one-time `INFO` log:
"using gtopt planning JSON `<path>` as topology source; consider
exporting a CEN-format `topology.json` for portability."

### 6.4 Public API

```python
@dataclass(frozen=True)
class TopologyJson:
    schema_version: int
    snapshot_date: date
    timezone: str
    buses: list[BusEntry]
    generators: list[GeneratorEntry]
    lines: list[LineEntry]

def load_topology(
    *, explicit_path: Path | None, search_root: Path,
) -> TopologyJson: ...

def to_canonical(t: TopologyJson, uid_map: UidMap) -> Topology: ...
```

`to_canonical` is the bridge into `_canonical_feed.Topology`; it
attaches UIDs (§7) and validates that every `bus_name` referenced by
generators/lines exists.

---

## 7. UID assignment + persistence (`_normalize.py`)

UIDs are **stable 31-bit integers**. Stability rules:

```text
priority 1: SIP catalogue ID         (Agent E supplies these from
                                      /centrales /barras /lineas)
priority 2: explicit "uid" field in  (user-managed; fail-loud on conflict)
            topology.json
priority 3: SHA-256(NFC-folded name) (deterministic fallback;
            truncated to 31 bits     CSV-only path)
```

### 7.1 NFC fold for hashing

```python
def fold_name_for_hash(name: str) -> str:
    s = unicodedata.normalize("NFC", name)
    s = s.replace("Nº", "N°")          # CEN glyph fix-ups
    s = s.casefold()                   # case-insensitive
    s = re.sub(r"\s+", " ", s).strip()
    return s
```

The fold is **only** for the hash key and for fuzzy matching; the
`name` field stored in the canonical feed retains the original
Spanish text including accents.

### 7.2 Hash → 31-bit UID

```python
def hash_uid(folded_name: str) -> int:
    digest = hashlib.sha256(folded_name.encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "big") & 0x7FFF_FFFF
```

31 bits keeps the value positive in any 32-bit signed integer column;
collision probability for ~10⁴ Chilean units is ~10⁻⁵ — acceptable
for v1, and the manifest stores the full name so collisions are
detectable post-hoc. A unit test asserts no collision among the
fixture set.

### 7.3 UID map persistence

`manifest.json` (written by §10) stores:

```json
{
  "uid_map": {
    "buses":      {"ALTO_JAHUEL_220": 12345, ...},
    "generators": {"BOCAMINA II":     67890, ...},
    "lines":      {"...":             11111, ...}
  }
}
```

On every subsequent run the module **reads the existing manifest
first**, applies its `uid_map` as the priority-1.5 source (above hash,
below explicit and SIP), and only generates new hashes for names not
yet in the map. This guarantees that re-running `cen2gtopt` produces
the same UIDs across invocations.

### 7.4 Time-zone helper

`_localise_chile_civil(df, src_col, *, tz)` lives here too — same
helper every CSV parser uses (§5.1 step 4). Centralising it ensures
the DST hour-25 logic stays in one place.

---

## 8. Rename resolution

* **SIP path**: Agent E uses `/centrales` history to resolve renames
  to a single canonical UID.
* **CSV-only path** (Agent D's responsibility):
  * Detect renames by comparing `unit_name` against the manifest's
    existing `uid_map.generators` keys after NFC-fold.
  * When a new name fuzzy-matches an existing one (Levenshtein ≤ 2,
    same `bus_name`), emit a one-time
    `WARNING [rename-suspected] BOCAMINA II → BOCAMINA 2` and reuse
    the existing UID.
  * Record both names under the same UID in
    `manifest.json:rename_aliases[]`. The Phase-1 reader can use
    this to track the canonical name across stages.
  * Do **not** silently merge unmatched renames; an unmapped unit
    gets a fresh UID and the user is asked to re-run with `--source
    sip` or to add the alias manually to `topology.json`.

---

## 9. Cache (`_cache.py`)

### 9.1 Layout

```text
~/.cache/gtopt/cen/                              # default --cache root
├── costo_marginal_real/
│   ├── 2024-04-01.csv.zst
│   ├── 2024-04-02.csv.zst
│   └── ...
├── costo_marginal_online/
├── generacion_real/
├── demanda_real/
└── _index.json     # endpoint × date × sha256 + filter-hash
```

### 9.2 Cache key

```text
cache_key = (endpoint, date, sha256(json(query_filters)))
```

`query_filters` is the canonicalised dict of `{bus, unit, source_tz}`
filters from the CLI (sorted, JSON-serialised, hashed). This makes a
re-run with `--bus ALTO_JAHUEL` write to a different cache file from
the all-buses run, so neither shadows the other.

### 9.3 Compression

Cached as `*.csv.zst` using `zstandard` (a dep already pinned by
`scripts/pyproject.toml`). Decompression is streamed into pandas via
`zstandard.ZstdDecompressor().stream_reader`. Per
`MEMORY.md:codecs`: parquet output is zstd; in-memory snapshots use
lz4. Cache is on-disk persistent → zstd is correct.

### 9.4 `--no-cache` semantics

```text
--no-cache  → ignore existing cache file, re-fetch and overwrite.
              The fresh download is still written to the cache (so
              subsequent runs without --no-cache benefit from it).
```

The flag does **not** delete the cache; it bypasses the read. To
prune, the user removes the cache directory by hand.

### 9.5 Public API

```python
def fetch_or_load(
    endpoint: CsvEndpoint, date: date, *,
    cache_root: Path, http_get: HttpGet, query_filters: dict,
    no_cache: bool = False,
) -> Path: ...
```

Returns the path to the (possibly freshly downloaded) cached
`.csv.zst` file. Network errors after retries raise
`HttpFetchError` → exit 4 with `<out>.partial` written by the writer.

---

## 10. Writer (`_writer.py`)

Thin wrapper around the Phase-0 helper
`scripts._canonical_feed.write_feed(path, topology, cells, manifest)`.
Agent D's writer:

1. Composes the `Cells` long-form frames from `_csv_parsers.parse_all`
   output (one frame per `canonical_target`), reindexing to the
   `(datetime_utc, uid)` pair the canonical schema expects.
2. Resolves names → UIDs via `_normalize.UidMap`.
3. Builds the `manifest.json` payload — a JSON object with keys:
   `schema_version` (1), `producer` (`"cen2gtopt"`),
   `producer_version`, `fetched_at` (ISO-8601 UTC), `args` (the
   normalised CLI invocation), `endpoints_hit[]` (one entry per
   endpoint URL with `name`, `urls`, `rows`, `sha256`, and a
   `source ∈ {"network","cache"}` discriminator), `file_hashes` (sha256
   of every emitted parquet under `topology/` and `cells/`), `uid_map`
   (the §7.3 dict), `rename_aliases[]`, and `missing_cells` (counter
   driving exit code 2).
4. Calls `_canonical_feed.write_feed(path=out, topology=top,
   cells=cells, manifest=manifest)`. The Phase-0 helper handles the
   actual parquet I/O and hash computation; Agent D's writer never
   touches `pyarrow` directly.
5. On any exception, writes the partial frames to `<out>.partial/`
   and returns exit code 4.

### 10.1 `--manifest-only` behaviour

When set, the writer skips the cells frames entirely and writes only
`topology/*.parquet` + `manifest.json`. Useful for the Phase-3 dry
run and for preflighting a date range without paying the network
cost. Exit code is always 0 on success.

---

## 11. Idempotency contract

Same args twice → byte-identical output **modulo
`manifest.json:fetched_at`**. Enforced by:

1. **Stable ordering**: `cells/*.parquet` sorted by `(datetime_utc,
   uid)`; `topology/*.parquet` sorted by `uid`.
2. **Parquet determinism**: `compression="zstd", compression_level=3,
   use_dictionary=True, write_statistics=False, version="2.6"`.
   Statistics off because string-column min/max can shift.
3. **UID stability** (§7).
4. **`fetched_at` is the only sanctioned non-determinism**;
   `test_writer.py` runs the writer twice with a pinned clock and
   sha256s every output to confirm.
5. **Cache hits do not perturb output**: parquet bytes are identical
   regardless of cache-vs-network source; the manifest's
   `endpoints_hit[].source` field records which path was taken.

---

## 12. Tests (Agent D's owned files; §9.6 master plan)

All files live in `scripts/cen2gtopt/tests/`. Per `MEMORY.md`
discipline: pytest always run with `-n auto` (`feedback_pytest_parallel`).
Never NaN, always `pd.NA` (`feedback_no_nan`). Coverage threshold 83%.

### 12.1 `test_csv_parsers.py` — Agent D

* **Fixtures**: `tests/data/cen_csv_fixtures/{cmgreal,cmgonline,
  gen_real,dem_real}_2024-04-01.csv` — hand-trimmed to 3 buses /
  5 units / 24 hours (~50 rows each).
* **Cases**:
  * Round-trip each endpoint → assert column set and dtypes match the
    canonical schema (`datetime_utc: tz=UTC, dtype=datetime64[ns,UTC]`).
  * `Chile/Continental` → UTC conversion: hand-pick three hours
    spanning the historical DST fall-back; assert hour-25 lands at
    the right UTC instant.
  * Accents and `N°` glyph: a fixture row uses `Ventanas N°1`;
    assert NFC fold and that the canonical `unit_name` column keeps
    accents.
  * Missing-row handling: drop one row from a fixture, assert the
    parser emits `pd.NA` and increments the missing-cell counter.
  * 15-min collapse for CMg Online: 4×15-min rows → 1 hour with the
    correct time-weighted mean.
  * Schema-drift refusal: rename a column in a fixture, assert
    `CsvSchemaDriftError` is raised.
  * Decimal separator detection: a `,`-decimal fixture parses
    correctly.
* **Acceptance**: ≥10 test cases, ≥95% line coverage of
  `_csv_parsers.py`.

### 12.2 `test_topology.py` — Agent D

* **Fixtures**: `tests/data/topology_csv_mode.json` (CEN format),
  `tests/data/system_planning_fragment.json` (gtopt format, 4 buses
  / 6 units / 5 lines).
* **Cases**:
  * CEN-format JSON → `Topology` round-trip; assert UIDs honoured
    when explicit, hashed when absent.
  * gtopt-JSON fallback path: load `system_planning_fragment.json`
    via `_topology.load_topology`, assert it produces the same
    `Topology` shape.
  * Topology resolution order: with both `--topology` and
    `./topology.json` present, the explicit flag wins.
  * UID stability: hash-mode UIDs are deterministic across two calls.
  * UID conflict: two topology files with the same name but different
    explicit UIDs → `UidConflictError` exit 3.
  * Generator references missing bus → exit 3 with a single-line msg.
* **Acceptance**: ≥8 cases, full branch coverage of `_topology.py`.

### 12.3 `test_writer.py` — Agent D

* **Fixtures**: synthesised `Topology` + `Cells` from the parser
  fixtures of §12.1 (no separate hand-rolled fixture).
* **Cases**:
  * Round-trip: `write_feed → read with the Phase-0 reader → assert
    frame equality`. This is the master-plan **schema lock-in test
    from Phase 0** for the cen2gtopt side.
  * Idempotency: run the writer twice with a pinned `fetched_at`,
    sha256 every output file, assert equality.
  * `--manifest-only`: only `topology/*.parquet` + `manifest.json`
    exist; `cells/` is absent.
  * Manifest contents: producer/version/uid_map/file_hashes all
    populated and correctly referenced.
  * Partial-write on exception: simulate a write failure, assert
    `<out>.partial/` exists and the main `<out>` does not.
* **Acceptance**: ≥7 cases, ≥90% coverage of `_writer.py`.

### 12.4 `test_cli_smoke.py` — Agent D

* **Cases** (all subprocess-driven via `python -m cen2gtopt`):
  * `--help` exits 0, lists every flag from §3.
  * `--manifest-only --topology fixture.json --from 2024-04-01 --to
    2024-04-01 --out /tmp/feed` exits 0 and writes only the
    manifest + topology.
  * `--from 2024-04-02 --to 2024-04-01` exits 3 with "from > to".
  * `--source sip` (no `$CEN_API_KEY`) exits 3 with "missing API key
    for --source sip".
  * `--source csv` with no topology and no `system_*.json` exits 3
    with "no topology available; supply --topology".
  * `--include flows --source csv` exits 3 with "flows endpoint not
    available via CSV".
  * `--source auto` with `$CEN_API_KEY` set raises
    `NotImplementedError("SIP path: Agent E (P2.B)")` until E lands —
    a `pytest.skip("Agent E pending")` until then.
* **Acceptance**: ≥6 cases, exit-code coverage of every §3.2 row.

### 12.5 `tests/integration/test_one_week.py` — Agent D (opt-in)

* Gated behind `GTOPT_RUN_CEN_BACKTEST=1` (master §9.6 row 6).
* Fetches one week of real data, runs the writer, then runs Phase-1
  `gtopt-marginal-units --input feed --mode real-reconstruct` (when
  the Phase-1 binary is on PATH; `pytest.skip` otherwise).
* Asserts master §7.5 quality bars: ≥95% of cells within ±2 USD/MWh
  of CEN's published number.
* Excluded from default CI; runs only on a manual sweep.

### 12.6 Note: Agent E ownership

`tests/test_sip_client.py` is **Agent E's** file (master §12 P2.E
sub-agent E2). Agent D does not write or modify it. The file shares
the `tests/conftest.py` and fixture directory with Agent D's tests
because both writers populate the same canonical schema; Agent D
designs `conftest.py` with this shared use in mind (avoid
test-name-collision per `MEMORY.md` parallel-test-collision note).

---

## 13. CI guard: `tests/test_no_scheduling.py` (master §11 P2 acc.3)

Single-purpose guard test that fails CI if anyone reintroduces
scheduling code. The test walks every `*.py` under `scripts/cen2gtopt/`
(skipping `tests/`), greps each line with the case-insensitive regex
`\b(cron|systemd|schedule|daemon)\b`, and asserts zero hits. The blunt
rule includes comments deliberately: no scheduling words anywhere in
production code. A docstring on every Agent D module
(`"""...this file deliberately contains no scheduling code; see master
plan §10..."""`) keeps the intent visible and avoids accidental hits.

---

## 14. Step-by-step coding order

Agent D's serial sub-tasks within Phase 2 (P2.0 → P2.A → P2.C → P2.D
→ P2.F). Per `feedback_minimize_builds`: edit, edit, edit, then run
the full lint+type+test gauntlet **once** per step, not after every
file.

1. **Step 1 — package skeleton (P2.0)**.
   * Create `scripts/cen2gtopt/{__init__,__main__,main}.py` stubs.
   * Add `pyproject.toml` patch (§2).
   * Implement `_no_scheduling` guard test (§13) and the smoke
     `--help` test.
   * Verify `pylint --jobs=0 cen2gtopt` and `mypy cen2gtopt
     --ignore-missing-imports` are green; `pytest -n auto -q` passes
     the two trivial tests.
   * **Gate**: signals Agent E that the skeleton is mergeable.

2. **Step 2 — CSV endpoint catalogue + HTTP shim**.
   * Implement `_csv_endpoints.py` registry with TODO markers.
   * Implement `_http.py` (`requests.get` + retry + timeout + UA);
     test against `httpretty` or `responses` for offline determinism.

3. **Step 3 — CSV parsers (P2.A)**.
   * Implement the four parsers per §5; check fixtures into
     `tests/data/cen_csv_fixtures/`.
   * Land `test_csv_parsers.py` (§12.1).

4. **Step 4 — cache layer**.
   * Implement `_cache.py` per §9 with the index file and
     `--no-cache` semantics.
   * Pure-unit test using a tmp-path cache root and a fake
     `http_get`.

5. **Step 5 — topology snapshot (P2.C)**.
   * Implement `_topology.py` per §6 (CEN-format loader + gtopt
     fallback).
   * Land `test_topology.py` (§12.2).

6. **Step 6 — UID assignment + manifest persistence**.
   * Implement `_normalize.py` per §7.
   * Land its unit tests (covered by `test_topology.py` and
     `test_csv_parsers.py`).

7. **Step 7 — writer (P2.D)**.
   * Implement `_writer.py` per §10. Depends on the Phase-0
     `_canonical_feed.write_feed` peer; if it has not landed,
     monkey-patch a stub and gate the live test on its presence.
   * Land `test_writer.py` (§12.3) including the round-trip
     idempotency assertion.

8. **Step 8 — CLI dispatcher (P2.F)**.
   * Wire `main.cli` per §3 with the source dispatch table.
   * The SIP branch raises `NotImplementedError("SIP path: Agent E
     (P2.B)")` until Agent E merges.
   * Land `test_cli_smoke.py` (§12.4).

9. **Step 9 — release polish**.
   * Sweep for `feedback_no_nan` violations.
   * Run full lint + type + tests; fix any drift.
   * Add `docs/scripts/cen2gtopt.md` user guide (Phase-2 doc P2.7,
     not strictly Agent D but adjacent — defer to Phase-3 if Agent F
     prefers).
   * Tag v1 of `cen2gtopt` per master P2.F.

---

## 15. Open questions

1. **CSV URL verification**. The four `url_template` strings in §4
   are placeholders carrying `TODO(verify-at-integration)`. The
   Coordinador front-end uses a JSON+session-cookie flow on the live
   pages; I have not pinned down whether the static `.csv` redirect
   is publicly accessible without browser automation. Resolution path:
   when the schema-owner's gold fixture lands (Phase 0), Agent D runs
   one manual `curl` against each endpoint with verbose output and
   transcribes the resolved URL pattern. Until then, all four
   downloaders raise `NotImplementedError("URL pending verification —
   see plan §15.1")` and the smoke test pins this with
   `pytest.raises`.

2. **CEN catalogue snapshot date semantics**. CEN sometimes publishes
   a single catalogue snapshot per month and sometimes per
   commercial-operation event. For `--from 2024-04-01 --to
   2024-04-30` should we fetch one catalogue (the one valid on
   2024-04-01) or load all snapshots overlapping the range? v1 plan:
   one catalogue at `--from`. Confirm with the user before
   implementing the multi-snapshot path.

3. **Topology JSON location for CSV-only users**. The §6.1 resolution
   order falls back to a working-directory `system_*.json`. Should
   we additionally search `$GTOPT_PLANNING_DIR` (a new env var) or
   the current directory's nearest ancestor that contains a
   `system_*.json`? v1 plan: working directory only. The user may
   prefer a per-project default like `<repo>/cases/SEN_topology.json`;
   answering this is a small follow-up.

4. **`--include lmp` source priority**. When both Costo Marginal Real
   and Costo Marginal Online return data for the same `(date, hour,
   bus)`, which wins? v1 plan: CMR (final, definitive) overwrites
   CMg Online (preliminary). Document in `manifest.json` which source
   each cell came from via a `cells/lmp_source.parquet` enum. Confirm
   the user wants the merge or prefers separate files.

5. **Hash collision audit**. The 31-bit truncated SHA-256 has ~10⁻⁵
   collision probability for ~10⁴ Chilean units (master §7.2). Should
   a collision auto-promote to 62-bit and emit a warning, or hard-fail
   so the user disambiguates explicitly? v1 plan: hard-fail. Confirm.

6. **`--bus` / `--unit` glob support**. Master §9.5 specifies
   `list-or-file` semantics; do users want shell-style globs (e.g.
   `--bus 'ALTO_*'`)? v1 plan: no globs, exact NFC-folded match
   only. Easy to add later if requested.
