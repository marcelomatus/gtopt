# Dual-Shared Aperture Cuts — Literature Provenance and Worked Examples

This page documents the literature basis of gtopt's
`aperture_solve_mode = dual_shared` / `screened` backward-pass modes,
maps the published methodology onto gtopt's implementation, and
describes the reconstructed test instances validated in
[`test/source/test_sddp_dual_shared_literature.cpp`](../../test/source/test_sddp_dual_shared_literature.cpp).

Cross-references:

- Theory: [`docs/formulation/sddp-cut-validity.md`](../formulation/sddp-cut-validity.md),
  §6 "Dual-shared aperture cuts (Infanger–Morton)", **Lemma AP2**.
- Implementation: `source/sddp_aperture.cpp`
  (`solve_apertures_for_phase`, `dual_shared_bound_correction`),
  `include/gtopt/sddp_enums.hpp` (`ApertureSolveMode`).

## 1. Provenance

The method descends from:

1. G. Infanger and D. P. Morton, "Cut sharing for multistage
   stochastic linear programs with interstage dependency",
   *Mathematical Programming* 75 (1996) 241–256.
   [DOI 10.1007/BF02592154](https://doi.org/10.1007/BF02592154).
   The journal article is **paywalled**; its content was recovered for
   this validation from the public technical-report-level source below.
2. D. P. Morton, *Algorithmic Advances in Stochastic Programming*,
   Stanford Systems Optimization Laboratory, Technical Report
   **SOL 93-6**, July 1993.  Full text public via OSTI:
   [osti.gov/biblio/10186619](https://www.osti.gov/biblio/10186619)
   (scanned PDF with text layer).  Chapters 2–3 contain the dual
   sharing formula, the validity lemmas, the cut-sharing recursions
   and the hydro-scheduling test-problem descriptions that the 1996
   paper refines.
3. Related listed reports (PDFs not publicly posted by the SOL
   archive): G. B. Dantzig and D. Morton, "Interstage dependency in
   multistage stochastic linear programming", SOL 94-2 (1994);
   G. Infanger, "Decomposition and (importance) sampling techniques
   for multi-stage stochastic linear programs", SOL 93-7 (1993).

### What the source actually states (facts, with locations)

From SOL 93-6 (page numbers are the report's):

- **Lemma 2.1 / Lemma 2.2** (§2.2, pp. 23–25): *feasible* dual
  vectors for the descendant subproblems generate a **valid cut** for
  the ancestor — for two-stage and, with valid cuts present in the
  descendants, for the general multistage nested case.  This is the
  weak-duality backbone; gtopt's Theorem O1 (theorem doc §3) is the
  same statement for the pinned-state column form.
- **The dual sharing formula** (§2.2, p. 26): having solved one
  stage-*t* subproblem to optimal duals `(π̄, ᾱ, μ̄)` (structural
  rows, cut rows, simple upper bounds), feasible dual prices for
  *another* stage-*t* subproblem are generated **directly, without
  solving it** — with a positive-part `[·]⁺` repair when the two
  subproblems differ in cost vectors.  When they differ only in
  RHS/simple bounds the dual point carries over unchanged, because the
  dual feasible set does not depend on `b`, `l`, `u`.
- **The weak-cut caveat** (§2.2, pp. 26–27): a naively shared cut
  "can be arbitrarily weak" — if a variable has `u = +∞` in both
  donor and recipient, the donor's `μ = 0`, and cost differences can
  drive the shared intercept to `−∞`.  Morton notes this does *not*
  arise on the hydro test problems because "the only arcs which can
  have nonzero shared μ's have natural finite upper bounds".
- **Interstage dependence and cut sharing** (§3.5, pp. 60–64): under
  the lag-one autoregressive RHS model `b_t = R_{t−1} b_{t−1} + η_t`
  (eq. 3.7, `η_t` independent, `R_t` deterministic), "the dual
  feasible regions of a stage *t* subproblem are unaffected by the
  interstage dependence of the right-hand-side vectors", so cuts can
  be shared across *scenarios and histories* by splitting the
  intercept into a scenario-independent term and a scenario-dependent
  term `g_dep = [P_{t+1} + A_{t+1} D_{t+1}] R_t b_t` (expected duals
  × RHS realization), with the compact recursion
  `D_t = [P_{t+1} + A_{t+1} D_{t+1}] R_t`, `D_T = 0`
  (**Theorem 3.1**).  **Corollary 3.2** extends this to the general
  additive lag model `b_t = Σ_{i<t} R_t^i b_i + η_t` (eq. 3.21).
  This is explicitly contrasted "with the case in which `A_t`, `c_t`,
  or `B_t` contain interstage dependencies" — where the dual set is
  scenario-dependent and sharing breaks.

### The paper's test problems (recovered metadata; data NOT published)

SOL 93-6 §2.3 (Tables 2.1–2.3) describes the instances later cited by
the 1996 paper: preliminary multistage stochastic hydroelectric
scheduling models for **Pacific Gas & Electric**, on two hydrological
basins in central/northern California — **Mokelumne (Moke)** and
**Yuba-Bear South Feather (Ybsf)**.  Stochastic parameters: natural
inflows into reservoirs, marginal values of energy, and simple bounds
on arc flows.  Complete recourse via a penalized auxiliary source.

| Name     | Stages | Scen./stage | Reservoirs | Det. eq. (rows × cols) |
|----------|--------|-------------|------------|------------------------|
| Moke3.9  | 3      | 9           | 7          | 7 237 × 28 473         |
| Ybsf3.9  | 3      | 9           | 12         | 13 687 × 53 489        |
| Moke4.45 | 4      | 45          | 7          | 35 736 × 140 656       |
| Ybsf4.45 | 4      | 45          | 12         | 68 012 × 265 836       |

Base-case performance (Table 2.3; FORTRAN 77 + NETSIDE network-simplex
on an HP 9000/750, solved to 0.01 % objective tolerance): 7–10
iterations per instance, CPU from ≈55 s up to a few hundred seconds
(the OCR of the scanned table is partially ambiguous on the larger
timings).  **No optimal objective values and no inflow/cost data
tables were published** — the instances themselves are therefore *not
recoverable*, only their structure.

## 2. Mapping onto gtopt (what is the same, what is narrower)

gtopt's `dual_shared` mode is the bound-parameterized specialization
of the dual sharing formula:

- The backward pass solves **one representative aperture** per
  (scene, phase) cell — the highest-weight one — and synthesizes every
  other aperture's cut from the representative's vertex duals.
- Apertures differ **only in column bounds** (the `update_aperture`
  flow-column rewrites carrying each aperture's inflow), so the dual
  feasible set is identical across apertures — Morton's observation
  restated for bounds instead of row RHS.  **Lemma AP2** (theorem doc
  §6) gives the per-aperture intercept
  `L_a = z* + Σ max(d_j,0)·Δl_j + Σ min(d_j,0)·Δu_j` from the
  representative's reduced costs `d` — the `λ·Δl − μ·Δu` term of the
  LP dual objective, i.e. exactly the shared-dual weak-duality bound.
- Morton's "arbitrarily weak cut" caveat maps to the **sentinel
  guard**: a bound that changes between finite and infinite has no
  finite delta; `dual_shared_bound_correction` returns `nullopt` and
  that aperture falls back to an exact solve.  (The `[·]⁺` cost-repair
  branch of the original formula is not needed: apertures never change
  `c`.)
- `screened` re-solves exactly the `aperture_screen_count` synthesized
  cuts with the largest `|correction|` — where the shared support is
  loosest — mirroring Morton's remark that one may instead "select the
  dual vectors which produce the strongest cut".
- Morton's *expected value method* (§2.5.1: advanced-start cuts from
  the mean-realization problem, valid exactly when the dual feasible
  regions are nonstochastic) is the historical antecedent of the
  "mean-realization anchor" follow-up sanctioned in the Lemma AP2
  remark.

**Narrower than the paper.**  Infanger–Morton's headline result is cut
sharing under *interstage-dependent row-RHS* processes (the
`b_t = R b_{t−1} + η_t` recursion with the `D_t` intercept recursion
above).  gtopt's adaptation prices **column-bound deltas only**; row
RHS deltas (`y·Δb`) are *not* priced.  In particular, AR inflow models
(`inflow_model`) move hydrology into row RHS, which `dual_shared`
does not see — **do not combine `dual_shared` with `inflow_model`**
(implementation guard: row-touching `update_aperture` writes disable
synthesis for the rest of the chunk).  The `y·Δb` extension is noted
as future work in the theorem doc.

**When to use.**  `cold`/`warm` re-solves are cheap on simplex
backends (a warm re-solve is a few pivots), so `dual_shared` buys
little there.  The target is **backends without a warm-start path** —
PDLP-style GPU solvers (cuOpt), where every aperture re-solve is a
full cold solve and duals are ε-optimal interior points (ledger F10:
the shared point is still dual-feasible; the intercept over-statement
is bounded by the primal–dual gap).  `screened` is the safety middle
ground when apertures span a wide hydrology range.

## 3. Reconstructed examples

> **Honesty note**: these are **reconstructions after the paper's
> description** (multistage hydro-thermal, single stochastic inflow
> process entering as simple-bound data, finite bounds everywhere),
> *not* the paper's instances — the PG&E basin data was never
> published (see §1).  Optimal values below are **analytic properties
> of the reconstruction**, not published numbers.

All three tests share one parametric fixture
(`dual_shared_lit::FixtureSpec`): 3 phases × 1 stage × 4 blocks of
1 h; one bus; hydro 50 MW @ 5 $/MWh; thermal 200 MW @ 50 $/MWh;
demand 45 MW; one reservoir, box [0, 200] dam³, `eini = 100`,
`flow_conversion_rate = 1`; one inflow `Flow` with a per-scenario
constant discharge schedule; one aperture per scenario, every phase
referencing the full aperture set; forward scene = scenario 1.
`scale_objective = 1` so objectives are physical dollars.

The parameters are chosen so every per-aperture stage value function
is **linear in both the state and the inflow over the entire reachable
region**: total water (160 dam³) is scarce against total demand energy
(540 MWh), no aperture can saturate a phase's demand
(`v_max + 4·q_wet ≤ 180 MWh` at every transition), and no reachable
state hits the reservoir box.  Water is then worth exactly
`50 − 5 = 45 $/dam³` everywhere, so the Lemma AP2 correction
`Σ rc·Δbound` reproduces the *exact* aperture intercept and the
synthesized cut **equals** the exact cut — the strongest form of the
paper's claim, and the reason the parity assertions below can be tight.

### 3.1 Heterogeneous apertures — bound parity (test 1)

Inflows {5, 2, 9} dam³/h (base/dry/wet), probabilities
{0.5, 0.2, 0.3}, fixed 12-iteration budget.  Asserts final LB and UB
of `dual_shared` and `screened` equal `cold`'s within 1e-6 relative.
Also documents the **measure-change caveat** (Theorem AP1): the
aperture mixture's mean inflow (5.6) is wetter than the forward path
(5.0), so the LB converges to the *mixture* optimum — analytically
`27000 − 45·(120 + 8·5.6) = 19 584` (unfolded) — strictly below the
realized forward cost, an expected non-closing gap identical across
modes.

### 3.2 Mean-matched apertures — analytic pin + convergence equivalence (test 2)

Inflows {5, 3, 7} with equal weights: the q-mean (5.0) equals the
forward inflow, and by linearity the mixture cost-to-go coincides with
the persistent one, so LB and UB both converge to the analytic
deterministic optimum:

```text
D = 3 × 4 h × 45 MW           = 540 MWh   total demand energy
W = 100 + 12 h × 5 dam³/h     = 160 dam³  total water (scarce, W < D)
optimum = 50·D − 45·W = 27 000 − 7 200    = 19 800 $
```

(The flat thermal price makes *any* dispatch that uses all the water
optimal, so the pin is insensitive to LP tie-breaking.)  Asserts, for
`cold`, `dual_shared` and `screened`: convergence at gap ≤ 1e-6, with
`UB == LB == 19 800` (1e-6 / 1e-5 relative), and
**iterations-to-tolerance equal across modes ± 2** — the
convergence-equivalence of shared vs exact cuts.  `dual_shared`
reaches this while solving one aperture LP per (scene, phase) per
backward pass by construction of the mode (no per-mode solve counter
is exported, so the count itself is not asserted).

### 3.3 Identical apertures — exactness probe (test 3)

All apertures reference the same inflow (5, 5, 5): every bound delta
is zero, the correction vanishes, and `dual_shared` / `screened` must
store **cut-for-cut identical** cuts to `cold` (type, placement, RHS,
coefficients; 1e-9), plus identical bounds.  This isolates "the shared
cut at the representative equals the exact cut" from any mixture
effect.

## 4. What could NOT be recovered / reproduced (honest gaps)

1. **The paper's instances.**  The Moke/Ybsf PG&E basin data
   (topologies, inflow discretizations, marginal-value curves) was
   never published; only dimensions and solve statistics survive
   (§1 table).  No published optimal objective values exist to pin.
2. **The journal article itself.**  Math. Prog. 75:241–256 is
   paywalled; this validation is grounded in the public SOL 93-6
   exposition (§2.2, §3.5), which the paper refines.  Statements
   specific to the 1996 text (e.g. its final dependency-model taxonomy
   and any added experiments) were not independently verified.
3. **The interstage-dependency generalization.**  Theorem 3.1 /
   Corollary 3.2 share cuts across *histories* of an additive-lag
   row-RHS process via the `D_t` recursion.  gtopt's `dual_shared`
   covers only bound-parameterized (aperture) randomness within one
   backward transition; reproducing the dependency examples requires
   the row-RHS `y·Δb` intercept extension flagged as future work in
   the theorem doc — and the AR `inflow_model` path must *not* be used
   with `dual_shared` until then.
4. **The cost-difference branch** of the dual sharing formula (the
   `[·]⁺` repair when subproblems differ in `c`) has no gtopt
   counterpart: apertures never rewrite costs, so it is intentionally
   out of scope.
