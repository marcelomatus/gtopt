# Marginal-unit analysis — design plan (two scripts, two phases)

> **Status**: planning. Implementation not started.
>
> **Goal**: identify, for every hour/block of an SEN-style power-system
> dispatch, the **marginal generating unit(s)** that set the energy
> price at each bus, together with their classification (marginal,
> forced-pmin, capped-pmax, off, inframarginal, hydro/water-value,
> etc.) and a **congestion-zone partition** of the network. Produce
> the attribution from either a simulated gtopt run *or* the realised
> operation of Chile's SEN published by the
> [Coordinador Eléctrico Nacional][cen-home] (CEN).
>
> **Two scripts, two phases** (see §1.5):
>
> 1. **`gtopt_marginal_units`** — pure analysis script. Reads a
>    gtopt output directory **or** a *canonical operation feed* (a
>    well-defined parquet/CSV schema, §3.3.4) and emits the
>    marginal-unit attribution. Phase 1 develops and validates this
>    script entirely against gtopt simulated data — no network access,
>    no CEN dependency.
>
> 2. **`cen2gtopt`** — data-retrieval script. Pulls the minimum
>    required real-operation data (topology + realised generation +
>    realised demand, §4.8.1) from the CEN public website and SIP API,
>    normalises it, and writes the canonical operation feed that the
>    Phase-1 script consumes unchanged. Phase 2 develops this script
>    in parallel once the schema is locked.
>
> The two scripts share **only the canonical schema** (§3.3.4); they
> are independently versioned and independently testable. The
> Phase-1 script never imports anything from `cen2gtopt`, never opens
> a network socket, and runs in CI on offline fixtures only.

> **Recent revisions (2026-05-06 — LME literature & ISO practice)**
>
> * §4.12.5 added — full literature/ISO survey for marginal
>   emissions: Lin & Tang 2024 (arXiv:2411.12104) provides the
>   theoretical justification for the recipe-table architecture
>   (`β = Φ(α)`, both share the generation-sensitivity term).
>   PJM publishes 5-min and hourly nodal LMEs for CO₂/SO₂/NOₓ via
>   Data Miner 2 — its "system marginal + one per binding
>   constraint, then average" rule is richer than our zone-based
>   §4.7 R3 and is documented as a v1.1 enhancement target.
>   WattTime MOER is the statistical alternative (regression on
>   EPA CEMS) — added as opt-in `--moer-compare` back-test.
>   MISO plans nodal LME for late 2025 following PJM. The CEBI
>   sourcing guide's three-method taxonomy (merit-order /
>   economic-dispatch / statistical) maps cleanly: our
>   `mode=real-reconstruct` is merit-order, our `mode=simulated` is
>   economic-dispatch (LP duals).
> * §4.12.6 — split confirmed-for-v1 vs targeted-for-v1.1
>   (multi-pollutant, PJM per-constraint decomposition,
>   MOER-style statistical mode).
> * §5 CLI gains `--moer-compare path/to/moer.csv`.
>
> **Recent revisions (2026-05-06 — marginal emission intensity)**
>
> * §4.12 added — marginal emission intensity ε_b
>   [kgCO₂eq/MWh] computed in lockstep with the marginal price. Same
>   merit-list pick drives both formulas; only the per-unit datum
>   differs (`emission_rate` vs `declared_MC`).
> * §3.3.3 topology gains optional `emission_rate:float|None`
>   column. §4.6 dataset gains a sibling
>   `bus_emission_intensity_recipe.parquet`. CI invariant: identical
>   `marginal_gen_uids` across the price and emission recipe tables.
> * §4.10 consumer API gains `bus_emission_intensity()`,
>   `recompute_emission(unit_emissions=...)`, and a combined
>   `bus_lmp_and_emission()` view.
> * §5 CLI adds `--emission-attribute co2` (multi-pollutant is v1.1).
> * §9.2.5 added — CEN per-unit emission-factor source via the
>   reportes-y-estadisticas portal; `cen2gtopt --include emissions`
>   populates the topology column.
> * §7.1 grew to 13 files (added `test_emission_intensity.py`).
>
> **Recent revisions (2026-05-05 — unit-commitment regime catalogue)**
>
> * §2.7 added — frames "forced pmin" as a special case of unit
>   commitment, the master hidden binary `u ∈ {0,1}` whose
>   *driver* (not the dispatch level) determines whether a unit
>   belongs in the merit list.
> * §4.11 added — twelve-driver catalogue of commitment regimes
>   that take a unit out of merit (forced pmin, gas inflexible,
>   SSCC assignments, N−1 contingency, planned/forced outage,
>   instructed dispatch, hydro irrigation, environmental, CC
>   commit lock, black-start, inertia), each classified as
>   observable / partially-observable / requires-CDC-data, with a
>   v1/v1.1/v1.2 ship plan. New §4.11.2 `merit_eligible` filter
>   that §4.7 R3 and §4.9 ladder both honour.
> * §3.3.3 canonical schema gains optional
>   `cells/commitment.parquet` (`u ∈ {0,1}`, gtopt-only) and
>   `cells/unit_regime.parquet` (the 12-driver regime flags). Both
>   default to "not forced" when absent so the script runs without
>   them.
> * §9.2.4 added — `cen2gtopt` source map for each driver
>   (Restricciones Operativas, RIO, RIO-SSCC, Indices de
>   Indisponibilidad, IRF, irrigation registry).
> * §6 edge case 15 added — the unit-commitment-regime
>   observability gap, with the new `--require-regime-data`
>   strict-audit flag.
> * §5 CLI adds `--require-regime-data` and the previously-promised
>   `--require-cdc-restriction` flag.
>
> **Recent revisions (2026-05-05 — CDC operational saturation)**
>
> * §3.3.3 canonical schema gains an optional
>   `cells/line_restriction.parquet` table with a per-`(cell, line)`
>   `restriction_declared` boolean — the machine-readable form of
>   CDC's operational saturation declarations. Phase-2's
>   `cen2gtopt` populates it from CEN's
>   [Restricciones Operativas][cen-restricciones],
>   [RIO][cen-rio], and [Novedades CDC][cen-novedades] feeds; other
>   producers may leave it absent.
> * §4.7 R1 rewritten with an explicit four-tier priority:
>   (1) CDC declaration, (2) LP dual (simulated only), (3) flow-only
>   inference, (4) PTDF estimate. Tiers 3–4 set
>   `confidence = fallback` and the audit table records which rule
>   was used per `(cell, line)`. New `--require-cdc-restriction`
>   flag forces strict-audit users to refuse the inference paths.
> * §6 edge case 14 added: explicit warning that operational
>   saturation in the SEN is a CDC declaration, *not* a flow
>   inference.
> * §9.2.3 added: the three CDC sources (RIO, Restricciones
>   Operativas, Novedades CDC) and how `cen2gtopt` ingests them,
>   with explicit deferral notes for the v1 vs v1.1 split.
>
> **Recent revisions (2026-05-05 — storage design, merit ladder, consumer API)**
>
> * §4.6 restructured: the output is now a **parquet dataset
>   directory** with five tables (`attribution/per_bus`,
>   `attribution/per_zone`, `merit_ladder`, `bus_price_recipe`,
>   `audit/*`) plus `manifest.json` instead of a single parquet
>   file. The primary table `attribution/per_bus.parquet` is sorted
>   for `(cell, bus)` lookups so "which unit set the price at bus B
>   in hour H?" is a one-line filter.
> * §4.6.4 + new §4.9 — **merit ladder** with default depth `K=3`
>   above and below the anchor; new `--merit-ladder-depth` flag.
>   Lets the user verify "if marginal-unit X trips, who sets the
>   price?" without re-running the LP.
> * §4.6.4 + new §4.10 — **bus price recipe** + **consumer API**:
>   the saved data is the source of truth for downstream price
>   recomputation; `gtopt_marginal_units.recipes.MarginalUnitDataset`
>   exposes `bus_lmp`, `recompute_lmp(unit_costs=...)`,
>   `outage_sensitivity(g)`, `merit_ladder(...)`. Two writer-side
>   invariants are CI-tested: recipe round-trip and writer
>   pre-persist consistency.
> * §7.1 test list grew to 12 files (added `test_merit_ladder.py`,
>   `test_recipes.py`, `test_writer_invariants.py`). Phase-1 Agent C
>   now drives 8 sub-agents instead of 6; the §12.2 cap was raised
>   accordingly.
> * §11 Phase 1 split into 3 new milestones P1.I/P1.J/P1.K under
>   Agent C; total Phase-1 estimate moves from 5 → 6 days.
> * §10 added two explicit non-goals: cross-zone merit-ladder rungs
>   and multi-step outage cascades.
> * §13 added 3 open questions (5–7) on the merit-ladder defaults.
>
> **Recent revisions (2026-05-05 — P0 fixes from the planner-fan-out review)**
>
> * Package renamed `_canonical_feed` → `gtopt_canonical_feed`
>   (leading underscore was being dropped by `setuptools.packages.find`
>   and skipped by the CLAUDE.md pylint/mypy command lists). [python-reviewer P0-1]
> * Auth env var aligned with the existing `scripts/cen_demanda/`
>   convention: primary `$CEN_USER_KEY`, deprecated alias `$CEN_API_KEY`. [ux-critic P0-4]
> * `cen2gtopt` date flags renamed `--from`/`--to` → `--start`/`--end`
>   (matches `cen_demanda` muscle memory). [ux-critic P0-1]
> * `--input gtopt|feed` renamed `--input-kind {gtopt-dir,feed-parquet,auto}`
>   with planning-vs-feed sniff auto-detection as the default. [ux-critic P0-2/P0-3]
> * §3.2 documents the v1 expansion-not-supported abort
>   (`flowp_cost` is the flow-column reduced cost; capacity duals
>   `capacityp_dual`/`capacityn_dual` are needed for expansion lines
>   and slated for v1.1). [lp-numerics P0.1]
> * §4.5 forced-pmin zone-picker now uses the new `tol_load_mw` (MW)
>   instead of `tol_price` ($/MWh) — earlier draft mixed units. [lp-numerics P0.2]
> * §4.7 R1 PTDF builds **per connected component** (no singular
>   matrix on islanded topologies); refuses to run on missing /
>   non-positive reactance instead of falling back silently. [lp-numerics P0.3]
> * §4.7 R3 all-pmax corner: `λ_z = demand_fail_cost` (not cheapest
>   pmax MC) — LMP theory requires the rationing cap when every unit
>   is supply-bound. [lp-numerics P0.4]
> * §4.7 R5 audit columns renamed to valid Parquet identifiers
>   (`cells_within_0p5_usd_per_mwh`, …); added
>   `cells_with_demand_fail_clamp`. [ux-critic P1]
> * §6.1 `assert` → `if not (...): raise ValueError(...)` so the
>   sanity check is not stripped by `python -O`. [python-reviewer P0-3]
> * §7.1 test list extended with the 5 P0 coverage gaps
>   (`hydro_marginal` row 4, `extramarginal_interior` row 7,
>   forced-pmin zone-picker fallback, R3 demand-fail clamp, exit-code-2
>   smoke test) plus `test_expansion_guard.py`. [test-coverage P0]

---

## 1. Why this script

After every gtopt run, post-processing teams (and Chile's CEN-style
operator workflow) need to answer: *"which unit set the price in
hour h, scenario s, stage p?"*. Today gtopt prints LMPs (`Bus/balance_dual`)
and dispatch (`Generator/generation_sol`) but never writes the
**marginal-unit attribution** — the user must reverse-engineer it by
hand. This script formalises that attribution and writes it to a
single canonical artifact.

It also makes visible, per cell:

* generators **forced at pmin** (must-run / inflexible) — the literature
  consistently flags these as price-setters under congestion, and they
  are the ones CEN-style "decoupling" rules expose;
* generators **capped at pmax** (binding-up) — extramarginal but
  inframarginal-priced;
* generators **dispatched in the strict interior** — the textbook
  marginal candidates;
* the **congestion-zone partition** induced by saturated lines, so each
  zone's marginal unit can be reported separately (CEN-style
  *subsistema desacoplado*).

---

## 1.5 Phase plan and script split

The work splits cleanly along a **schema boundary** (the canonical
operation feed of §3.3.4). Everything *below* the boundary is data
plumbing; everything *above* is methodology. This split lets us
develop the two halves in parallel after a short Phase 0.

```
                   ┌─────────────────────────────────────┐
                   │ Phase 0 — schema lock-in (1 day)    │
                   │ • freeze §3.3.4 canonical schema    │
                   │ • write the gold parquet fixture    │
                   │ • land scripts/gtopt_canonical_feed/  │
                   └────────────────┬────────────────────┘
                                    │
              ┌─────────────────────┴──────────────────────┐
              │                                            │
              ▼                                            ▼
  ┌────────────────────────────┐              ┌────────────────────────────┐
  │ Phase 1 — methodology      │              │ Phase 2 — CEN data         │
  │ scripts/gtopt_marginal_    │              │ scripts/cen2gtopt/         │
  │ units/                     │              │                            │
  │                            │              │                            │
  │ Inputs:                    │              │ Inputs:                    │
  │  • gtopt output dir        │              │  • date range, --bus list  │
  │  • OR canonical feed       │              │  • CEN CSV exports / SIP   │
  │ Outputs:                   │              │ Outputs:                   │
  │  • marginal_units.parquet  │              │  • canonical feed parquet  │
  │  • report.md               │              │    (§3.3.4)                │
  │                            │              │                            │
  │ Runs entirely on gtopt     │              │ Network-bound, runs in CI  │
  │ data; CI-stable.           │              │ only against fixtures.     │
  └────────────┬───────────────┘              └─────────────┬──────────────┘
               │                                            │
               └─────────────────────┬──────────────────────┘
                                     ▼
                ┌───────────────────────────────────────┐
                │ Phase 3 — integration (1.5 days)      │
                │ • run marginal script over the feed   │
                │   produced by cen2gtopt               │
                │ • back-test reconstruction §4.7       │
                │   against published CEN LMP           │
                └───────────────────────────────────────┘
```

* **Phase 0** locks the schema. After it, the two scripts can be
  worked on in parallel by separate agents (§11) without coordination.
* **Phase 1** is the higher-risk methodology work — what the user is
  actually paying us to figure out. It must work end-to-end against
  gtopt simulated data **before** any CEN data is fetched.
* **Phase 2** is data plumbing — well-bounded, no methodology
  uncertainty, parallelisable into per-endpoint sub-tasks.
* **Phase 3** is the join: feed `cen2gtopt` output into
  `gtopt_marginal_units --input canonical-feed`, run the §4.7
  reconstruction against real data, audit against CEN's published
  number.

The contract between Phase 1 and Phase 2 is **only** the canonical
schema in §3.3.4. Neither script imports the other; they are wired
together through a parquet on disk. This is deliberate: it lets us
run `gtopt_marginal_units` without ever touching CEN, and it lets
`cen2gtopt` evolve independently as CEN's API changes.

---

## 2. Domain background — what is a "marginal unit"

### 2.1 Textbook (LMP / DC-OPF)

For a lossless DC-OPF the locational marginal price (LMP) at bus *b*
decomposes (in gtopt, no losses) as

> λ_b = energy_component + congestion_component_b

A generator *g* connected to bus *b* is the **marginal unit** of bus *b*
if, at the LP optimum:

1. it is dispatched in the **strict interior** of its feasible range,
   `pmin_g < g_dispatch < pmax_g`; and
2. its **marginal cost equals the bus LMP**, `MC_g = λ_b` (within tol);
   equivalently, its reduced cost is zero.

When several units satisfy both conditions, all of them are marginal
(typical with piecewise-linear costs and/or LP degeneracy). When *no*
unit satisfies them at a bus (e.g. all units are pinned at bounds), the
marginal "unit" is a **virtual demand-side resource** — load shedding
(`demand_fail_cost`) — and the LMP equals that penalty cap.

### 2.2 Congestion-induced electrical sub-zones

Transmission saturation breaks the *copperplate* structure. When a line
limit binds, the network behaves as if split into sub-zones, each with
its own dispatch and its own marginal unit. CEN's procedure for
*Costo Marginal Real* reflects this in the rule:

> *"When a subsystem operates in a decoupled manner, the marginal cost
> of its buses must be calculated as the cost necessary to supply an
> additional unit of energy in the buses of said decoupled subsystem."*
> — CEN, [Costos Marginales][cen-costos]

Operationally:

* identify saturated lines (`|flow| ≥ tmax · (1 − tol)` AND
  `|line_dual| > tol`);
* remove them from the topology graph;
* connected components of the resulting graph = **congestion zones**;
* within each zone the LMP should be (numerically) constant — that is
  the zone's marginal price, and the unit(s) sitting at the interior of
  their bounds at that price is/are the zone's marginal unit(s).

This handles the two flavours the user asked for:

* **real (physical) islands** — disconnected subnetworks in the
  topology itself; their zones exist regardless of dispatch;
* **temporary islands** induced by line saturation — same algorithm,
  applied dispatch-by-dispatch.

### 2.3 Forced units, must-run, technical minimum

The literature (and CEN's operating rules) carve out specific
classifications that this script must surface even though they are *not*
the marginal unit by definition:

| Status | LP signature | Meaning |
|---|---|---|
| `marginal` | interior dispatch, `MC ≈ λ_b` | sets the price |
| `forced_pmin` | `g ≈ pmin_g > 0`, reduced cost > 0 | must-run / technical minimum |
| `capped_pmax` | `g ≈ pmax_g`, reduced cost < 0 | capacity-limited inframarginal |
| `inframarginal` | interior, `MC < λ_b` | profitable but not price-setting |
| `extramarginal_off` | `g ≈ 0`, `MC > λ_b` | uneconomic, undispatched |
| `hydro_water_value` | reservoir/battery dispatched at interior | priced via storage dual, not gcost |

`forced_pmin` units are economically distinguished from `marginal`
units even when CEN's "merit-order" headline rule would lump them
together: their MC may be **above** the LMP because they are running
*in spite of* economics, and removing them would not lower the LMP.
Conversely, in a single-unit zone where the only running unit is at
`pmin`, that unit *does* set the price (load = pmin) and the script
must report it as `forced_pmin_marginal` — see §4.3.

### 2.4 Hydro reservoirs and batteries

For hydro/storage the relevant marginal cost is the **water value** =
the dual of the reservoir/battery balance constraint, not `gcost`. In
the LP:

> ∂(objective) / ∂(volume_t) = water_value_t  ($/m³ or $/MWh-eq)

For the marginal-unit identification we therefore:

* treat hydro/battery units as marginal candidates whenever they are
  dispatched in the interior of their (turbine flow / power) bounds;
* use their **discharge price**, computed from the reservoir/battery
  shadow price, instead of `gcost`, when comparing against the bus LMP.

For v1 we do not require an exact water-value match — we will simply
**flag** hydro/battery as `hydro_marginal` when it is interior, and
report the bus LMP and reservoir/battery dual side-by-side; the user
can audit the equality numerically.

### 2.5 Piecewise-linear costs

gtopt encodes piecewise costs through `commitment` segments
(`pmax_segments`, `heat_rate_segments` in
`include/gtopt/commitment.hpp`) plus `Generator/generation_cost` per
unit. The active segment is the one whose cumulative capacity straddles
the dispatch level. The unit's MC at the optimum is the slope of that
segment; that is the value the script compares against the bus LMP.

When the dispatch sits exactly at a segment break, two segment slopes
are consistent — the script reports the unit as marginal with an
ambiguous-segment flag.

### 2.6 Tie-breaking and degeneracy

LP degeneracy can produce a continuum of optimal bases that are all
"marginal" with the same primal solution. Symptoms:

* multiple units interior at the same `MC ≈ λ_b`;
* an LMP equal to the MC of a unit pinned at `pmin` or `pmax`
  (boundary-marginal — the unit *would* be marginal if perturbed);
* zero LMP at a bus with strictly positive load (only happens when
  spillage / curtailment is binding).

The script reports all such cases with a `degenerate=True` flag and a
human-readable reason. We do **not** try to pick a single "canonical"
marginal unit — that would hide real ambiguity.

### 2.7 Unit commitment is the master regime — observability gap

Every conventional thermal generator carries a hidden **commitment
status** `u ∈ {0, 1}` per period. The economic dispatch
`g ∈ {0} ∪ [pmin, pmax]` is feasible only because of `u`:

* `u = 0` ⇒ `g = 0` (unit off; contributes nothing to merit order);
* `u = 1` ⇒ `g ∈ [pmin, pmax]` (unit on; *eligible* — but only
  marginal when `g` lies *strictly* in the interior `(pmin, pmax)`).

"Forced pmin" and "must-run" are not separate concepts from
commitment — they are **commitment decisions made for a non-economic
reason**. A unit at `u=1, g=pmin` with `MC > λ_z` was committed
*because* of some external driver (a gas obligation, a reserve
assignment, an N−1 contingency, …) and only happens to be at its
floor because that's the cheapest way to honour the commitment.
Identifying the *driver* is what tells us the unit is out of merit;
the dispatch level alone does not.

In `mode=simulated`, `u` is the LP/MIP commitment binary. gtopt
exposes it directly (`Generator/commitment_sol` when MIP commitment
is enabled, otherwise the LP relaxation), and `u=1, g=pmin` is
distinguishable from `u=1, g>pmin` by reading the variable. The
reduced-cost sign on the `pmin ≤ g` bound tells you whether the unit
is *forced* at its minimum (positive reduced cost — unit would shut
down if commitment were free) or *coincidentally* there (zero
reduced cost — degenerate but economic).

In `mode=real` / `mode=real-reconstruct`, `u` is **not observable
from realised dispatch alone**. The crucial pathological case:

> A unit running at `g ≈ pmin` with `MC > λ_z` could be
> economic-but-degenerate, or could be *committed against economics*
> for one of a dozen reasons (gas inflexible, ancillary service
> assignment, N−1 contingency, voltage support, …). The
> merit-order rule §4.7 R3 quietly assumes the first interpretation;
> the second interpretation **takes the unit out of the merit list**
> and the script will pick the wrong price-setter.

This is the same data-observability problem as CDC line-saturation
declarations (§4.7 R1) but for generators: the *reason* a unit is
committed sits in CDC's operational decisions, not in the
generation/demand time-series. §4.11 enumerates every
commitment-driver that takes a unit out of the merit list,
classifies each as observable-from-data vs. requires-CDC-data-source,
and maps each to the canonical schema's optional regime columns.

---

## 3. Inputs `gtopt_marginal_units` consumes (Phase 1)

The Phase-1 script reads from exactly **two source kinds** through a
single `--input` flag:

1. **A gtopt output directory** (with the planning JSON used to
   produce it). Native simulated mode — every column documented in
   §3.2 is available.
2. **A canonical operation feed** (§3.3.4) — a single parquet (or
   parquet-dataset) conforming to a frozen schema. Phase 1 ships a
   reader that loads this feed into the same in-memory frames used
   by simulated mode; Phase 2's `cen2gtopt` is the first producer of
   such a feed, but anyone can produce one.

The classifier behaviour is then driven by `--mode`:

| Mode | Cell key | LMP source | Available with `--input-kind gtopt-dir` | Available with `--input-kind feed-parquet` |
|---|---|---|---|---|
| `simulated` *(default)* | `(scene, stage, block)` | LP `Bus/balance_dual` | yes | n/a |
| `real` | `(date_utc, hour)` | from feed (`lmp` column populated) | n/a | yes |
| `real-reconstruct` | `(date_utc, hour)` | reconstructed from merit order on realised dispatch (§4.7) | n/a | yes (with `lmp` column ignored or absent) |
| `compare` | both | side-by-side | yes (paired with a feed via `--feed-against`) | yes (paired with a gtopt dir via `--gtopt-against`) |

`real-reconstruct` is the audit mode: it deliberately **does not
read** the LMP column from the feed and instead recovers it from
realised generation, demand, declared variable costs, and the
topology (§4.7). Running `mode=compare` afterwards then checks the
reconstruction against whatever LMP the feed *did* contain (typically
CEN's published Costo Marginal Real piped through `cen2gtopt`).

The classifier in §4 operates on the **canonical schema** (§3.3.4),
which both `--input-kind gtopt-dir` and `--input-kind feed-parquet` populate. The
mode-specific code lives only in two thin reader modules
(`scripts/gtopt_marginal_units/_gtopt_reader.py`,
`scripts/gtopt_marginal_units/_feed_reader.py`); everything else is
mode-agnostic.

### 3.1 The planning JSON (topology + costs)

Loaded via the existing helper
`scripts/gtopt_check_output/_reader.py:load_planning(...)`. Fields used:

| Path | Use |
|---|---|
| `system.bus_array[*].uid, .name` | bus → name |
| `system.generator_array[*].uid, .name, .bus, .pmin, .pmax, .gcost, .capacity` | unit attributes |
| `system.generator_array[*].commitment{pmax_segments, heat_rate_segments, ...}` | piecewise costs |
| `system.line_array[*].uid, .name, .bus_a, .bus_b, .tmax_ab, .tmax_ba` | DC-OPF topology |
| `system.reservoir_array[*]`, `.battery_array[*]`, `.generator_profile_array[*]` | hydro/storage/intermittent classification |
| `simulation.block_array[*].duration` | hour weights |
| `simulation.stage_array`, `.scenario_array` | (scene,stage) keying |
| `options.demand_fail_cost`, `.scale_objective`, `.use_kirchhoff`, `.use_single_bus` | sanity-checks for LMP scale and topology mode |

`gcost` may itself be a time-series reference (`OptTRealFieldSched`).
v1 only handles the scalar form; v1.1 will resolve the scheduled form
through `gtopt_field_extractor`.

### 3.2 The output CSV / Parquet directory

Read via `scripts/gtopt_check_output/_reader.py:read_table(...)`,
which transparently handles single-file, partitioned-parquet, and
sharded-CSV layouts.

| Stem | Provides | Required |
|---|---|---|
| `Generator/generation_sol` | dispatch `g_{s,p,b,uid}` | yes |
| `Generator/generation_cost` | per-cell variable cost (audit) | optional |
| `Bus/balance_dual` | LMP `λ_{s,p,b,uid}` | yes |
| `Line/flowp_sol` | line flow `f_{s,p,b,uid}` | yes (else single-bus mode) |
| `Line/flowp_cost` | reduced cost on flow var = capacity dual | yes |
| `Line/theta_dual` | KVL Lagrangian (informational) | optional |
| `Demand/load_sol`, `Demand/fail_sol` | for ENS / demand-fail flag | optional |
| `Reservoir/.../*dual*`, `Battery/.../*dual*` | water values | optional (warns if missing for hydro-marginal cells) |

> **Which line dual is the congestion price?** From IEEE 14b output we
> see `theta_dual` carrying the KVL multiplier (nonzero on swing-bus
> reference) and `flowp_cost` carrying the reduced cost on the flow
> variable, which is exactly the bound multiplier on
> `−tmax_ba ≤ f_l ≤ tmax_ab`. The script uses **`flowp_cost`** for
> congestion detection and reports `theta_dual` only as auxiliary info.
> An invariant test in §7 will pin this down on a deliberately-congested
> 2-bus case.
>
> **⚠ v1 limitation — transmission expansion not supported.** When a
> line carries a capacity-expansion variable, the LP replaces the
> bound `−tmax ≤ f ≤ tmax` with a capacity *constraint row*; the
> congestion shadow then lives on `Line/capacityp_dual` /
> `Line/capacityn_dual` (not yet exposed in the canonical schema)
> and `flowp_cost` collapses to zero. v1 of `gtopt_marginal_units`
> aborts with exit code 3 and a clear message when it detects any
> generator/line with `expansion=true` in the planning JSON. v1.1
> will extend the canonical schema with optional `capacityp_dual` /
> `capacityn_dual` columns and route the congestion-detection rule
> through whichever pair is non-NA per cell. See §10 (out of scope).

### 3.3 The canonical operation feed (Phase 1 / Phase 2 contract)

Phase 1 reads either a gtopt output directory or a **canonical
operation feed** through `--input-kind feed-parquet`. The feed is the single API
between the two scripts: Phase-2's `cen2gtopt` (§9) writes one,
Phase-1's `gtopt_marginal_units` reads it. Anyone can produce a feed
from any data source as long as they conform to this schema.

The CEN-side details — which website pages, which API endpoints, how
to authenticate, how to handle accents and time zones — are
**deliberately not in this section**; they live in §9, the
Phase-2 spec. Phase 1 is data-source-agnostic.

#### 3.3.1 What the canonical feed contains and does not contain

| Field | Required | Notes |
|---|---|---|
| topology (bus/gen/line catalogues) | yes | as described in §3.3.3 |
| dispatch per generator per cell | yes | the merit-order driver |
| demand per bus per cell | yes | the zone-load reference |
| LMP per bus per cell | optional | when present, used directly; when absent, reconstructed by §4.7 |
| flow per line per cell | optional | improves zone partition; PTDF estimate substitutes when absent |
| flow capacity dual per line per cell | optional, **simulated-only** | enables `confidence=lp_dual` classification |
| reservoir / battery dual per cell | optional, **simulated-only** | enables exact water-value matching |
| per-segment cost data | optional | enables piecewise-active-segment detection |

The four "simulated-only" rows are populated by the gtopt reader from
LP duals and are **never** populated by `cen2gtopt` (CEN does not
publish duals — there is no LP behind the realised operation; see §9
for the consequences). The feed schema reserves the columns so a
single reader can handle both producers.

#### 3.3.2 What the realised-operation feed does **not** provide

| Data the LP classifier uses | Real-mode availability |
|---|---|
| Reduced cost on `g ≤ pmax` | **absent** — no LP, no duals |
| Capacity shadow on `−tmax ≤ f ≤ tmax` | **absent** |
| KVL Lagrangian | **absent** |
| Reservoir / battery dual (water value) | **absent** |
| Per-segment marginal cost on a piecewise unit | **partial** (only the *active* segment slope can be inferred from declared variable cost at the realised dispatch) |

In `mode=real`/`real-reconstruct` the classifier therefore runs a
**degraded ruleset** (§4.4) that uses only
`(disp, pmin, pmax, declared_MC, λ_b)`. Every emitted row carries
`data_source` and `confidence` columns (§4.6) so a downstream consumer
can never confuse the two regimes.

#### 3.3.3 Canonical schema (frozen Phase-0 contract)

Both readers populate the same in-memory frames; the on-disk feed
written by `cen2gtopt` (and any other producer) maps 1-to-1 to these:

```text
TopologyFrame:                         # written once per feed
  bus(uid:int, name:str, region:str|None)
  generator(uid:int, name:str, bus_uid:int, pmin:float, pmax:float,
            declared_MC:float|None, kind:str, segments:list|None,
            emission_rate:float|None)   # kgCO2eq/MWh (§4.12)
  line(uid:int, bus_a_uid:int, bus_b_uid:int,
       tmax_ab:float, tmax_ba:float, active:bool)

CellFrame (one row per (cell_key, gen_uid|bus_uid|line_uid)):
  cell_key:tuple             # (scene,stage,block) or (date_utc,hour)
  data_source:str            # "simulated" | "real"
  dispatch[gen_uid]:float
  commitment[gen_uid]:int|NA        # 0/1 hidden commitment status (§2.7)
                                    # populated in simulated mode from
                                    # Generator/commitment_sol; NA in real* unless
                                    # cen2gtopt infers from regime columns below
  lmp[bus_uid]:float|NA      # NA → reconstructed in mode=real-reconstruct
  flow[line_uid]:float|NA
  flow_dual[line_uid]:float|NA      # NA outside simulated mode
  restriction_declared[line_uid]:bool|NA  # CDC operational saturation flag (§9.2.3)
                                          # NA outside real* modes; True/False
                                          # only when cen2gtopt could pull RIO/
                                          # Restricciones-Operativas
  regime[gen_uid]:struct|NA         # per-unit operational-regime flags (§4.11);
                                    # NA in simulated mode and outside real*
                                    # modes; populated by cen2gtopt from RIO /
                                    # SSCC / Indisponibilidades / irrigation registry
  load[bus_uid]:float|NA
  ens[bus_uid]:float|NA
```

On-disk layout (Parquet, the format Phase-2 writes):

```
canonical_feed.parquet/
  topology/bus.parquet
  topology/generator.parquet
  topology/line.parquet
  cells/dispatch.parquet              # long form (cell_key, gen_uid, value)
  cells/commitment.parquet            # optional; u ∈ {0,1} per (cell, gen_uid)
  cells/lmp.parquet
  cells/flow.parquet
  cells/line_restriction.parquet      # optional; CDC operational declarations
  cells/unit_regime.parquet           # optional; commitment-driver flags (§4.11)
  cells/load.parquet
  manifest.json                       # producer name+version, schema version,
                                      # source URLs, fetch timestamp,
                                      # hash of each parquet file
```

`cells/line_restriction.parquet` and `cells/unit_regime.parquet` are
both **optional** but follow the same contract: when present they
are the authoritative source for the corresponding decision
(operational saturation / commitment-driver), and the consumer must
honour them; when absent the consumer falls back to data-only
inference with a `confidence` downgrade and an audit-trail entry.
Phase-2's `cen2gtopt` populates `cells/line_restriction.parquet`
from the RIO / *Novedades CDC* / *Restricciones Operativas*
endpoints (§9.2.3) and `cells/unit_regime.parquet` from the
twelve-driver catalogue documented in §4.11.1 (§9.2.4 below).
`cells/commitment.parquet` is populated only by the gtopt-side
reader from `Generator/commitment_sol` and is `NA` (file absent)
in feeds produced by `cen2gtopt`.

The `cells/unit_regime.parquet` long-form schema:

```text
columns:
  cell_key columns,
  gen_uid:int,
  forced_pmin:bool, inflexible_fuel:bool,
  fuel_kind:str|null,           # "gas-top", "gas-mue", "diesel-stock", …
  sscc:str|null,                # "cpf"|"cpr"|"spinning"|"voltage"|"black_start"|"inertia"
  network_security:bool, instructed_dispatch:bool,
  hydro_irrigation:bool, environmental_limit:bool,
  committed_run_lock:bool,
  unavailable:bool, unavail_reason:str|null,  # "maintenance" | "forced_outage" | null
  source:str,                   # which CEN feed produced this row
  vigencia_desde:datetime|null, vigencia_hasta:datetime|null
```

A row is written only when at least one flag is `True`. Absent
rows are read as "all flags False" by the merit-eligibility filter
(§4.11.2).

The `manifest.json` is the audit trail: any feed read by Phase 1 can
be traced back to its producer (gtopt run UUID, `cen2gtopt` invocation
arguments, CEN endpoints + dates) without coupling the two scripts.

The `cell_key` is opaque to the classifier; only the
`gtopt_marginal_units.compare` module knows how to align simulated
and real cells (it joins on
`(generator.name, datetime_from_block(planning, scene, stage, block))`,
falling back to fuzzy-name matching with a flag).

---

## 4. Algorithm

The core loop is per cell `(scene, stage, block)`. Every step below is
vectorisable with pandas/NumPy; the prose form is for clarity.

### 4.1 Build static lookups (once per run)

* `gen_df`: one row per generator with `uid, name, bus_uid, pmin, pmax,
  gcost, kind ∈ {thermal, hydro, battery, profile}`.
* `bus_df`: one row per bus (`uid, name`).
* `line_df`: one row per line (`uid, bus_a_uid, bus_b_uid, tmax_ab,
  tmax_ba`).
* `gen_at_bus`: dict `bus_uid → list[gen_uid]`.

### 4.2 Build dynamic per-cell tables

For each cell `(s, p, b)`:

```text
disp[g]   = generation_sol[s,p,b,g]
lmp[bus]  = balance_dual[s,p,b,bus]
flow[l]   = flowp_sol[s,p,b,l]
mu[l]     = flowp_cost[s,p,b,l]   # congestion shadow on the flow var
mc[g]     = active_segment_slope(g, disp[g])  # piecewise lookup
                                             # falls back to gcost
```

`scale_objective` is **not** applied to LMPs in `Bus/balance_dual` (the
solver dual is already in `$/MWh` because gtopt scales the objective in
the LP, not on output — sanity-check by recomputing total cost from
`gcost · disp · duration` against `solver_status.json:objective_value`).
The script asserts this on first read and warns otherwise.

### 4.3 Identify the congestion-zone partition

```text
saturated = { l : |flow[l]| ≥ tmax_l · (1 − tol_flow)
                  AND |mu[l]| > tol_mu }
G' = (Bus, Line \ saturated)
zones = connected_components(G')
```

In single-bus mode (`use_single_bus=True`) or when no flow output is
present, `zones` collapses to one zone covering all buses.

For each zone `z`:

* `λ_z` = median of `lmp[bus]` for buses in `z` (median is robust to
  numerical noise; we additionally assert `max - min ≤ tol_lmp` and warn
  otherwise — a violation indicates the zone partition is wrong, see
  §6).

### 4.4 Classify each generator

For each generator `g` (let `b = bus(g)`):

```text
zone_g  = zone_of[b]
λ_g     = λ_zone[zone_g]
status  = classify(disp[g], pmin_g, pmax_g, mc[g], λ_g)
```

`classify` rule table (in priority order — the first match wins):

| Test | Status |
|---|---|
| `disp[g] ≤ ε` AND `pmin_g ≤ ε` | `off` |
| `pmin_g − ε < disp[g] < pmin_g + ε` AND `pmin_g > 0` | `forced_pmin` |
| `pmax_g − ε < disp[g] ≤ pmax_g + ε` | `capped_pmax` |
| `pmin_g + ε < disp[g] < pmax_g − ε` AND `kind ∈ {hydro,battery}` | `hydro_marginal` |
| `pmin_g + ε < disp[g] < pmax_g − ε` AND `\|mc[g] − λ_g\| ≤ tol_price` | `marginal` |
| `pmin_g + ε < disp[g] < pmax_g − ε` AND `mc[g] < λ_g − tol_price` | `inframarginal` |
| `pmin_g + ε < disp[g] < pmax_g − ε` AND `mc[g] > λ_g + tol_price` | `extramarginal_interior` *(should not occur — flagged as LP-degeneracy / segment-mismatch)* |
| `disp[g] ≥ ε` AND `kind == profile` | `profile_dispatched` *(non-dispatchable, never marginal)* |

Tolerances (defaults, all CLI-overridable):

| Symbol | Default | Note |
|---|---|---|
| `ε`            | `1e-4 · pmax_g`  | absolute slack on dispatch bounds (MW) |
| `tol_price`    | `1e-3 · max(\|λ_g\|, 1.0)` | LMP vs MC gap ($/MWh) |
| `tol_flow`     | `5e-3` | fractional saturation threshold (dimensionless) |
| `tol_mu`       | `1e-2` | dual nonzero threshold ($/MWh) |
| `tol_lmp`      | `1e-2 · max(\|λ_z\|, 1.0)` | intra-zone LMP spread ($/MWh) |
| `tol_load_mw`  | `1.0` | absolute load-balance slack used by §4.5 forced-pmin attribution and §4.7 R3 demand-fail check (MW) |

`tol_price` and `tol_load_mw` carry **different units** and must
never be conflated. Earlier drafts of this plan reused `tol_price`
for both the LMP-vs-MC test (§4.4 row 5) and the zone-load-vs-Σpmin
test (§4.5 forced-pmin) — that was a unit-mismatch bug fixed in this
revision (P0 from the lp-numerics review).

### 4.5 Pick the marginal unit(s) per zone

For each zone `z`:

```text
candidates = { g : zone(g)=z AND status[g] ∈ {marginal, hydro_marginal} }
```

* If `len(candidates) ≥ 1` → emit them as the zone's marginal units.
* If `len(candidates) == 0`:
  * If any unit in `z` has `status = forced_pmin` AND
    `|zone_load − Σ pmin| ≤ tol_load_mw` (MW units, not $/MWh) →
    emit them as `forced_pmin_marginal` (single-unit-zone or
    all-must-run case).
  * Else if `λ_z ≈ demand_fail_cost` (within `tol_price`) → emit a
    synthetic `__demand_fail__` "unit" for that zone (load shedding
    sets the price).
  * Else → emit `__unattributed__` with `degenerate=True` and a reason
    string. This is the LP-degeneracy escape hatch — the report still
    contains the row so downstream consumers never silently lose a
    cell.

### 4.6 Output artifact (canonical layout)

The script writes a **parquet dataset** (a directory) rather than a
single file — this lets downstream consumers selectively load only
the table they need, mirrors the layout of the canonical operation
feed (§3.3.3), and keeps the merit-ladder and price-recipe tables
out of the per-row hot path.

```
marginal_units.parquet/
  attribution/
    per_bus.parquet              # primary table — one row per (cell, bus, marginal-gen)
    per_zone.parquet             # summary — one row per (cell, zone)
  merit_ladder.parquet           # one row per (cell, zone, rank)
  bus_price_recipe.parquet       # one row per (cell, bus) — the f(MC) formula
  bus_emission_intensity_recipe.parquet   # one row per (cell, bus) — analogue of bus_price_recipe (§4.12)
  audit/
    saturated_lines.parquet      # one row per (cell, line) where saturated
    unattributed.parquet         # one row per (cell, bus) for degenerate cells
    out_of_merit.parquet         # one row per (cell, gen) excluded by §4.11.2 filter
  manifest.json                  # producer + version + schema_version + file hashes
```

`--csv` additionally emits `attribution/per_bus.csv` and
`attribution/per_zone.csv` for ad-hoc inspection (the merit ladder
and recipe tables are parquet-only — the long-form is awkward in
CSV). The Parquet dataset is the source of truth.

#### 4.6.1 Primary attribution table — `attribution/per_bus.parquet`

**Indexed for the user's first question: "which unit set the price
at bus B in hour H?"** Sorted/partitioned-by `(cell_key, bus_uid)`.

```
columns:
  scenario:int|null, stage:int|null, block:int|null,    # gtopt cell key
  date_utc:date|null, hour:int|null,                    # feed cell key
  zone_id:int, zone_lmp:float,
  bus_uid:int, bus_name:str,
  gen_uid:int|null, gen_name:str|null,                  # null for synthetic units
  status:str,                                           # §4.4 label, or one of
                                                        # __demand_fail__ / __renewable_curtailment__ / __unattributed__
  dispatch:float, pmin:float, pmax:float,
  marginal_cost:float,                                  # MC at active segment
  reduced_cost:float,                                   # MC − λ_b   (NA in real* modes)
  active_segment:int,                                   # −1 if scalar gcost
  is_marginal:bool,                                     # True for marginal/hydro_marginal/forced_pmin_marginal
  data_source:str,                                      # simulated|real|real-reconstruct
  confidence:str,                                       # lp_dual|merit_order|fallback
  degenerate:bool, reason:str                           # for unattributed cells
```

A bus with K marginal units (ties on MC, LP degeneracy) emits K
rows. A bus with no attributed marginal unit emits exactly one row
with `gen_uid=null` and a synthetic `status` (`__demand_fail__`,
`__renewable_curtailment__`, or `__unattributed__`).

The schema is **stable across modes**: `simulated`, `real`, and
`real-reconstruct` all produce the same columns. The `confidence`
column records how the classification was derived (`lp_dual` only in
simulated; `merit_order` in the real* modes; `fallback` for
unattributed cells). The `reduced_cost` column is `NA` in the real*
modes since no LP duals exist there.

#### 4.6.2 Per-zone summary — `attribution/per_zone.parquet`

One row per `(cell_key, zone_id)` for fast aggregate queries:

```
columns:
  cell_key columns (as above),
  zone_id:int, zone_lmp:float,
  bus_uids:list[int], bus_count:int,
  marginal_gen_uids:list[int],                 # all marginal units at this zone
  marginal_gen_names:list[str],
  zone_load_mw:float, zone_dispatch_mw:float,  # for the demand-fail check
  saturated_line_uids:list[int],               # lines bounding this zone
  status:str,                                  # marginal|forced_pmin_marginal|demand_fail|...
  degenerate:bool, confidence:str,
  data_source:str
```

This mirrors the row layout `gtopt_results_summary` already
produces, so the two scripts can share helpers.

#### 4.6.3 Merit ladder — `merit_ladder.parquet`

The single most-requested feature for "what if the marginal unit
trips" / "how robust is this attribution" workflows. **One row per
`(cell_key, zone_id, rank)`** for `rank ∈ {−K, …, −1, 0, +1, …, +K}`
where `0` is the current marginal unit, positive ranks are the next
units **upward** in merit order (the units that would set the price
if the current marginal moved to its `pmax`), and negative ranks
are the next units **downward** (the units that would set the price
if load *fell* and the current marginal moved off interior).

```
columns:
  cell_key columns,
  zone_id:int,
  rank:int,                                    # 0 = current marginal
  gen_uid:int|null, gen_name:str|null,
  declared_MC:float|null,                      # the canonical price datum
  active_segment_MC:float|null,                # for piecewise units
  dispatch:float|null,
  pmin:float, pmax:float,
  headroom_up_mw:float,                        # pmax − dispatch
  headroom_down_mw:float,                      # dispatch − pmin
  available:bool,                              # active in this stage AND not at a binding bound that excludes it
  hypothetical_lmp:float|null,                 # the λ_z this rung would produce if it became marginal
  is_actual_marginal:bool                      # True only on rank=0
```

Default depth `K=3` (override via `--merit-ladder-depth`). Setting
`K=0` writes only the current marginal (rank=0) row, equivalent to
the old behaviour. The ladder is **always** written so the
verification/sensitivity workflow is universally available; the
storage cost is a small constant per cell.

The synthetic units (`__demand_fail__`, `__renewable_curtailment__`,
`__unattributed__`) appear only at rank=0 — they have no merit-order
neighbours by construction.

#### 4.6.4 Bus price recipe — `bus_price_recipe.parquet`

The **explicit, auditable formula** that turns the unit-cost
catalogue into the bus price λ_b. This is the contract that lets
downstream consumers recompute λ_b from the saved data without
re-running the LP — the use case the user is asking about.

```
columns:
  cell_key columns,
  bus_uid:int,
  zone_id:int,
  formula_kind:str,                            # see below
  marginal_gen_uids:list[int],                 # inputs to the formula
  marginal_weights:list[float],                # one per uid; sum to 1.0 for ties
  marginal_costs:list[float],                  # MC of each input, captured at write time
  formula_constant:float,                      # 0.0 for unit-driven; demand_fail_cost for rationing
  formula_explanation:str,                     # human-readable, e.g. "λ_b = MC of g123"
  recomputed_lmp:float                         # λ_b reconstructed from the formula at write time
                                               # (must equal zone_lmp within tol_price)
```

`formula_kind` enumeration:

| Kind | Formula | Inputs |
|---|---|---|
| `single_unit`           | `λ_b = MC[g₀]`                                | one gen UID |
| `tied_units`            | `λ_b = MC[g_i]` (any tied i; weights uniform) | K gen UIDs |
| `forced_pmin_marginal`  | `λ_b = MC[g₀]`                                | one or more must-run gen UIDs |
| `hydro_marginal`        | `λ_b = water_value[g₀]` (proxy MC)            | one gen UID; flagged for water-value sensitivity |
| `demand_fail`           | `λ_b = demand_fail_cost` (constant)           | empty UID list, formula_constant carries the value |
| `renewable_curtailment` | `λ_b = 0`                                     | empty UID list |
| `unattributed`          | undefined; `recomputed_lmp = NA`              | empty |

Two invariants (both are CI-tested in §7):

1. `recomputed_lmp` is computed by the writer using the same
   `marginal_costs` it just stored, and the writer asserts
   `|recomputed_lmp − zone_lmp| ≤ tol_price` before persisting. A
   violation means the recipe table is internally inconsistent and
   the run aborts with exit 3.
2. The downstream consumer API in §4.10 reads the recipe and
   reproduces λ_b *only* from `marginal_gen_uids` + an
   externally-supplied `unit_costs` map. If the caller's costs match
   the captured `marginal_costs`, the recomputed price equals the
   stored `zone_lmp` exactly — the round-trip is the regression
   test for the recipe schema.

#### 4.6.5 Audit tables — `audit/*.parquet`

* `audit/saturated_lines.parquet` — per `(cell, line)` where the
  zone partition declared the line saturated; columns: flow,
  flow_dual (or `NA`), tmax, fractional saturation,
  detection_rule (`flow_and_dual` / `flow_only` / `physical_island`).
* `audit/unattributed.parquet` — per `(cell, bus)` where the cell
  could not be attributed; columns: reason string, candidate UIDs
  considered, why each was rejected. This is the human-readable
  trail for the §4.5 escape hatch.

#### 4.6.6 manifest.json

```json
{
  "producer": "gtopt_marginal_units",
  "producer_version": "1.0.0",
  "schema_version": "1.0.0",
  "input_kind": "gtopt-dir",
  "input_paths": ["..."],
  "mode": "simulated",
  "tol_price": 0.001,
  "tol_load_mw": 1.0,
  "merit_ladder_depth": 3,
  "written_at": "2026-05-05T17:30:11Z",
  "row_counts": {
    "attribution/per_bus": 21024,
    "attribution/per_zone": 168,
    "merit_ladder": 1176,
    "bus_price_recipe": 168
  },
  "file_hashes": { "attribution/per_bus.parquet": "sha256:...", ... }
}
```

The manifest is the audit trail; the consumer API in §4.10 verifies
hashes on every read.

### 4.7 Reconstructing CEN's published *Costo Marginal Real*

Goal: given only **realised generation per unit**, **realised demand
per bus**, the **topology** (bus / generator / line catalogue), and
each unit's **declared variable cost**, reconstruct the bus-level LMP
that CEN publishes — and identify the marginal unit consistent with
that price. This is what makes `mode=real-reconstruct` viable without
any LP solve and gives the user an independent audit of the CEN
number.

The procedure is the **dispatch-implied LMP** rule used in the LMP
literature (Hogan/Litvinov; CAISO Tariff Appendix C) specialised to
the "no-loss, dispatch-fixed" subcase:

**Step R1 — recover the saturation pattern.**

Saturation in real operation is **not** the same thing as
`|f_l| ≥ tmax_l`. CEN's CDC (Centro de Despacho de Carga)
**operationally declares** lines / corridors as restricted based on
security-and-quality-of-service criteria (N−1 contingency margins,
voltage limits, stability constraints), independently of whether
the realised flow is at the thermal limit. CEN itself documents
this distinction:

> *"The Coordinador includes the component of transmission system
> limitations in the determination of real marginal costs for each
> node."* — CEN, [Reporte Anual Art 72-15][cen-reporte]

Equivalently, a line at `|f| < tmax` can be operationally saturated
because CDC declared the corridor restricted, and a line at
`|f| ≈ tmax` may *not* be operationally saturated — the system was
simply scheduled to that operating point. **You cannot identify
operational saturation purely from the flow time-series.**

R1 therefore prefers a declared-saturation source over a
flow-inferred one, in this priority:

1. **`cells/line_restriction.parquet` from the canonical feed**
   (optional schema column added in this revision). When
   `cen2gtopt` succeeds in pulling the operational-restriction
   register from RIO / *Novedades CDC* / *Restricciones Operativas*
   (§9.2.3), every cell carries a boolean `restriction_declared`
   per line. R1 uses this as the saturation source and sets
   `detection_rule = "cdc_declaration"` in the audit.
2. **`flow_dual` non-zero AND `|flow| ≥ tmax · (1 − tol_flow)`**
   (the simulated-mode case from §4.3). Used in `mode=simulated`
   only — duals from a real LP are an *exact* indicator of the
   binding line, including LP-degeneracy ties.
   `detection_rule = "lp_dual"`.
3. **Flow-only test `|flow| ≥ tmax · (1 − tol_flow)`** when realised
   flows are present in the feed but no CDC declaration is.
   `detection_rule = "flow_only"`, `confidence = "fallback"`. The
   cell's `audit/saturated_lines.parquet` row carries this rule so
   downstream consumers can filter it out — every flow-only-inferred
   saturation is visibly less reliable than a CDC declaration.
4. **PTDF estimate from realised injections** when neither flow nor
   CDC data is present. We solve `f = PTDF · netload` once per cell
   (no optimisation, just linear algebra). PTDF is built **per
   connected component** of the topology graph (so naturally-islanded
   networks like the Chilean north–south interconnection during a
   separation event yield a block-diagonal PTDF rather than a
   singular matrix). If any line in the topology has a missing or
   non-positive reactance, the script **refuses to run** under R1's
   PTDF path with exit code 3 and a message naming the offending
   lines — safer to abort than to emit silently-wrong zones from a
   uniform-reactance fallback. `detection_rule = "ptdf_estimate"`,
   `confidence = "fallback"`.

The user can opt out of the inference paths via:
* `--zone-mode physical` — skip flow-based partitioning entirely
  and use only the topology's `active=False` lines.
* `--require-cdc-restriction` — refuse to run the flow-only / PTDF
  paths; produce a `__unattributed__` row instead of an inferred
  zone partition. This is the strict-audit setting for users who
  must publish only CEN-aligned attributions.

When mode `compare` runs against CEN's published Costo Marginal
Real, every cell where R1's inferred saturation disagrees with the
CDC-declared set (if available) is added to
`cells_with_topology_mismatch` and surfaced in the report's
top-outliers list. This is the audit trail that explains *why* the
reconstructed and CEN-published prices diverge.

**Step R2 — partition into zones.**
Apply the same connected-components rule of §4.3 on the saturation
pattern from R1.

**Step R3 — recover λ_z from the realised dispatch.**
Inside each zone, the price-setting candidates are the units that
**realised generation places strictly between their `pmin` and `pmax`**
(plus a small ε). Among those, the **highest declared MC** is the
zone's marginal cost — this is the standard merit-order rule, valid
because the zone's interior dispatch is by definition the *most
expensive* unit that was needed to clear the zone's load:

```text
λ_z = max{ declared_MC[g] : zone(g)=z AND
                            pmin_g + ε < disp[g] < pmax_g − ε }
```

R3 cascades through five corner cases in priority order:

1. **Zone load > Σ pmax over the zone** — the zone is rationing.
   `λ_z = demand_fail_cost`, marginal "unit" = `__demand_fail__`,
   `confidence = fallback`.
2. **All units in the zone pinned at pmax** (binding-up everywhere,
   no interior unit). LMP theory says the zone is supply-limited
   and the price must be at the rationing cap; therefore
   `λ_z = demand_fail_cost`, marginal "unit" = `__demand_fail__`,
   `confidence = fallback`. (Earlier drafts proposed using the
   cheapest pmax-pinned MC here — that was a P0 lp-numerics
   correction.)
3. **At least one interior unit** — apply the merit-order formula
   above; ties on MC produce all tied units in the marginal-unit
   list (lowest-uid first); `confidence = merit_order`.
4. **No interior unit, but at least one `forced_pmin` unit AND
   `|zone_load − Σ pmin| ≤ tol_load_mw`** — this is the zone-picker
   forced-pmin fallback; emit `forced_pmin_marginal`,
   `confidence = merit_order`, `degenerate = True`.
5. **Otherwise** — emit `__unattributed__`, `confidence = fallback`,
   `degenerate = True`, with a `reason` string naming what was tried.

After the R3 cascade chooses λ_z, a final clamp enforces
`λ_z ∈ [0, demand_fail_cost]`; values above the cap (which can only
happen if a unit's declared MC is above the rationing cap — usually
a catalogue error) are clamped to `demand_fail_cost` with
`confidence = fallback` and the offending unit is surfaced in the
report.

**Step R4 — apply the §4.4 classifier.**
With `lmp[bus_uid] := λ_z` for every bus in zone z, the rest of the
pipeline (classify, pick marginal units, write artifact) runs
unchanged. `confidence` is set to `merit_order` for every emitted row.

**Step R5 — compare against CEN's published number (when present).**
If the reader also loaded `Costo Marginal Real` (mode `compare
--against real`), the script emits a `lmp_delta = λ_reconstructed −
λ_published_CEN` column per `(date, hour, bus)`. A summary row at
the end of the report tallies (all column names are valid Parquet
identifiers — no parentheses, no Unicode `±` glyph, no spaces, no
unit suffixes inside the name):

* `cells_within_0p5_usd_per_mwh` — count of `|lmp_delta| ≤ 0.5`
* `cells_within_2_usd_per_mwh` — count of `|lmp_delta| ≤ 2.0`
* `cells_with_topology_mismatch` — zone partition disagrees with the
  bus groups CEN's CSV implies (CEN saw a saturated line we missed
  or vice-versa)
* `cells_with_must_run_override` — CEN's published price is *below*
  the cheapest interior unit's MC (usually means CEN counted a
  must-run / forced-pmin unit as price-setter, which our R3 rule
  already encodes; this serves as a reverse-validation of §2.3)
* `cells_with_demand_fail_clamp` — count of cells where R3's final
  clamp pinned λ_z to `demand_fail_cost`.

This gives the user a deterministic, auditable reconstruction of
CEN's number and a traceable explanation when the two disagree —
exactly the workflow the operator side requested.

### 4.8 Minimum data required to run

The script's classifier needs only **four canonical fields per cell**
plus the topology to produce a valid attribution. Anything beyond is
strictly an enrichment.

#### 4.8.1 Strict minimum (any mode, any data source)

| Field | Per | Why it's required |
|---|---|---|
| `bus(uid, name)` catalogue | once | maps generators to a bus |
| `generator(uid, name, bus_uid, pmin, pmax, declared_MC, kind)` catalogue | once | provides bound and merit-order info |
| `line(uid, bus_a_uid, bus_b_uid, tmax_ab, tmax_ba)` catalogue | once | enables zone partition; can be empty for single-bus mode |
| `dispatch[gen_uid]` | per cell | drives the bound-status classification |
| `lmp[bus_uid]` *or* the data needed to reconstruct it | per cell | sets the price comparison reference |

The third row collapses to *no rows* under `--single-bus`, in which
case the algorithm produces a single zone covering all buses; the
script accepts a topology with zero lines.

The fifth row is the only place the script accepts a substitution: in
`mode=real-reconstruct` the `lmp[bus_uid]` is **not** read from any
file; it is reconstructed by §4.7 step R3 from the other four fields.
Concretely, the **absolute minimum file set for `real-reconstruct`
mode** is:

```
- topology snapshot   (bus + generator + line catalogues, with
                       declared_MC, pmin, pmax, tmax)
- realised dispatch   (per generator, per hour)
- realised demand     (per bus, per hour)
```

…three artifacts. Everything else CEN publishes (LMP, line flows,
forecasts, programmed values) is optional and only used to enrich
confidence flags or to back-test the reconstruction.

#### 4.8.2 Recommended (raises confidence)

| Field | Lifts confidence from → to |
|---|---|
| `lmp[bus_uid]` | `merit_order` → `published` (used directly, no R3) |
| `flow[line_uid]` | (any) → enables exact zone partition without PTDF estimate |
| `flow_dual[line_uid]` | `merit_order` → `lp_dual` (only available in `simulated`) |
| Reservoir / battery duals | enables exact water-value classification for hydro/storage units |
| Per-segment cost data | enables piecewise-active-segment detection |

#### 4.8.3 Required-by-mode summary (Phase-1 view)

The Phase-1 script sees only two input shapes; the upstream data
producer is opaque to it:

| `--input` | `--mode` | Topology | Dispatch | LMP | Flows | Duals |
|---|---|---|---|---|---|---|
| `gtopt`   | `simulated` | planning JSON | `Generator/generation_sol` | `Bus/balance_dual` | `Line/flowp_sol` (single-bus exempt) | `Line/flowp_cost` (single-bus exempt) |
| `feed`    | `real` | feed `topology/*` | feed `cells/dispatch.parquet` | feed `cells/lmp.parquet` | feed `cells/flow.parquet` (optional) | n/a |
| `feed`    | `real-reconstruct` | feed `topology/*` | feed `cells/dispatch.parquet` | **reconstructed** (§4.7); ignores `cells/lmp.parquet` if present | feed `cells/flow.parquet` (optional, improves zone partition) | n/a |
| `gtopt` + `--feed-against` | `compare` | union | union | union | union | union (only on the gtopt side) |

Anything not in the table is optional. The script's dispatcher checks
the mode-specific minimum at startup and exits with code `3` and a
single-line "missing X for mode Y" message when the minimum is not
met — never silently downgrading.

The producer of the `feed` (Phase 2's `cen2gtopt` for the SEN; an
external partner's writer for any other system) is responsible for
the **§4.8.1 strict minimum**: topology + dispatch + demand. LMP and
flows are pass-through enrichments.

### 4.9 Merit-ladder construction

The merit ladder is computed per zone, in the same per-cell loop
that produces §4.5's marginal-unit pick. Inputs:

* the zone's generator set `Gz` after the §4.4 classification;
* the per-unit `(declared_MC, dispatch, pmin, pmax, kind, status,
  active)` already in scope;
* the depth knob `K` (default `3`, `--merit-ladder-depth`).

Algorithm:

1. **Filter to ladder-eligible units** within the zone:
   * exclude `kind=profile` (zero-MC, non-dispatchable — they cannot
     set price);
   * exclude units with `active=False` (out-of-stage);
   * keep `kind ∈ {thermal, hydro, battery}`. Hydro/battery enter
     with their declared_MC if available, else with their water-value
     proxy (or `NA` and `available=False` for v1 if neither is
     present).
2. **Build the merit-order list** `Lz` sorted by
   `(declared_MC ASC, gen_uid ASC)`. The lowest-cost unit is index
   0; the highest is `len(Lz)−1`.
3. **Locate the marginal anchor**:
   * if the §4.5 pick produced one or more interior `marginal` /
     `hydro_marginal` units, the anchor is the *cheapest* of them
     (anchor index `i⋆`);
   * if the pick was `forced_pmin_marginal`, the anchor is the
     forced-pmin unit (typically near the bottom of `Lz`);
   * if the pick was synthetic (`__demand_fail__`,
     `__renewable_curtailment__`, `__unattributed__`), only the
     rank-0 row is emitted with `gen_uid=null`; no neighbours.
4. **Emit upward rungs `rank = +1 … +K`**: the next units up in
   `Lz` from `i⋆`, regardless of their current bound status.
   Compute their `hypothetical_lmp` as the MC of that rung *if it
   replaced the anchor* — for non-piecewise units this is just
   `declared_MC`; for piecewise units it is the slope of the segment
   that *would* be active given the zone's residual load (the
   writer estimates this via R3's merit-order algorithm with the
   anchor moved to its `pmax`).
5. **Emit downward rungs `rank = −1 … −K`**: the next units down in
   `Lz` from `i⋆`. Same `hypothetical_lmp` rule, with the anchor
   moved to its `pmin`.
6. **Cap at zone boundaries**: rungs that would index outside `Lz`
   are skipped (the ladder is shorter than `2K+1` near the zone's
   merit-order endpoints). The written rows always carry contiguous
   `rank` values; consumers must not assume a fixed row count per
   zone.

Per-row fields (`available`, `headroom_up_mw`, `headroom_down_mw`)
are direct lookups; no extra LP solve is needed. The whole ladder is
a one-pass operation per zone — total cost is `O(|cells| · |zones| ·
log|Gz|)` from the per-zone sort, dominated by the existing §4.5
work.

#### 4.9.1 Edge cases

* **Tied MCs** — multiple units at the same `declared_MC` collapse
  to a single ladder rung; the row's `gen_uid` carries the
  lowest-uid representative and `gen_name` is suffixed with
  `(tied with N units)`. A separate audit row in
  `audit/unattributed.parquet` lists every tied UID for full
  traceability.
* **Anchor at zone endpoint** — if the marginal unit is already the
  cheapest (or most expensive) in the zone, the upward (or
  downward) ladder is empty.
* **Synthetic anchor** — see step 3; the ladder degenerates to one
  row.
* **Cross-zone neighbours** — v1 does **not** include units from
  other zones in the ladder, even though if a saturated line tripped
  the zone partition would change. Cross-zone "what-if" analysis is
  v1.1; documented as an explicit non-goal in §10.

### 4.10 Downstream consumer API — recomputing price from saved data

The output dataset is designed to be **the source of truth for the
bus price** in any downstream notebook, dashboard, or audit script,
*without* re-running `gtopt_marginal_units`. To make this contract
explicit, the package ships a tiny read-only helper module
`gtopt_marginal_units.recipes` (10–20 lines of public surface, the
rest is plumbing).

```python
from gtopt_marginal_units.recipes import MarginalUnitDataset

ds = MarginalUnitDataset.open("path/to/marginal_units.parquet/")

# Q1: who set the price at bus 42 in hour 17?
ds.marginal_units(bus_uid=42, cell_key=(2026, 4, 17))
# → DataFrame[gen_uid, gen_name, status, marginal_cost, is_marginal]

# Q2: full per-bus price series for one stage.
ds.bus_lmp(stage=4)
# → DataFrame[cell_key, bus_uid, zone_lmp]

# Q3: recompute λ_b under a new cost catalogue
#    (e.g. revised fuel prices, sensitivity study).
new_costs = {gen_uid: new_mc for gen_uid, new_mc in catalog.items()}
ds.recompute_lmp(unit_costs=new_costs)
# → DataFrame[cell_key, bus_uid, zone_lmp_recomputed,
#             zone_lmp_original, lmp_delta]

# Q4: outage sensitivity — what happens to λ_b if generator g trips?
ds.outage_sensitivity(gen_uid=g, depth=1)
# → for every cell where g was rank-0 marginal: read merit_ladder.parquet,
#   take rank=+1 row, return its hypothetical_lmp as the post-outage λ_z.
# → DataFrame[cell_key, bus_uid, lmp_pre_outage, lmp_post_outage,
#             replacing_gen_uid, replacing_gen_name]

# Q5: merit ladder for a single cell + zone (verification view).
ds.merit_ladder(cell_key=(2026, 4, 17), zone_id=2)
# → DataFrame ordered by rank ∈ {−K, …, +K}
```

The `recompute_lmp` method follows the **recipe table verbatim**:
for every row in `bus_price_recipe.parquet`, look up each
`marginal_gen_uids[i]` in the caller's `unit_costs` dict, apply
`marginal_weights[i]`, add `formula_constant`, and return.
`single_unit` and `tied_units` produce a weighted sum of the input
costs; `demand_fail` returns the constant directly. If a UID is
missing from the caller's dict, that cell's recomputed LMP is `NA`
and the count goes into a `missing_units_count` summary attribute on
the returned frame.

`outage_sensitivity` is the verification view the user is asking
about: it composes the merit ladder with the recipe to answer "if
the marginal unit is too small / unavailable / trips, what is the
next price?". For the common case (rank-0 anchor → rank+1 takeover)
this is a one-pass parquet read — no recomputation needed.

#### 4.10.1 Round-trip invariants tested in CI

* `ds.recompute_lmp(unit_costs=ds._captured_costs())` returns
  `lmp_delta == 0` everywhere — confirms the recipe is internally
  consistent.
* `ds.bus_lmp() == per_bus_attribution.zone_lmp` — confirms the
  consumer API agrees with the writer.
* `ds.outage_sensitivity(g)` returns a row only for cells where g
  was actual marginal; the rest of the dataset is unaffected.
* The manifest hash check on open detects any external mutation of
  the parquet files.

#### 4.10.2 Documentation

The consumer API is documented in
`docs/scripts/marginal_unit_recipes.md` (a Phase-1 deliverable, see
§8). The doc shows worked examples for each `formula_kind`, the
sensitivity-study recipe, and the limitations (tied units, hydro
water values, missing-UID handling).

### 4.11 Out-of-merit operational regimes

Per §2.7, a unit is **eligible for the merit ladder** only when it
is committed *for economic reasons* — i.e. when its dispatch is
explained by the merit-order itself, not by an external operational
driver. Several drivers force commitment for non-economic reasons,
and a unit dispatched under any of them must be **excluded from the
zone-marginal pick (§4.7 R3)** and from the merit-ladder ranks
(§4.9). Including them silently is what makes a real-data
attribution diverge from CEN's published number.

#### 4.11.1 Driver catalogue

Each row maps a real-world commitment driver to:
- whether it can be **observed** from the canonical-feed minimum
  (topology + dispatch + demand) alone;
- the **CEN data source** that records it explicitly when not
  observable (link-references resolve in §References);
- the canonical-schema column the script reads to filter on it
  (all optional, all in the proposed `cells/unit_regime.parquet`,
  see §3.3.3 update below);
- whether v1 / v1.1 / v1.2 ships ingestion for it.

| # | Driver | Observable? | CEN source | Schema column | Ships in |
|---|---|---|---|---|---|
| 1 | **Forced pmin / must-run** (operator-declared technical minimum to keep the unit committed despite economics) | Partial — `g≈pmin AND MC>λ_z` is a strong hint but not proof. False-positives on degenerate dispatch. | [RIO][cen-rio], [Restricciones Operativas][cen-restricciones], [Novedades CDC][cen-novedades] | `regime.forced_pmin:bool` | v1 (Restricciones Operativas), v1.1 (RIO) |
| 2 | **Gas inflexible** (TOP / take-or-pay, MUE / minimum-use-energy under gas-supply contracts — unit must burn contracted gas) | No — the contract obligation is invisible from generation data; gas-inflexible blocks look like ordinary thermals at pmin. | RIO with the *combustible inflexible* flag; declared in the weekly fuel availability submissions ("Declaración Semanal de Costos Variables"). | `regime.inflexible_fuel:bool`, `regime.fuel_kind:str` | v1.1 |
| 3 | **Ancillary service assignment** (CPF primary frequency, CPR secondary frequency, spinning reserve, voltage support, black-start) | No — a unit kept on for reserve duty looks like a commitment decision; the reserve commitment itself is a separate variable not shipped in dispatch data. | [RIO-SSCC][cen-rio-sscc] (Registro de Instrucciones de Operación – SSCC y Energía); the annual *ISSCC / Plan Anual de SSCC* defines pre-assigned units. | `regime.sscc:str|null` enumerating `cpf` / `cpr` / `spinning` / `voltage` / `black_start` / `inertia` | v1 (RIO-SSCC), v1.1 (full annual plan resolution) |
| 4 | **N−1 contingency reserve** (unit committed to cover a transmission-element trip per the security study) | No — the contingency is a security-policy decision; observable only as an "uneconomic commit" signature. | [Restricciones en el Sistema de Transmisión][cen-restricciones-tx]; CDC operational studies referenced in [Novedades CDC][cen-novedades]. | `regime.network_security:bool` | v1.1 |
| 5 | **Programmed maintenance** (unit unavailable for a planned outage — out of catalogue but still listed in topology) | Partial — observable as `g=0` for the entire window if the catalogue lists `pmax>0`. | [Índices de Indisponibilidad][cen-indispo]; programa de mantenimientos mayores published by CEN. | `regime.unavailable:bool, regime.unavail_reason='maintenance'` | v1 (Indisponibilidades feed) |
| 6 | **Forced outage / falla** (unforeseen unavailability) | Partial — same observable signature as maintenance. | CEN [IRF — Informe Resumen de Falla][cen-irf]; daily CDC novedades. | `regime.unavailable:bool, regime.unavail_reason='forced_outage'` | v1.1 |
| 7 | **Operator override / instructed dispatch** (CDC overrides merit dispatch for security/quality reasons; *despacho instruido*) | No — looks like an arbitrary operating point chosen by the operator. | [RIO][cen-rio] entries explicitly flagged "instrucción". | `regime.instructed_dispatch:bool` | v1.1 |
| 8 | **Hydro irrigation flow** (turbine release dictated by water-rights / irrigation agreements, not by water value) | Partial — observable when realised flow tracks an irrigation schedule rather than the water-value-implied schedule, but the inference is fragile. | Already handled in this codebase via the `gtopt_irrigation` machinery (Maule, Laja agreements); CEN does not publish irrigation drivers separately. | `regime.hydro_irrigation:bool` | v1 (re-uses existing irrigation registry) |
| 9 | **Environmental / RCA limit** (emission permit caps SO₂/NOₓ/PM, or thermal-cooling water-temperature limit binding) | No — environmental constraint is not visible in the operational data. | RCA registers (SEA / SMA), CEN may publish in *Restricciones Operativas* when the limit binds operationally. | `regime.environmental_limit:bool` | v1.2 |
| 10 | **Combined-cycle / minimum up-down time** (once committed, the CC cannot back off below pmin without a costly shutdown — start-up cost amortisation) | Partial — multi-hour `g≈pmin` runs can suggest CC commitment lock-in, but require a multi-period view. | RIO entries; declared cycling parameters in the unit's technical minimums register. | `regime.committed_run_lock:bool` | v1.2 |
| 11 | **Black-start / island restoration commitment** (unit must be online and ready to restart the system) | No — looks like an ordinary commitment. | RIO-SSCC; CEN's black-start designation list. | `regime.sscc='black_start'` (subset of #3) | v1.1 (with #3) |
| 12 | **Inertia / synchronous-machine requirement** (low system-inertia hours force a synchronous unit to stay online) | No — looks like an ordinary commitment. | RIO entries flagged for inertia; CDC operational-study output. | `regime.sscc='inertia'` (subset of #3) | v1.2 |

#### 4.11.2 The merit-eligibility filter

The Phase-1 script computes a single boolean per `(cell, gen_uid)`:

```text
merit_eligible[g] :=
  (g.kind != "profile")             # renewables never set price
  AND (NOT regime.unavailable[g])    # off → cannot be marginal
  AND (NOT regime.forced_pmin[g])
  AND (NOT regime.inflexible_fuel[g])
  AND (regime.sscc[g] IS NULL)       # any SSCC assignment excludes
  AND (NOT regime.network_security[g])
  AND (NOT regime.instructed_dispatch[g])
  AND (NOT regime.hydro_irrigation[g])
  AND (NOT regime.environmental_limit[g])
  AND (NOT regime.committed_run_lock[g])
```

Any column that is `NA` (the producer didn't ship it) defaults to
`False` — i.e. **absent regime data is treated as "not forced"**,
matching the v1 reality where most regime columns will be missing.
This is conservative: the script may classify an actually-forced
unit as merit-eligible (false positive on price-setter) but never
the other way around. Every emitted row carries
`regime_data_completeness:str` ∈ `{full, partial, none}` so the
consumer can filter to high-confidence cells.

§4.7 R3 changes from "interior units in zone z" to "interior **and
merit-eligible** units in zone z". §4.9 ladder ranks exclude
non-eligible units; they are still recorded in
`audit/out_of_merit.parquet` (new) so the user can audit the
attribution.

#### 4.11.3 Observability summary

Of the twelve drivers above, **only #1 (forced pmin), #5 (planned
maintenance), #6 (forced outage), #8 (hydro irrigation) and #10
(CC commit lock-in)** can be partially inferred from
generation+demand+topology data. The remaining seven (#2 gas
inflexible, #3 SSCC, #4 network security, #7 instructed dispatch,
#9 environmental, #11 black-start, #12 inertia) **cannot be
recovered without a CDC-side data source**. This is the
operational-decision observability gap the user flagged: it is
larger for generator commitment than for line saturation, and
larger still for the *reasons* behind each commitment.

#### 4.11.4 What v1 actually does

v1 ships:

* **filter machinery** (the `merit_eligible` column and the §4.7
  R3 / §4.9 integration) wired up against every regime column —
  but with `NA` defaults so the user can run without any regime
  data;
* **ingestion for #1, #5, #8** (Restricciones Operativas, Indices
  de Indisponibilidad, irrigation registry) — the three drivers
  the team has the highest confidence in for v1;
* **schema columns** for all 12 drivers (so v1.1 / v1.2 can fill
  them without a schema-version bump);
* **`audit/out_of_merit.parquet`** that records, per cell, every
  unit excluded by the filter and *which* regime column triggered
  it. This is the explicit traceability the operator needs.

Drivers #2 (gas inflexible), #3 (SSCC), #4, #7, #11 ship in v1.1.
Drivers #9, #10, #12 ship in v1.2 or are flagged out-of-scope (§10).

### 4.12 Marginal emission intensity (analogous to marginal price)

The marginal-unit attribution machinery doubles as a **marginal
emission intensity** computer. The economic logic
"the marginal unit's variable cost sets the bus price" carries over
verbatim to "the marginal unit's emission factor sets the bus
marginal emission intensity":

> ε_b [kgCO₂/MWh] = emission_rate[g_marginal at b]

The same recipe table machinery (§4.6.4) that lets a downstream
consumer recompute λ_b under a new cost catalogue *trivially*
extends to recomputing ε_b under a new emission catalogue — the
formula structure is identical, only the per-unit datum is
different. v1 ships this as a first-class output, not a follow-up.

#### 4.12.1 Per-unit emission factor

The canonical schema's `Topology.generator` row gains an optional
`emission_rate:float|NA` column (kgCO₂-equivalent per MWh
delivered). Sources:

* **simulated mode** — read from the planning JSON's
  `system.generator_array[*].emission_rate` field if present
  (gtopt already supports it as an optional input attribute);
  otherwise NA;
* **real mode** — `cen2gtopt` populates from CEN's per-unit
  emission factor register (see §9.2.5 below);
* **other producers** — any feed-writer may populate the column
  from its own catalogue.

When the column is NA, all emission outputs for that unit are NA
(but cell rows are still emitted with `emission_intensity=NA`,
never silently dropped).

For multi-pollutant analysis the user can pass repeated
`--emission-attribute=co2,so2,nox,pm` to read additional columns
of the same shape; v1 ships only `co2` (the column called
`emission_rate` in the schema is a CO₂-equivalent in
kgCO₂eq/MWh). Multi-pollutant is v1.1 work.

#### 4.12.2 The emission-intensity recipe table

Output dataset (§4.6) gains a sibling table:

```
marginal_units.parquet/
  bus_emission_intensity_recipe.parquet   # analogue of bus_price_recipe
```

```
columns:
  cell_key columns,
  bus_uid:int, zone_id:int,
  formula_kind:str,                          # see below
  marginal_gen_uids:list[int],               # same uids as the price recipe
  marginal_weights:list[float],
  marginal_emission_factors:list[float],     # captured at write time
  formula_constant:float,                    # 0.0 for unit-driven; 0.0 for demand_fail
  formula_explanation:str,
  recomputed_emission_intensity:float        # ε_b reconstructed from the formula
```

The `marginal_gen_uids` and `marginal_weights` are **always
identical** to those in `bus_price_recipe.parquet` for the same
`(cell_key, bus_uid)` — the same merit-list pick drives both
formulas. The only thing that differs is the per-unit datum
(`marginal_costs` → `marginal_emission_factors`). This means a
downstream consumer who already trusts the price recipe trusts the
emission recipe by construction.

`formula_kind` enumeration (parallel to §4.6.4):

| Kind | ε_b formula |
|---|---|
| `single_unit`           | `ε_b = emission_rate[g₀]` |
| `tied_units`            | `ε_b = Σ w_i · emission_rate[g_i]` (weights sum to 1) |
| `forced_pmin_marginal`  | `ε_b = emission_rate[g₀]` (the must-run unit's factor) |
| `hydro_marginal`        | `ε_b = 0` (zero-emission proxy; reservoir/hydro turbines emit nothing at the bus bar) |
| `demand_fail`           | `ε_b = 0` (load shedding has no associated generation) — flagged in audit |
| `renewable_curtailment` | `ε_b = 0` |
| `unattributed`          | `ε_b = NA` |

The `demand_fail` row is the one corner where the price-vs-emission
attribution diverges in interpretation: the price snaps to the
rationing cap while the emission intensity drops to zero (no MWh
was generated to serve that load). The recipe table's
`formula_explanation` makes this explicit so an audit consumer
never confuses a "high-price, zero-emission" cell with a clean-grid
hour.

#### 4.12.3 Consumer API extensions

The `gtopt_marginal_units.recipes.MarginalUnitDataset` (§4.10)
gains the following public methods:

```python
ds.bus_emission_intensity(stage=4)
# → DataFrame[cell_key, bus_uid, emission_intensity_kg_per_mwh]

ds.recompute_emission(unit_emissions={gen_uid: ef})
# → DataFrame[cell_key, bus_uid,
#             emission_intensity_recomputed,
#             emission_intensity_original,
#             emission_delta]

# Combined dual-currency view — both λ_b and ε_b in one frame:
ds.bus_lmp_and_emission(stage=4)
# → DataFrame[cell_key, bus_uid, zone_lmp, emission_intensity]
```

This is the operational use case: an analyst wants to see
*generation merit-order* and *carbon merit-order* side by side, and
the dataset lets them join without an LP rerun.

#### 4.12.4 CI invariants

The same writer-side and reader-side round-trip checks (§4.6.4
invariants 1–2 and §4.10.1) apply, with `emission_rate` substituted
for `MC` and `recomputed_emission_intensity` for `recomputed_lmp`.
The two recipe tables share `marginal_gen_uids` exactly (CI test:
inner join on `(cell_key, bus_uid)`, asserting the two UID lists
are identical).

#### 4.12.5 Literature and ISO practice — how others compute LME

The marginal-emission machinery has a substantial literature and is
already deployed in production by several US ISOs. Before locking
the v1 design we surveyed the leading methodologies; the headline
findings are below, and the three methodologies fall into clean
categories that map onto our v1 / v1.1 roadmap.

**(a) Lin & Tang (2024) — theoretical foundation, *Is LMP All You
Need for LME?* [arXiv:2411.12104][lit-lin2024].** Working from
multi-parametric programming over SCED, the authors prove that LME
is a unique function of LMP within each *critical region* of the
constraint set. The headline equations:

> `αⱼ = ∂C/∂lⱼ = Σᵢ (∂xᵢ/∂lⱼ) · cᵢ`     (LMP, eq. 2)
>
> `βⱼ = ∂E/∂lⱼ = Σᵢ (∂xᵢ/∂lⱼ) · eᵢ`     (LME, eq. 3)
>
> `β = Φ(α)`                              (mapping, eq. 13)

Both share the same generation-sensitivity term `∂xᵢ/∂lⱼ` and only
differ in the per-unit datum (cost coefficient `cᵢ` vs emission
rate `eᵢ`). **This is exactly the recipe-table architecture we
adopted in §4.12.2** — same `marginal_gen_uids` + `marginal_weights`
drive both formulas, only the data column differs. Lin & Tang give
us the theoretical reason this is correct.

**(b) PJM — the production reference for nodal LME.** PJM publishes
[*Five-Minute Marginal Emission Rates*][pjm-5min] and
[*Hourly Marginal Emission Rates*][pjm-hourly] at every pricing
node (~10,000 nodes), for **CO₂, SO₂, and NOₓ simultaneously**, via
its Data Miner 2 platform. PJM's method per the
[Marginal Emissions Primer][pjm-primer] is:

> *In a given five-minute interval, there is one marginal unit on
> the system, plus an additional marginal unit for each
> transmission constraint that is being experienced. The
> mathematical average of the emission rates for all marginal units
> in each five-minute interval forms a marginal emission rate.*

This is **richer than our v1 zone-based rule**. Our §4.7 R3 picks
*one* unit (or a tie set) per zone — the highest-MC interior unit.
PJM picks *one system-wide marginal unit* (the unconstrained
marginal — the unit setting the system energy price) **plus one
per binding transmission constraint**, then averages across them
(simple or load-weighted) at the location of interest.

The two rules agree on the *uncongested* case and on the *single
binding constraint* case; they diverge when multiple constraints
are simultaneously binding at the same node:

* **PJM**: the location's MEF is the average of N+1 marginal units
  (1 system + N binding constraints), each weighted by the
  location's shift factor on that constraint.
* **gtopt v1**: the zone's MEF is the single highest-MC interior
  unit in the zone induced by the saturation pattern.

The CEN procedure cited in §2.2 (*subsistema desacoplado*) is
zone-based and matches our v1 rule, so v1 is correct *for the
Chilean SEN methodology*. The PJM-style per-constraint shift-factor
decomposition is documented as a v1.1 enhancement (§10) for users
who want PJM-aligned attributions.

**(c) WattTime MOER — statistical alternative.** WattTime's
[Marginal Operating Emissions Rate][watttime] is a **statistical /
regression-based** estimator that operates on top of EPA CEMS
hourly emissions data. It does **not** require an LP/SCED solve —
it fits a regression of hourly emissions vs. hourly load
conditional on grid state, deseasonalised. Output is per-region,
5-minute, with 72-hour forecasts.

WattTime's approach is the right tool when LP-side data is
unavailable; gtopt's approach is the right tool when the LP/dispatch
representation is available (because it preserves the per-unit
attribution that statistical models lose). The two are
complementary, not competing — the user can in principle compare
gtopt's reconstructed `bus_emission_intensity` against WattTime's
MOER for the same region as a back-test.

**(d) MISO — what's coming.** [MISO's Emissions Dashboard][miso-em]
currently publishes zonal CO₂-only "Fuel on the Margin" with a
one-day lag. MISO has announced it will publish PJM-style nodal
LMEs in late 2025. CAISO/ERCOT/SPP/NYISO/ISONE LMEs are exposed
through commercial providers ([Resurety][resurety],
[Impact][impact]) rather than primary feeds.

**Methodology taxonomy** (from the
[CEBI sourcing guide][cebi-guide]): the three approaches are
*merit order* (closed-form, our R3), *economic dispatch* (full LP
re-solve under perturbation, the textbook ground truth), and
*statistical* (WattTime). Our v1 uses merit-order in
`mode=real-reconstruct` and reads the LP duals directly in
`mode=simulated` — i.e., we straddle (a) and (b).

#### 4.12.6 What v1 ships vs v1.1 enhancements

Confirmed for v1:

* `bus_emission_intensity_recipe.parquet` with the same
  `formula_kind` enumeration as price (§4.12.2) — vindicated by
  Lin & Tang.
* CO₂-only emission factor; `emission_rate` topology column
  (multi-pollutant deferred).
* Zone-based attribution per §4.7 R3 — matches CEN's *subsistema
  desacoplado* procedure.
* Optional WattTime MOER cross-check via
  `--moer-compare path/to/moer.csv` (back-test only, not a CI gate
  per `feedback_no_alpha_fix_for_compress`).

Targeted for v1.1:

* **Multi-pollutant**: extend `emission_rate` to a list of
  `(pollutant, factor)` pairs; PJM publishes CO₂/SO₂/NOₓ together
  and the recipe table generalises trivially since each pollutant
  is just a different `cᵢ`-vs-`eᵢ` substitution.
* **PJM-style per-constraint marginal-unit decomposition**: when
  more than one transmission constraint binds simultaneously,
  attribute the location's MEF as a shift-factor-weighted average
  across (system marginal + binding-constraint marginals). This
  requires PTDF and the GLDF (generation load distribution factor)
  matrix — already needed for §4.7 R1's PTDF estimate path, so
  marginal cost.
* **MOER-style statistical fallback** when neither dispatch nor
  declared MC is available — out of scope as primary v1 method but
  could be a `--mode=real-statistical` follow-up.

#### 4.12.7 Out of scope (v1)

* **Marginal-emission attribution under flow-induced congestion**
  beyond the zone-LMP convention. v1 reports only the headline
  zone-level intensity, matching the LMP treatment in §4.6 and the
  CEN methodology in §2.2; PJM-style per-constraint decomposition
  is v1.1 (above).
* **Multi-pollutant** intensity (SO₂, NOₓ, PM, Hg). v1 ships only
  the `co2` column; v1.1 generalises the schema.
* **Emission-cost internalisation** (carbon-tax-adjusted MC). v1
  reports `emission_intensity` separately; folding a carbon price
  into `marginal_cost` itself is a v1.2 feature gated behind
  `--carbon-price-usd-per-tco2`.
* **Statistical / regression marginal-emission estimation**
  (MOER-style without LP data) — covered as comparison only via
  `--moer-compare`, not as a primary mode.

---

## 5. Public CLI

Convention follows `scripts/gtopt_check_output/` and
`scripts/gtopt_results_summary/` (each under
`scripts/<name>/{__init__.py, __main__.py, main.py}`):

```text
gtopt-marginal-units \
    [--input-kind {gtopt-dir,feed-parquet,auto}]      # default: auto (sniffed)
    --mode {simulated,real,real-reconstruct,compare}  # classification mode
    [--planning  path/to/planning.json]               # gtopt-dir
    [--output    path/to/output_dir/]                 # gtopt-dir
    [--feed      path/to/canonical_feed.parquet]      # feed-parquet
    [--feed-against path/to/canonical_feed.parquet]   # --mode compare on gtopt side
    [--gtopt-against path/to/output_dir/]             # --mode compare on feed side
    [--out       path/to/marginal_units.parquet]      # default: ./marginal_units.parquet
    [--csv]                                           # also write .csv view
    [--scenes 1,2] [--stages 1-3] [--blocks 1-24]     # filters (gtopt-dir input)
    [--dates ...] [--hours ...]                       # filters (feed-parquet input)
    [--tol-price 1e-3] [--tol-flow 5e-3] [--tol-mu 1e-2]
    [--tol-load-mw 1.0] [--eps 1e-4]                  # load-balance tol (separate units)
    [--single-bus]                                    # force copperplate
    [--zone-mode {congestion,physical,both}]          # default: congestion
    [--merit-ladder-depth 3]                          # K rungs above + below per zone
    [--require-cdc-restriction]                       # refuse to infer line saturation
    [--require-regime-data]                           # refuse to infer unit-commitment regime
    [--emission-attribute co2]                        # which Topology.generator column to use; v1 supports co2
    [--moer-compare path/to/moer.csv]                  # optional WattTime-style MOER back-test
    [--report    path/to/report.md]                   # human-readable digest
    [-v|-q]
```

The output `--out` is a parquet **dataset directory** (§4.6), not a
single file. `--merit-ladder-depth 0` writes only the rank-0 row
(equivalent to no ladder); the merit ladder is otherwise always
populated.

**Auto-detection** (`--input-kind auto`, the default): if `--planning`
is set, kind is `gtopt-dir`; if `--feed` is set, kind is
`feed-parquet`; if both are set, exit 3. The startup banner logs the
resolved kind so the user can audit the choice.

The illegal `(--input-kind, --mode)` combinations from §4.8.3 are
listed inline in `--mode`'s help text so the constraint is visible at
`--help` time, not only at runtime.

The Phase-1 CLI **does not** know about CEN. There are no `--cen-*`
flags here; CEN-specific knobs live on the Phase-2 `cen2gtopt` CLI
(§9). To analyse real SEN operation:

```bash
# Phase 2 — fetch + normalise (one-shot)
cen2gtopt --start 2026-04-01 --end 2026-04-07 --out feed.parquet

# Phase 1 — analyse (no network access required)
gtopt-marginal-units --input-kind feed-parquet --feed feed.parquet \
    --mode real-reconstruct --report report.md
```

Exit codes:

* `0` — every cell attributed cleanly;
* `2` — at least one cell was unattributed (`__unattributed__` rows
  emitted; degenerate flag in artifact);
* `3` — input files missing / inconsistent (no output written).

---

## 6. Edge cases & pitfalls

1. **LMP sign / scale**. gtopt's objective is in scaled $; the reported
   `Bus/balance_dual` is in **unscaled $/MWh**. The script enforces
   `λ_z ≤ 1.05 · demand_fail_cost` with an explicit
   `if not (...): raise ValueError(...)` (never `assert`, since
   `python -O` strips assertions). Values above mean the user forgot
   `--scale_objective` consistency on a non-default run, and the
   script aborts with exit code 3 and a clear message instead of
   producing nonsense classifications. (See
   `feedback_no_alpha_fix_for_compress` for the matching pattern in
   SDDP land — never silently reinterpret a suspect dual.) The same
   "raise, never assert" rule applies to every sanity check listed
   in this section.

2. **Single-bus mode**. When `options.use_single_bus=true`, every line
   variable disappears and the topology graph is empty. The script
   skips line/zone analysis and reports a single zone over all buses.

3. **Hydro / battery without water value**. If reservoir/battery duals
   are not present in the output (the user did not request them), the
   script still emits `hydro_marginal` rows but sets `marginal_cost =
   NaN` and `reduced_cost = NaN` and adds a one-time warning. (Per
   `feedback_no_nan` we use `pd.NA` / `Optional` in the in-memory
   DataFrame, NaN only at the Parquet boundary.)

4. **Profile generators**. Wind/solar (`generator_profile_array`) are
   never marginal (zero MC) but are dispatched up to the profile cap.
   They are reported with `status=profile_dispatched` and never enter
   the marginal-unit list, even at zero LMP. A zero LMP combined with
   curtailed profile generation is reported as
   `__renewable_curtailment__` (the curtailment shadow sets the price).

5. **Demand-fail / load shedding**. When zone LMP equals
   `demand_fail_cost` within `tol_price`, the script emits
   `__demand_fail__` as the marginal "unit" (this matches CEN's
   convention of attributing the price to the rationing component when
   it binds).

6. **Numerical noise on saturation**. Lines occasionally show
   `|flow| ≈ tmax` with `mu ≈ 0` (alternative basis). We **require
   both** the flow and the dual conditions to declare a line saturated;
   this matches the `lp-numerics-expert` review pattern.

7. **Zones that disagree on LMP**. If `max(λ_b) − min(λ_b) > tol_lmp`
   inside a zone, the partition is wrong (we missed a saturated line).
   The script logs the offending line list (lines with
   `|flow|/tmax > 0.95` not currently in `saturated`) and downgrades
   the cell to `degenerate=True`.

8. **Multiple stages with different topology**. A line that is
   `active=false` in stage *p* is dropped from the static lookup for
   that stage — handled by indexing `line_df` per stage. Same for
   generators with `active=false`.

9. **Real (physical) islands**. `--zone-mode physical` ignores the
   dispatch and partitions purely by the line topology (any line whose
   thermal limit is `0` or that is `active=false`). `congestion` (the
   default) takes the union of physical and dispatch-induced
   saturations. `both` writes two independent attributions side by
   side, useful when the user wants to know what the price *would* be
   without congestion.

10. **CEN UID / name alignment**. CEN names units in Spanish with
    accents and parentheses (e.g. `BOCAMINA II`, `Ventanas N°1`); gtopt
    JSON typically uses ASCII-folded names. The reader pre-pass
    normalises to NFC, lowercases, strips punctuation, and falls back
    to a fuzzy-match (Levenshtein ≤ 2) with a warning when an exact
    match fails. Unmapped CEN units are reported in a side file
    `unmapped_units.csv`; unmapped gtopt units → likewise.

11. **Time-zone alignment**. CEN publishes hours in **Chile-continent
    civil time** (no DST since 2015 for billing-relevant data, but
    historical files before 2015 do shift). gtopt blocks are
    timezone-naive. The reader stamps every CEN row with
    `Chile/Continental` localised, then converts to UTC for the
    canonical `cell_key`; the simulated reader localises the
    planning-defined start time identically. A `--cen-tz`
    override lets the user pin a different zone (e.g. SING legacy
    data).

12. **CEN catalogue churn**. CEN routinely renames units (commercial
    operation date, ownership changes). When `--cen-from` spans a rename,
    the reader uses the SIP `centrales` history endpoint if available,
    otherwise emits a one-time warning per renamed unit and treats the
    pre/post names as the same canonical UID.

13. **Reconstructed LMP exceeds demand-fail cap**. If §4.7 step R3
    yields a λ_z above `demand_fail_cost` it means the merit-order
    candidate has a declared MC above the rationing cap — usually a
    data error in the declared cost catalogue. The script clamps
    λ_z to `demand_fail_cost`, sets `confidence=fallback`, and
    surfaces the offending unit in the report.

14. **CDC operational saturation ≠ flow-binding**. In real
    operation, line saturation is an **operator declaration** by
    CDC based on N−1 / voltage / stability margins; it is **not**
    inferable from `|f| ≥ tmax` alone (a line at the limit may be
    a scheduled operating point; a line below the limit may still
    be declared restricted). §4.7 R1 therefore prefers an explicit
    CDC declaration (`cells/line_restriction.parquet`, populated by
    `cen2gtopt` from RIO / Restricciones Operativas / Novedades CDC)
    over any flow-based inference, and downgrades `confidence` to
    `fallback` whenever the inference path was used. The
    `--require-cdc-restriction` flag forces a hard refuse-to-run
    instead of falling back, for strict-audit users.

15. **Unit commitment regime ≠ economic dispatch**. A unit at
    `g ≈ pmin` with `MC > λ_z` may be (a) economic-but-degenerate
    or (b) committed against economics for one of twelve
    operational drivers (§4.11.1) — gas inflexible, ancillary
    service, N−1 reserve, voltage support, etc. Only five of those
    drivers are partially observable from generation+demand+topology
    data (#1, #5, #6, #8, #10); the other seven require CDC-side
    sources (RIO, RIO-SSCC, ISSCC, IRF, RCA registers). The merit-
    eligibility filter (§4.11.2) defaults to "not forced" for absent
    regime columns, which is conservative — it can promote a
    committed-against-economics unit to price-setter and produce a
    spurious LMP. Every emitted row carries
    `regime_data_completeness ∈ {full, partial, none}` so consumers
    can filter to high-confidence cells. The
    `--require-regime-data` flag forces a hard refuse-to-run when
    the regime feed is not present, for strict-audit users.

---

## 7. Test plan

Per `feedback_proactive_tests` — every PR ships unit + integration
tests, fanned out via parallel agents per
`feedback_parallel_agents_for_tests`.

### 7.1 Unit tests (`scripts/gtopt_marginal_units/tests/`)

| File | Covers |
|---|---|
| `test_classify.py` | `classify` rule table — **all 8 statuses including `hydro_marginal` (row 4) and `extramarginal_interior` (row 7) which earlier drafts missed**, with synthetic inputs covering `pmin=0`, `pmin>0`, segment breaks, demand-fail cap, and `kind ∈ {thermal, hydro, battery, profile}` per row. Priority-order is verified by a fixture where two rules would both match. |
| `test_zones.py` | Connected-components partition: 3-bus chain with mid-line saturated → 2 zones; 3-bus ring all-saturated → 3 zones; single-bus mode → 1 zone; **disconnected (islanded) topology — PTDF builds per component, no singularity**. |
| `test_segments.py` | Piecewise-linear MC lookup: 3-segment unit, dispatch in segment 0/1/2/at-break. Dispatch-at-break must report two MC candidates. |
| `test_tolerances.py` | LMP-vs-MC gap exactly at `tol_price`; zone-load vs Σ pmin gap exactly at `tol_load_mw` (verifies the units-fix from this revision); flow exactly at `tmax`; matrix of (flow, mu) → saturated/not-saturated. |
| `test_io_roundtrip.py` | Read CSV-shard layout, parquet-partitioned layout, single-file layout (using a tiny synthetic case in `tests/data/`). |
| `test_cli_smoke.py` | `gtopt-marginal-units --help` exits 0; missing flags exit 3 with a clear message; runs end-to-end on a 1-cell synthetic case; **also asserts exit code 2 on a fixture that has at least one `__unattributed__` cell** (covers the operator-monitoring contract). |
| `test_cen_reader.py` | (Phase-2 `cen2gtopt` territory — moved to §9.6.) |
| `test_reconstruct.py` | §4.7 R3 on a hand-constructed 3-bus / 3-unit case in five regimes: (a) interior-MC-max (the textbook case), (b) all-pmax binding-up — **assert `λ_z = demand_fail_cost`, not the cheapest pmax-pinned MC** (P0 lp-numerics correction), (c) `forced_pmin` zone-picker fallback (no interior unit, but Σpmin matches load within `tol_load_mw`), (d) `λ_z > demand_fail_cost` clamp branch, (e) load > Σ pmax → `__demand_fail__`. PTDF path covered separately in `test_zones.py`. |
| `test_minimum_data.py` | Each `(--input-kind, --mode)` combination rejects with exit-3 when its required minimum (§4.8.3) is not met, and accepts when it is. The auto-detection branch is covered (planning-only → gtopt-dir; feed-only → feed-parquet; both → exit 3). |
| `test_expansion_guard.py` | Planning JSON with at least one `expansion=true` line/generator → `gtopt-marginal-units` exits 3 with the v1-not-supported message documented in §3.2. |
| `test_merit_ladder.py` | §4.9 ladder construction on a hand-built 5-unit zone: rank-0 = anchor, rank +1/+2/+3 are next-up units in cost order with correct `headroom_up_mw`/`hypothetical_lmp`; rank −1/−2/−3 are next-down; ladder shorter than `2K+1` near zone endpoints; tied-MC collapse to single rung with audit row; synthetic anchor → only rank-0 row; piecewise unit's `hypothetical_lmp` reflects the active segment under the displaced anchor. |
| `test_recipes.py` | §4.10 consumer API round-trips: `recompute_lmp(unit_costs=captured)` → `lmp_delta=0` everywhere; missing-UID returns `NA` with a `missing_units_count` summary; `outage_sensitivity(g)` agrees with merit-ladder rank+1 for every cell where g was actual marginal; manifest hash mismatch on open raises; every `formula_kind` exercised end-to-end including `demand_fail` and `renewable_curtailment`. |
| `test_writer_invariants.py` | The writer's pre-persist check `|recomputed_lmp − zone_lmp| ≤ tol_price` aborts with exit 3 on a synthetic recipe-table-vs-zone-LMP mismatch (covers §4.6.4 invariant 1). |
| `test_emission_intensity.py` | §4.12 emission-intensity recipe round-trip on a 3-unit fixture (gas / coal / hydro emission factors): `single_unit` formula yields the expected ε_b; `tied_units` weighted sum; `hydro_marginal` and `renewable_curtailment` produce ε_b = 0; `demand_fail` produces ε_b = 0 with audit-flag set; missing `emission_rate` yields NA + `formula_kind=unattributed`. CI invariant: `bus_price_recipe.marginal_gen_uids == bus_emission_intensity_recipe.marginal_gen_uids` for every cell. |

### 7.2 Integration tests (against the existing IEEE benchmarks)

Run after the standard `gtopt` integration suite produces output. Each
test reads `build/integration_test/test_output/<case>/` and asserts
properties the LP must satisfy.

| Case | What we assert |
|---|---|
| `ieee_4b_ori`  | exactly one zone every cell; the marginal unit is the merit-order match. |
| `ieee_9b_ori`  | uncongested (every `flowp_cost = 0`); `λ_z = 35` everywhere; marginal unit = the `gcost=35` thermals. |
| `ieee_9b`      | solar profile cells reported as `profile_dispatched`, never marginal. |
| `ieee_14b`     | **congested** cells (line 1 saturated per the dataset above); zone partition has ≥ 2 zones; LMP differs across zones by exactly `μ_l`. |
| `c0`           | multi-stage capacity expansion: same generator can change status across stages — assert the script reports per-stage attribution correctly. |

### 7.3 Property tests (Hypothesis)

* For any random generator + LMP triple, `classify` returns exactly one
  status.
* For any random connected graph + saturated-line subset, the zone
  partition is a partition (every bus in exactly one zone).
* `is_marginal` is preserved under permutation of generator-uid order.

### 7.4 PLP cross-check (manual / opt-in)

The script writes a CSV that is shape-compatible with the `cmgnal` /
`cmgnud` exit files PLP produces (see
`scripts/plp2gtopt/`). A `--plp-compare path/to/plp/cmgnud.csv`
flag will side-by-side a sample of cells; this is a debugging aid, not
a CI gate (per `feedback_no_alpha_fix_for_compress` we never compare
two engines as a proof of correctness — we use it to *find* bugs, not
*deny* them).

### 7.5 CEN reconstruction back-test (manual / opt-in)

A `tests/data/cen_sample/` fixture pins a small (one-week, 24×7 cells)
real-operation snapshot for one Chilean date with a known congestion
event (e.g. north-south split). The integration test runs
`mode=real-reconstruct` against it and asserts:

* ≥ 95 % of `(date, hour, bus)` cells have `|λ_reconstructed −
  λ_published_CEN| ≤ 2 USD/MWh`;
* zone partition matches CEN's *subsistemas desacoplados* report for
  the same hour (when published);
* every cell with a delta > 5 USD/MWh appears in the `topology_mismatch`
  or `must_run_override` audit list — i.e. the script *explains* every
  large discrepancy rather than hiding it.

This back-test runs only when `GTOPT_RUN_CEN_BACKTEST=1` (network +
fixture availability) — it is a quality gate for v1.1 release, not a
CI gate.

---

## 8. Documentation deliverables (Phase 1)

(Phase-2 docs are listed separately in §9.7.)

* `docs/scripts/gtopt_marginal_units.md` — user guide for the
  Phase-1 script (CLI flags, output schema, examples). Cross-link
  from `docs/scripts-guide.md`.
* `docs/methods/marginal_unit_classification.md` — the criteria
  library (§2 + §4.4 + §6 of this plan), promoted to a stable
  reference. Cross-link from
  `docs/formulation/mathematical-formulation.md` (LMP section) and
  from this plan.
* `docs/scripts/canonical_operation_feed.md` — the **schema
  reference** owned jointly by Phase 1 and Phase 2; produced as
  Phase-0 deliverable P0.1. Both scripts cross-link it.
* `docs/scripts/marginal_unit_recipes.md` — the **consumer-API
  reference** for `gtopt_marginal_units.recipes` (§4.10). Worked
  examples per `formula_kind`, the outage-sensitivity recipe, the
  merit-ladder semantics, recompute_lmp limitations, and the
  `marginal_units.parquet/` dataset layout (§4.6).
* `docs/scripts/marginal_units_workflow.md` — Phase-3 deliverable;
  end-to-end recipe wiring `cen2gtopt` and `gtopt_marginal_units`
  together.
* Inline docstrings on every public helper.
* CEN-procedure references in the user guides:
  [Costos Marginales][cen-costos],
  [Costo Marginal Real][cen-real],
  [Decoupled-subsystem rule][cen-decoupling].

---

## 9. `cen2gtopt` — Phase-2 script spec

### 9.1 Purpose and scope

`scripts/cen2gtopt/` is a **separate, independently versioned**
Python package whose only job is:

> When invoked, fetch CEN's published real-operation data **on the
> spot** for the requested date range and bus/unit selection,
> normalise it, and write a canonical operation feed (§3.3.3) that
> `gtopt_marginal_units --input-kind feed-parquet` can consume unchanged.

It is the SEN-specific data plumber. The marginal-units script never
imports from it; it never imports from the marginal-units script.
Their only contract is the canonical schema + manifest.

#### 9.1.1 v1 is on-demand, single-shot

`cen2gtopt` v1 is **strictly a one-shot CLI**: each invocation is a
self-contained pull → normalise → write cycle, then the process exits.
There is no daemon, no scheduler, no `update` sub-command, no
streaming append. Reasons:

* keeps the script's failure modes trivially debuggable (one process,
  one CEN-time-window, one output file, one exit code);
* lets the user re-run on demand from any shell, notebook, or CI job
  without state coordination;
* keeps `cen2gtopt` testable entirely against offline fixtures —
  scheduling logic would force network-touching tests into the
  default suite.

A separate **wrapper** that schedules `cen2gtopt` (e.g. cron / systemd
timer / `loop` skill) is explicitly **deferred**. Once v1 stabilises
we will revisit and pick the cheapest scheduling layer that fits the
team's workflow; this plan does not commit to one. See §10 (Out of
scope) for the deferred items.

### 9.2 Inputs (CEN endpoints)

Two access channels — one preferred, one fallback. The script supports
both and degrades gracefully when only one is available.

#### 9.2.1 Public website CSV exports (fallback path, no auth)

Each "Operación Real" / "Mercados" graph on `coordinador.cl` exposes
an **Export → CSV** button backed by a stable URL pattern. The
script ships four downloaders, each cached on disk under
`<cache>/<endpoint>/<YYYY-MM-DD>.csv.zst` so reruns don't re-hit the
network:

| Source | URL | Provides |
|---|---|---|
| Costo Marginal Real | [Costo Marginal Real (Nuevo)][cen-cmgreal-nuevo] (post-2024-07-15) / [legacy `cmgreal.coordinador.cl`][cen-cmgreal] (pre-2024-07-14) / [bulk download (CSV + XLS, filters)][cen-cmgprel] | hourly LMP per bus (`USD/MWh`) |
| Costo Marginal Online | [Costo Marginal Online][cen-cmgonline] / [bulk download][cen-cmgonline-dl] | preliminary hourly LMP per bus (15-min integrated) |
| Generación Real | [Operación Real → Generación Real][cen-gen-real] | hourly generation per unit (`MWh`) |
| Demanda Real | [Operación Real → Demanda Real][cen-dem-real] | hourly demand per bus (`MWh`) |

Pure-`requests` HTTPS GETs against the static CSV endpoints exposed
under each page (no login needed for these four historical datasets).
CI always pins against an offline fixture; the network downloader is
opt-in via `--fetch`.

#### 9.2.2 SIP public API (preferred path, requires auth)

For machine-readable access (and for unit-bus topology / declared
variable costs not exposed via the static CSV pages) the script uses
CEN's [**API Pública del SIP**][cen-sip-api] (`sipubv2`, documented at
[`portal.api.coordinador.cl/documentacion?service=sipubv2`][cen-sip-portal]).

* **Auth**: registration + API key (`Authorization` header) per the
  [v1.1 manual][cen-sip-pdf-2019]. Reads `$CEN_USER_KEY` (the
  established convention used by `scripts/cen_demanda/`); also
  accepts `$CEN_API_KEY` as a deprecated alias with a one-time
  warning. Absent ⇒ script falls back to the CSV exporters in §9.2.1
  (warning emitted) and proceeds with whatever data the CSV path can
  provide. Every run logs at INFO level which auth path was actually
  used (`auth=sip` / `auth=csv-fallback` / `auth=alias`).
* **Format**: JSON, paginated.
* **Endpoints used**:
  * `/sipub/<v>/costo_marginal_real` — bus, datetime, value;
  * `/sipub/<v>/generacion_real` — unit, datetime, value;
  * `/sipub/<v>/demanda_real` — bus, datetime, value;
  * `/sipub/<v>/centrales` — unit catalogue
    (`costo_variable, pmin, pmax, bus, tecnologia`);
  * `/sipub/<v>/barras` — bus catalogue;
  * `/sipub/<v>/lineas` — line catalogue with thermal limits.

The SIP catalogue endpoints are what give the topology + declared
cost in machine-readable form; the website CSVs assume the user
already has the topology (which is where v0 of `cen2gtopt` requires
a user-supplied JSON; v1 makes the SIP path the default).

#### 9.2.3 Operational-restriction sources (CDC declarations)

Line-saturation status in real operation is **a CDC operational
declaration**, not a flow-derived quantity (§4.7 R1). CEN publishes
this declaration through three overlapping channels — the script
attempts them in order and merges what it finds:

| Source | URL / endpoint | Coverage | Format |
|---|---|---|---|
| **RIO — Registro de Instrucciones de Operación** | [Operación → Documentos → RIO][cen-rio] (and RIO-SSCC-Energía [cen-rio-sscc] for the SSCC + energy split) | Per-instruction; CDC's authoritative log; 30-day download cap per request | XLSX / CSV via the reporting platform |
| **Novedades CDC — Informe Diario de la Operación** | [Operación → Documentos → Novedades CDC][cen-novedades] | Daily-summary level; lists operational events including line / corridor restrictions | PDF (machine-extractable) + tabular HTML on the daily page |
| **Restricciones Operativas** | [Parámetros operacionales → Restricciones Operativas][cen-restricciones] | Per-restriction register published by the Coordinador with effective windows | XLSX / CSV |
| (related) **Restricciones en el Sistema de Transmisión** | [Estudios → Restricciones en el Sistema de Transmisión][cen-restricciones-tx] | Periodic operational-study reports identifying constrained corridors | PDF (informational only) |

`cen2gtopt` v1 supports the **Restricciones Operativas** feed as the
default machine-readable source (well-shaped XLSX, hourly window
fields, line-uid mappable). RIO ingestion is v1.1 work because the
30-day download cap and per-instruction granularity require a
chunking layer and an instruction-to-line resolver. *Novedades CDC*
PDF parsing is opt-in (`--include cdc-novedades`) and quality-gated
by the `confidence` column in the output.

The fetched data is normalised into one row per `(cell_key,
line_uid)` with a boolean `restriction_declared` and (optional) the
restriction's start/end timestamps. This populates the new
`cells/line_restriction.parquet` column documented in §3.3.3.

When **none** of the three CDC channels return data for a
requested date range, the script writes the canonical feed *without*
`cells/line_restriction.parquet` and emits a one-time INFO log
(`cdc_restrictions=missing`). Phase-1's R1 then degrades to the
flow-based fallbacks (§4.7 R1 priority list); the consumer always
sees this in the audit table and the report.

#### 9.2.4 Unit-regime sources (commitment drivers)

Mirrors §9.2.3 but for the twelve commitment drivers documented in
§4.11.1. `cen2gtopt` v1 ingests the three highest-confidence
sources; v1.1 / v1.2 add the rest.

| Driver (§4.11.1 #) | Source | Endpoint / page | Format | Ships |
|---|---|---|---|---|
| #1 forced pmin / must-run | [Restricciones Operativas][cen-restricciones] (also captured in Restricciones Operativas registry alongside line restrictions) | XLSX register | XLSX/CSV | v1 |
| #1 forced pmin (per-instruction) | [RIO][cen-rio] | per-instruction log | XLSX, 30-day cap | v1.1 |
| #2 gas inflexible | RIO with combustible-inflexible flag; weekly *Declaración Semanal de Costos Variables* | XLSX | v1.1 |
| #3 SSCC assignment | [RIO-SSCC][cen-rio-sscc]; ISSCC annual plan | XLSX/PDF | v1 (RIO-SSCC only), v1.1 (full plan) |
| #4 N−1 contingency | [Restricciones en el Sistema de Transmisión][cen-restricciones-tx] | PDF studies | v1.1 |
| #5 planned maintenance | [Índices de Indisponibilidad][cen-indispo] + programa de mantenimientos mayores | XLSX | v1 |
| #6 forced outage | CEN [IRF — Informe Resumen de Falla][cen-irf]; daily Novedades CDC | PDF/XLSX | v1.1 |
| #7 instructed dispatch | RIO entries flagged "instrucción" | per-instruction log | v1.1 |
| #8 hydro irrigation | This codebase's existing `gtopt_irrigation` registry (Maule, Laja agreements) | local data, no CEN call | v1 |
| #9 environmental limit | RCA registers (SEA / SMA); CEN Restricciones Operativas when binding | XLSX | v1.2 |
| #10 CC commit lock | Multi-period inference + RIO declared cycling parameters | XLSX | v1.2 |
| #11 black-start | RIO-SSCC; CEN black-start designation list | XLSX | v1.1 (with #3) |
| #12 inertia must-run | RIO + CDC operational-study output | XLSX/PDF | v1.2 |

The script reads each source it can, normalises to the
`cells/unit_regime.parquet` schema (§3.3.3), and writes one row per
`(cell_key, gen_uid)` for every flag that is `True`. The
`source` column records which CEN feed produced the row so the
downstream consumer can trace any disputed attribution back to the
operator's primary record.

When a driver column has no source available (e.g. v1 leaves
`inflexible_fuel`, `network_security`, `instructed_dispatch`,
`environmental_limit`, `committed_run_lock`, `inertia` all
empty), the column is simply absent from the output rows and the
consumer reads it as `False` by §4.11.2's NA-defaults rule. The
`manifest.json` `regime_data_completeness` summary attribute
records which drivers were covered by the run, so a downstream
audit can immediately see what level of confidence the merit-list
filter operated under.

#### 9.2.5 Per-unit emission factor source

To populate the `Topology.generator.emission_rate` column (§4.12),
`cen2gtopt` reads CEN's per-unit emission-factor register. The
relevant published artifacts:

* CEN [Reporte de Emisiones de CO₂ del SEN][cen-emisiones] — annual
  per-unit factor table (kgCO₂eq/MWh) used in CEN's own market
  reports. The reporte references CMS-3 / EF-1 unit-level factors.
* CNE / Ministerio de Energía emission-factor reports —
  fuel-type defaults that serve as a fallback when a unit-specific
  value is missing (declared technology + heat rate ⇒ derived
  factor).

`cen2gtopt --include emissions` adds the column to the topology
file. When the source is unavailable, the column is left NA and
Phase-1's emission-intensity recipe table (§4.12.2) emits NA rows
with `formula_kind = 'unattributed'` and a `reason =
'missing_emission_factor'`.

### 9.3 Minimum data the script must fetch

To produce a feed that satisfies `gtopt_marginal_units` §4.8.1 strict
minimum, `cen2gtopt` **must** fetch:

1. **Topology snapshot** at the start of the requested date range:
   bus catalogue, generator catalogue (with declared variable cost,
   pmin, pmax, bus mapping, technology), line catalogue (with thermal
   limits).
2. **Realised generation per unit per hour** for every cell in the
   range (CEN *Generación Real*).
3. **Realised demand per bus per hour** for every cell in the range
   (CEN *Demanda Real*).

It **may** also fetch (optional enrichments):

* CEN *Costo Marginal Real* (the published LMP, only used for
  back-testing the §4.7 reconstruction);
* CEN *Costo Marginal Online* (preliminary hourly LMP);
* line flows (when CEN publishes them under *Operación Real → Flujos*
  — improves zone partition).

### 9.4 Normalisation rules (CSV / JSON → canonical schema)

* **Time zone**: every CEN row is localised in `Chile/Continental`
  civil time, then converted to UTC for the canonical
  `cell_key = (date_utc, hour)`. A `--source-tz` override exists for
  legacy SING data; default is `Chile/Continental`.
* **Unit names**: NFC-normalise (`BOCAMINA II`, `Ventanas N°1`),
  ASCII-fold for matching, keep the canonical Spanish name in the
  feed `name` column. The `manifest.json` stores both.
* **UID assignment**: the script picks **stable integer UIDs** based
  on the SIP `centrales`/`barras`/`lineas` catalogue IDs when
  available, falling back to a hash of the normalised name. UIDs are
  written to `manifest.json` so subsequent runs reuse them.
* **Catalogue churn (renames)**: when a unit appears under two names
  in the requested range, the SIP `centrales` history endpoint
  resolves the canonical UID. CSV-only fallback emits a one-time
  warning and keeps the pre/post names mapped to the same UID.
* **Missing cells**: gaps in the realised data (CEN routinely has
  metering holes) are written as `NA` in the feed; downstream
  processing decides what to do (Phase-1 flags those cells as
  `degenerate=True, reason="missing_realised_data"`).

### 9.5 Public CLI (Phase 2)

One sub-command, no daemon, no scheduler — see §9.1.1.

```text
cen2gtopt \
    --start YYYY-MM-DD --end YYYY-MM-DD            # required date range
    --out  path/to/canonical_feed.parquet          # required output
    [--bus  list-or-file]                          # restrict to buses
    [--unit list-or-file]                          # restrict to units
    [--source {sip,csv,auto}]   # default: auto (sip if $CEN_USER_KEY set)
    [--api-key $CEN_USER_KEY]                       # SIP API auth
    [--cache  ~/.cache/gtopt/cen]                  # on-disk cache dir
    [--no-cache]                                   # bypass cache, refetch
    [--include {dispatch,demand,lmp,flows}+]       # default: dispatch,demand
    [--source-tz Chile/Continental]                # tz override
    [--manifest-only]                              # just write manifest, no data
    [-v|-q]
```

Default behaviour writes the **strict-minimum** feed (topology +
dispatch + demand) — the smallest payload `gtopt_marginal_units` can
work with. `--include lmp` adds the published Costo Marginal Real
(useful for `mode=compare` back-testing); `--include flows` adds
realised line flows when available.

#### 9.5.1 Behaviour: always on-the-spot

* The CLI **always hits CEN by default** (subject to the on-disk
  cache, see below). There is no `--fetch` opt-in flag — fetching is
  the script's only purpose; if you don't want a network call, you
  should not be invoking `cen2gtopt`.
* The on-disk cache (`--cache`) is keyed by
  `(endpoint, date, query_filters)`. A cache hit reuses the file;
  `--no-cache` forces a re-fetch. The cache exists purely as a
  reliability/cost optimisation; nothing relies on its presence.
* Each invocation writes **one** canonical feed parquet to `--out`.
  If the file already exists it is overwritten (with a one-line
  warning). The script does **not** read the existing file and
  attempt to merge — that would be the deferred wrapper's job.
* The script is idempotent: invoking it twice with the same
  arguments produces byte-identical output (modulo the
  `manifest.json` `fetched_at` timestamp), enabling reproducibility
  for audits.

#### 9.5.2 Exit codes

* `0` — feed written successfully;
* `2` — feed written but with one or more cells flagged
  `missing_realised_data` (CEN had a metering hole);
* `3` — required arguments missing / inconsistent (e.g.
  `--start > --end`, missing API key for `--source sip`); no output
  written;
* `4` — network error after retries; partial output may exist at
  `<out>.partial` for inspection.

### 9.6 Phase-2 tests

| File | Covers |
|---|---|
| `tests/test_csv_parsers.py`  | CEN CSV fixtures (one per endpoint) → canonical frames. Covers tz, accents, `N°` glyph, hour-25 (DST historical), missing rows. |
| `tests/test_sip_client.py`   | SIP API mock (offline JSON fixtures) — auth header, pagination, error retry, fallback to CSV when key absent. |
| `tests/test_topology.py`     | UID stability across runs; rename resolution via SIP `centrales` history; bus-mapping consistency. |
| `tests/test_writer.py`       | Round-trip: write feed parquet, re-read it with the Phase-1 reader, assert frame equality. (This is the **schema lock-in** test from Phase 0.) |
| `tests/test_cli_smoke.py`    | `cen2gtopt --help` exits 0; `--manifest-only` works without network; `--start > --end` exits 3; missing API key + `--source sip` exits 3 with a clear message. |
| `tests/integration/test_one_week.py` (opt-in, `GTOPT_RUN_CEN_BACKTEST=1`) | Fetch one week of real data, write feed, run `gtopt_marginal_units --input-kind feed-parquet --mode real-reconstruct`, assert §7.5 quality bars. |

### 9.7 Phase-2 docs

* `docs/scripts/cen2gtopt.md` — user guide (CLI flags, endpoint
  catalogue, auth setup, cache layout). Cross-link from
  `docs/scripts-guide.md`.
* `docs/scripts/canonical_operation_feed.md` — the schema reference
  (Phase-0 deliverable; lives at the schema boundary, owned jointly
  by both packages).
* CEN-procedure references: [Costos Marginales][cen-costos],
  [Costo Marginal Real][cen-real], [SIP API][cen-sip-api].

---

## 10. Out of scope (v1)

* **Loss components** — gtopt's piecewise line-loss models change LMP
  decomposition; v1 reports the headline LMP from `Bus/balance_dual`
  and a single congestion shadow. v1.1 will split LMP into
  `energy + congestion + losses` using `Line/loss_*` outputs.
* **Time-series gcost** — v1 only handles scalar `gcost`. Scheduled
  `gcost` (`OptTRealFieldSched`) requires plumbing through
  `gtopt_field_extractor`; planned for v1.1.
* **Unit-commitment binaries** — gtopt's MIP commitment is treated as
  fixed at the LP-relaxation optimum for the purpose of marginal-unit
  detection. Reporting the UC dual (start-up amortisation) is v1.2.
* **Multi-snapshot / contingency LMPs** — only the base case is read.
* **Web/GUI integration** — v1 is CLI; the `guiservice` panel is a
  follow-up that consumes the Parquet artifact.
* **`cen2gtopt` scheduling wrapper** — v1 is strictly an on-the-spot
  one-shot CLI (§9.1.1). A wrapper that runs it on a regular daily
  basis (cron / systemd timer / `claude-code-loop` skill / GitHub
  Actions schedule) is **deferred**. Once v1 stabilises and we have
  evidence of how the team uses it, we will pick the cheapest
  scheduling layer that fits — at that point this becomes its own
  small spec, not a feature inside `cen2gtopt`.
* **Live streaming / append-mode for `cen2gtopt`** — same reason; v1
  always overwrites `--out`. A subscribe-and-append mode that grows
  an existing feed file as new CEN data lands is deferred together
  with the scheduling wrapper.
* **Cross-zone merit-ladder rungs** — v1's ladder (§4.9) only
  enumerates units in the *same* zone as the anchor. Cross-zone
  what-if analysis (the marginal unit trips, a saturated line clears,
  the zones merge, a new zone-wide marginal emerges) requires a
  what-if zone-partition recompute and is deferred to v1.1.
* **Outage cascades** — `outage_sensitivity(g)` (§4.10) is one-step
  only: it returns the rank+1 takeover, not what happens to the
  takeover unit's own merit position. Multi-step outage chains
  ("if g1 trips and the rank+1 unit also trips") are deferred —
  the saved data supports them via repeated calls but no helper is
  shipped.

---

## 11. Implementation milestones (phased)

The plan splits into three sequential phases, with parallel sub-tracks
inside each phase. Total ≈ 9–10 working days; the bottleneck is
Phase 1.

### Phase 0 — schema lock-in (1 day, single agent)

| # | Deliverable | Owner |
|---|---|---|
| P0.1 | Freeze the canonical schema in `docs/scripts/canonical_operation_feed.md`. | schema-owner |
| P0.2 | Land `scripts/gtopt_canonical_feed/` (shared helper package): `Topology`, `Cells` dataclasses, parquet I/O, `manifest.json` writer/reader, hash check. | schema-owner |
| P0.3 | Gold fixture: one tiny synthetic feed (3 buses, 3 units, 24 hours) checked into `scripts/gtopt_canonical_feed/tests/data/gold_feed.parquet`. Both Phase-1 and Phase-2 round-trip tests load it. | schema-owner |

After Phase 0 the schema is **frozen**; further changes require a
schema-version bump.

### Phase 1 — methodology (6 days, 3 agents in parallel after P1.0)

| # | Deliverable | Track / agent |
|---|---|---|
| P1.0 | Skeleton package `scripts/gtopt_marginal_units/`, `pyproject.toml` entries, lint+type CI green, CLI smoke test passing. | A (sequential, blocks all others) |
| P1.A | gtopt-output reader + per-cell static lookups; uncongested `ieee_9b_ori` integration test passing. | A |
| P1.B | Canonical-feed reader on Phase-0 gold fixture; round-trip integration test passing. | B (parallel with A) |
| P1.C | Congestion-zone partition + classification rules; `ieee_14b` congested-case integration test passing. | A |
| P1.D | §4.7 reconstruction algorithm; PTDF builder; synthetic 3-bus case integration test passing (using the gold feed with `lmp` column dropped). | B |
| P1.E | Piecewise MC, hydro/battery, profile, demand-fail flagging; full property-test suite green. | A |
| P1.F | Output **parquet dataset** writer (§4.6 — five tables + manifest); per-bus & per-zone attribution; `--report` Markdown digest. | C (parallel with A,B once P1.0 lands) |
| P1.I | **Merit-ladder construction** (§4.9) and `merit_ladder.parquet` writer; `test_merit_ladder.py` green. | C |
| P1.J | **Bus-price-recipe** (§4.6.4) and `bus_price_recipe.parquet` writer with the writer-side invariant check; `test_writer_invariants.py` green. | C |
| P1.K | **Consumer API** `gtopt_marginal_units.recipes` (§4.10): `MarginalUnitDataset`, `bus_lmp`, `recompute_lmp`, `outage_sensitivity`, `merit_ladder`; `test_recipes.py` green. | C |
| P1.G | Unit-test fan-out per `feedback_parallel_agents_for_tests`: one sub-agent per file in §7.1 (test fan-out targets the now-12-file list including `test_merit_ladder.py`, `test_recipes.py`, `test_writer_invariants.py`). | C drives, subagents 1–8 |
| P1.H | `mode=compare` joiner + delta report; v1 release of `gtopt_marginal_units`. | A |

Phase-1 acceptance: all integration tests in §7.2 pass; the
reconstruction back-test on the Phase-0 gold fixture matches the
analytical answer to within `tol_price`.

### Phase 2 — CEN data retrieval (3.5 days, 2 agents in parallel)

Phase-2 deliverable is **strictly an on-the-spot one-shot CLI**
(§9.1.1, §9.5.1). No daemon, no scheduler, no `update` sub-command.
The regular-cadence wrapper is deferred to a separate spec once v1
has been used in anger.

| # | Deliverable | Track / agent |
|---|---|---|
| P2.0 | Skeleton package `scripts/cen2gtopt/`, `pyproject.toml` entries, lint+type CI green, CLI smoke test passing. | D (sequential, blocks all others) |
| P2.A | CSV exporter readers (Costo Marginal Real, Generación Real, Demanda Real); offline-fixture tests in §9.6. | D |
| P2.B | SIP API client (`sipubv2`, paginated JSON, auth via `$CEN_USER_KEY`); offline JSON-fixture tests. | E (parallel with D) |
| P2.C | Topology snapshot (catalogue endpoints), UID assignment, rename resolution. | D |
| P2.D | Canonical-feed writer; round-trip test against the Phase-0 gold fixture. | D |
| P2.E | Test fan-out for §9.6 — five `tests/test_*.py` files in parallel. | E drives, subagents 1–5 |
| P2.F | v1 release of `cen2gtopt` — single `cen2gtopt --start … --end … --out feed.parquet` invocation. | D |

Phase-2 acceptance:
1. `cen2gtopt --start … --end … --out feed.parquet` on a one-day
   fixture produces a feed that the Phase-1 reader round-trips
   bit-for-bit against the gold fixture.
2. Re-running the same command produces a byte-identical output
   (modulo `manifest.json:fetched_at`) — idempotency check.
3. The script ships **no scheduling code** of any kind. CI verifies
   this by grep-ing the package for `cron|systemd|schedule|daemon`
   and failing if hits are found.

### Phase 3 — integration (1.5 days, 1 agent)

| # | Deliverable | Owner |
|---|---|---|
| P3.A | Run Phase-2 against the §7.5 one-week SEN snapshot fixture; pipe into Phase-1 `mode=real-reconstruct`. | F |
| P3.B | Back-test reconstruction vs. CEN-published LMP; assert §7.5 quality bars (95% within ±2 USD/MWh, every >5 USD/MWh delta explained by `topology_mismatch`/`must_run_override`). | F |
| P3.C | End-to-end user guide in `docs/scripts/marginal_units_workflow.md` showing the two-script flow with copy-pasteable commands. | F |

Total ≈ 1 + 5 + 3.5 + 1.5 = **11 working days** for v1.

> **Why phased**: Phase 1 is methodology — the part where we don't
> know the answer. We must finish it on simulated data (where the
> answer is verifiable from LP duals) **before** committing to a
> real-data interpretation that has no ground truth. Phase 2 is
> well-defined data plumbing that adds no new risk to the
> methodology. Phase 3 is the controlled experiment.

---

## 12. Multi-agent execution plan

Per `feedback_parallel_agents_for_tests` and `feedback_minimize_builds`
/ `feedback_no_parallel_builds`, the execution plan fans out to
multiple agents in some places and serialises them in others. The
diagram below is the master plan; individual agents receive
self-contained briefs (file paths, line numbers, the schema in
§3.3.3) without needing to read this whole document.

```
TIME →

Phase 0  ┌──────────┐
         │ schema-  │   serialised — schema must freeze before any
         │ owner    │   downstream agent can start
         └────┬─────┘
              │
Phase 1   ┌───┴───────────────────────────────────────────────┐
          │ Agent A: skeleton + gtopt reader + classifier     │  (P1.0,A,C,E,H)
          ├───────────────────────────────────────────────────┤
          │ Agent B: feed reader + reconstruction §4.7        │  (P1.B,D)
          ├───────────────────────────────────────────────────┤
          │ Agent C: writer/report + ladder + recipes +       │  (P1.F,I,J,K,G)
          │          consumer API + test-suite fan-out:       │
          │   ├─ subagent C1: test_classify.py                │
          │   ├─ subagent C2: test_zones.py                   │
          │   ├─ subagent C3: test_segments.py                │
          │   ├─ subagent C4: test_io_roundtrip.py            │
          │   ├─ subagent C5: test_cli_smoke.py               │
          │   ├─ subagent C6: test_minimum_data.py            │
          │   ├─ subagent C7: test_merit_ladder.py            │
          │   └─ subagent C8: test_recipes.py                 │
          └───────────────────────────────────────────────────┘
                              │
Phase 2   ┌───┬───────────────┴───────────────────────────────┐
          │ Agent D: skeleton + CSV readers + writer + UIDs   │  (P2.0,A,C,D,F)
          ├───────────────────────────────────────────────────┤
          │ Agent E: SIP API client +  test fan-out:          │  (P2.B,E)
          │   ├─ subagent E1: test_csv_parsers.py             │
          │   ├─ subagent E2: test_sip_client.py              │
          │   ├─ subagent E3: test_topology.py                │
          │   ├─ subagent E4: test_writer.py                  │
          │   └─ subagent E5: test_cli_smoke.py               │
          └───────────────────────────────────────────────────┘
                              │
Phase 3   ┌───┴───────────┐
          │ Agent F: e2e  │   serialised — needs both phases done
          └───────────────┘
```

### 12.1 Agent briefs (one paragraph each — what to copy into each Agent prompt)

* **schema-owner (Phase 0)**. Freeze the schema in §3.3.3 of this
  plan. Land `scripts/gtopt_canonical_feed/` with `Topology` and `Cells`
  pydantic-style dataclasses, parquet read/write helpers, and a
  `manifest.json` round-trip utility. Write the gold fixture (3 buses,
  3 units, 24 hours, hand-computed expected LMPs). Do **not** edit
  `scripts/gtopt_marginal_units/` or `scripts/cen2gtopt/`. Acceptance:
  the round-trip test (`write → read → assert equal`) passes for both
  a feed *with* and *without* the optional LMP column.

* **Agent A (Phase 1, sequential within Phase 1)**. Owner of the
  Phase-1 skeleton and the classifier engine. Reads §2, §4 and §6 of
  this plan. Implements P1.0, P1.A, P1.C, P1.E, P1.H. After P1.0
  lands, signals Agents B and C to start in parallel. Always batches
  edits before building per `feedback_minimize_builds`; never builds
  in parallel with another agent per `feedback_no_parallel_builds`.

* **Agent B (Phase 1, parallel with A and C)**. Owner of the feed
  reader and the §4.7 reconstruction. Reads §3.3 and §4.7 of this
  plan and the gold fixture from Phase 0. Implements P1.B and P1.D.
  Writes integration tests against the gold fixture; does not need
  any IEEE benchmark output until P1.D is signed off.

* **Agent C (Phase 1, parallel with A and B)**. Owner of the output
  parquet-dataset writer (§4.6 — five tables + manifest), the
  merit-ladder construction (§4.9), the bus-price-recipe table
  (§4.6.4) with the writer-side invariant check, the
  `gtopt_marginal_units.recipes` consumer API (§4.10), the
  `--report` Markdown digest, and the unit-test fan-out. Implements
  P1.F, P1.I, P1.J, P1.K, then drives 8 sub-agents (C1–C8) one per
  test file in §7.1 — fanned out per
  `feedback_parallel_agents_for_tests`. Each sub-agent receives a
  one-paragraph brief naming its target file and the rule rows it
  must cover.

* **Agent D (Phase 2, sequential within Phase 2)**. Owner of
  `cen2gtopt` skeleton, CSV readers, writer, UID assignment.
  Implements P2.0, P2.A, P2.C, P2.D, P2.F. Reads §9 of this plan and
  the schema from Phase 0. Never imports from
  `gtopt_marginal_units/`; only depends on `gtopt_canonical_feed/`.

* **Agent E (Phase 2, parallel with D)**. Owner of the SIP API client
  and the Phase-2 test fan-out. Reads §9.2.2 of this plan and the
  CEN SIP v1.1 PDF fixture. Implements P2.B; drives 5 sub-agents
  (E1–E5) one per test file in §9.6. Network-dependent integration
  tests are gated behind `GTOPT_RUN_CEN_BACKTEST=1` and excluded from
  default CI.

* **Agent F (Phase 3, serialised)**. Owner of end-to-end integration.
  Runs `cen2gtopt` against the §7.5 one-week SEN snapshot fixture,
  pipes into `gtopt_marginal_units --input-kind feed-parquet --mode
  real-reconstruct`, asserts the §7.5 quality bars, writes the
  user-facing workflow doc.

### 12.2 Coordination rules

1. **No two agents build at the same time** (`feedback_no_parallel_builds`).
   Each agent owns its own scratch build dir
   (`tools/mk_scratch_build.sh`); when one is building, others are
   editing only.
2. **Schema is frozen after Phase 0**. Any bug in the schema requires
   a coordinator (the user) to bump the schema version; agents never
   silently change it.
3. **No agent edits another agent's package**. Cross-package fixes go
   through a hand-off PR comment, not a direct edit.
4. **Feedback memory lookup** is mandatory for every agent: at start,
   read all `feedback_*` memories listed in `MEMORY.md` and apply
   them to its work plan.
5. **Test-suite fan-out caps at 8 sub-agents per parent agent** to
   keep CI noise tractable. (Bumped from 6 in the 2026-05-05
   storage-design revision; see §7.1 for the now-12 test files.)

---

## 13. Open questions for the user

1. **Hydro water-value source of truth**: do we read the dual from the
   `Reservoir/.../*dual*` parquet/csv (currently exported by gtopt
   only when an option is set), or compute it from cuts? v1 will
   read; tell us if you want v1 to reconstruct from cuts when the
   dual is absent.
2. **Profile (renewable) marginals**: report curtailed profile units
   as `profile_curtailed_marginal` when LMP=0 ⇒ they *are* setting
   the price, just at zero. v1 plan above treats them as
   non-marginal — confirm.
3. **CEN cross-check format**: do we want a `--cen-format` exporter
   on `gtopt_marginal_units` that matches the column layout of CEN's
   *Costo Marginal Real* download, or is the canonical feed's
   `cells/lmp.parquet` enough for Coordinador-style audits?
4. **Phase-2 storage**: should `cen2gtopt`'s on-disk cache be a
   local user dir (`~/.cache/gtopt/cen/`) or a project-relative dir
   shared by collaborators (e.g. `data/cen/`)? The latter helps team
   reproducibility but makes Git ignore rules trickier.
5. **Merit-ladder default depth**: `K=3` rungs above + below is the
   proposed default. Operators sometimes want `K=5` for full
   merit-order context; researchers may want `K=∞` for full-zone
   sensitivity. Confirm `K=3` is the right v1 default and that
   `--merit-ladder-depth` is the right knob name.
6. **Hydro/battery on the merit ladder**: v1 plan (§4.9 step 1)
   admits hydro/battery to the ladder using their declared MC or
   water-value proxy. Should units with `declared_MC=NA` and no
   water-value be (a) excluded from the ladder, (b) included with
   `available=False`, or (c) included with the zone's interior MC as
   a placeholder? The plan picks (b); confirm.
7. **Cross-zone ladder neighbours**: v1 deliberately excludes
   cross-zone units from the ladder (§10). Confirm this is the right
   default — the alternative (recompute zones for each rank) is
   substantially more expensive but better captures the "marginal
   unit trips, line clears, zones merge" cascade.

> **Decided 2026-05-05** (was open question 5, now renumbered):
> `cen2gtopt` v1 is an on-the-spot one-shot CLI only (§9.1.1,
> §9.5.1). The regular-cadence wrapper (cron / systemd / `loop`) is
> deferred to a separate small spec once v1 has been used in anger.

---

## References

[cen-home]: https://www.coordinador.cl/
[cen-costos]: https://www.coordinador.cl/costos-marginales/
[cen-real]: https://www.coordinador.cl/mercados/graficos/costos-marginales/costo-marginal-real/
[cen-decoupling]: https://www.coordinador.cl/mercados/graficos/costos-marginales/costo-marginal-real/
[cen-cmg-hub]: https://www.coordinador.cl/costos-marginales/
[cen-cmgreal-nuevo]: https://www.coordinador.cl/mercados/graficos/costos-marginales/costo-marginal-real-nuevo/
[cen-cmgreal]: https://cmgreal.coordinador.cl/
[cen-cmgprel]: https://www.coordinador.cl/mercados/graficos/descarga-datos-costos-marginales/costo-marginal-preliminar-real/
[cen-cmgonline]: https://www.coordinador.cl/mercados/graficos/costos-marginales/costo-marginal-online/
[cen-cmgonline-dl]: https://www.coordinador.cl/mercados/graficos/descarga-datos-costos-marginales/costo-marginal-en-linea-descarga-datos-costo-marginal/
[cen-gen-real]: https://www.coordinador.cl/operacion/graficos/operacion-real/generacion-real/
[cen-dem-real]: https://www.coordinador.cl/operacion/graficos/operacion-real/demanda-real/
[cen-sip-api]: https://www.coordinador.cl/api-del-sistema-de-informacion-publica/
[cen-sip-portal]: https://portal.api.coordinador.cl/documentacion?service=sipubv2
[cen-sip-pdf-2019]: https://www.coordinador.cl/wp-content/uploads/2019/01/Uso-Api-SIP-Sistema-Informacion-Publica-v1.1.pdf
[cen-rio]: https://www.coordinador.cl/operacion/documentos/registro-de-instrucciones-de-operacion-rio/
[cen-rio-sscc]: https://www.coordinador.cl/operacion/documentos/registro-de-instrucciones-de-operacion-rio-sscc-energia/
[cen-novedades]: https://www.coordinador.cl/operacion/documentos/novedades-cdc/
[cen-restricciones]: https://www.coordinador.cl/parametros-operacionales/documentos/restricciones-operativas/
[cen-restricciones-tx]: https://www.coordinador.cl/operacion/documentos/estudios-para-la-seguridad-y-calidad-del-servicio/restricciones-en-el-sistema-de-transmision/
[cen-reporte]: https://www.coordinador.cl/wp-content/uploads/2024/04/CEN-ReporteArt72-15ano2023v2.pdf
[cen-indispo]: https://www.coordinador.cl/operacion/documentos/indices-de-indisponibilidad/
[cen-irf]: https://www.coordinador.cl/operacion/documentos/
[cen-emisiones]: https://www.coordinador.cl/reportes-y-estadisticas/
[lit-lin2024]: https://arxiv.org/html/2411.12104
[pjm-primer]: https://www.pjm.com/-/media/DotCom/etools/data-miner-2/marginal-emissions-primer.ashx
[pjm-5min]: https://dataminer2.pjm.com/feed/fivemin_marginal_emissions/definition
[pjm-hourly]: https://dataminer2.pjm.com/feed/hourly_marginal_emissions/definition
[pjm-emissions]: https://www.pjm.com/markets-and-operations/m/emissions
[watttime]: https://watttime.org/data-science/methodology-validation/
[watttime-moer]: https://watttime.org/wp-content/uploads/2023/11/WattTime-MOER-modeling-20221004.pdf
[miso-em]: https://help.misoenergy.org/knowledgebase/article/KA-01501/en-us
[resurety]: https://resurety.com/solutions/locational-marginal-emissions/
[impact]: https://impactcredits.com/
[cebi-guide]: https://cebi.org/wp-content/uploads/2022/11/Guide-to-Sourcing-Marginal-Emissions-Factor-Data.pdf

* [Coordinador Eléctrico Nacional — homepage][cen-home]
* [Coordinador Eléctrico Nacional — Costos Marginales][cen-costos]
* [Coordinador Eléctrico Nacional — Costo Marginal Real][cen-real]
* [Coordinador Eléctrico Nacional — Costos Marginales (canonical hub)][cen-cmg-hub]
* [Coordinador Eléctrico Nacional — Costo Marginal Real (Nuevo) — post-2024-07-15][cen-cmgreal-nuevo]
* [Coordinador Eléctrico Nacional — Costo Marginal Real (legacy `cmgreal.coordinador.cl`)][cen-cmgreal]
* [Coordinador Eléctrico Nacional — Costo Marginal Preliminar/Real (CSV+XLS bulk download)][cen-cmgprel]
* [Coordinador Eléctrico Nacional — Costo Marginal Online][cen-cmgonline]
* [Coordinador Eléctrico Nacional — Costo Marginal Online (CSV+XLS bulk download)][cen-cmgonline-dl]
* [Coordinador Eléctrico Nacional — Generación Real][cen-gen-real]
* [Coordinador Eléctrico Nacional — Demanda Real][cen-dem-real]
* [Coordinador Eléctrico Nacional — Solicitud de acceso a API del SIP][cen-sip-api]
* [Coordinador Eléctrico Nacional — Portal API documentación (sipubv2)][cen-sip-portal]
* CEN (2019), [*Documentación API Pública SIP v1.1*][cen-sip-pdf-2019]
* CNE (2021), [*Norma Técnica de Coordinación y Operación del SEN*](https://www.cne.cl/wp-content/uploads/2021/08/NT-de-Coordinacion-y-Operacion-del-SEN.pdf)
* CNE (2023), [*NT-CyO — Capítulo Declaración de Costos Variables*](https://www.cne.cl/wp-content/uploads/2023/06/Norma-Tecnica-de-Coordinacion-y-Operacion-Capitulo-sobre-la-Declaracion-de-Costos-Variables-2.pdf)
* CNE (2025), [*Norma Técnica de Seguridad y Calidad de Servicio*](https://www.cne.cl/wp-content/uploads/2025/01/NTSyCS-Ene-2025.pdf) (Operación en Isla)
* CEN (2024), [*Reporte Anual Art. 72-15 Año 2023*](https://www.coordinador.cl/wp-content/uploads/2024/04/CEN-ReporteArt72-15ano2023v2.pdf)
* Colbún (2023), [*Cómo se definen los costos de generación en el sistema eléctrico chileno*](https://www.colbun.cl/corporativo/sala-de-prensa/newsletter/suministradores/detalle/como-se-definen-los-costos-de-generacion-en-el-sistema-electrico-chileno)
* PSU EME 801, [*Locational Marginal Pricing*](https://courses.ems.psu.edu/eme801/node/693)
* Hogan, Litvinov, et al. (2009), [*Locational Marginal Pricing Basics*](https://faculty.sites.iastate.edu/tesfatsi/archive/tesfatsi/LMPBasics.IEEEPESGM2009.HLLT.pdf)
* CAISO Tariff Appendix C, [*Locational Marginal Price*](https://www.caiso.com/documents/appendicesc-f-fifthreplacementcaisotariff_15-dec-10.pdf)
