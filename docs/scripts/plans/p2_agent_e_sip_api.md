# Phase-2 Agent E — SIP API client (`sipubv2`) implementation plan

> **Scope**: P2.B (`scripts/cen2gtopt/_sip_client.py` and `_sip_endpoints.py`)
> plus the test fan-out P2.E for `tests/test_sip_client.py` and
> `tests/test_sip_topology.py`.
> **Owner**: Phase-2 Agent E.
> **Reads**: master plan §9.2.2, §9.3, §9.4, §9.6, §11 P2.B/P2.E, §12.1.
> **Does not touch**: `scripts/_canonical_feed/` (Phase-0), Agent D's CSV
> readers / writer (`_csv_*`, `_writer.py`), `gtopt_marginal_units/`.
> Cross-package fixes go through hand-off PR comments per §12.2 rule 3.
>
> **Status**: planning. No production code in this PR.

---

## 0. Pre-flight (mandatory per §12.2 rule 4)

Before writing any code, re-read these `MEMORY.md` feedback notes and apply
them throughout:

- `feedback_pytest_parallel` — every `pytest` invocation uses `-n auto`.
- `feedback_pylint_parallel` — every `pylint` invocation uses `--jobs=0`.
- `feedback_proactive_tests` — every public function ships unit tests
  alongside it; integration tests behind opt-in env flag.
- `feedback_no_magic_strings` — endpoint paths and JSON field names live in
  `_sip_endpoints.py` as module-level constants, never inlined.
- `feedback_no_nan` — missing values are `pd.NA` / `Optional`, never NaN.

---

## 1. Module layout

```
scripts/cen2gtopt/
  _sip_client.py            # public SipClient + exceptions
  _sip_endpoints.py          # endpoint paths, field-name constants, mappings
  tests/
    test_sip_client.py       # auth, pagination, retries, errors, dates
    test_sip_topology.py     # rename history → canonical UID resolution
    data/
      sip_fixtures/
        costo_marginal_real_2026-04-01_page{1,2,3}.json
        generacion_real_2026-04-01.json
        demanda_real_2026-04-01.json
        centrales_2026-04-01.json   centrales_with_rename.json
        barras_2026-04-01.json      lineas_2026-04-01.json
        error_429.json   error_500.json   empty_data.json
        rename_history.json
tools/
  sip_capture.py            # one-shot fixture refresh (Phase 2.5; see §10)
```

`_sip_client.py` and `_sip_endpoints.py` are the only production files
created by Agent E. The tests directory is co-owned with sub-agents E2/E3.
`tools/sip_capture.py` is the network-gated fixture generator (§10).

The package's `__init__.py` (created by Agent D in P2.0) re-exports
`SipClient`, `MissingApiKey`, `SipNetworkError`, `SipAuthError`. Agent E
adds the re-exports as a hand-off note to D, not by editing `__init__.py`
directly (per §12.2 rule 3).

---

## 2. Public API surface — `_sip_client.py`

### 2.1 Exceptions

```python
class SipError(RuntimeError):
    """Base for every SIP-client failure."""

class MissingApiKey(SipError):
    """Raised when the API key is required but absent.
    main.py decides between fail-hard (--source sip) or fall-back
    (--source auto) based on the CLI flag, not the exception class."""

class SipAuthError(SipError):
    """401/403 from the API — bad/expired key."""

class SipNetworkError(SipError):
    """5xx, connection error, or retries exhausted on 429."""

class SipSchemaError(SipError):
    """Response JSON missing a required field — fixture / API drift."""
```

### 2.2 The client class

```python
class SipClient:
    def __init__(
        self,
        api_key: str,
        base_url: str = "https://portal.api.coordinador.cl/sipub/v2/",
        timeout: float = 30.0,
        retries: int = 3,
        page_size: int = 1000,           # see §4
        session: requests.Session | None = None,
    ) -> None: ...

    # Series endpoints — all return a long-form pandas DataFrame
    # with columns: ts_utc:datetime64[ns, UTC], <key>_id:str, value:float
    def get_costo_marginal_real(
        self,
        date_from: datetime.date,
        date_to: datetime.date,
        bus: list[str] | None = None,
    ) -> pd.DataFrame: ...

    def get_generacion_real(
        self,
        date_from: datetime.date,
        date_to: datetime.date,
        unit: list[str] | None = None,
    ) -> pd.DataFrame: ...

    def get_demanda_real(
        self,
        date_from: datetime.date,
        date_to: datetime.date,
        bus: list[str] | None = None,
    ) -> pd.DataFrame: ...

    # Catalogue endpoints — return one row per entity, point-in-time
    def get_centrales(self, at_date: datetime.date) -> pd.DataFrame: ...
    def get_barras(self, at_date: datetime.date) -> pd.DataFrame: ...
    def get_lineas(self, at_date: datetime.date) -> pd.DataFrame: ...

    def healthcheck(self) -> bool:
        """GET <base>/healthcheck (or a known-cheap endpoint).
        Returns True if 200, False on any error. Never raises.
        TODO: confirm the exact path with CEN — sipubv2 may not expose a
        ping; if absent, fall back to centrales?at_date=today&page_size=1."""
```

All series getters internally: page through results (§4), concatenate,
parse `fecha_hora` → `ts_utc` (§6), coerce numeric columns to float
(missing → `pd.NA`), rename Spanish keys to canonical column names (§6),
sort by `(<key>_id, ts_utc)` and return.

### 2.3 Optional helper

```python
def sip_client_from_env(
    *,
    api_key: str | None = None,
    base_url: str | None = None,
) -> SipClient:
    """CLI bridge.
    - If api_key is None, read $CEN_API_KEY.
    - Raise MissingApiKey when neither is set.
    - main.py wraps the call and decides fail vs. CSV fallback."""
```

---

## 3. Authentication

Per the v1.1 PDF the SIP API expects an API key in the `Authorization`
header. **TODO (integration time)**: confirm the exact header form:

- `Authorization: <token>` (per the v1.1 PDF wording — most likely);
- `Authorization: Bearer <token>` (HTTP convention);
- `Authorization: ApiKey <token>` or `x-api-key: <token>` (some CEN
  services use this).

The implementation uses a single private `_auth_header()` method so the
exact form is changed in one place when verified. Default ships
`Authorization: <token>` (matches v1.1 PDF wording) and prints a
`logger.warning` once per process when CEN replies 401 with hints about
header format.

API key lookup precedence:

1. `--api-key <value>` CLI flag (Agent D wires this on `main.py`).
2. `$CEN_API_KEY` environment variable.
3. Absent → raise `MissingApiKey`.

`main.py` (Agent D) decides on `MissingApiKey`:

- `--source sip` → propagate (exit code 3 with a clear message);
- `--source auto` → log a warning and fall back to the CSV path;
- `--source csv` → never instantiates `SipClient`, no exception raised.

The client never logs or echoes the key value. The constructor also runs
`assert api_key.strip(), "empty api_key"` to fail fast on whitespace-only
strings.

---

## 4. Pagination

**TODO**: verify v2 envelope at integration. The plan assumes:

```json
{ "data": [ ... ], "page": 1, "total_pages": 5, "page_size": 1000 }
```

Loop strategy:

```python
def _paginate(self, path: str, params: dict) -> Iterator[dict]:
    page = 1
    while True:
        resp = self._get(path, {**params, "page": page,
                                 "page_size": self.page_size})
        body = resp.json()
        data = body.get("data", [])
        for row in data:
            yield row
        total = body.get("total_pages")
        if total is None:
            # Fallback: stop when an empty page comes back.
            if not data:
                break
            page += 1
            continue
        if page >= total:
            break
        page += 1
        # Guard against runaway loops if API misreports total_pages.
        if page > 10_000:
            raise SipSchemaError(f"pagination runaway at page={page}")
```

`page_size` is exposed as a constructor knob (default 1000). Smaller
values reduce server load and 429 risk; larger values reduce round-trips.
We default to 1000 because CEN's v1.1 PDF mentions "hasta 1000 registros
por página" as the documented cap. **TODO**: confirm v2 cap.

If CEN actually emits cursor-based pagination (`next` URL) the
`_paginate` body changes but the public API does not. We isolate the
shape behind `_paginate` for that reason.

---

## 5. Rate limiting / retries

Three retries (constructor-tunable) with exponential back-off and jitter:

```
attempt:        0     1     2     3
delay (s):      0   1.0   2.0   4.0   (+ uniform(0, 0.5) jitter on each)
```

Formula: `delay = base * (2 ** attempt) + random.uniform(0, jitter_max)`
with `base = 0.5`, `jitter_max = 0.5`.

Status-code policy:

| Status | Action |
|---|---|
| 200 | Return body. |
| 401, 403 | Raise `SipAuthError` immediately (do not retry). |
| 404 | Raise `SipSchemaError` (endpoint typo / out of range). |
| 429 | Honor `Retry-After` header if present, else backoff; retry up to N. |
| 5xx | Backoff + retry up to N. |
| Other | Raise `SipNetworkError`. |
| Connection error / timeout | Backoff + retry up to N. |

After `retries` failed attempts on retryable codes → `SipNetworkError`
with the last status and the count of attempts.

`Retry-After` may come as integer seconds or HTTP-date; the parser
handles both. **TODO**: confirm CEN actually emits it; if not, the
exponential schedule is the only signal.

---

## 6. Schema mapping per endpoint

All field names live in `_sip_endpoints.py` as `Final[str]` constants
(per `feedback_no_magic_strings`). `_sip_client.py` imports them; tests
also import them so a rename in CEN's API only touches one module.

> **Caveat**: every Spanish field name below is the **best inference**
> from the v1.1 PDF and the public-portal documentation page.
> Each endpoint has a `# TODO(integration): confirm field names with a
> live response` marker; sub-agent E2 refreshes the fixtures via
> `tools/sip_capture.py` (§10) once the API key arrives, and updates the
> constants in one PR.

### 6.1 `costo_marginal_real`

- Path: `costo_marginal_real` (full URL: `<base>/costo_marginal_real`).
- Query: `fecha_inicio=YYYY-MM-DD`, `fecha_fin=YYYY-MM-DD`,
  optional `barra=B1,B2`.
- Response row keys → canonical:
  | SIP key | Canonical |
  |---|---|
  | `barra_mnemonico` (or `barra_id`) | `bus_id` |
  | `fecha_hora` | `ts_utc` (after tz conversion) |
  | `costo_marginal_usd_mwh` | `value` |
- Returned frame: `["ts_utc", "bus_id", "value"]`.

### 6.2 `generacion_real`

- Path: `generacion_real`.
- Row keys → canonical:
  | SIP key | Canonical |
  |---|---|
  | `central_mnemonico` | `unit_id` |
  | `fecha_hora` | `ts_utc` |
  | `generacion_mwh` | `value` |
- Frame: `["ts_utc", "unit_id", "value"]`.

### 6.3 `demanda_real`

- Path: `demanda_real`.
- Row keys → canonical:
  | SIP key | Canonical |
  |---|---|
  | `barra_mnemonico` | `bus_id` |
  | `fecha_hora` | `ts_utc` |
  | `demanda_mwh` | `value` |
- Frame: `["ts_utc", "bus_id", "value"]`.

### 6.4 `centrales` (catalogue)

- Path: `centrales`.
- Query: `fecha=YYYY-MM-DD` (point-in-time view).
- Row keys → canonical (Topology.generator):
  | SIP key | Canonical |
  |---|---|
  | `central_id` (int) | `uid` |
  | `central_mnemonico` (str) | `name` |
  | `barra_mnemonico` | `bus_uid` (resolved against barras) |
  | `pmin_mw` | `pmin` |
  | `pmax_mw` | `pmax` |
  | `costo_variable_combustible_usd_mwh` (+ non-combustible adders) | `declared_MC` |
  | `tecnologia` | `kind` (mapped: térmica→thermal, hidro→hydro, …) |
  | `vigencia_desde` / `vigencia_hasta` | rename window (§7) |

### 6.5 `barras` (catalogue)

- Path: `barras`.
- Row keys → canonical (Topology.bus):
  | SIP key | Canonical |
  |---|---|
  | `barra_id` | `uid` |
  | `barra_mnemonico` | `name` |
  | `subsistema` (or `region`) | `region` |

### 6.6 `lineas` (catalogue)

- Path: `lineas`.
- Row keys → canonical (Topology.line):
  | SIP key | Canonical |
  |---|---|
  | `linea_id` | `uid` |
  | `barra_a_mnemonico` | `bus_a_uid` (resolved) |
  | `barra_b_mnemonico` | `bus_b_uid` (resolved) |
  | `capacidad_ab_mw` | `tmax_ab` |
  | `capacidad_ba_mw` | `tmax_ba` |
  | `en_servicio` (bool) | `active` |

### 6.7 Date-time handling

CEN returns ISO 8601 strings of the form `2026-04-01T00:00:00-04:00` (or
`-03:00` during DST historical data). The parser:

1. `pandas.to_datetime(value, utc=False)` → tz-aware Timestamp;
2. `.tz_convert("UTC")` → canonical UTC.

Any parse error or naive timestamp → `SipSchemaError` with the offending
value. The `--source-tz` override in `main.py` (Agent D) only applies to
CSV-path data; SIP rows always carry an explicit offset and are parsed
verbatim.

---

## 7. Topology integration

`get_centrales`, `get_barras`, `get_lineas` produce DataFrames that
Agent D's `_topology.py` joins into the canonical `Topology` triple
(`bus`, `generator`, `line` parquet files in §3.3.3).

### 7.1 Bus / line UID resolution

- `barras` returns `barra_id` directly → use as `bus_uid`.
- `centrales.barra_mnemonico` is a **string mnemonic**. The integration
  pass must `merge` it against `barras` on `name == barra_mnemonico` to
  fill `bus_uid`. Agent E exposes `get_barras` → Agent D performs the
  join. If a `centrales` row's mnemonic does not resolve, Agent E logs a
  one-time warning and tags the row `bus_uid = pd.NA` (Agent D drops or
  flags downstream — Agent E does not mutate Topology directly).

### 7.2 Rename history

Per §9.4: when a unit appears under two names in the requested range,
collapse them to a single canonical UID.

The plan:

1. `get_centrales(at_date)` returns one row per active unit on that
   date. `central_id` is the canonical UID.
2. **TODO (integration)**: confirm whether `sipubv2` exposes a
   `centrales/historico` endpoint with `vigencia_desde` /
   `vigencia_hasta`. The v1.1 PDF mentions a history view but the
   exact path is unverified. The plan reserves
   `EP_CENTRALES_HISTORICO` in `_sip_endpoints.py`.
3. If the history endpoint exists, `get_centrales(at_date)` is augmented
   with a lookup: for each row whose `vigencia_desde > at_date - window`
   call the history endpoint and emit a `previous_names` column listing
   prior mnemonics that mapped to the same `central_id`.
4. Agent D consumes `previous_names` to build a name → uid alias table
   so dispatch rows arriving with the **old** mnemonic still resolve to
   the canonical UID.
5. **CSV-only fallback** (no SIP key): no history available; Agent D
   emits a one-time warning per renamed unit and treats the pre/post
   names as the same canonical UID using a heuristic (Levenshtein ≤ 2
   on normalized names).

The collapse logic itself lives in Agent D's `_topology.py`; Agent E
exposes only the data. `test_sip_topology.py` covers the data contract:
"given a fixture with two `central_mnemonico`s sharing a `central_id`,
the resulting frame yields one canonical UID with both names listed".

---

## 8. Tests

Both files use `pytest`; HTTP layer mocked with `responses` (preferred
for `requests` compatibility; `requests-mock` is the fallback if
already pulled in by the repo). Dependency added to
`scripts/pyproject.toml` by Agent D in P2.0; Agent E supplies the line
to add as a hand-off comment.

Coverage target: **100 % of `_sip_client.py` and `_sip_endpoints.py`
public API**. Anything below is a sub-agent failure to be addressed
before P2.E ships.

### 8.1 `tests/test_sip_client.py` (sub-agent E2)

| # | Case | Asserts |
|---|---|---|
| 1 | `test_auth_header_present` | A `responses` callback captures the header; assert `Authorization` is set and matches the API key. |
| 2 | `test_paginates_three_pages` | Stub 3 pages with `total_pages=3`; client returns concatenated 3-page result; assert exactly 3 GETs. |
| 3 | `test_429_then_success` | First response 429 with `Retry-After: 0`, second 200; assert one retry, one success. |
| 4 | `test_500_retries_exhausted` | All 4 attempts (initial + 3 retries) return 500; assert `SipNetworkError` raised; assert exactly 4 GETs. |
| 5 | `test_401_no_retry` | Single 401 response; assert `SipAuthError` raised on first attempt. |
| 6 | `test_missing_api_key` | `sip_client_from_env(api_key=None)` with `CEN_API_KEY` unset (`monkeypatch.delenv`) → `MissingApiKey`. |
| 7 | `test_date_range_filter` | Mock callback inspects query params; assert `fecha_inicio` and `fecha_fin` are emitted as `YYYY-MM-DD` strings. |
| 8 | `test_empty_result` | Stub returns `{"data": [], "total_pages": 0}`; assert empty DataFrame with the right columns. |
| 9 | `test_timezone_conversion` | Fixture row `2026-04-01T00:00:00-04:00`; assert resulting `ts_utc` equals `2026-04-01T04:00:00Z`. |
| 10 | `test_healthcheck_ok` | 200 response → `True`. |
| 11 | `test_healthcheck_failure` | 500 response → `False`, no exception. |
| 12 | `test_rate_limit_backoff_schedule` | Patch `time.sleep` and `random.uniform` to deterministic values; trigger three 429s; assert sleep called with `[0.5, 1.0, 2.0]` (plus jitter offsets). |
| 13 | `test_get_centrales_columns` | Fixture round-trip → frame has expected canonical columns and dtypes. |
| 14 | `test_get_barras_columns` | Same as above for `barras`. |
| 15 | `test_get_lineas_columns` | Same as above for `lineas`. |
| 16 | `test_invalid_response_raises_schema_error` | Fixture missing `data` key → `SipSchemaError`. |

All run under `pytest -n auto` (per `feedback_pytest_parallel`).

### 8.2 `tests/test_sip_topology.py` (sub-agent E3)

| # | Case | Asserts |
|---|---|---|
| 1 | `test_centrales_canonical_columns` | `get_centrales` returns frame with `[uid, name, bus_uid, pmin, pmax, declared_MC, kind, vigencia_desde, vigencia_hasta]` (TODO-marked columns optional). |
| 2 | `test_rename_history_collapses_to_uid` | Fixture: two records share `central_id=42` with different mnemonics + non-overlapping `vigencia` windows → frame yields one row per (central_id, name) with consistent UID; `previous_names` column lists the old mnemonic. |
| 3 | `test_unresolved_mnemonic_warns` | Fixture: a `centrales` row with `barra_mnemonico` not present in `barras`; assert `bus_uid is pd.NA` and a warning was emitted exactly once. |
| 4 | `test_kind_mapping` | `tecnologia: "térmica"` → `kind: "thermal"`, `"hidroeléctrica"` → `"hydro"`, `"solar"` → `"profile"`, etc. — exhaustive against the constants in `_sip_endpoints.py`. |
| 5 | `test_lineas_active_flag` | `en_servicio=False` → `active=False`. |
| 6 | `test_topology_uid_stability` | Two consecutive calls on the same `at_date` produce identical UIDs (sort-deterministic). |

All fixtures live under `tests/data/sip_fixtures/`. JSON content is
hand-authored small payloads (≤ 50 rows) — not full CEN exports.

### 8.3 Out of scope for these test files

Live network access (deferred to `tools/sip_capture.py` and the opt-in
`GTOPT_RUN_CEN_BACKTEST=1` integration test owned by Agent F in Phase 3),
schema-version negotiation, and real-data SLA assertions.

---

## 9. Test fan-out — sub-agent briefs

Agent E owns two of the five files in §9.6 of the master plan. The
remaining three (`test_csv_parsers.py`, `test_writer.py`,
`test_cli_smoke.py`) are owned by Agent D's track but Agent E **drives**
the parallel fan-out per §11 / §12.1. Below are the one-paragraph briefs
Agent E hands to each sub-agent:

> **Sub-agent E1 — `tests/test_csv_parsers.py`**.
> Pin-test Agent D's CSV readers against offline fixtures
> (`tests/data/csv_fixtures/`). Cover the four endpoints from §9.2.1,
> tz handling (`Chile/Continental` → UTC), accent / `N°` glyph
> normalisation, hour-25 historical DST, missing rows. Use `pytest -n
> auto`. Do not import `_sip_client` or modify it.

> **Sub-agent E2 — `tests/test_sip_client.py`**.
> See §8.1 above. Owned by Agent E.

> **Sub-agent E3 — `tests/test_sip_topology.py`**.
> See §8.2 above. Owned by Agent E.

> **Sub-agent E4 — `tests/test_writer.py`**.
> Round-trip the canonical-feed parquet writer (Agent D's `_writer.py`)
> against the Phase-0 gold fixture; assert byte-identical output across
> two runs (idempotency, modulo `manifest.json:fetched_at`). This is the
> schema lock-in test from Phase 0 promoted to Phase 2. Do not import
> `_sip_client`.

> **Sub-agent E5 — `tests/test_cli_smoke.py`**.
> `cen2gtopt --help` exits 0; `--manifest-only` works without network;
> `--from > --to` exits 3; missing API key + `--source sip` exits 3
> with a clear single-line message; `--source auto` with no key falls
> back to CSV with a warning. Use `subprocess` + `--cache <tmp>` to
> avoid touching the user cache.

Sub-agents E2 and E3 (the ones Agent E directly writes) start as soon as
P2.B `_sip_client.py` skeleton is in place. E1, E4, E5 start once Agent
D signals their respective production modules are landed.

Per `feedback_no_parallel_builds` Agent E does not run a build while
Agent D is building. Test-only changes (Python edits) do not count as
builds; pytest invocations are fine to run in parallel with D's edits.

---

## 10. Offline fixture generator — `tools/sip_capture.py`

A one-shot helper that records real responses for fixture refresh.
**Outside P2.B/P2.E scope**; flagged Phase 2.5 / opt-in.

CLI:

```text
sip_capture.py
    --endpoint {costo_marginal_real,generacion_real,demanda_real,
                centrales,barras,lineas,rename_history}
    --date YYYY-MM-DD                  # for catalogue endpoints
    [--date-from YYYY-MM-DD]            # for series endpoints
    [--date-to   YYYY-MM-DD]
    --out scripts/cen2gtopt/tests/data/sip_fixtures/<name>.json
    --allow-network                    # required; no-op without it
    [--api-key $CEN_API_KEY]
    [--page-size 50]                    # keep fixtures small
```

Behaviour:

- Without `--allow-network` → exits 0 with a "fixture refresh is
  network-bound; pass --allow-network to enable" message. CI never
  passes the flag.
- With `--allow-network` → instantiates `SipClient`, hits the endpoint,
  writes the *raw* JSON envelope (not the canonical frame) to `--out`.
- Re-running with the same args produces a byte-identical file modulo
  the `fetched_at` field (which is excluded from the dump).

Test: a single `test_sip_capture_smoke.py` (gated behind
`GTOPT_RUN_CEN_BACKTEST=1`) verifies the CLI exits 0 without
`--allow-network`. Live invocation is manual.

This tool is **deferred**: Agent E ships the design, but the
implementation lands either at the end of P2.B (if time permits) or as
its own follow-up PR after Phase 2 closes.

---

## 11. Step-by-step coding order

1. **Land `_sip_endpoints.py`** with all path constants
   (`EP_COSTO_MARGINAL_REAL`, …) and the JSON-key constants
   (`KEY_FECHA_HORA`, …) as `Final[str]`. Empty body, no logic.
   Lint+type passes.
2. **Skeleton `_sip_client.py`**: exception classes, `SipClient`
   constructor, private `_get` and `_paginate` returning iterators —
   no public methods yet. Land `tests/test_sip_client.py` cases 1–4
   (auth header, pagination, 429 retry, 500 exhaust) using `responses`.
3. **Implement `get_costo_marginal_real`** end-to-end (paging → tz
   conversion → frame). Add fixtures for it. Land tests 7–9 of §8.1.
4. **Implement `get_generacion_real` and `get_demanda_real`** — same
   shape, different keys. Reuse paging / tz code via
   `_fetch_series(endpoint, key_field)` private helper.
5. **Implement `healthcheck`** + tests 10–11.
6. **Implement catalogue endpoints** (`get_centrales`, `get_barras`,
   `get_lineas`) and `tests/test_sip_topology.py`. Wire kind mapping
   constants in `_sip_endpoints.py`.
7. **Implement `sip_client_from_env`** + test 6 of §8.1
   (`MissingApiKey`).
8. **Drive the fan-out**: hand E1/E4/E5 briefs to Agent D's
   sub-agents; E2/E3 are already done. Run the full test suite
   (`pytest -n auto`), lint (`pylint --jobs=0`), type
   (`mypy --ignore-missing-imports`). Coverage ≥ 100 % on the two
   modules; ≥ 83 % overall (per CLAUDE.md threshold).

Build/test never runs in parallel with Agent D.

---

## 12. Open questions (TODO at integration time)

1. **Exact `sipubv2` paths.** §9.2.2 of the master plan lists logical
   names; the v2 portal (`portal.api.coordinador.cl/documentacion?service=sipubv2`)
   is the source of truth. All endpoint constants in
   `_sip_endpoints.py` are TODO-marked until verified.
2. **`Authorization` header format.** v1.1 PDF says the API key goes in
   `Authorization`; whether bare, `Bearer`, or `ApiKey`-prefixed is
   unverified. Default ships bare; logger warns on 401 with header-
   format hints.
3. **Pagination envelope.** The plan assumes `{ "data": [...], "page",
   "total_pages" }`. v2 may use cursor pagination (`next` URL) instead.
   Isolated behind `_paginate`.
4. **API-key lifecycle.** Does the key expire? Is there a refresh
   endpoint? The v1.1 PDF mentions registration but not expiry.
   `MissingApiKey` is the only error the client raises; expiry shows up
   as 401 → `SipAuthError`. **TODO**: confirm with CEN and document the
   renewal procedure in `docs/scripts/cen2gtopt.md`.
5. **Rate-limit headers.** `Retry-After` honour assumed. CEN may emit
   `X-RateLimit-Remaining` / `X-RateLimit-Reset`; if so we should plumb
   them through to a `client.last_rate_limit_state` introspection
   property to help users size `--cache` and `--source`.
6. **Catalogue history endpoint path.** v1.1 PDF mentions a history
   view; exact `sipubv2` path unverified. `EP_CENTRALES_HISTORICO`
   reserved as TODO.
7. **`fecha` query parameter name.** Catalogue endpoints might accept
   `fecha`, `fecha_consulta`, or `at_date`; reserved as TODO.
8. **Page-size cap.** v1.1 PDF mentions 1000 as the cap; v2 unverified.
   Constructor knob makes this a one-line change when confirmed.

Each TODO has an inline `# TODO(integration): …` marker in the source so
sub-agent E2 can sweep them in a single PR after the first live call.

---

## 13. Summary

In scope of P2.B/P2.E: `_sip_client.py` (6-method, 4-exception client with
paging, retries, auth, tz-aware date parsing); `_sip_endpoints.py` (single
source of truth for paths and JSON keys); two test files with ~22 cases
mocked against offline fixtures; coordination of sub-agents E1–E5 for the
five §9.6 files; a deferred `tools/sip_capture.py` design for fixture
refresh. No invented endpoint paths, no imports from
`gtopt_marginal_units/`, no edits to Agent D's modules — every unverified
assumption carries a `TODO(integration)` marker.
