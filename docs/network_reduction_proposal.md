# Automatic Network Reduction & Disaggregation for gtopt

> **v1 implementation landed**: see `scripts/gtopt_reduce_network/`.
> CLI: `gtopt-net reduce | project-results | project-investment | cascade-run`.
> Integration-tested against IEEE-57 (gated on `GTOPT_BIN`).
>
> **v2 cascade integration landed (2026-05-14)**:
> - C++ — `CascadeLevel.system_file` field
>   (`include/gtopt/cascade_options.hpp:152`,
>   `source/cascade_method.cpp:118–166`) lets any cascade level load its
>   own `system` block from an external JSON. The level's own
>   `model_options` stay authoritative; the loaded JSON's `options` are
>   deliberately ignored. State variables and Benders cuts survive the
>   swap (keyed by `(class_name, col_name, uid)`, not bus uid).
> - plp2gtopt — new `--method cascade-reduced` (plus 10 tuning flags)
>   drives the reducer twice on the in-memory planning to produce
>   `<case>.L1.*` (ONB/6 transport-only) and `<case>.L2.*` (ONB/3
>   Kirchhoff + per-demand `lossfactor` uplift) artefacts alongside the
>   main JSON, and writes a 4-level cascade pointing each reduced level
>   at its file via `system_file`.
> - Uplift mode now writes a per-demand `Demand.lossfactor` scalar rather
>   than scaling `lmax` — parquet schedule files are never touched.
>
> The rest of this document remains the design reference.

> **Status**: design proposal, v1 implemented.
> **Scope**: a *generic*, case-agnostic preprocessing tool that takes any gtopt
> JSON case, produces an electrically-equivalent reduced case at a target bus
> count K, and provides the inverse mapping needed to project capacity-expansion
> investments and dispatch results back onto the original nodal grid.
>
> **Two motivating workloads**:
> 1. **Cascade dispatch / SDDP** — insert a `reduced(K)` level between
>    `uninodal` (1 bus) and `full_network` (every original bus) so per-iteration
>    cost grows gracefully instead of jumping ~50× (juan: 25 s → 20 min).
> 2. **Capacity-expansion studies** — invest at the cluster level, then
>    disaggregate the investment back to physical buses for downstream
>    operational simulation, dispatch reporting, and AC validation.

---

## 1. Motivating example: the juan case (Chilean SEN)

Used here only as a sizing reference; the tool itself is case-agnostic.

| Metric | Value |
|---|---|
| Buses | **236** |
| Lines | **330** |
| Voltage levels | 66 / 100 / 110 / 154 / 220 / 345 / 500 kV |
| 500 kV backbone | **26 lines**, ~14 substations (Kimal, LosChangos, Parinas, Cumbre, NvaCardones, NvaMaitenc, NvaPAzucar, Polpaico, LoAguirre, AJahuel, Ancoa, Ancoa500AuxS, EntreRios, Charrua) |
| 220 kV mesh | 206 lines (the bulk) |
| Sub-220 kV | 96 lines (66/100/110/154 kV) — mostly radial stubs into a 220 kV parent |

PLP per-block LP shape (`/tmp/plp_run/01-f1P-001-01-00.lp`):
**~7 581 rows × ~47 893 vars** for the *whole stage's* 10-block LP
(per-block ≈ 1 100 rows × 1 350 cols, see §6).

---

## 2. Two-axis attack on the slowdown

The `25 s → 20 min` jump has **two independent contributors** that should be
treated as orthogonal axes:

* **Axis A — fewer buses & lines** (this proposal's main subject). Reduces
  `N_bus`, `N_line`, `N_cycles` directly.
* **Axis B — fewer rows/cols *per* bus/line** (LP-shape simplifications). The
  LP-comparison agent (report at `/tmp/lp_comparison_juan.md`) found three
  zero-correctness-cost wins that **do not require any network reduction**:

  | Lever | Knob | Per-stage saving (juan) |
  |---|---|---|
  | Switch to cycle-basis KVL | `model_options.kirchhoff_mode: cycle_basis` | −2 360 theta cols, KVL 3 300 → 950 rows (−71%) |
  | Drop loss segments to 1 in the early cascade levels | `loss_segments: 1` | −660 cols/block (−6 600/stage) |
  | Use `piecewise_direct` for non-expansion lines | `line_losses_mode: piecewise_direct` | −330 loss cols/block |

Axis B is **already supported in the code** (`source/kirchhoff_cycle_basis.cpp`,
`source/line_losses.cpp:739`) and should be enabled in the cascade JSON
*before* this proposal lands — it's the cheapest 30–60% the user can get today
and is a legitimate baseline against which to measure Axis A.

The rest of the document focuses on Axis A but is **designed to compose with
Axis B**, so the reduced case can also be solved with `cycle_basis` + K=1
losses for compounding wins.

---

## 3. Literature & similar tools

Three families of approaches are mature and well-cited; the proposal below is
a synthesis tuned for typical GTEP-grade meshed grids.

### 3.1 Voltage-level collapse + transformer removal

`pypsa-eur/scripts/simplify_network.py` projects the European grid onto a
single voltage level (380 kV in EU), removing transformers and merging buses
that share a substation. Done first because the downstream clustering
primitives "do not work reliably with multiple voltage levels and transformers"
([PyPSA-Eur docs][pypsa-simplify]). For typical meshed grids this single pass
is the cheapest and most defensible reduction step.

### 3.2 Electrical-distance clustering

* **Cotilla-Sánchez & Hines (2013)** — multi-attribute partitioning via
  *electrical distance* (Z-bus diagonal entries); hybrid k-means + evolutionary
  refinement designed so that intra-zone transactions weakly perturb out-of-zone
  flows ([Cotilla-Sánchez & Hines][cotilla]).
* **PTDF-based zonal aggregation** (Shi & Tylavsky / Oh) — buses clustered by
  similarity of their PTDF rows; equivalent inter-zonal reactances chosen to
  minimise Euclidean distance between original and reduced inter-zonal flows
  ([Shi PhD thesis][shi-thesis], [Oh IEEE PES][oh-tep]).
* **Hierarchical agglomerative clustering (HAC)** with electrical-distance
  metric — bottom-up merge of the closest pair until target K is reached;
  produces a dendrogram so the same pipeline gives 30/50/100 reductions for
  free ([Energy Informatics 2022][einf-hac]).

### 3.3 Equivalencing methods (preserve specific quantities exactly)

* **Ward equivalent** (and *modified-Ward* for TEP) — partitions buses into
  internal/external; eliminates external buses and preserves the
  retained-line flows exactly for the operating point used to build the
  equivalent ([Oh][oh-tep], [Shi][shi-thesis]).
* **REI / modified-REI** — replaces the external system with a single
  equivalent bus + radial generators per zone; fast but distorts losses.

### 3.4 What comparable optimisation tools do

| Tool | Reduction strategy | Notes |
|---|---|---|
| **PyPSA-Eur** | `simplify_network` → `cluster_network` (k-means / HAC / greedy modularity) | Two-stage by design; `busmap` is exposed for downstream projection ([repo][pypsa-eur]) |
| **GenX** | Pure zonal model — number of zones is a modelling input | Sweet spot 30–60 zones for ISO-scale ([Lara 2025][lara-zonal]) |
| **PSR SDDP** | Configurable per study — supports nodal and zonal | Brazil ISO uses ~12 EER for hydro + hundreds of buses for transmission |
| **PLEXOS** | DC-OPF with optional zonal aggregation | Same family of decisions |

Lara et al. (2025), studying an 8 000-bus California network, find that
"well-designed small spatial aggregations can yield good approximations but
coarser zonal models result in large distortions of investment decisions"
([arXiv 2510.23586][lara-zonal]). For a 236-bus system, K = 40–60 sits
squarely in the safe regime, **provided the aggregation is electrically aware
and the disaggregation step (§5) is correct**.

---

## 4. Proposed reducer: a generic, case-agnostic algorithm

Input: any gtopt JSON case with a `bus_array` and `line_array` (and optionally
voltage tags / substation labels). Output: a reduced JSON case + a `busmap`
(original_bus → cluster_bus) + a `linemap` (original_line → reduced_line or
*intra-cluster*) + an `aggregator_table` describing how every component was
relocated. All three artefacts are needed for §5.

### Pass A — Substation collapse (topology-driven, name-free)

Name-based heuristics (split `AltoNorte220` → `AltoNorte`) are too fragile to
ship: aux buses (`Frontera220_II`, `Atacama220_BP1`, `Nogales220_aux`),
nameless or numerically-keyed datasets, and non-Chilean conventions all break
them. The substation signal is **electrical, not lexical**.

A transformer in any standard DC-OPF dataset is encoded as a line with
**resistance ≈ 0**. Looking at juan's `plpcnfli.dat`:

```
'Andes345->Andes220'         750  750  3  2  345  R=0      X=15.473  ...
'Capricorn220->Capricorn110' 100  100 10  9  220  R=0      X=62.436  ...
'Andes220->Oeste220'         290  290  2 65  220  R=3.431  X=15.802  ...   ← real line
```

The first two are transformers, the third is a transmission line. The
discriminator is `R == 0` (or, more loosely and equally robust, `X/R > 50` —
typical transformer X/R is 10–50, transmission line X/R is 0.5–2).

The substation-collapse pass therefore is:

1. Build the *transformer subgraph*: every line with `resistance == 0` (or
   `R == 0` flag from the source format), or per a configurable threshold
   `--transformer-xr-threshold` (default 50).
2. Compute connected components of that subgraph — each component is one
   physical substation.
3. Per component, pick the bus on the highest-voltage line as representative
   (voltage per bus derived from `max(line.voltage)` over incident lines —
   no name parsing).
4. Move all loads, generators, batteries, hydro attachments to the
   representative; drop the intra-component transformer lines.
5. Inter-component lines have their endpoints rewritten to the representatives.

Optional inputs that override the topology heuristic if present:

* explicit `substation` field on each bus in the source JSON;
* explicit `is_transformer` flag on each line;
* explicit `coordinates` on buses, with merging by spatial proximity
  (`< --coords-merge-radius-m`, default off).

Falls back gracefully to a no-op when neither metadata nor zero-R lines exist
— the algorithm just becomes "Pass B with no anchors merged a priori",
which is still correct, just slightly heavier per-LP.

**Important corollary**: Pass A is **strictly an optimisation**, not a
correctness requirement. Pass B's electrical-distance clustering treats two
transformer-connected buses as `d_ij ≈ 0` (the |X| is tiny relative to
real transmission lines) and **clusters them together automatically**.
The only reason to run Pass A separately is auditability — humans can
read the substation list, compare it against single-line diagrams, and
flag mistakes before the K-clustering step erases the trace.

### Pass B — Anchor + electrical-distance clustering to target K

1. **Anchor set** (must-keep buses), union of:
   * every bus on a line with `voltage ≥ --anchor-min-kv` (default 345 kV);
   * every bus carrying load > `--anchor-min-load-mw` (default ∞ → off);
   * every bus carrying generation > `--anchor-min-gen-mw` (default ∞ → off);
   * every bus listed in `--anchor-list` (manual override);
   * every bus that hosts a hydro reservoir > `--anchor-min-reservoir-mwh`.
2. **Distance metric** between non-anchor buses i, j:
   * default: shortest-path in the line graph weighted by `|X|` (line
     reactance in pu) — cheap, no Z-bus inversion;
   * optional `--distance=zbus`: full electrical distance
     `d_ij = Z_ii + Z_jj − 2 Z_ij` from the DC Z-bus;
   * optional `--distance=ptdf`: similarity of PTDF rows (recommended for
     capacity-expansion studies because it preserves what the LP cares about).
3. **Constrained clustering**: greedy/HAC with the constraint that each
   anchor seeds exactly one cluster. Non-anchor buses join the cluster of
   their nearest anchor; remaining ties broken by HAC merges of the closest
   pair until total cluster count = `K - n_anchors`.
4. Each cluster's representative bus = its anchor (if any) or the bus with
   highest weighted degree.

For a typical GTEP case the anchors are the HV backbone; for a system with
no clear voltage hierarchy the anchor set may be empty and the algorithm
degenerates to plain HAC — still well-defined.

### Pass C — Equivalent-line aggregation

For every pair of clusters (u, v):

* enumerate all original lines between any bus in u and any bus in v;
* equivalent capacity `F_uv = Σ F_ij` (sum, not max — see Shi/Oh; the slight
  over-capacity is corrected by the equivalent reactance forcing
  PTDF-correct sharing);
* equivalent reactance `X_uv` from parallel-series reduction *or*
  PTDF-fit (`min ‖PTDF_full − A · PTDF_reduced‖²`); one extra LSQ solve
  per cluster pair, cheap with numpy;
* intra-cluster lines are dropped but recorded in the linemap;
* loads, generators, hydro inflows, batteries attached to cluster members
  move to the cluster representative — the **aggregator_table** records
  `(component_id, original_bus, cluster_bus, share)` so the inverse
  projection (§5) can split them back.

### Pass D — Optional rewire

Generators of the same fuel/owner sharing a cluster can be merged or kept
separate (`--keep-units`, default *on* so the unit-commitment / piecewise-cost
layer is unchanged).

---

## 5. Inverse projection: from reduced solution back to nodal

This is the new section the user asked for. It has **two distinct flavours**
because the inverse problem differs between *capacity expansion* and
*operational dispatch*.

### 5.1 Capacity-expansion projection (investment level)

The reduced LP outputs investment `Δ_u` per cluster `u` per technology
(generation, storage, transmission). Disaggregating to physical buses uses a
**share matrix** `S` derived from one of:

| Mode | Share rule | Use case |
|---|---|---|
| `proportional_to_existing` | bus's pre-existing tech capacity / cluster total | brownfield expansion |
| `proportional_to_load` | bus's peak load / cluster total | distributed storage, demand response |
| `proportional_to_resource_quality` | per-bus capacity factor / cluster mean | VRE (solar / wind) — the most defensible for greenfield |
| `nodal_optimal_lp` | small per-cluster LP minimising injection imbalance s.t. local line caps | when nothing else applies |
| `manual` | user-supplied per-bus weights via `expansion_share.csv` | regulatory / siting constraints |

For each cluster `u` and technology `t`, the projected nodal investment is
`Δ_{i,t} = S_{u,i,t} · Δ_{u,t}`.

**Feasibility check**: after projection, run a single deterministic
full-network DC-OPF (or even just a power-flow check) on the projected
investment plan. If congestion appears that the reduced model could not
see, flag the offending clusters and either (a) split them in a finer
re-run with smaller K, or (b) tighten the reduced inter-cluster line
capacity by the AC margin and re-solve.

### 5.2 Operational-dispatch projection (LP solution level)

For each solved cell (scene, phase, block) the reduced LP gives:

* `g_u^*` — cluster-level generation per unit;
* `f_{uv}^*` — inter-cluster flows;
* `λ_u^*` — bus shadow prices (same value for every bus in a cluster);
* `q_u^*` — reservoir/battery state.

Disaggregation rules:

* **Generation**: per-unit dispatch is already nodal because we kept
  `--keep-units` on by default — no projection needed; the unit's bus
  becomes the original bus from the aggregator_table.
* **Loads**: cluster load is exact (sum of original); attribute back to
  the original bus by the recorded share.
* **Inter-cluster flows** (`f_{uv}^*`): split across the original parallel
  paths using the **PTDF of the original network** evaluated at the
  reduced-injection vector — this is a single sparse solve per (scene, phase,
  block) and recovers the right line-by-line flows in the original network
  to within the equivalencing error.
* **Intra-cluster flows**: by definition the reduced LP cannot see them.
  Recover by a per-cluster DC power-flow with cluster-level injections and
  tie-line flows as boundary conditions. Cheap.
* **Bus marginal prices (LMPs)**: the reduced-LP λ_u becomes the LMP for
  every bus in cluster u. Spatial granularity is lost; this is a fundamental
  limit of any reduction.

### 5.3 The artefact set the projector needs

The reducer must persist enough metadata to be invertible **without the
original LP** being available:

* `busmap.csv` — `original_bus, cluster_bus`
* `linemap.csv` — `original_line, reduced_line | intra_cluster, F_orig, X_orig`
* `aggregator_table.csv` — `component_id, kind, original_bus, cluster_bus, share`
* `reducer_config.json` — exact CLI / config used (target K, distance metric,
  anchor list, transformer-cap policy) so a re-run is bit-reproducible.

This is the same contract PyPSA-Eur exposes via `network.busmap` and
`network.linemap` ([PyPSA clustering][pypsa-cluster]); we adopt it directly.

---

## 6. Where to implement in this repo

The reduction belongs in **preprocessing**, not in gtopt's LP assembly.
Concretely:

* New script package `scripts/gtopt_reduce_network/` (Python; same layout as
  `scripts/plp2gtopt/`):
  * CLI: `gtopt-reduce <case.json> --target-buses K -o <case_K.json>`.
  * Inputs: a gtopt JSON case + reduction config.
  * Outputs: reduced JSON + `busmap.csv` + `linemap.csv` +
    `aggregator_table.csv` + `reducer_config.json`.
* Companion CLI in the same package:
  * `gtopt-project --busmap busmap.csv --aggregator aggregator_table.csv
    <reduced_solution.parquet> -o <projected_solution.parquet>` for
    operational results.
  * `gtopt-project-investment --share-mode proportional_to_resource_quality
    <reduced_invest.json> -o <nodal_invest.json>` for expansion results.
* The cascade driver (`source/cascade_method.cpp`) gains a third level
  `reduced(K=50)` between `uninodal` and `full_network`. Implementation: the
  cascade level loads the reduced JSON; cuts are projected up to `full_network`
  by the same busmap (the cut coefficients on cluster-state variables are
  expanded to per-bus state via the share matrix).
* No gtopt C++ changes are required for the MVP — the reducer emits a
  standalone JSON case the existing solver consumes unchanged.
* A second iteration can push the busmap into `cascade_method.cpp` so the
  same case JSON is loaded once and projected at solve time, but that's an
  optimisation, not a prerequisite.

Tests:

* Unit tests in `scripts/gtopt_reduce_network/tests/` covering each pass on
  a deterministic 9-bus IEEE input, a 14-bus IEEE input, and a synthetic
  multi-voltage-level fixture.
* Integration test: reduce → solve → project; compare against full-network
  solve on `ieee_14b` with `K = 7` (target: < 1% cost error, all anchors
  preserved).

---

## 7. Expected runtime savings

For juan (236 → 50 buses, K=1 losses, cycle-basis KVL):

* per-block LP rows: 1 800 → ~600 (down ~3×)
* per-block LP cols: 4 650 → ~1 600 (down ~3×)
* per-iteration wall time (extrapolating Pereira-Pinto's `cols^1.5`): 20 min → ~2.5 min.

For comparable industry studies:

* PyPSA-Eur reducing 5 000+ ENTSO-E nodes to 100–200 clusters reports
  roughly linear-in-rows speedup with < 5 % cost error on yearly dispatch.
* CDEC-historical Chilean studies routinely use 30–60 zone reductions for
  long-term planning.

---

## 8. Validation plan

Three layers, in increasing expense:

1. **Static**: per (stage, block), compare cluster-aggregated DC-OPF flows
   `F_uv` vs the sum-of-original-line flows on the matching cut, for a fixed
   historical dispatch. Target: < 5 % MAE on inter-zonal flows for K = 50,
   < 2 % for K = 100.
2. **Dispatch**: run a single deterministic stage end-to-end (one block, one
   scenario) with full / reduced / uninodal and compare total cost, dispatch,
   shadow prices on bus loads, and the projected (§5.2) bus prices. Target:
   cost error < 0.5 % at K = 50, projected line flows within 5 % of full.
3. **SDDP**: run the cascade with `uninodal → reduced(50) → full` and
   compare convergence vs `uninodal → full`. Target: same upper-bound value
   within MIP tolerance, faster wall time. Repeat with the Axis-B
   simplifications enabled to confirm they compose.

Benchmark cases to run validation on, in order:

* `ieee_14b` (K = 7) — sanity check, fast iteration;
* `ieee_57b` (K = 20) — first non-trivial case;
* `juan` (K = 50) — the production target.

---

## 9. Knobs the user gets

| Flag | Default | Effect |
|---|---|---|
| `--target-buses K` | 50 | desired final cluster count |
| `--anchor-min-kv` | 345 | voltage threshold for pinned buses |
| `--anchor-min-load-mw` | off | also pin buses with load > this |
| `--anchor-min-gen-mw` | off | also pin buses with gen capacity > this |
| `--anchor-min-reservoir-mwh` | off | also pin reservoir-host buses |
| `--anchor-list` | empty | manual must-keep list |
| `--distance` | `reactance-shortest-path` | also: `zbus`, `ptdf`, `geographic` |
| `--keep-transformer-caps` | off | re-emit substation transformer limits |
| `--keep-units` | on | don't merge generators at the same cluster |
| `--linemap-out` | `linemap.csv` | line-aggregation audit trail |
| `--busmap-out` | `busmap.csv` | bus-to-cluster audit trail |
| `--aggregator-out` | `aggregator_table.csv` | per-component projection table |
| `--reducer-config-out` | `reducer_config.json` | reproducibility manifest |
| `--share-mode` (projector) | `proportional_to_existing` | investment disaggregation rule |

---

## 10. Open questions for the human

* **Anchor heuristic**: 345 kV gives the right backbone for juan but is
  arbitrary for other systems. Should the default be a percentile of the
  voltage distribution (e.g. top-15 % of voltage levels) or absolute kV?
  (Default proposal: percentile, with absolute-kV override.)
* **PTDF-fit vs series-parallel for X_uv**: PTDF-fit is one extra LSQ per
  cluster pair, but is what Shi/Oh recommend. Worth it in v1?
* **Cut projection across cascade levels**: when the cascade promotes a cut
  from `reduced(K)` to `full`, the cut coefficients live on cluster state
  variables. The right projection is `cut_full_i = cut_reduced_u · S_{u,i}`
  where S is the same share matrix as for investment projection. Is this
  correct *and* tight, or do we need a per-cell re-solve to re-derive the
  cuts at the finer resolution? (Lit: typically yes for SDDP — cuts
  generated at lower fidelity remain valid (lower bounds), they're just
  loose. Worth revisiting if convergence stalls.)
* **Where in the cascade** does `reduced(K)` go? `uninodal → reduced(50) →
  full` is the obvious choice; a cheaper variant is `uninodal → reduced(50)`
  for early-stage screening studies.

[pypsa-simplify]: https://pypsa-eur.readthedocs.io/en/latest/simplification/simplify_network.html
[pypsa-eur]: https://github.com/PyPSA/pypsa-eur/blob/master/scripts/cluster_network.py
[pypsa-cluster]: https://docs.pypsa.org/v1.0.2/user-guide/clustering/
[cotilla]: https://www.semanticscholar.org/paper/Multi-Attribute-Partitioning-of-Power-Networks-on-Cotilla-S%C3%A1nchez-Hines/d0767244777a5d00e62768d507ed010e15688d19
[shi-thesis]: https://files.core.ac.uk/download/pdf/79564835.pdf
[oh-tep]: https://www.semanticscholar.org/paper/A-New-Network-Reduction-Methodology-for-Power-Oh/83caefbc587731ef9baa792e9825e691af98ba12
[einf-hac]: https://link.springer.com/article/10.1186/s42162-022-00187-7
[lara-zonal]: https://arxiv.org/abs/2510.23586
