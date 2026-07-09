# SDDiP and Integer Capacity Expansion — Investigation and Plan

> **Status**: deliverable 1 of the SDDiP / integer-expansion campaign
> (2026-07-08).  Analyses how gtopt's SDDP machinery treats integer
> (binary / integer-expansion / unit-commitment) columns, summarises
> SDDiP (Zou, Ahmed & Sun 2019) and what production tools actually do,
> and records the agreed plan: (1) **strengthened Benders cuts**
> (implemented — see §6), (2) an **OptGen-style investment-master
> Benders loop** (design in §7, extraction API implemented), (3) full
> SDDiP **not now**.
>
> Companion documents: `docs/formulation/sddp-cut-validity.md` (the
> LP cut-validity theorem document; §3 remark is the starting point),
> `test/source/test_sddp_cut_oracle.cpp` (extensive-form oracle
> harness), `test/source/test_sddp_strengthened_cuts.cpp` (the
> MIP-fixture oracle added with deliverable 2).

## Table of Contents

1. [Problem: what gtopt does with integers today](#1-problem)
2. [Why the LP-relaxation cut is valid but weak](#2-lp-cut)
3. [SDDiP in one page](#3-sddip)
4. [Cut families for integer recourse](#4-cut-families)
5. [Fit analysis: gtopt vs SDDiP preconditions](#5-fit)
6. [Strengthened Benders cuts (implemented)](#6-strengthened)
7. [OptGen-style investment-master loop (design)](#7-optgen)
8. [Effort estimates and certification plan](#8-effort)
9. [References](#9-references)

---

<a id="1-problem"></a>
## 1. Problem: what gtopt does with integers today

gtopt LPs can carry integer columns from three sources:

- **integer expansion modules** — `integer_expmod` on any
  `CapacityObjectBase` element (`capacity_object_lp.cpp`): the
  per-stage `expmod` column becomes `IntegerDomain::Integer` unless
  the phase is continuous-relaxed.  The chained `capainst` /
  `capacost` columns are SDDP **state variables** (registered via
  `add_state_col`), so Benders cuts already carry
  ∂V/∂capacity subgradients.
- **unit commitment** — `SimpleCommitment` binaries per block
  (`simple_commitment_lp.cpp`), plus the richer commitment /
  SOS2-loss machinery.  Not state variables (no cross-phase
  chaining); they make the *subproblem* a MIP.
- **line commitment / switching** and other binary-bearing elements.

Under SDDP, per-cell subproblems with integer columns are handled
inconsistently today:

1. **Aperture backward clones** explicitly relax integrality before
   reading duals (`sddp_aperture.cpp:690`, `clone.relax_integers()`),
   so aperture cuts are LP-relaxation cuts — *valid but convexified*
   (theorem doc §3 remark: the cut supports $V_{LP} \le V_{MIP}$).
2. **The pure-Benders backward path**
   (`sddp_method_iteration.cpp::backward_pass_single_phase`) does
   *not* relax: `backward_resolve_target` re-solves the live cell LP,
   which is a MIP when integer columns exist, and then reads
   `get_col_cost_raw()` — reduced costs.  A MIP has no reduced
   costs.  The CPLEX backend calls `CPXgetdj` and **ignores the error
   code** (`plugins/cplex/cplex_solver_backend.cpp:1365-1373`), so
   the rc buffer contains zeros (first call) or stale values from a
   previous LP solve.  The emitted cut is then
   $\alpha \ge z_{MIP}(\hat v)$ with wrong/absent slopes — a **flat
   cut at the MIP incumbent value**, which *overshoots*
   $V_{MIP}(x)$ for any state $x$ better than the trial $\hat v$.
   This is an unsound path, previously latent because production
   SDDP cases are pure LPs.
3. **The forward pass** solves the MIP and recovers duals only in the
   *simulation* pass (`fix_integers_and_resolve`,
   `sddp_forward_pass.cpp:389-434`); training-pass rc mirrors on
   integer cells carry the same garbage as (2).

**Conclusion**: integer expansion (and any integer recourse) is
currently valued by its convexification at best (aperture path), and
by an invalid flat cut at worst (pure-Benders path).  Deliverable 2
(§6) fixes both under an opt-in option by making the LP relaxation
explicit and then *tightening* the intercept with one MIP solve.

<a id="2-lp-cut"></a>
## 2. Why the LP-relaxation cut is valid but weak

Write the phase-$(t{+}1)$ subproblem of scene $s$ at incoming state
$x$ (theorem doc §1 conventions; dependent columns $z$ pinned
$lo = hi = x$):

$$
V_{MIP}(x) \;=\; \min_{u, z} \; c^\top u
\quad \text{s.t.} \quad (u, z) \in X_{MIP},\; z = x ,
$$

where $X_{MIP}$ includes the integrality constraints and
$X_{LP} \supseteq X_{MIP}$ is its LP relaxation.  Solving the LP
relaxation at $\hat v$ yields $z^\*_{LP} = V_{LP}(\hat v)$ and pinned
reduced costs $\lambda_i$ (the multipliers of $z_i = \hat v_i$), and
Theorem O1 of the cut-validity document gives

$$
V_{MIP}(x) \;\ge\; V_{LP}(x) \;\ge\;
z^\*_{LP} + \langle \lambda, x - \hat v\rangle .
$$

So the LP cut is **valid** for the MIP recursion — but it supports
the convex hull value $V_{LP}$, not $V_{MIP}$: wherever integrality
binds ($V_{MIP}(\hat v) > V_{LP}(\hat v)$), the cut undervalues the
true cost-to-go by at least the integrality gap.  For integer
*expansion* this systematically undervalues lumpy investments: the
policy sees the fractional-build cost of capacity, never the
all-or-nothing cost.

<a id="3-sddip"></a>
## 3. SDDiP in one page

**SDDiP** (Zou, Ahmed & Sun, *Stochastic dual dynamic integer
programming*, Math. Prog. 175:461-502, 2019) extends SDDP to
multistage stochastic **integer** programs with the following
architecture:

- **Binary state variables.**  The *exactness theorem* (their
  Thm. 1-2) requires the state vector passed between stages to be
  binary (0/1).  Continuous or general-integer states must first be
  **binarized**: $x = \sum_{k} 2^{k-1}\varepsilon\, w_k$ with
  $w_k \in \{0,1\}$, at precision $\varepsilon$ — $K =
  \lceil \log_2(x^{max}/\varepsilon + 1)\rceil$ binaries per state.
- **Cut families** (§4) generated in the backward pass on the
  binarized state; the key family — **Lagrangian cuts** — is *tight*
  at binary trial points because the Lagrangian dual of a MIP with
  binary complicating variables has zero duality gap there (their
  Prop. 3, building on the reformulation-linearization of the copy
  constraint $z = \hat v$).
- **Convergence.**  With tight + valid + finite cut families,
  forward sampling, and complete recourse, SDDiP converges to the
  exact optimum of the multistage stochastic MIP almost surely in
  finitely many iterations.

The cost: every backward-pass cut requires an **iterative Lagrangian
dual solve** (subgradient / bundle, each iteration one MIP), on a
state space blown up by the binarization factor $K$.  Published
applications stay at small state dimension (a handful of reservoirs
or units) precisely because of this.

<a id="4-cut-families"></a>
## 4. Cut families for integer recourse

For the stage MIP $V_{MIP}(x)$ above, at a trial point $\hat v$:

| Family | Form | Valid | Tight at $\hat v$ | Cost per cut |
|---|---|---|---|---|
| Benders (LP) | $z^\*_{LP} + \langle\lambda_{LP}, x - \hat v\rangle$ | yes (any $x$) | only if LP tight | 1 LP |
| Strengthened Benders | $L(\lambda_{LP}) + \langle\lambda_{LP}, x - \hat v\rangle$ | yes (any $x$, weak duality) | no (but $\ge$ Benders) | 1 LP + 1 MIP |
| Integer optimality (Laporte–Louveaux) | $V_{MIP}(\hat v)(1 - \|x - \hat v\|_1^{bin})$-style | only for **binary** $x$ | yes | 1 MIP |
| Lagrangian | $L(\lambda^\*) + \langle\lambda^\*, x - \hat v\rangle$, $\lambda^\* \in \arg\max L$ | yes (any $x$) | yes for **binary** state | iterative dual, many MIPs |

where the **Lagrangian relaxation** of the copy constraint is

$$
L(\lambda) \;=\; \min_{u,z}\;\Big[\, c^\top u
  + \langle \lambda, \hat v - z\rangle
  \;:\; (u, z) \in X_{MIP},\; z \in B \,\Big],
$$

with $B$ the state box (the copy variable's natural bounds).  The
**strengthened Benders cut** evaluates $L$ at the *LP-optimal*
multiplier $\lambda_{LP}$ — a single extra MIP solve instead of an
iterative dual — and is the standard cheap upgrade (Zou et al. §5;
also the default "strengthened Benders" in SDDP.jl's SDDiP
implementation).

Ordering of the intercepts at $\hat v$:

$$
z^\*_{LP} \;=\; L_{LP}(\lambda_{LP})
\;\le\; L(\lambda_{LP})
\;\le\; \max_{\lambda} L(\lambda)
\;\le\; V_{MIP}(\hat v),
$$

the first equality by LP strong duality (dualizing only the pins),
the first inequality because $X_{MIP} \subseteq X_{LP}$ shrinks the
feasible set of the inner min, the last by weak duality.  So the
strengthened cut is **never looser** than the Benders cut and never
over-tightens.

<a id="5-fit"></a>
## 5. Fit analysis: gtopt vs SDDiP preconditions

**What fits well.**

- Expansion states (`capainst`, integer `expmod`) are naturally
  integer or even binary per stage — the states SDDiP wants.
- The cut plumbing (state registration, `StateVarLink`, physical cut
  rows, parquet persistence) is state-name-agnostic; capacity
  subgradients already flow through it.
- The oracle harness (extensive-form tail solves) extends to MIP
  tails mechanically — integers kept in the tail Planning.

**What does not.**

- The dominant SDDP state — reservoir energy — is **continuous**.
  Exact SDDiP would binarize it: $K \approx 20$–30 binaries per
  reservoir at useful precision, ×40+ reservoirs on CEN-scale cases
  → thousands of binary state columns and MIP subproblems per cell
  per backward pass.  Not viable at production scale.
- Lagrangian cuts require an iterative dual per cut: 10–100 MIP
  solves per (scene, phase) per iteration.  The current backward
  pass budget is ~1 LP resolve per cell.
- **Production tools do not run SDDiP for planning.**  NEWAVE and
  PSR's SDDP run LP subproblems (commitment relaxed or handled
  heuristically).  PSR pairs **OptGen** — an investment master MIP
  over discrete builds — with **SDDP for operations**, exchanging
  Benders cuts in capacity space.  Exact integer treatment is
  reserved for the *here-and-now* investment decisions, not the
  recourse.

**Agreed recommendation** (maintainer-approved):

1. **Strengthened Benders cuts** (§6) as the near-term soundness fix
   wherever subproblems genuinely carry integers — opt-in
   (`integer_cuts_mode = strengthened`), one extra MIP per backward
   cell, silent LP fallback.  Fixes the §1 unsound path as a side
   effect (the LP relaxation becomes explicit).
2. **OptGen-style investment master** (§7) for integer *expansion*:
   master MIP over builds + operational SDDP with capacities fixed,
   iterating via capacity-space Benders cuts extracted from the
   phase-0 FCF.  Reuses the existing capacity-state subgradients.
3. **Full SDDiP only if exact integer *recourse*** (commitment
   valued exactly inside the recursion) becomes a requirement.  Not
   now: binarization cost on reservoir states and Lagrangian-dual
   cost per cut are both prohibitive, and no peer production tool
   pays them.

<a id="6-strengthened"></a>
## 6. Strengthened Benders cuts (implemented)

### 6.1 Mechanism

Option: `SddpOptions::integer_cuts_mode` (JSON
`sddp_options.integer_cuts_mode`), enum `IntegerCutsMode` = `none`
(default, byte-identical to previous behaviour) | `strengthened`.
Resolved into `SDDPOptions::integer_cuts`.

Gate (per backward step, `backward_pass_single_phase`): option is
`strengthened` **and** the target cell LP `has_integer_cols()`.  On a
pure-LP cell the path is never entered; on an LP-only solver backend
integer columns cannot exist (`load_flat` refuses them), so the MIP
capability gate is implied.

On the gated path, `build_strengthened_benders_cut`
(`benders_cut.cpp`) does, on a **clone** of the solved target LP
(pins $z = \hat v$ intact):

1. record the integer column set, `relax_integers()`;
2. solve the **LP relaxation** at $\hat v$ → $z^\*_{LP}$, pinned
   reduced costs $\lambda$ (this replaces the garbage-rc path of §1
   item 2);
3. build the ordinary Theorem-O1 cut from the clone:
   $\alpha + \sum_i(-\lambda_i)x_i \ge b_{LP}$,
   $b_{LP} = z^\*_{LP} - \langle\lambda, \hat v\rangle$ (the
   `cut_coeff_eps` filter applies; dropped links get $\lambda_i = 0$);
4. `restore_integers`, **relax every state pin** to its physical
   source box $[x_i^{min}, x_i^{max}]$ (same box
   `relax_fixed_state_variable` uses), and **charge the multiplier on
   the state deviation**: objective coefficient of each dependent
   column $z_i$ += $-\lambda_i$ (physical units), for exactly the
   $\lambda_i$ kept in step 3;
5. solve the resulting **MIP** once:

$$
m^\* \;=\; \min_{u,z}\;\Big[\, c^\top u - \langle\lambda, z\rangle
  \;:\; (u,z) \in X_{MIP},\; z \in B\,\Big]
\;=\; L(\lambda) - \langle\lambda, \hat v\rangle ;
$$

6. emit the cut with intercept $\max(b_{LP},\, m^\*)$ and the
   step-3 slopes — i.e. $m^\*$ **is** the strengthened RHS directly
   (the $\langle\lambda,\hat v\rangle$ correction is already inside
   it), and the `max` guards against MIP solver noise ever loosening
   the cut below the LP baseline.

### 6.2 Validity statement

**Theorem SB1 (strengthened Benders validity).**  *Let $\lambda$ be
the pinned reduced costs of the LP relaxation at $\hat v$ (step 2),
kept coefficients only ($|\lambda_i| \ge$ `cut_coeff_eps`, others set
to 0), let $B = \prod_i [x_i^{min}, x_i^{max}]$ be the state box, and
let $m^\*$ be the exact optimum of the step-5 MIP.  Then for every
$x \in B$:*

$$
V_{MIP}(x) \;\ge\; m^\* + \langle \lambda, x\rangle
\;=\; L(\lambda) + \langle \lambda, x - \hat v\rangle .
$$

*Proof.*  Fix $x \in B$.  The point set
$\{(u,z) : (u,z) \in X_{MIP},\, z = x\}$ is contained in the step-5
feasible set $\{(u,z) \in X_{MIP},\, z \in B\}$ (the pins were
relaxed to $B$, and $x \in B$).  On that subset the step-5 objective
equals $c^\top u - \langle\lambda, x\rangle$.  Minimizing over the
larger set can only decrease the value:

$$
V_{MIP}(x) - \langle\lambda, x\rangle
= \min_{(u,z)\in X_{MIP},\, z = x}
  \big[c^\top u - \langle\lambda, z\rangle\big]
\;\ge\; m^\* .
$$

Rearranging gives the claim; the second form substitutes
$m^\* = L(\lambda) - \langle\lambda,\hat v\rangle$.
$\qquad\blacksquare$

**Corollary SB2 (tightening).**  $m^\* \ge b_{LP}$, i.e. the
strengthened intercept is never looser than the LP cut's: by LP
strong duality for the pin constraints,
$b_{LP} = z^\*_{LP} - \langle\lambda,\hat v\rangle =
L_{LP}(\lambda) - \langle\lambda,\hat v\rangle$, and restricting the
inner minimization from $X_{LP}$ to $X_{MIP} \subseteq X_{LP}$ can
only increase it.  Strict tightening occurs exactly when integrality
binds in the Lagrangian subproblem. $\qquad\blacksquare$

**Recursion.**  As in Theorem O2 of the cut-validity document, the
objective of the step-5 MIP includes the cell's own future term
(α columns bounded by installed cuts).  If those cuts underestimate
the true (MIP) cost-to-go, the strengthened cut underestimates the
exact MIP recursion; the induction closes at the terminal phase.

### 6.3 ε-validity caveats (oracle tolerances)

- **`cut_coeff_eps` drops** — identical Theorem-O3 budget as the LP
  path: dropped links contribute
  $\le \varepsilon\,\mathrm{diam}(B_i)$ each.  Note the strengthened
  intercept itself is *exact* for the kept-$\lambda$ vector (the
  Lagrangian is evaluated at exactly the emitted slopes; dropping a
  link sets its $\lambda_i = 0$ *before* the MIP solve).
- **MIP gap** — solvers return the *incumbent* value, which is an
  upper bound on $m^\*$: a positive MIP gap can over-tighten the cut
  by up to `mip_gap` × $|m^\*|$.  The implementation therefore pins
  `mip_gap = 1e-9` (and `mip_gap_abs`) on the strengthening solve;
  the residual enters the oracle tolerance like a solver
  feasibility term.  Follow-up (punted): read the solver's **best
  bound** instead of the incumbent, which is always $\le m^\*$ and
  hence unconditionally safe.
- **Fallbacks** — if the LP-relaxation clone solve fails, the step
  returns nothing and the caller falls back to the legacy
  mirror-based cut (today's behaviour, logged WARN).  If only the
  MIP solve fails or times out (`time_limit`, default 30 s on the
  strengthening solve when the backward options carry none), the
  step-3 LP-relaxation cut is emitted — still valid, merely not
  tightened (logged DEBUG, aperture-timeout style).

### 6.4 Scope

The strengthened path is wired into the **pure-Benders backward
pass** only.  The aperture backward pass keeps its explicit
`relax_integers` LP cuts (its measure-change semantics, chunked
warm-start machinery, and Agent-D's in-flight dual-shared work make
it a separate change; the per-aperture MIP cost would also multiply
by the aperture count).  Extension is mechanical once wanted: the
per-aperture cut builder already has the clone in hand.

<a id="7-optgen"></a>
## 7. OptGen-style investment-master loop (design)

### 7.1 Architecture (PSR OptGen + SDDP parity)

Two-level Benders decomposition separating **investment**
(here-and-now, integer) from **operation** (multistage, continuous):

- **Master (MIP)**: variables = per-stage builds $n_{g,t} \in
  \mathbb{Z}_+$ (bounded by `expmod`), capacity accounting
  $K_{g,t} = K_{g,t-1}(1-\delta) + \bar e_g\, n_{g,t}$ (mirroring the
  `capainst` row of `capacity_object_lp.cpp`, lead-time shifts
  optional), objective = annualized CAPEX + $\theta$, plus Benders
  cuts $\theta \ge \rho_k + \langle \gamma_k, K \rangle$ and optional
  budget rows.  Built directly on `LinearInterface` (no Planning
  needed — tens of columns).
- **Operational oracle**: gtopt SDDP with the candidate capacities
  **fixed** (expansion disabled: `expmod` bounds pinned to the
  master's $n$, or capacities overridden via the `--set` /
  programmatic `Planning` mutation route), returning (i) the
  expected operational cost (LB at convergence, UB as the
  statistical estimate) and (ii) subgradients w.r.t. capacity.
- **Cut extraction**: the phase-0 stored cuts already carry
  $\partial V/\partial \texttt{capainst}$ coefficients (capacity
  columns are `add_state_col` state variables).  The extraction API
  (`extract_capacity_cuts`, implemented — deliverable 3 subset)
  filters phase-0 optimality cuts to capacity coordinates
  (`col_name ∈ {capainst, capacost}`), reporting any dropped
  non-capacity coordinates (e.g. reservoir energy) so the caller
  knows when the projection is exact (pure-expansion state) vs
  heuristic (mixed state — see §7.3).
- **Loop**: solve master → fix capacities → SDDP → extract capacity
  cuts (+ the run's expected cost as the cut intercept anchor) → add
  to master → repeat.  LB = master objective (CAPEX + θ);
  UB = CAPEX($n$) + SDDP expected cost at $n$; stop on
  `UB − LB ≤ tol` or iteration cap.

### 7.2 Validity

With operations convex in capacity (they are: capacities enter the
operational LP as bounds/RHS), the expected operational cost
$\Phi(K) = \sum_s p_s V_s^{(0)}(K)$ is convex piecewise-linear, and
each extracted cut is a valid support by Theorem O1/O2 — the master
is a textbook Benders master and the loop converges finitely.  The
master's integrality is exact because integers live *only* in the
master (this is precisely why PSR pairs OptGen with SDDP instead of
running SDDiP).

### 7.3 v1 caveats

1. **Mixed initial state.**  Phase-0 cuts on a hydro system carry
   reservoir coefficients too; restricting to capacity coordinates
   is exact only when the non-capacity coordinates are pinned at
   fixed initial values across master iterations (they are: `eini`
   is data).  The extraction API records the dropped coordinates and
   the trial state should be re-anchored at `eini`; v1 documents
   this and the master test fixture uses a pure-expansion system.
2. **Cut intercepts.**  StoredCut RHS is in the cut row convention
   $\alpha + \sum(-g)x \ge b$; the master must consume
   $\theta \ge b + \langle g, K\rangle$ — the extraction API returns
   both pieces explicitly.
3. **Per-scene α vs expected cost.**  Under `cut_sharing = none`
   the phase-0 cuts are per-scene ($p_s$-folded); the master's θ
   should be $\sum_s \theta_s$ with per-scene cuts, or the per-scene
   cuts summed at matching iterations.  v1: per-scene θ columns.
4. **Fixing capacities.**  Least-invasive route: run the operational
   Planning with the expansion element's `expmod` schedule bounds
   pinned (`expcap` unchanged, `expmod = n` as both bound and
   `capmax` consistency), i.e. a pure-data mutation of the input
   `Planning` before `PlanningLP` construction — no LP surgery.

### 7.4 Delivery status (honest scope)

Shipped in this campaign: the **extraction API + unit tests + this
design**, plus the **minimal master loop** (`solve_investment_master`,
`sddp_investment_master.{hpp,cpp}`) with its pure-expansion acceptance
test against the monolithic MIP
(`test_sddp_investment_master.cpp`) — the loop now converges
(LB = UB = 1200, build 1 module) on the fixture.

**Convergence bug FIXED (2026-07-09) — the capacity subgradient
source.**  The first cut of the loop is
`θ_s ≥ V_s + Σ_j g_j·(K_j − K̂_j)`, with `g_j = ∂V_s/∂K_j` the
per-scene value-function slope w.r.t. installed capacity.  The initial
implementation read `g_j` as the `capainst` COLUMN reduced cost via
`LinearInterface::get_col_cost()`.  That is **structurally 0**:
`capainst` is a BASIC column, pinned by its own defining equality
`capainst = base + expcap·expmod + prev_capainst`
(`capacity_object_lp.cpp`), so its reduced cost is 0 by simplex
stationarity.  Every cut was therefore FLAT (`θ_s ≥ V_s`, no
K-dependence); the master saw `θ_s ≥ max(600, 8000) = 8000 ∀K`, built
0, and "converged" at LB = UB ≈ 16000 — well above the true optimum
1200.  Diagnosed trajectory: iter0 (free K̂ = 100) → V_s = 600, g = 0;
iter1 (pinned build 0, K̂ ≈ 0) → V_s ≈ 8000, g = 0 → false
convergence.

Investigated (via DIAG instrumentation) every candidate source at the
two evaluated capacity points:

- **capainst column rc** — 0 (basic), as above.
- **expmod column rc** — 0: at K = demand the marginal module beyond
  the served load has no value (a value-function kink); at K ≈ 0 the
  bound-pinned column is degenerate.
- **capainst-equality / capacost row duals** — folded to a constant 1
  at BOTH capacity points (row-max equilibration of the ±expcap
  coefficient masks the value); NOT the −74/scene true slope.
- **phase-0→phase-1 `capainst_prev` dependent-column rc** — 0: in the
  fixture each stage builds its OWN capacity via its own pinned
  `expmod`, so stage-2 dispatch does not depend on stage-1's carried
  capacity → no coupling reduced cost.

The **correct source is the `capacity` constraint dual** (`generation
≤ capainst`, `generator_lp.cpp` `CapacityName`): its dual π_cap ≥ 0 is
the shadow price of the dispatch ceiling — a valid subgradient of the
convex value function at the trial point (standard LP sensitivity).
Each per-block dual is already folded by its `cost_factor = prob ×
discount × duration`, the SAME probability-folded, `scale_objective`-
unscaled space as `V_s = get_obj_value()` (= `scene_lower_bounds`), so
`g_j = −Σ_{(phase, block)} π_cap` is the slope directly in the master's
θ_s units, no extra scaling.  Summing across phases folds in the
stage-≥1 cost-to-go (the master pins the same build to every stage, so
every stage's ceiling moves with K_j).  On the fixture this yields a
valid (non-flat) support at K̂ = 100 and the master converges to
build 1 in one round trip.  See `SceneCapSubgradients` in
`sddp_investment_master.cpp` for the derivation.

Per-stage staggered builds (distinct `n*` per stage) remain the
documented remainder — see §8.

<a id="8-effort"></a>
## 8. Effort estimates and certification plan

| Item | Estimate | Status |
|---|---|---|
| Strengthened cuts (option, builder, backward hook) | 2-3 d | **done** |
| MIP-fixture oracle tests (tail-MIP extensive form) | 1-2 d | **done** |
| Capacity-cut extraction API + tests | 1 d | **done** |
| Investment master loop v1 + acceptance test | 2-3 d | **done** (§7.4; capacity-dual subgradient, converges to the monolithic MIP on the pure-expansion fixture) |
| Per-stage staggered builds (distinct n* per stage) | 1-2 d | remainder (§7 caveat 2) |
| Best-bound intercept (remove MIP-gap ε term) | 0.5 d | punted |
| Strengthened cuts on the aperture path | 1-2 d | punted (Agent-D conflict) |
| Full SDDiP (binarization + Lagrangian cuts) | months | rejected for now (§5) |

**Certification plan** (mirrors the LP campaign): every integer-cut
claim is gated by an extensive-form oracle on a small fixture —
monolithic **MIP** tail solves at swept states, integers kept
(`test_sddp_strengthened_cuts.cpp`):

- (a) every strengthened cut underestimates the per-scene MIP tail
  at every grid state (`cut_sharing = none`, strict, Theorem SB1);
- (b) strengthened intercept ≥ LP-relaxation intercept (Corollary
  SB2) — asserted both at the unit level (hand-built MIP with known
  Lagrangian value) and across a paired none/strengthened SDDP run;
- (c) option off ≡ previous behaviour byte-identical on pure-LP
  fixtures (paired-run cut and LB-trajectory equality).

MIP-dependent tests gate on `SolverRegistry::has_mip_solver()` /
`solver_test::first_mip_solver()` + `run_or_skip_license`, matching
the unit-commitment test conventions.

<a id="9-references"></a>
## 9. References

1. J. Zou, S. Ahmed, X. A. Sun, "Stochastic dual dynamic integer
   programming", *Mathematical Programming* 175, 461-502 (2019).
   DOI 10.1007/s10107-018-1249-5.
2. G. Laporte, F. V. Louveaux, "The integer L-shaped method for
   stochastic integer programs with complete recourse", *OR Letters*
   13, 133-142 (1993).
3. M. V. F. Pereira, L. M. V. G. Pinto, "Multi-stage stochastic
   optimization applied to energy planning", *Math. Prog.* 52,
   359-375 (1991).
4. PSR, *OptGen — expansion planning model* and *SDDP — operational
   model* methodology manuals (OptGen investment master + SDDP
   operational Benders exchange).
5. O. Dowson, L. Kapelevich, "SDDP.jl: a Julia package for
   stochastic dual dynamic programming", *INFORMS J. Computing* 33,
   27-33 (2021) — `SDDP.StrengthenedBenders` cut option.
6. In-repo: `docs/formulation/sddp-cut-validity.md` (§3 remark, §4
   ε-validity); `docs/methods/sddp.md`.
