# CEN Marginal Emissions Toolkit — UX Review and Feature Roadmap

Review and forward-looking feature catalogue for the
`scripts/cen2gtopt/` package, in the context of the international
state of the art on marginal-emission accounting.

---

## 1. New layout (post-reorganisation)

All implementation now lives inside the installable
`scripts/cen2gtopt/` package.  The `tools/cen_emission_factor_v3.py`
file remains as a thin backward-compat shim.

| Module | Purpose | Invocation |
|:---|:---|:---|
| `cen2gtopt.marginal_units` | Per-day pure-compute library + diagnostic single-day CLI | `python -m cen2gtopt.marginal_units` |
| `cen2gtopt.marginal_period` | Multi-day orchestrator with `multiprocessing.Pool` and Hive-partitioned parquet output | `python -m cen2gtopt.marginal_period --month YYYY-MM --output DIR ...` |
| `cen2gtopt.buses` | Bus-catalogue CLI: `list / show / categorize / refresh / islands` | `python -m cen2gtopt.buses {subcommand}` |
| `cen2gtopt._bus_catalogue` | Bus-catalogue library (discovery + persistence + filter helpers) | imported |
| `cen2gtopt.cache` | Cache CLI with daily-update subcommand (existing) | `python -m cen2gtopt.cache daily-update --tier minimal` |
| `cen2gtopt.daily_update` | Refresh-policy library for the cache | imported |

Backward-compat:

```bash
python tools/cen_emission_factor_v3.py     # still works → cen2gtopt.marginal_units
```

---

## 2. UX consistency review

A 5-axis audit of all six CLIs in the package.

### 2.1 Axis 1 — period / date arguments

| Tool | Period syntax |
|:---|:---|
| `marginal_period` | `--start-date / --end-date` *or* `--year` *or* `--month YYYY-MM` *or* `--last-n-days N` |
| `cache daily-update` | `--lookback DAYS` *or* `--today YYYY-MM-DD` (override) |
| `cen2gtopt main` (Phase-2 fetcher) | `--start YYYY-MM-DD --end YYYY-MM-DD` (no shortcuts) |
| `marginal_units` (single-day) | inherits module-level `DATE` constant; no flag |

**Inconsistency**: three different conventions for "what day(s) to process."

**P1 recommendation**: standardise on a single helper
`cen2gtopt._period.parse_period(args)` that supports
`--start/--end + --year + --month + --last-n-days + --today` across
all CLIs.  Move `parse_period` from `marginal_period.py` to a shared
module.

### 2.2 Axis 2 — output paths

| Tool | Output flag |
|:---|:---|
| `marginal_period` | `--output DIR` (Hive parquet dataset) |
| `cen2gtopt` (Phase-2 fetcher) | `--out DIR` |
| `cache daily-update` | (writes to `~/.cache/...`, no flag) |
| `buses` | (catalogue path via `--catalogue`) |

**Inconsistency**: `--output` vs `--out` vs implicit default.

**P2 recommendation**: standardise on `--output / -o`; keep `--out` as
a deprecated alias in the Phase-2 fetcher.

### 2.3 Axis 3 — verbosity / logging

| Tool | Flags |
|:---|:---|
| `marginal_period` | `-v / --verbose` |
| `cen2gtopt main` | `--verbose / --quiet` |
| `cache` | (logging.basicConfig at INFO; no flag) |
| `buses` | (logging.basicConfig at INFO; no flag) |

**Inconsistency**: only two of four expose verbosity controls.

**P1 recommendation**: standardise on `-v / --verbose` *and*
`-q / --quiet` everywhere.  Add a `cen2gtopt._cli.add_verbosity(parser)`
helper.

### 2.4 Axis 4 — cache control + force flags

| Tool | Cache controls |
|:---|:---|
| `cache fetch` | `--no-cache` |
| `cache daily-update` | `--force / --skip-weekly / --skip-scvic` |
| `marginal_period` | `--force` (re-process days), `--refresh-buses cache\|api` |
| `cen2gtopt main` | `--cache DIR / --no-cache` |
| `buses refresh` | `--from-cache / --from-api` |

**Inconsistency**: `--force` vs `--no-cache` vs `--refresh-*` all
overlap conceptually.

**P2 recommendation**: leave each tool's verb unchanged but document a
common semantic:
- `--force` always means "ignore prior outputs and re-execute"
- `--no-cache` means "ignore the *upstream* HTTP/parquet cache"

### 2.5 Axis 5 — exit codes + error reporting

| Tool | Exit codes |
|:---|:---|
| `marginal_period` | `0` ok, `1` >max-failures, `2` safety-prompt blocked |
| `cen2gtopt main` | `0/2/3/4` per master plan §9.5.2 |
| `cache daily-update` | `0/1` |
| `buses` | `0/1` |

**Inconsistency**: scattered code conventions.

**P2 recommendation**: adopt the master-plan §9.5.2 scheme everywhere:
- `0` — success
- `2` — partial success (ran but some rows missing)
- `3` — input/usage error
- `4` — network error

### 2.6 Common flags to add (P1)

Eight flags every CEN CLI in this package SHOULD expose for
consistency with industry-standard tools (PJM Data Miner, gridstatus,
WattTime SDK):

| Flag | Purpose | Already present? |
|:---|:---|:---|
| `--help / -h` | Usage | ✓ everywhere |
| `--version / -V` | Print package version | ✗ missing |
| `--verbose / -v` | INFO + DEBUG logging | partial |
| `--quiet / -q` | suppress all but ERROR | ✗ missing |
| `--output-format {text,json,parquet}` | machine-readable output | ✗ missing |
| `--no-color` | disable ANSI in TTY output | n/a (no colour yet) |
| `--config FILE` | load default args from JSON/TOML | ✗ missing |
| `--dry-run` | show plan without side effects | ✓ on `marginal_period` only |

The `--output-format json` flag is particularly valuable for piping
into downstream tools (`jq`, DuckDB, gridstatus-style aggregators).

---

## 3. Literature review

Concise summary of the four main reference tools / methodologies, and
where we sit relative to each.

### 3.1 PJM Locational Marginal Emissions (LMP-style ε)

**What.** PJM publishes 5-min marginal emission rates per pricing
node (~1 200 nodes) for **CO₂, NOₓ and SO₂** through the Data Miner 2
platform.  The methodology was developed jointly with Resources for
the Future and is a direct LMP analogue: every load node has its own
marginal-emission shadow price, decomposed into **energy, loss and
congestion** components.

**What we have.** A single CO₂ marginal per (bus, quarter), with no
formal LMP decomposition.  The "constructed" ε at the bus uses the
empirical λ-ratio (review §1.5) — not the proper marginal loss-factor
matrix.

**Gap.** Multi-pollutant, congestion-decomposition, dispatchable
forecasts.

### 3.2 WattTime Marginal Operating Emission Rate (MOER)

**What.** A per-balancing-area machine-learning estimate updated every
5 minutes with a **72-hour forecast**.  Built from publicly-available
EIA-930 dispatch data plus EPA eGRID per-unit emission factors.
Trained per-region with a quantile-regression model that predicts
"which fuel will respond to the next +1 MWh of demand."

**What we have.** A look-up table (per-fuel ε) + reverse-engineered
marginal unit.  No forecast; no ML.  Our advantage: per-unit
identification (`marginal_id_central`), which WattTime cannot produce
because the underlying dispatch data is too coarse.

**Gap.** Forecasting; per-unit ε from RETC instead of fuel-default.

### 3.3 Electricity Maps (formerly tmrow)

**What.** Hourly per-zone *consumption-based* CO₂ intensity for ~200
zones globally, including Chile's SEN.  Uses a flow-tracing algorithm
to attribute imports/exports correctly.  As of 2024 they have
**stopped publishing marginal signals**, citing fundamental
limitations of marginal accounting (their published critique is worth
reading: <https://www.electricitymaps.com/content/marginal-emission-factors-in-scope-2-accounting>).

**What we have.** Marginal-only.  No average-intensity output.  No
flow-tracing across CEN's interconnections to Argentina or future
HVDC lines.

**Gap.** Average-intensity companion table; explicit
methodology-limitations disclosure (we already have this in the
critical review, but it's not in the user-facing output).

### 3.4 NREL Cambium

**What.** *Long-run* marginal emission rates (vs WattTime's short-run
operational marginals).  Hourly profiles projected through 2050 across
~134 US balancing zones.  Used for capacity-expansion / EV-grid
studies.  Direct counterpart to gtopt's expansion-planning use case.

**What we have.** Short-run only.  No "what would be the marginal in
2030 under scenario X" output.

**Gap.** Forward-looking capability ties directly to gtopt's GTEP
scenarios — natural integration target for v4.

### 3.5 CAISO Today's Outlook

**What.** Live emissions dashboard at
<https://www.caiso.com/todays-outlook/emissions> — a public web
visualisation for non-experts.  Updates every 5 minutes, exportable
to CSV per series.

**What we have.** Tabular text output only.  No dashboard.

**Gap.** Operator-facing UI; CSV export per series.

### 3.6 CEN Chile (the system we're modelling)

**What CEN itself publishes.**

- *Indicadores Ambientales* page on <https://energia.gob.cl> publishes
  the **annual** average-emission factor for the SEN (kg CO₂ / MWh,
  2020-base updated yearly).
- Energía Abierta has a "Factor de Emisión SIC/SING" visualisation
  (legacy zones, predates 2017 unification).
- The 4eChile guide at <https://4echile.cl/wp-content/uploads/2021/11/Guia-del-usuario.pdf>
  describes a manual marginal-emission-reduction estimation tool for
  MDL projects.

**What CEN does NOT publish.**

- A real-time / per-bus marginal-emission factor.
- A per-unit ε table audited against the SCVIC declarations.
- An LMP-decomposition (energy/loss/congestion) for emissions.

**This toolkit fills exactly the gap CEN itself does not cover for
Chile**, with a methodology comparable to PJM's Locational Marginal
Emissions.

### 3.7 Recent academic critique (Stanford ASL 2022, arXiv 2507.11377)

The 2022 Stanford paper "Dynamic locational marginal emissions via
implicit differentiation" formalises the LMP-style decomposition
PJM uses, and the 2025 follow-up "Moving beyond marginal carbon
intensity" argues for **counterfactual co-simulation** instead of
empirical correlation.  Both are too compute-intensive for our
single-day reverse-engineering, but the **counterfactual oracle**
idea (run the LP twice with ±1 MW perturbation) is feasible offline
and would calibrate our reverse-engineering's ground truth.

---

## 4. Proposed feature roadmap

A prioritised list of features, drawn from the gaps in §3 above and
the open issues from the critical review.

### 4.1 P0 — production-readiness (block any "v1.0" claim)

| # | Feature | Effort | Inspiration |
|:---:|:---|:---:|:---|
| F1 | **Per-unit ε from RETC** instead of FUEL_EF lookup. Wire in `~/.cache/cen2gtopt/retc.parquet` derived from <https://retc.mma.gob.cl> stationary-source declarations. | 2-3 d | WattTime, EPA eGRID |
| F2 | **Reservoir-hydro promotion**: when interior-dispatched, a reservoir hydro IS the marginal at revealed water-value λ. Critical-review §1.4. | 0.5 d | SDDP duality |
| F3 | **Multi-pollutant ε** — add `epsilon_nox_kg_per_mwh`, `epsilon_so2_kg_per_mwh`, `epsilon_pm25_kg_per_mwh` columns from RETC. | 0.5 d (after F1) | PJM LMEs, EPA eGRID |
| F4 | **PCP-oracle back-test** — wire `cmg_programado_pcp` + `gen_programado_pcp` × `costo_generacion_usd` as the ground-truth marginal-unit-per-hour. Critical review §5.20. | 1 d | self-validation |
| F5 | **`marginal-prog-vs-real` report** — daily HTML with per-bus, per-fuel breakdown of where the picker disagrees with the PCP oracle. | 1 d | PJM Data Miner reports |

### 4.2 P1 — methodology improvements (close the WattTime gap)

| # | Feature | Inspiration |
|:---:|:---|:---|
| F6 | **`--forecast HOURS`** — extend the period orchestrator to consume `cmg_programado_pid` and emit forecasted ε for the next 4-72 hours. |  WattTime 72-h forecast |
| F7 | **Average-intensity companion table** — alongside the marginal-ε column, write `epsilon_average_at_bus` from the system mix. Avoids the "marginal-only confusion" that Electricity Maps documented. | Electricity Maps |
| F8 | **Loss-factor proper formula** — replace `λ_bus / λ_ref` with CEN's published Factores de Penalización por Pérdidas matrix (per ND 89/2017 art. 4). | CEN nodal-pricing law |
| F9 | **Counterfactual oracle** — for selected hours, refit a tiny LP (`gen-real` + SCVIC + KCL) with ±1 MW perturbation at each bus to derive a true-marginal ε vector. Validates the closest-CV picker. | Stanford ASL 2022 |
| F10 | **Topology-driven islanding** — replace λ-clustering with binding-line partitioning from `flujo-programado-pcp` + `limitaciones`. Critical-review §3.12. | PJM LMP decomposition |

### 4.3 P2 — UX / consumer features (close the CAISO Today's Outlook gap)

| # | Feature | Inspiration |
|:---:|:---|:---|
| F11 | **`cen2gtopt.dashboard`** — a small Flask/Streamlit web app that reads the parquet dataset and renders today's λ + ε + island map. | CAISO Today's Outlook |
| F12 | **CSV export subcommand** — `python -m cen2gtopt.marginal_period export --date YYYY-MM-DD --bus BUS --format csv`. | CAISO data export |
| F13 | **Time-zone awareness** — output a `quarter_iso_local` column in CLT (UTC-3, CEN's operating-day basis), not just UTC. | gridstatus |
| F14 | **`--output-format json`** for `buses list / show / categorize / islands` — pipes cleanly into `jq` / DuckDB. | gridstatus, PJM API |
| F15 | **Carbon-tax sensitivity flag** — `--co2-price USD_PER_TON` overrides the Chile $5/tCO₂ default to model a higher carbon-price scenario. | Cambium |

### 4.4 P3 — research-grade enhancements

| # | Feature | Inspiration |
|:---:|:---|:---|
| F16 | **Long-run marginal companion** — once gtopt's GTEP solver outputs a 2030 dispatch, run the same identifier against it to produce a Cambium-style long-run ε table. | NREL Cambium |
| F17 | **BESS round-trip ε accounting** — track BESS-marginal cells separately, attribute their ε to the unit that supplied them when charging. Critical-review §3.13. | WattTime |
| F18 | **Cross-border attribution** — when CEN imports from Argentina (Salta link), use Argentine generation-mix ε for the imported fraction. Sets up Electricity Maps-style flow-tracing. | Electricity Maps |
| F19 | **Web hooks → cron output**: when `cen2gtopt.cache daily-update` fetches new D-1 data, automatically trigger `marginal_period --last-n-days 7 --refresh` and push the result to a configured S3 / GCS / object-store bucket. | PJM Data Miner subscriptions |
| F20 | **OpenAPI spec for a thin REST wrapper** — expose `GET /api/v1/marginal?date=YYYY-MM-DD&bus=NN` returning the parquet row as JSON. Lets non-Python consumers reuse the dataset. | PJM API Portal |

---

## 5. Concrete next-step recommendation

If only one feature gets built next, build **F2 (reservoir-hydro
promotion)**.  It is the single largest source of physical incorrectness
in the current pipeline (excludes 14 of Chile's largest plants) and
costs less than a day of work.

If only two features get built, add **F1 (per-unit ε from RETC)**.
That changes the output from "thermal-marginal-fuel classifier" to a
defensible per-unit emissions attribution comparable to the PJM Data
Miner LME feed.

If the goal is to ship a *user-visible* deliverable in a week:
F2 + F1 + F11 (Streamlit dashboard) + F14 (`--output-format json`) is
a sensible sprint.

---

## Sources

- [PJM Five-Minute Marginal Emission Rates — Data Miner 2](https://dataminer2.pjm.com/feed/fivemin_marginal_emissions/definition)
- [PJM Marginal Emissions Primer (PDF)](https://www.pjm.com/-/media/DotCom/etools/data-miner-2/marginal-emissions-primer.ashx)
- [PJM Inside Lines — Marginal Emission Rates added to Data Miner](https://insidelines.pjm.com/marginal-emission-rates-added-to-data-miner-tool/)
- [PJM Locational Marginal Emissions — Harvard HEPG](https://hepg.hks.harvard.edu/links/pjm-locational-marginal-emissions-web-page)
- [WattTime — Average vs Marginal](https://watttime.org/data-science/data-signals/average-vs-marginal/)
- [WattTime — Methodology + Validation](https://watttime.org/data-science/methodology-validation/)
- [Electricity Maps — Marginal vs Average](https://www.electricitymaps.com/technology/marginal-emissions-what-they-are-and-when-to-use-them)
- [Electricity Maps — Why MEFs are unsuitable for scope-2 accounting](https://www.electricitymaps.com/content/marginal-emission-factors-in-scope-2-accounting)
- [NREL Cambium — Forward-Looking Marginal Emission Rates](https://www.nrel.gov/news/detail/program/2023/cambium-offers-forward-looking-publicly-available-data-to-build-into-grid-planning-workflows)
- [NREL — Long-run Marginal Emission Rates Workbooks 2023](https://www.osti.gov/biblio/2305481)
- [CAISO — Today's Outlook (Emissions tab)](https://www.caiso.com/todays-outlook/emissions)
- [Ministerio de Energía Chile — Indicadores Ambientales SEN](https://energia.gob.cl/indicadores-ambientales-factor-de-emisiones-gei-del-sistema-electrico-nacional)
- [Energía Abierta CNE — System Emission Factors SIC/SING](http://energiaabierta.cl/visualizaciones/factor-de-emision-sic-sing/?lang=en)
- [CEN Coordinador — Reporte Anual Art. 72 (2024)](https://www.coordinador.cl/wp-content/uploads/2025/04/CEN-Reporte-Art-72-15-ano-2024.pdf)
- [4eChile / GIZ — Guía marginal-emission-reduction-tool MDL](https://4echile.cl/wp-content/uploads/2021/11/Guia-del-usuario.pdf)
- [Stanford ASL — Dynamic locational marginal emissions via implicit differentiation, IEEE TPS 2022](https://stanfordasl.github.io/wp-content/papercite-data/pdf/Valenzuela.Degleris.ea.IEEETPS22.pdf)
- [arXiv 2507.11377 — Moving Beyond Marginal Carbon Intensity (2025)](https://arxiv.org/html/2507.11377v1)
- [ScienceDirect — Towards objective evaluation of marginal emission factors (2025)](https://www.sciencedirect.com/science/article/pii/S1364032125001819)
- [WattTime / pyiso — Python ISO data clients](https://github.com/WattTime/pyiso)
- [gridstatus — open-source ISO data Python library](https://opensource.gridstatus.io/en/latest/autoapi/gridstatus/pjm/index.html)
