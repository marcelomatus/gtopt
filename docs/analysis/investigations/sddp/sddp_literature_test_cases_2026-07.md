# SDDP/SDDiP Literature Benchmark Catalog for gtopt (2026-07)

> **Status**: benchmark-research deliverable of the SDDP cut-validity
> campaign (branch `handoff/sddp-cut-validity` @ 0e5ebc67).  Surveys
> the SDDP/SDDiP literature and PSR-class tool documentation for
> validation test cases that gtopt's new stochastic framework can now
> reproduce, and catalogs them with conversion paths, published
> reference values, and a prioritized implementation plan.
>
> Companion documents: `docs/formulation/sddp-cut-validity.md`
> (cut-validity theorems; §14 references),
> `docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md`
> (SDDiP framing), `test/source/test_sddp_cut_oracle.cpp`
> (extensive-form oracle harness).

## Table of Contents

1. [gtopt capabilities under test](#1-capabilities)
2. [Existing conversion precedent in the repo](#2-precedent)
3. [Catalog A — SDDP.jl example gallery](#3-sddpjl)
4. [Catalog B — classic SDDP literature instances](#4-classic)
5. [Catalog C — SDDiP instances](#5-sddip)
6. [Catalog D — PSR / CEPEL tool cases](#6-psr)
7. [Catalog E — benchmark collections (MSPLib etc.)](#7-msplib)
8. [Prioritized top-5 implementation plan](#8-top5)
9. [Gaps: standard cases gtopt cannot yet reproduce](#9-gaps)
10. [References](#10-references)

---

<a id="1-capabilities"></a>
## 1. gtopt capabilities under test

Verified in-repo at `0e5ebc67` (file:line evidence):

| Capability | Where | What a benchmark can exercise |
|---|---|---|
| `cut_sharing = multicut` | `include/gtopt/sddp_enums.hpp:202` — N α columns per scene-LP, each bounded only by its own scenario's cuts, priced `w_r = p_s` (theorems M1/M4; valid LB for the stagewise-resampled process) | textbook multicut SDDP vs published optima |
| `cut_sharing = markov` | `sddp_enums.hpp:208` — M α columns (one per Markov state), transition-matrix pricing; needs `SddpOptions::markov_states` + `markov_transition` (`include/gtopt/sddp_options.hpp:62,68`) | Markovian policy-graph cases |
| `forward_sampling_mode = resampled` | `sddp_enums.hpp:350` — probability-weighted re-draw at each phase boundary, deterministic seed | textbook SDDP forward pass; statistical UB estimators |
| AR(1) inflow states | `include/gtopt/flow.hpp:55-56,116-118` (`inflow_model`, `type = "ar1"`, `phi`); LP recursion row `q_b − phi·lag = mu_b − phi·mu_ref` (`include/gtopt/flow_lp.hpp:49`); lagged inflow is a cut state | AR-inflow hydro-thermal cases (PAR(p) designed but punted) |
| `integer_cuts = strengthened` | `sddp_enums.hpp:296` — LP-relaxation cut + Lagrangian (MIP) intercept, `max(b_LP, m*)` (`include/gtopt/benders_cut.hpp:337`) | SDDiP-class MIP subproblems (commitment, integer expansion) |
| `aperture_solve_mode = dual_shared / screened` | `sddp_enums.hpp:609,612` — Infanger–Morton dual sharing with per-aperture intercept correction (Lemma AP2) | Infanger–Morton cut-sharing instances |
| Capacity expansion as SDDP state | `capainst`/`capacost` chained state columns; `extract_capacity_cuts` (`include/gtopt/sddp_capacity_cuts.hpp:98`) | OptGen-style investment master; expansion-planning cases |
| Extensive-form oracle harness | `test/source/test_sddp_cut_oracle.cpp` (tail-LP/MIP oracle patterns) | doctest-tier exact validation for tiny instances |
| e2e golden framework | `integration_test/CMakeLists.txt` `add_sddp_case` (`cmake/add_sddp_case.cmake`), golden JSONs at iter 0/3/50 per solver in `cases/sddp_hydro_3phase/` | integration-tier objective/LB pinning |

Not available (relevant to case selection; see §9): PAR(p) inflows,
risk measures (CVaR/nested risk), objective/price states
(SDDP.jl-style objective states), pure Lagrangian/binarized SDDiP
cuts, cyclic/infinite-horizon policy graphs.

<a id="2-precedent"></a>
## 2. Existing conversion precedent in the repo

**Key finding: every existing "literature" case in `cases/` is a
_deterministic linearization_, not a stochastic reproduction.** The
new framework is what makes true stochastic reproductions possible;
no case yet pins a published *stochastic* optimum.

- `cases/hydro_thermal_sddpjl/` — SDDP.jl `Hydro_thermal.jl`
  (unicyclic graph, 3 nodes, ρ = 0.95) collapsed to a
  single-stage 3-block deterministic LP using the worst-case noise;
  analytical optimum 21.111… pinned by
  `e2e_hydro_thermal_sddpjl_compare_solution` and
  `test/source/test_hydro_thermal_benchmark.cpp`.  README documents
  the `flow_conversion_rate = 3.6` unit subtlety that any SDDP.jl
  translation must respect.
- `cases/hydro_valley_sddpjl/` — SDDP.jl `hydro_valley.jl`
  2-reservoir cascade, deterministic, first PWL segment only
  (`production_factor = 1.1`); analytical optimum 0.
- `cases/psri_case2_via_sddp2gtopt/`, `psri_case3_...` — PSR
  multi-system samples vendored from `PSRClassesInterface.jl`
  (MPL-2.0) converted by `scripts/sddp2gtopt/`; v0 converter drops
  the `PSRFuel.Custo × efficiency` cost path so goldens pin
  `obj = 0` (topology validation only).
- `cases/sddp_hydro_3phase/` — the in-house SDDP e2e fixture with
  per-solver golden JSONs at iterations 0/3/50 (+hotstart), the
  model for how a literature case would be gated.

**Converter state** (`scripts/sddp2gtopt/`): reads PSR
`psrclasses.json` (v0) and legacy `.dat` cases (`dat_loader.py`,
`dat_parsers.py` — v1 tier is present), reuses `plp2gtopt` writers.
Explicitly **out of scope: SDDP.jl `.sof.json`** (DESIGN.md §1) —
SDDP.jl examples are hand-built as gtopt JSON, not converted.
plp2gtopt estimates AR(1) `phi` from historic inflows
(`inflow_model` emission).  No converter exists for raw
academic-instance formats (SMPS, MSPLib).

<a id="3-sddpjl"></a>
## 3. Catalog A — SDDP.jl example gallery

SDDP.jl (odow/SDDP.jl, MIT) ships every example with an executable
`Test.@test` assertion on `SDDP.calculate_bound(model)` — these are
the best-quality published reference values available anywhere in
the SDDP world (exact, tolerance-annotated, CI-enforced upstream).
All examples below verified 2026-07-08 from
`https://github.com/odow/SDDP.jl/tree/master/docs/src/examples` and
`https://sddp.dev/stable/`.

### A1. First-steps hydrothermal (3-stage, stagewise independent)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl "An introduction to SDDP.jl" tutorial, <https://sddp.dev/stable/tutorial/first_steps/> |
| What it validates | `multicut` + `resampled` end-to-end: LB convergence to the true optimum of a tiny finite process |
| Published values | LB converges to **8333.33** (10 iterations shown in docs); 200 MWh reservoir (initially full), demand 150 MWh/stage, fuel cost [50, 100, 150] $/MWh, inflows {0, 50, 100} w.p. 1/3 |
| Conversion path | Hand-build gtopt JSON (Reservoir + Turbine + Generator + Demand + 3 Flow apertures per phase); mind `flow_conversion_rate = 3.6` (see `cases/hydro_thermal_sddpjl/README.md` for the unit algebra) |
| Size/effort | S — 1–2 days |
| Test tier | doctest oracle (extensive form is 3^2 = 9 leaf paths — exact) + integration golden |

### A2. Markovian policy-graph hydrothermal (3-stage, wet/dry chain)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl "Markovian policy graphs" tutorial, <https://sddp.dev/stable/tutorial/markov_uncertainty/> |
| What it validates | **`cut_sharing = markov`** (`markov_states` + `markov_transition`); transition-matrix cut pricing `w = p_s·P[m(s)][m']/π` |
| Published values | Best bound **8072.917** (docs training output, 40 iterations); transition matrices `[1.0]`, `[0.75 0.25]`, `[0.75 0.25; 0.25 0.75]`; wet-state inflow probs [1/6, 1/3, 1/2] over {0, 50, 100}, dry-state reversed; same physical system as A1 |
| Conversion path | Hand-build JSON; scenes = (Markov state × inflow) pairs, `markov_states` maps scene → state, `markov_transition` = the 2×2 matrix |
| Size/effort | S/M — 2–3 days (first markov-mode fixture; needs care that node (1,1) is deterministic) |
| Test tier | doctest oracle (extensive form 5-node graph is tiny) + integration golden pinning LB ≈ 8072.917 |

### A3. FAST hydro-thermal (2-stage max-sense LP)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl `FAST_hydro_thermal.jl` (from the FAST toolbox test set), <https://sddp.dev/stable/examples/FAST_hydro_thermal/> |
| What it validates | multicut on a 2-stage problem where LB is **exactly** the deterministic-equivalent optimum; upstream asserts `objective_value(det) == -10` and `SDDP.calculate_bound(model) == -10` (equality, no tolerance) |
| Published values | **−10** exact (max sense); storage x ∈ [0, 8], x₀ = 0, demand p + y ≥ 6, stage-2 rainfall {2, 10} equiprobable, objective −5·p |
| Conversion path | Hand-build JSON; note gtopt is min-sense — negate. `cases/fast_hydro_thermal/` already holds a deterministic variant to extend |
| Size/effort | S — 1 day |
| Test tier | doctest oracle (2-stage, 2 leaves — exact equality) |

### A4. Hydro valley variants (stagewise inflows × Markov prices)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl `hydro_valley.jl`, <https://sddp.dev/stable/examples/hydro_valley/> |
| What it validates | ladder of features on ONE physical system: deterministic → stagewise-independent inflows (`multicut`+`resampled`) → Markov prices (`markov`) → both |
| Published values | upstream `@test` bounds: deterministic **835.0** (±0.001); stagewise inflows **838.33** (±0.01); Markov prices **851.8** (±0.01); both **855.0** (±1.0); (risk-averse EAVaR 828.157 and DRO 836.695 variants are §9 gaps) |
| Conversion path | extend `cases/hydro_valley_sddpjl/` (deterministic linearization exists; README documents element mapping); add inflow apertures, then Markov price states. **Caveat**: Markov *prices* enter the stage objective, not the RHS — gtopt models Markov states via scenes, so price-per-Markov-state needs scene-indexed `gcost`/price profiles; verify gtopt can vary objective coefficients per scene before committing |
| Size/effort | M — 3–5 days for the full ladder |
| Test tier | integration golden per variant (one `add_sddp_case` each) |

### A5. Generation expansion (5-stage, integer investment state)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl `generation_expansion.jl`, <https://github.com/odow/SDDP.jl/blob/master/docs/src/examples/generation_expansion.jl> |
| What it validates | **capacity expansion as SDDP state + `integer_cuts = strengthened`** together: binary `invested[1:5]` state variables, "can't un-invest" chaining, demand noise (8 scenarios/stage), unmet-demand penalty 5e5 |
| Published values | `@test SDDP.calculate_bound(model) ≈ 2.078860e6 atol = 1e3` — upstream runs BOTH `ContinuousConicDuality` (LP-relaxation cuts) and `LagrangianDuality`; both reach the same bound, so **strengthened cuts suffice** — exactly gtopt's regime |
| Conversion path | Hand-build JSON: `integer_expmod` expansion modules (invest cost 1e4, gen cost 4) + Demand with 8 apertures; `capainst` state chaining is native |
| Size/effort | M — 3–4 days |
| Test tier | doctest MIP-oracle (pattern from `test/source/test_sddp_strengthened_cuts.cpp`) + integration golden LB 2.07886e6 ± 1e3 |

### A6. Air conditioning (3-stage integer production planning)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl `air_conditioning.jl`, <https://github.com/odow/SDDP.jl/blob/master/docs/src/examples/air_conditioning.jl> |
| What it validates | `integer_cuts = strengthened` on the smallest classic integer-recourse instance; integer state (`stored_production`) AND integer control (`production`) |
| Published values | `@test isapprox(lb, 62_500.0, atol = 0.1)`; first-stage optimal production 200, storage 100; demand 100 then {100, 300} equiprobable; costs 100/300/50 (production/overtime/storage). Upstream passes with both duality handlers → strengthened-only is enough |
| Conversion path | Hand-build; storage state maps to a Battery-like element or a bare state column via the oracle-harness fixture style (doctest-tier JSON, no physical network needed) |
| Size/effort | S — 1–2 days |
| Test tier | doctest MIP-oracle (extensive form 1×2×2 = 4 leaves — exact) |

### A7. Asset management (Birge & Louveaux, 4-stage Markov)

| Field | Content |
|---|---|
| Case + citation | SDDP.jl `asset_management_simple.jl`; original: Birge & Louveaux, *Introduction to Stochastic Programming*, Springer (2011) |
| What it validates | `markov` mode against a *textbook* (not just SDDP.jl) published optimum: 2-state Markov chain, transition `[0.5 0.5]`, stocks/bonds returns (1.25, 1.14)/(1.06, 1.12), target 80, initial 55 |
| Published values | `@test SDDP.calculate_bound(model) ≈ 1.514 atol = 1e-4` (matches Birge & Louveaux's published solution) |
| Conversion path | Hand-build; abstract (non-power) — needs bare-state fixture columns, doable with the oracle-harness style but stretches gtopt's element vocabulary (returns = state-dependent production factors). Consider only if a non-power fixture idiom is desired |
| Size/effort | M — 3 days (element-vocabulary stretch) |
| Test tier | doctest oracle |

### A8. Other SDDP.jl examples (lower priority, quick reference)

| Example | Bound (`@test`) | gtopt fit |
|---|---|---|
| `FAST_production_management.jl` | ≈ **−23.96** (atol 1e-2) | multicut; abstract inventory — S |
| `FAST_quickstart.jl` | (trivial 2-stage) | smoke only |
| `StochDynamicProgramming.jl_multistock.jl` | ≈ **−4.349** (atol 0.01), 5 stages, 27 noise/stage | multicut stress (3 coupled states) — M |
| `StructDualDynProg.jl_prob5.2_2stages.jl` | ≈ **340315.52** (atol 0.1) | 2-stage expansion LP; capacity-cut check — S |
| `stochastic_all_blacks.jl` | ≈ **8.0** (LagrangianDuality) | binary state; needs pure Lagrangian cuts — likely BEYOND strengthened-only (§9) |
| `sldp_example_one.jl` (Ahmed–Cabral–da Costa SLDP §4.2) | bound ≤ **1.1675** | nonconvex Lipschitz — out of scope (§9) |
| `Hydro_thermal.jl` | best bound ≈ **236.43** (docs output) | **cyclic/infinite-horizon graph (ρ = 0.95)** — gtopt has no cyclic policy graphs (§9); the repo's `cases/hydro_thermal_sddpjl` deterministic linearization stays the coverage for this one |
| `infinite_horizon_hydro_thermal.jl` | ≈ **119.167** (atol 0.1) | same cyclic gap (§9) |
| `objective_state_newsvendor.jl` | (price/objective states) | objective states unsupported (§9) |
| `biobjective_hydro.jl` | (weighted sweep) | out of scope |

**Feature-mapping note**: SDDP.jl's default forward pass is Monte
Carlo sampling of the policy graph ≙ gtopt
`forward_sampling_mode = resampled`; SDDP.jl "Markovian policy
graph" ≙ gtopt `cut_sharing = markov` with scene→state map +
transition matrix; SDDP.jl *objective/price states* (interpolated
price processes) have **no** gtopt equivalent — gtopt's AR(1) is on
the RHS (inflows), not on objective coefficients.

<a id="4-classic"></a>
## 4. Catalog B — classic SDDP literature instances

### B1. Pereira & Pinto (1985/1991) — the founding instances

| Field | Content |
|---|---|
| Case + citation | Pereira & Pinto, *Water Resources Research* 21(6):779–792 (1985), DOI 10.1029/WR021i006p00779 (37-reservoir Brazilian case study); Pereira & Pinto, *Math. Programming* 52:359–375 (1991), DOI 10.1007/BF01582895 (39-reservoir application) — already ref [1] of `docs/formulation/sddp-cut-validity.md` §14 |
| What it validates | historical anchor only — multicut + resampled |
| Published values | **none reproducible** — the papers report aggregate case-study results for the Brazilian system; no instance data was ever archived publicly |
| Conversion path | none; the *lineage* survives as the aggregate 4-reservoir Brazilian system (see B3/E2), which is the community's de-facto "Pereira–Pinto instance" |
| Size/effort | n/a |
| Test tier | research-only (citation anchor) |

### B2. Infanger & Morton (1996) — cut sharing under interstage dependency

| Field | Content |
|---|---|
| Case + citation | Infanger & Morton, "Cut sharing for multistage stochastic linear programs with interstage dependency", *Math. Programming* 75:241–256 (1996), DOI 10.1007/BF02592154 |
| What it validates | `aperture_solve_mode = dual_shared / screened` — gtopt's implementation cites this mechanism directly (Lemma AP2, `docs/formulation/sddp-cut-validity.md:351`) |
| Published values | **none publicly archived** — the paper's computational section is not accompanied by downloadable instances (verified: no instance archive found via Springer/Semantic Scholar, 2026-07-08) |
| Conversion path | do NOT chase the original instances. The mechanism test is intrinsic: on any RHS-stochastic case (A1, D2, or an AR(1) fixture), a `dual_shared` cut for aperture a′ must equal the exact re-solve cut for a′ up to the intercept correction — assert equality against `aperture_solve_mode = cold` on the same seed. `screened` additionally re-solves the top `aperture_screen_count` cuts, giving a built-in self-check |
| Size/effort | S — 1–2 days on top of an existing stochastic fixture |
| Test tier | doctest oracle (cut-for-cut comparison) + integration golden (LB equality within tolerance across the three modes) |

### B3. de Matos, Philpott & Finardi (2015) — cut-selection benchmark

| Field | Content |
|---|---|
| Case + citation | "Improving the performance of Stochastic Dual Dynamic Programming", *J. Comput. Appl. Math.* 290:196–208 (2015), DOI 10.1016/j.cam.2015.04.048 |
| What it validates | multicut at scale; cut-count discipline (gtopt has no cut selection — a *negative* catalog entry) |
| Published values | Brazilian system aggregated to **4 energy-equivalent reservoirs** (158 hydro + 151 thermal underlying), 120 monthly stages; results are relative speed-ups, not reproducible optima; inflows are **PAR(p)** (§9 gap) |
| Conversion path | via the msppy-published variant of the same system (E2), which ships data |
| Size/effort | L (via E2) |
| Test tier | research-only until E2 lands |

### B4. Philpott & de Matos — DOASA / NZ mid-term scheduling

| Field | Content |
|---|---|
| Case + citation | Philpott & de Matos, "Dynamic sampling algorithms for multi-stage stochastic programs with risk aversion", *EJOR* 218(2):470–483 (2012); DOASA overview at <https://www.epoc.org.nz/publications.html> |
| What it validates | would exercise markov (inflow regimes) + risk aversion (§9 gap) |
| Published values | none public — NZEM instance data (9 reservoirs, weekly stages) is not distributed |
| Conversion path | none available |
| Size/effort | n/a |
| Test tier | research-only |

### B5. Shapiro, Tekaya, da Costa & Soares (2013) — risk-neutral baseline

| Field | Content |
|---|---|
| Case + citation | "Risk neutral and risk averse Stochastic Dual Dynamic Programming method", *EJOR* 224(2):375–391 (2013), DOI 10.1016/j.ejor.2012.08.022 |
| What it validates | risk-neutral SDDP LB/statistical-UB methodology on the Brazilian interconnected system (the data lineage behind E2) |
| Published values | in-paper bounds for the 120-stage aggregate system; the **reproducible published derivative is msppy's Table 3** (see E2) which reruns this exact system with shipped data |
| Conversion path | via E2 |
| Size/effort | (folded into E2) |
| Test tier | research-only directly; integration via E2 |

<a id="5-sddip"></a>
## 5. Catalog C — SDDiP instances

### C1. Zou, Ahmed & Sun (2019) — SGEP generation expansion

| Field | Content |
|---|---|
| Case + citation | Zou, Ahmed & Sun, "Stochastic dual dynamic integer programming", *Math. Programming* 175:461–502 (2019), DOI 10.1007/s10107-018-1249-5; preprint <https://mitsloan.mit.edu/shared/ods/documents?PublicationDocumentID=9333> (extracted 2026-07-08); generator data from Jin, Ryan, Watson & Woodruff, *Energy Systems* (2011) [their ref 45] |
| What it validates | `integer_cuts = strengthened` + capacity-expansion state, on THE instance class that motivated strengthened Benders cuts |
| Published values | base instance: 10 stages, 3 realizations/stage (3⁹ = 19,683 scenarios), 6 generator types (Coal, CC, CT, Nuclear, Wind, IGCC); extensive-form CPLEX: incumbent **7056.7**, best bound **6551.6** (7.16% gap, 2 h); SDDiP LB stabilizes at **6701.1 $MM** under every convergent cut combo (SB+I, B+L, SB+L, SB+I+L — paper Tables 1–2). Scalability set (T=5..9, 30–50 branches): LB **2246.4 / 2818.9 / 3564.5 / 4159.4 / 5058.0–5058.9 $MM**. Key quote (p.18): *"the optimal Lagrangian dual multipliers do not deviate much from the LP dual optimal in these instances. Therefore, strengthened Benders' cuts and Lagrangian cuts are 'similar'"* — i.e. **strengthened-only (gtopt's regime) suffices on SGEP** |
| Conversion path | instance data NOT archived; demand/gas-price processes described (uniform demand, truncated-normal gas price, stagewise independent) — regenerate a *gtopt-flavored* SGEP from the paper's template + Jin et al. cost data; pin small variants (T≤4, ≤3 branches) against gtopt's own extensive-form oracle, treat 6701.1 as a plausibility target, not a pin. Caveats: (i) the paper binarizes the cumulative-build state (48 binaries/stage) — gtopt keeps a continuous `capainst` state with integer local `expmod`, so cuts differ; only LB validity + gap size are comparable; (ii) gtopt has no integer-optimality (I) cuts — expect SB-alone behavior |
| Size/effort | M — 4–6 days (data reconstruction dominates) |
| Test tier | doctest MIP-oracle (small T) + integration golden (LB stability + gap ≤ few % on a mid-size regeneration) |

### C2. msppy MIP power-system variant (thermal security constraint)

| Field | Content |
|---|---|
| Case + citation | Ding, Ahmed & Shapiro, "A Python package for multi-stage stochastic programming", Optimization Online (2019), <https://optimization-online.org/wp-content/uploads/2019/05/7199.pdf>, §16.1 Table 6; code/data: <https://github.com/lingquant/msppy> |
| What it validates | `integer_cuts = strengthened` on a *hydro-thermal* MIP (binary `z_i`: stored energy below threshold forces thermal minimum) — closer to gtopt's domain than SGEP |
| Published values | Table 6 (TS discretization, 100 samples/stage): **T=2: LB = UB = 1,623,203** (extensive = SDDiP-B, exact); **T=3: extensive LB 3,078,162 / UB 3,084,143 (0.19%)**, SDDiP-SB LB **2,998,418** (2.53% gap), SDDiP-Lagrangian **2,999,141** (2.51%) — strengthened ≈ Lagrangian here too; T=6: SDDiP-SB LB **4,946,515**, gap 2.22% |
| Conversion path | msppy ships the Brazilian data; hand-build the 4-reservoir gtopt JSON (Reservoir/Generator/Line per region + deficit tiers) + `z_i` via commitment-style binaries; T=2/3 are extensive-form-oracle checkable inside gtopt itself |
| Size/effort | M — 4–5 days (shares system build with E2) |
| Test tier | doctest MIP-oracle (T=2 exact; T=3 gap-bounded) — the natural "next" fixture after `test_sddp_strengthened_cuts.cpp` |
| Expectation-setting | SB does NOT close the MIP gap (2.5% at T=3 in the paper) — gtopt tests must assert `LB_strengthened ≥ LB_relaxed` and `LB ≤ V_MIP` (oracle), never LB = optimum |

### C3. Zou–Ahmed–Sun portfolio & airline instances; later SDDiP literature

| Field | Content |
|---|---|
| Case + citation | same paper §6.2–6.3; Chen & Luedtke, "On generating Lagrangian cuts", *INFORMS J. Computing* (2022); Füllner & Rebennack, "Stochastic dual dynamic programming and its variants — a review", *SIAM Review* 67(1) (2025), <https://epubs.siam.org/doi/full/10.1137/23M1575093> |
| What it validates | little for gtopt: portfolio needs binarized continuous states + transaction structure; airline revenue management needs 72-dim Markovian NHPP arrivals; both stray far from gtopt's element vocabulary. Chen–Luedtke instances need pure Lagrangian-cut machinery (§9) |
| Published values | in-paper tables (not extracted here — not actionable) |
| Conversion path / tier | skip; research-only. The Füllner–Rebennack survey is the right index if a second SDDiP family is wanted later |

<a id="6-psr"></a>
## 6. Catalog D — PSR / CEPEL tool cases

### D1. PSRClassesInterface.jl sample cases (already partially converted)

| Field | Content |
|---|---|
| Case + citation | `psrenergy/PSRClassesInterface.jl` test data (MPL-2.0), vendored at `scripts/sddp2gtopt/tests/data/psri_case*/` |
| What it validates | conversion fidelity (`sddp2gtopt`), multi-system topology; today obj = 0 only (v0 drops `PSRFuel.Custo × efficiency` — `cases/psri_case2_via_sddp2gtopt/README.md`) |
| Published values | none (fixtures, not benchmarks) |
| Conversion path | exists (`scripts/sddp2gtopt/`, JSON tier v0 + `.dat` tier); **the actionable step is wiring fuel costs**, which turns the goldens into non-trivial objective pins and unlocks any real PSR case |
| Size/effort | S/M — 2–4 days (converter work, not case work) |
| Test tier | existing e2e goldens regenerate; add stochastic variant once inflow scenarios are wired |

### D2. PSR-manual-style single-bus hydrothermal (hand-built)

| Field | Content |
|---|---|
| Case + citation | PSR SDDP user documentation & methodology notes, <https://www.psr-inc.com/en/software/sddp/> (installer ships "sample cases and example tours"; manual PDF links rot — direct fetch of `SddpUsrEng.pdf` returned 404 on 2026-07-08, **unverified**) |
| What it validates | the PLP-faithful `multicut` semantics on PSR's canonical shape: 1 bus, few thermal + 1 hydro, N inflow scenarios/stage, deficit cost segments |
| Published values | none published openly; PSR sample-case results ship inside the installer (license-encumbered — do not vendor) |
| Conversion path | hand-build a "PSR-style" case: it is exactly the shape of gtopt's `cases/sddp_hydro_3phase` grown to N apertures + deficit segments; certify with the in-repo extensive-form oracle rather than external numbers |
| Size/effort | S — 2 days |
| Test tier | doctest oracle + `add_sddp_case` golden (self-certified) |

### D3. NEWAVE / CEPEL PAR(p) methodology cases

| Field | Content |
|---|---|
| Case + citation | CEPEL NEWAVE methodology; Maceira et al., "Twenty Years of Application of Stochastic Dual Dynamic Programming in Official and Agent Studies in Brazil — Main Features and Improvements on the NEWAVE Model", PSCC (2018), <https://www.researchgate.net/publication/327332350>; PAR(p) structure: Pereira/CEPEL PAR(p) with log-normal residuals, 4 energy-equivalent subsystems |
| What it validates | would validate PAR(p) inflows — **gtopt has AR(1) only** (`include/gtopt/flow.hpp:55` — "only \"ar1\" is supported") |
| Published values | official ONS/CCEE monthly PMO decks are public (Brazilian data portals) but require NEWAVE deck parsers; no simple pinned optimum |
| Conversion path | none today; needs (a) PAR(p) per-month φ vectors in `InflowModel`, (b) a NEWAVE deck reader — both future work (§9) |
| Size/effort | L — weeks |
| Test tier | research-only; catalog the AR(1)-coverable subset: any *single-φ annualized* approximation of a NEWAVE subsystem is D2/E2-shaped and coverable now |

<a id="7-msplib"></a>
## 7. Catalog E — benchmark collections

### E1. MSPLib (Seranilla & Löhndorf) — MSPFormat instances

| Field | Content |
|---|---|
| Case + citation | <https://github.com/bonnkleiford/MSPLib-Library> — "library of multistage stochastic programming problems to measure the computational performance of different implementations of SDDP … used to test MSPPy, QUASAR, and SDDP.jl"; 19 problem tarballs in MSPFormat (JSON) incl. Problem 01 = the simplified 3-stage hydrothermal scheduling problem |
| What it validates | multicut + resampled on neutral third-party instances; MSPFormat JSON is machine-readable (problem file + lattice file), so a small `msplib2gtopt` shim is feasible |
| Published values | per-instance bounds plots/first-stage solutions ship in the tarballs (per README naming convention); SDDP.jl can read MSPFormat natively (`SDDP.MSPFormat`; see SDDP.jl release notes — lagged-state MSPFormat fix, <https://github.com/odow/SDDP.jl/releases>) giving an independent cross-solver referee for any instance we adopt |
| Conversion path | new small converter (MSPFormat JSON → gtopt JSON) for the LP subset; the lattice file maps to phases/apertures; lagged states map to AR(1) states only when lag = 1 |
| Size/effort | M — 3–5 days for the shim + first instance |
| Test tier | integration golden, cross-checked against SDDP.jl's bound on the same instance |

### E2. msppy Brazilian interconnected system (Shapiro-lineage, data shipped)

| Field | Content |
|---|---|
| Case + citation | Ding, Ahmed & Shapiro (2019) §16.1 (URL in C2); data in `lingquant/msppy` (GitHub); system per Shapiro et al. (2013) |
| What it validates | the flagship AR-inflow case: 4 energy-equivalent reservoirs, 120 monthly stages, discount γ = 0.9906; **TS discretization ≙ gtopt AR(1) states**; **Markov-chain discretization (SA/RSA/SAA) ≙ gtopt `markov` mode** — one system exercising BOTH new state models |
| Published values | paper Table 3 deterministic LBs ($M): SA50 **185.0**, SA100 **186.7**, RSA50 181.3, RSA100 184.2, SAA50 177.2, TS50 **186.2**, TS100 **188.6**; policy-value CIs on the true problem ≈ 223–242 $M (Table 5). Gaps are 10–27% — these are *ranges*, not pins |
| Conversion path | hand-build the 4-node system (bounded effort — the LP is small; stages are many); inflows: fit gtopt AR(1) per reservoir from msppy's shipped historical data (plp2gtopt's estimator is the precedent); Markov variant needs a K-state chain fitted offline, then `markov_states`/`markov_transition` |
| Size/effort | L — 1–2 weeks all-in (but C2 shares the build) |
| Test tier | integration (LB within published band; TS-vs-Markov LB ordering) + research report. Caveats: msppy's TS is VAR(1) with cross-region correlation — gtopt AR(1) is per-Flow scalar (no cross-correlation, §9), so expect LB shift; document, don't pin |

### E3. POSTS / SMPS two-stage collections

Two-stage SMPS test sets (POSTS, SIPLIB) are not multistage-SDDP
relevant; skip.  GAMS model library `sddp.gms` (Vattenfall data,
<https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html>)
has no published optimum — usable as a smoke model only.

<a id="8-top5"></a>
## 8. Prioritized top-5 implementation plan

Ranked by validation value per unit effort; every pick has a
published number (or an exact in-repo oracle) and exercises at
least one NEW feature.

### #1 — A1: SDDP.jl first-steps hydrothermal (multicut + resampled)

- **Fixture**: `cases/sddpjl_first_steps/` — 1 bus, Reservoir
  (emax 200, eini 200), Turbine, thermal Generator with per-phase
  `gcost` [50, 100, 150], Demand 150, 3 phases × 3 apertures
  (inflow 0/50/100, p = 1/3); `method = sddp`,
  `cut_sharing = multicut`, `forward_sampling_mode = resampled`,
  fixed seed.  Watch `flow_conversion_rate` (units algebra in
  `cases/hydro_thermal_sddpjl/README.md`).
- **Assertions**: (a) doctest — extensive-form oracle
  (`test_sddp_cut_oracle.cpp` pattern, 9 leaf paths) equals SDDP
  LB at convergence; (b) integration `add_sddp_case` golden — LB =
  **8333.33** rel. tol 1e-4 at iter N (pin N per solver like
  `sddp_hydro_3phase`); (c) LB monotone nondecreasing across
  iterations.
- **Gating**: CI default solver (`GTOPT_SOLVER=clp`); goldens per
  solver as in `cases/sddp_hydro_3phase/`.
- **Effort**: S (1–2 days).  Also the substrate for #4 and B2.

### #2 — A2: SDDP.jl Markovian policy graph (markov mode)

- **Fixture**: same physical system as #1; scenes = (climate
  state, inflow) pairs; `markov_states = [.. scene→{0,1} ..]`,
  `markov_transition = [0.75, 0.25, 0.25, 0.75]`; wet/dry inflow
  probabilities [1/6, 1/3, 1/2] vs [1/2, 1/3, 1/6] over
  {0, 50, 100}; stage-1 node deterministic.
- **Assertions**: (a) doctest — extensive-form oracle over the
  5-node policy graph equals converged LB (exact; the graph has
  1 + 2·3 + (2·3)² weighted paths); (b) integration — LB ≈
  **8072.92** rel. tol 1e-3 (SDDP.jl's published training bound;
  our oracle gives the authoritative digits); (c) cross-mode sanity
  — `markov` LB ≥ `multicut` LB is NOT required (different
  processes); instead assert markov LB equals oracle.
- **Gating**: same as #1; mark `markov` experimental in the test
  name so failures triage cleanly.
- **Effort**: S/M (2–3 days).  First end-to-end certification of
  `markov` mode against an external reference.

### #3 — A5: SDDP.jl generation_expansion (integer state + strengthened cuts)

- **Fixture**: 5 phases; 5 `integer_expmod` expansion candidates
  (invest 1e4, gen cost 4, unmet-demand penalty 5e5), monotone
  build (native `capainst` chaining); Demand with 8 apertures/phase
  from the upstream `demand_vals` matrix;
  `integer_cuts = strengthened`, `cut_sharing = multicut`.
- **Assertions**: (a) doctest MIP-oracle
  (`test_sddp_strengthened_cuts.cpp` pattern) on a T=3 truncation —
  `LB ≤ V_MIP(oracle)` and `LB_strengthened ≥ LB_none`; (b)
  integration — LB ≈ **2.078860e6 abs. tol 1e3** (upstream's own
  test tolerance; upstream passes with continuous-conic AND
  Lagrangian duality, so strengthened-only should land inside);
  (c) `extract_capacity_cuts` returns cuts whose ∂V/∂capacity
  slopes price the marginal unit ≤ invest cost.
- **Gating**: needs a MIP-capable solver — gate on CBC/HiGHS
  presence (cf. MIP test blind spots: CI runs `GTOPT_SOLVER=clp`;
  register under `GTOPT_BUILD_INTEGRATION_TESTS` with a solver
  check).
- **Effort**: M (3–4 days).

### #4 — B2 + D2: Infanger–Morton dual sharing, PSR-style case

- **Fixture**: grow #1 to N = 10 apertures/phase with a deficit
  segment ladder (PSR manual shape, D2).  Run three ways:
  `aperture_solve_mode = cold`, `dual_shared`, `screened`
  (`aperture_screen_count = 3`), same seed.
- **Assertions**: (a) doctest — for a fixed backward cell, the
  `dual_shared` cut for aperture a′ equals the `cold` re-solve cut
  up to the Lemma AP2 intercept correction (coefficients equal —
  same basis; intercept ≥ cold intercept − eps, validity); (b)
  integration — final LB under all three modes within rel. 1e-6
  of each other and equal to the #1-style oracle; (c) `screened`
  re-solved cuts are ≥ their shared predecessors (tightening).
- **Gating**: CI default; pure-LP so no solver gate.
- **Effort**: S (1–2 days on top of #1).  This is the honest
  Infanger–Morton validation: the 1996 instances are not archived
  (B2), so certify the *mechanism* exactly instead.

### #5 — E2/C2: msppy Brazilian 4-reservoir system (AR(1) + markov + MIP)

- **Fixture**: 4 buses (SE/S/N/NE) + exchange Lines +
  transshipment node, deficit-cost tiers, per-region Reservoir with
  `inflow_model = {type: "ar1", phi: fitted}` from msppy's shipped
  inflow history; γ = 0.9906 discount; two horizons: T=12
  (test tier) and T=120 (research tier).
- **Assertions**: staged —
  (a) doctest MIP-oracle: the C2 thermal-security binary variant at
  T=2 pins **LB = V = 1,623,203** (paper Table 6, extensive =
  SDDiP exact) and at T=3 asserts `LB_strengthened` within 3% of
  the extensive optimum **3,078,162–3,084,143** (paper SB gap was
  2.53%);
  (b) integration T=12: self-certified goldens (LB vs in-repo
  extensive oracle on a thinned aperture set; AR(1) recursion row
  duals present in cuts);
  (c) research-only T=120: LB lands in the published Table 3 band
  (**177–189 $M**) for the TS-like configuration; Markov-fitted
  variant compared for LB ordering — document, don't gate.
- **Gating**: (a)+(b) in CI (MIP part gated on CBC/HiGHS); (c)
  manual/`-L research` label.
- **Effort**: M for (a), L overall (1–2 weeks).  Highest domain
  value: it is the community's Pereira–Pinto/Shapiro lineage
  instance, with data actually shipped.
- **Caveat**: msppy's inflows are VAR(1) (cross-region
  correlation); gtopt AR(1) is per-Flow scalar — expect a modeled
  LB shift; quantify it in the research report rather than pinning.

**Fast-follows** (not top-5 but cheap once #1–#3 exist): A3
FAST_hydro_thermal (exact −10, 1 day); A6 air_conditioning
(62,500 exact, MIP-oracle, 1–2 days); D1 sddp2gtopt fuel-cost
wiring (unlocks real PSR cases and de-trivializes the psri goldens).

<a id="9-gaps"></a>
## 9. Gaps: standard cases gtopt cannot yet reproduce

Future-work notes, with the literature case each gap blocks:

1. **PAR(p) inflows** (designed, punted — `flow.hpp` supports
   `"ar1"` only).  Blocks: NEWAVE/CEPEL official methodology cases
   (D3), de Matos–Philpott–Finardi PAR(p) benchmark (B3), faithful
   msppy TS reproduction (E2 uses periodic lag structure).  AR(1)
   covers: single-lag stationarized approximations.
2. **Cross-variable correlation in inflow states** — gtopt AR(1)
   is per-Flow scalar; msppy/Shapiro use VAR(1) with spatial
   correlation.  Blocks exact E2 reproduction.
3. **Risk measures** (CVaR/AVaR, nested; DRO) — no risk-measure
   hook in the cut/pricing path.  Blocks: hydro_valley risk-averse
   variants (EAVaR 828.157; DRO 836.695), Philpott–de Matos
   risk-averse NZ cases (B4), msppy risk-averse portfolio, Dual-SDDP
   UB literature.
4. **Objective / price states** (SDDP.jl objective states,
   interpolated price processes) — gtopt AR states live on the RHS.
   Blocks: `objective_state_newsvendor.jl`, European price-process
   examples, mid-term bid-based simulators.
5. **Cyclic / infinite-horizon policy graphs** (discounted
   unicyclic graphs).  Blocks: `Hydro_thermal.jl` (bound 236.43),
   `infinite_horizon_hydro_thermal.jl` (119.167).  gtopt phases are
   a finite chain; the repo's deterministic linearization of
   Hydro_thermal stays a partial cover.
6. **Pure Lagrangian cuts / binarization / integer-optimality
   cuts** — gtopt has strengthened-only.  Blocks:
   `stochastic_all_blacks.jl` (8.0 needs LagrangianDuality), SLDP
   examples, Chen–Luedtke Lagrangian-cut instances, exact SDDiP
   convergence claims (expect the documented 2–3% SB gap on MIP
   cases, msppy Table 6).
7. **Cut selection** (Level-1/de Matos et al.) — performance, not
   validity; blocks only the *speed* comparisons of B3.
8. **Belief / partially-observable graphs** (`belief.jl`) and
   **biobjective** (`biobjective_hydro.jl`) — out of scope.

<a id="10-references"></a>
## 10. References

1. SDDP.jl (O. Dowson et al.), examples & tutorials:
   <https://sddp.dev/stable/>; sources
   <https://github.com/odow/SDDP.jl/tree/master/docs/src/examples>.
   Individual pages cited inline (§3).  Dowson & Kapelevich,
   "SDDP.jl: a Julia package for stochastic dual dynamic
   programming", *INFORMS J. Computing* 33(1) (2021).
2. M. V. F. Pereira, L. M. V. G. Pinto, *Water Resources Research*
   21(6):779–792 (1985), DOI 10.1029/WR021i006p00779.
3. M. V. F. Pereira, L. M. V. G. Pinto, *Mathematical Programming*
   52:359–375 (1991), DOI 10.1007/BF01582895.
4. G. Infanger, D. P. Morton, "Cut sharing for multistage
   stochastic linear programs with interstage dependency",
   *Mathematical Programming* 75:241–256 (1996),
   DOI 10.1007/BF02592154.
5. V. L. de Matos, A. B. Philpott, E. C. Finardi, "Improving the
   performance of Stochastic Dual Dynamic Programming", *J.
   Comput. Appl. Math.* 290:196–208 (2015).
6. A. B. Philpott, V. L. de Matos, "Dynamic sampling algorithms
   for multi-stage stochastic programs with risk aversion", *EJOR*
   218(2):470–483 (2012); EPOC publications:
   <https://www.epoc.org.nz/publications.html>.
7. A. Shapiro, W. Tekaya, J. P. da Costa, M. P. Soares, "Risk
   neutral and risk averse Stochastic Dual Dynamic Programming
   method", *EJOR* 224(2):375–391 (2013).
8. J. Zou, S. Ahmed, X. A. Sun, "Stochastic dual dynamic integer
   programming", *Mathematical Programming* 175:461–502 (2019),
   DOI 10.1007/s10107-018-1249-5; preprint
   <https://mitsloan.mit.edu/shared/ods/documents?PublicationDocumentID=9333>.
9. L. Ding, S. Ahmed, A. Shapiro, "A Python package for
   multi-stage stochastic programming" (msppy), Optimization
   Online (2019),
   <https://optimization-online.org/wp-content/uploads/2019/05/7199.pdf>;
   code/data <https://github.com/lingquant/msppy>.
10. C. Füllner, S. Rebennack, "Stochastic dual dynamic programming
    and its variants — a review", *SIAM Review* 67(1) (2025),
    <https://epubs.siam.org/doi/full/10.1137/23M1575093>.
11. MSPLib problem library:
    <https://github.com/bonnkleiford/MSPLib-Library>; B. K.
    Seranilla, PhD dissertation (Univ. Luxembourg, 2023),
    <https://orbilu.uni.lu/bitstream/10993/58728/1>.
12. S. Jin, S. M. Ryan, J.-P. Watson, D. L. Woodruff, "Modeling
    and solving a large-scale generation expansion planning problem
    under uncertainty", *Energy Systems* 2:209–242 (2011) — SGEP
    generator data.
13. PSR SDDP product documentation:
    <https://www.psr-inc.com/en/software/sddp/>;
    PSRClassesInterface.jl sample data:
    <https://github.com/psrenergy/PSRClassesInterface.jl>.
14. M. E. P. Maceira et al., "Twenty Years of Application of
    Stochastic Dual Dynamic Programming in Official and Agent
    Studies in Brazil — Main Features and Improvements on the
    NEWAVE Model", PSCC (2018).
15. GAMS model library, `sddp.gms`:
    <https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html>.
16. J. R. Birge, F. Louveaux, *Introduction to Stochastic
    Programming*, 2nd ed., Springer (2011) — asset-management
    Markov instance (A7).
17. S. Ahmed, F. G. Cabral, B. F. P. da Costa, "Stochastic
    Lipschitz dynamic programming", *Mathematical Programming*
    191:755–793 (2022) — SLDP examples (§9 gap).

All external claims above were fetched and verified 2026-07-08
except where marked **unverified** (PSR manual PDF, B2 instance
non-existence is a negative claim — best-effort search).
