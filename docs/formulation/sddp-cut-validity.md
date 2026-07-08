# SDDP Cut Validity — Theorem Document

> **Status**: deliverable 1 of the cut-validity certification campaign
> (2026-07-07/08).  States and proves the underestimator property for
> every Benders-cut path in gtopt's SDDP/cascade machinery, and makes
> every tolerance and assumption explicit.  Companion artifacts: the
> findings ledger
> (`docs/analysis/investigations/sddp/sddp_cut_validity_findings_ledger_2026-07-08.md`)
> and the planned extensive-form oracle harness (deliverable 2).
>
> **Rendering note**: GitHub Flavored Markdown math — `$$...$$` display
> blocks, `$...$` inline.
>
> All `file:line` references are as of branch `handoff/sddp-cut-validity`
> (commit `e93290fe`).  Spot-check line numbers before relying on them.

## Table of Contents

1. [Setting and Notation](#1-setting-and-notation)
2. [Spaces, Scaling, and Scale Invariance](#2-spaces-scaling-and-scale-invariance)
3. [The Fresh Backward Optimality Cut](#3-the-fresh-backward-optimality-cut)
4. [ε-Validity: the `cut_coeff_eps` Family](#4-ε-validity-the-cut_coeff_eps-family)
5. [Cut Combinators](#5-cut-combinators)
6. [Aperture Expected Cuts](#6-aperture-expected-cuts)
7. [Cut Sharing: Mode-by-Mode Validity](#7-cut-sharing-mode-by-mode-validity)
8. [Multicut: the Resampled-Process Theorem](#8-multicut-the-resampled-process-theorem)
9. [Terminal α, Boundary Cuts, and the α-Rebase](#9-terminal-α-boundary-cuts-and-the-α-rebase)
10. [α Floors and Ceilings](#10-α-floors-and-ceilings)
11. [Feasibility Cuts](#11-feasibility-cuts)
12. [Persistence: Save, Reload, Recover, Cascade Inheritance](#12-persistence-save-reload-recover-cascade-inheritance)
13. [Assumption Registry and Open Defects](#13-assumption-registry-and-open-defects)
14. [References](#14-references)

---

## 1. Setting and Notation

gtopt's SDDP decomposes the planning horizon into **phases**
$t = 0, \dots, T$ and the uncertainty into **scenes**
$s = 1, \dots, N$ with probabilities $p_s$ ($\sum_s p_s = 1$ assumed;
see A7).  Each (scene, phase) cell owns an LP.  Scene $s$ is a single
persistent sample path: its inflow/demand/cost realizations are fixed
for the whole horizon.

**Cost folding.**  Every LP cost coefficient carries
$\text{cost\_factor} = p_s \cdot \text{discount} \cdot \text{duration}$
via `CostHelper::block_ecost` (`include/gtopt/cost_helper.hpp`).  We
write $c_s(u)$ for the *unfolded* (physical, undiscounted-by-$p_s$)
stage cost and fold explicitly, so a scene-LP objective is
$p_s\, c_s(u) + (\text{future term})$.  Discount and duration are
absorbed into $c_s$ throughout (they are phase-local constants and play
no role in cut validity).

**State variables and links.**  A state variable $x_i$ produced at
phase $t$ (e.g. reservoir `efin`) is consumed at phase $t{+}1$ through
a **dependent column** pinned to the trial value
$\hat v_i$ by `propagate_trial_values` (`benders_cut.cpp:100-134`):
the dependent column's bounds are set $lo = hi = \hat v_i$.

**Value functions.**  For scene $s$, let

$$
V_s^{(t)}(x) \;=\; \text{optimal value of scene-}s\text{'s phase-}t
\text{ LP at incoming state } x,
$$

in **folded physical** units (i.e. including the $p_s$ factor on
costs).  $V_s^{(t)}$ is convex piecewise-linear in $x$ (LP value
function of a right-hand-side/bound parametrization).

**Future-cost columns.**  Each cell's LP carries future-cost columns
$\varphi$ registered by `register_alpha_variables`
(`sddp_method_alpha.cpp:136-265`):

- single-α modes: one column, priced $1.0$;
- `multicut`: $N$ columns $\varphi_0, \dots, \varphi_{N-1}$ (uid
  $= \texttt{sddp\_alpha\_uid} + s$), each priced $1/N$
  (`sddp_method_alpha.cpp:174-175`), in **every** scene-LP.

All $\varphi$ columns are bootstrap-pinned $[0, 0]$ until the first cut
referencing them arrives (`bound_alpha_for_cut`, §10).

**Cut row convention.**  All cuts are built in physical space as

$$
\alpha + \sum_i (-g_i)\, x_i \;\ge\; b
\qquad\Longleftrightarrow\qquad
\alpha \;\ge\; b + \sum_i g_i\, x_i ,
$$

stored as a `SparseRow` with `lowb` $= b$, α-coefficient $1.0$, and
`pivot_col` = the α column (§2).

---

## 2. Spaces, Scaling, and Scale Invariance

Cuts are built in physical units and translated to LP (solver) units by
`LinearInterface::add_row` (`linear_interface.cpp:1908-2126`).  The
composition, in order:

1. **Column scaling** — each coefficient is multiplied by
   `col_scales[col]` (physical → LP column units).  The α column's
   scale is `scale_alpha` and is exempt from Ruiz equilibration
   (`pin_scale = true`, `sddp_method_alpha.cpp:190-193`) so all scenes
   share one α scale.
2. **`SparseRow::scale` division** — the row's own scale contract
   (cuts use $1.0$).
3. **Objective scaling** — coefficients and finite bounds divided by
   `scale_objective`, and the divisor folded into the recorded
   `composite_scale` (`linear_interface.cpp:2023-2062`).  This keeps
   α-bearing cut rows consistent with the objective, which was itself
   divided by `scale_objective` at `flatten()`.
4. **Per-row equilibration** — when the LP was built with
   equilibration: divide by the α-pivot coefficient
   (`row.pivot_col`, `linear_interface.cpp:2071-2115`), falling back to
   row-max.  The pivot choice keeps α's LP coefficient at $1.0$; it is
   a conditioning device only.

**Lemma S1 (scale invariance).**  *Steps 1–4 multiply the inequality
$\alpha + \sum (-g_i) x_i \ge b$ by a positive scalar and apply a
positive diagonal change of variables ($x^{LP}_i = x_i /
\text{col\_scale}_i$).  Both operations preserve the feasible set
exactly; hence a cut valid in physical space is valid in LP space and
vice versa.*

*Proof.*  Positive row scaling preserves an inequality's solution set.
The diagonal substitution is applied consistently to every row and the
objective at `flatten()`, so the LP is the image of the physical
problem under an invertible positive diagonal map. $\qquad\blacksquare$

**Readback invariants** (needed so that values *read back* from the LP
are in the units the cut builders assume; verified in
`linear_interface.cpp:1997-2062` comments and the round-trip tests):

$$
\texttt{get\_row\_low}[i] = b_{\text{phys}}, \qquad
\texttt{get\_row\_dual}[i] = \pi_{\text{phys}}, \qquad
\texttt{get\_obj\_value}() = z_{\text{phys}} .
$$

**Reduced-cost units.**  `StateVariable::reduced_cost_physical(σ)`
returns $rc_{LP} \cdot \sigma / \text{var\_scale}$
(`state_variable.hpp:329-333`).  Despite the name this is the folded
physical reduced cost: the `cost_factor` ($p_s \cdot$ discount $\cdot$
duration) baked into the objective is still present.  §3 shows this is
exactly the right object — cut RHS and coefficients are *uniformly*
folded, and the cut bounds the folded value function $V_s^{(t)}$; no
unit cancellation is required (this replaces the "cancellation" claim
in the header comment; ledger C1).

---

## 3. The Fresh Backward Optimality Cut

Built by `build_benders_cut_physical` (three overloads,
`benders_cut.cpp:137, 176, 247`) and installed on the *previous*
phase's α by `add_cut_row`
(`sddp_method_iteration.cpp:627-674`).

**Theorem O1 (support inequality).**  *Let the phase-$(t{+}1)$ LP of
scene $s$ be solved at trial state $\hat v$ (dependent columns pinned
$lo = hi = \hat v_i$), with optimal value $z^\* = V_s^{(t+1)}(\hat v)$
and reduced costs $rc_i$ on the dependent columns.  Then for every
state $x$:*

$$
V_s^{(t+1)}(x) \;\ge\; z^\* + \sum_i rc_i\,(x_i - \hat v_i).
$$

*Proof.*  For a column pinned at both bounds, the reduced cost equals
the multiplier of the implicit constraint $x_i = \hat v_i$, i.e.
$rc \in \partial_{\hat v} V_s^{(t+1)}(\hat v)$ (LP sensitivity /
strong duality).  Convexity of the LP value function in the pin
right-hand side gives the subgradient inequality. $\qquad\blacksquare$

The emitted row is precisely
$\alpha + \sum_i(-rc_i)\,x_i \ge z^\* - \sum_i rc_i \hat v_i$
(modulo the ε-filter of §4).

**Theorem O2 (recursive validity).**  *Suppose every cut installed on
the phase-$(t{+}1)$ LP's future-cost columns is a valid underestimator
of the corresponding true cost-to-go (per the mode-specific semantics
of §7–§9), and every α-floor applied is implied or assumed valid
(§10).  Then the phase-$(t{+}1)$ LP's value function $\widehat V(x)$
satisfies $\widehat V(x) \le V^{\text{true}}(x)$ for all $x$, and the
fresh cut of Theorem O1 — a support of $\widehat V$ — underestimates
$V^{\text{true}}$ everywhere:*

$$
z^\* + \langle rc, x - \hat v\rangle
\;\le\; \widehat V(x) \;\le\; V^{\text{true}}(x).
$$

*Proof.*  Relaxing the future term (cuts underestimate) can only
decrease the LP optimum pointwise, so $\widehat V \le V^{\text{true}}$.
Theorem O1 makes the cut a global support of $\widehat V$.  Chain the
inequalities.  Backward induction over phases closes the recursion:
the base case is the terminal phase, whose α is either pinned at $0$
(the model's *definition* of zero post-horizon cost) or bounded by
boundary cuts assumed to support the supplied terminal FCF (§9,
A5). $\qquad\blacksquare$

*Remarks.*

- $z^\*$ is `forward_full_obj_physical` — the **full** objective
  including the target's own future term — or the
  `backward_resolve_target` re-solve.  Either way it is the LP optimum
  at $\hat v$, which is what Theorem O1 needs.
- Non-finite cuts are dropped with an error log
  (`sddp_method_iteration.cpp:640-663`); dropping a cut never breaks
  validity (one fewer underestimator).
- MIP subproblems are relaxed to LPs before duals are read
  (`sddp_aperture.cpp:672-683` and the backward path): the cut then
  supports $V_{LP} \le V_{MIP}$ — valid but weaker for MIP recursions.

---

## 4. ε-Validity: the `cut_coeff_eps` Family

Exact validity is deliberately traded for numerical hygiene at several
sites.  The correct statement everywhere is **ε-validity**: the cut may
over-tighten by a bounded amount.  The oracle harness must assert with
tolerances derived from this section (ledger §2 has the full table).

**Theorem O3 (build-time filter).**  *`build_benders_cut_physical`
drops link $i$ when $|rc_i| < \varepsilon$
(`benders_cut.cpp:161, 207, 276`), removing both the coefficient and
its $rc_i \hat v_i$ RHS correction.  The emitted cut $\tilde\alpha(x)$
satisfies*

$$
\tilde\alpha(x) \;\le\; V^{\text{true}}(x)
  \;+\; \varepsilon \sum_{i \in \text{dropped}} |x_i - \hat v_i|
$$

*and in particular is valid up to
$\varepsilon \cdot \sum_{i\in\text{dropped}} \mathrm{diam}(\text{box}_i)$
over the state box.*

*Proof.*  The dropped term $rc_i (x_i - \hat v_i)$ has magnitude at
most $\varepsilon |x_i - \hat v_i|$; removing it shifts the cut
function by at most that amount in either direction.
$\qquad\blacksquare$

Additional filter sites (each with the analogous bound; ledger E2–E7):

| Site | Semantics |
|------|-----------|
| install (`add_cut_row` → `to_flat(eps)`) | absolute drop of physical coefficients, RHS unchanged — error $\le \varepsilon\,|x_i|$ per drop (not recentred at $\hat v$) |
| parquet reload (`sddp_cut_parquet.cpp:904-918`) | **relative** drop below $\varepsilon \cdot \max_j |g_j|$ (α included in the max), RHS unchanged |
| boundary CSV install (`sddp_boundary_cuts.cpp:652-657`) | absolute, as install |
| per-link feasibility RHS bump (`benders_cut.cpp:1225`) | deliberate escalating over-tightening, §11 |

**Compounding.**  Emit-side (absolute) and reload-side (relative)
filters apply in sequence across a cascade level transition; their
error bounds add.  Reload additionally *silently drops* coefficients
whose `(class, var, uid)` no longer resolves
(`sddp_cut_parquet.cpp:893-901`) with an unbounded a-priori error —
this is stated as precondition A6 (state-variable universe preserved
across save/load), not as a tolerance.

---

## 5. Cut Combinators

`benders_cut.cpp:1582-1708` provides three combinators used by the
sharing and aperture paths.

**Lemma A1 (convex combinations).**  *If cuts
$\alpha \ge b_k + \langle g_k, x\rangle$, $k = 1..K$, are each valid
underestimators of the same function $F$ on the same α column, then
for any weights $w_k \ge 0$, $\sum w_k = 1$, the combined cut
$\alpha \ge \sum_k w_k b_k + \langle \sum_k w_k g_k,\, x\rangle$ is
valid for $F$.*

*Proof.*  $\sum_k w_k (b_k + \langle g_k, x\rangle) \le
\sum_k w_k F(x) = F(x)$. $\qquad\blacksquare$

- `average_benders_cut` ($w_k = 1/K$) and
  `weighted_average_benders_cut` (weights normalized internally) are
  instances of Lemma A1 — valid *provided all inputs bound the same
  function on the same α* (see §6 for what "the same function" means
  on the aperture path).
- `accumulate_benders_cuts` is a plain **sum**: RHS and coefficients
  add without normalization.  A sum of $K$ valid cuts asserts
  $\alpha \ge \sum_k(\cdot) \approx K \cdot F(x)$ — **invalid** for
  $K > 1$.  This is the broken `accumulate` sharing mode (§7).
- Implementation note: the combined row inherits
  `cuts.front().scale`; all current callers build inputs with
  `scale = 1.0`.  Combining mixed-scale rows would be incorrect
  (ledger F8) — a latent hazard, not a live one.

---

## 6. Aperture Expected Cuts

The backward pass may replace the single target re-solve with a set of
**apertures** — perturbed realizations (inflow scenarios) of the target
phase — solved on a shared clone
(`sddp_aperture.cpp`, `solve_apertures_for_phase`).

Per aperture $a$: solve the clone at $\hat v$ under aperture data,
build a Theorem-O1 cut with $z^\*_a$ and clone reduced costs
(`sddp_aperture.cpp:789-793`), weight
$w_a = \text{count}_a \times \text{probability\_factor}_a$
(`:562`).  The **expected cut** is the $w$-weighted average
(`weighted_average_benders_cut`, `:980`), landing on the source
phase's α (own $\varphi_S$ under multicut).

**Theorem AP1.**  *Let $Q_a(x)$ be the aperture-$a$ value function and
$q_a = w_a / \sum_{a'} w_{a'}$ over the solved-and-feasible aperture
set $\mathcal F$.  The expected cut is a valid underestimator of
$\bar Q(x) = \sum_{a \in \mathcal F} q_a Q_a(x)$ — the cost-to-go of
the aperture-modified process under measure $q$.*

*Proof.*  Each per-aperture cut is valid for its $Q_a$ (Theorem O1 on
the clone); apply Lemma A1 with weights $q_a$. $\qquad\blacksquare$

**Measure-change caveats** — the LB statements are relative to the
modified measure $q$, *not* automatically to the full aperture list:

1. **`num_apertures` truncation** (`sddp_aperture.cpp:95-104`): only
   the first $N$ apertures (wettest-first after `plp2gtopt`'s sort)
   enter; $q$ renormalizes over the survivors.  Deliberate
   approximation; the model being bounded is the truncated process.
2. **Genuine infeasibility** (`:892-916`): an infeasible aperture has
   $Q_a = +\infty$, so *any* finite expected cut trivially
   underestimates the full-measure expectation — dropping it is safe.
3. **Timeout treated as infeasible** (`:894-905`): the dropped
   aperture has a *finite* $Q_a$; renormalizing over the rest can
   over-tighten when the dropped aperture is cheaper than the
   surviving average.  Operational caveat (ledger F3); the oracle
   harness must avoid timeouts.
4. **Non-positive `probability_factor` fallback to 1.0** (`:550-561`):
   changes $q$; WARN-logged.

---

## 7. Cut Sharing: Mode-by-Mode Validity

`share_cuts_for_phase` (`sddp_cut_sharing.cpp`) redistributes each
iteration's backward cuts.  Verdicts, with the architecture fact that
under all modes except `multicut` every cut references the single
shared-per-LP α column ($\varphi_0$):

| Mode | Mechanism | Verdict |
|------|-----------|---------|
| `none` | scene-$s$ cuts stay on scene-$s$'s own α | **VALID** unconditionally (Theorem O2 per scene; Theorem N1 below) |
| `accumulate` | sum of all scenes' cuts broadcast to every α | **INVALID** for $K>1$ cuts (Lemma A1 remark: sums over-count) — even for identical scenes the RHS sums; only degenerate single-cut iterations escape |
| `broadcast_mean` | per-scene average, then **sum** across scenes, broadcast | **INVALID** for heterogeneous scenes; the cross-scene *sum* of per-scene averages is again an over-count unless the per-scene averages coincide and... see remark below |
| `max` | every cut broadcast onto every LP's shared α | **INVALID** for heterogeneous scenes: forces $\varphi_0^D \ge p_S Q_S^\*(\cdot)$ for all $S$, i.e. the max of unrelated bounds; valid only for literally identical sample paths (identical-Q) |
| `multicut` | scene-$S$ cuts land on dedicated $\varphi_S$ in every LP | **CONDITIONALLY VALID** — see Theorem M1/M3 (§8): valid LB for the *resampled* process under uniform probabilities; **unsound** under non-uniform probabilities |

**Theorem N1 (`none` is valid).**  *Under `cut_sharing = none`, at
every iteration $LB = \sum_s V_s^{(0)}(\text{initial state})$ computed
by `compute_iteration_bounds`
(`sddp_method_iteration.cpp:1781-1846`) satisfies
$LB \le \sum_s p_s\, C_s^{\text{opt}} = $ the true expected cost of the
persistent-scene process.*

*Proof.*  Scene-$s$'s cuts are built exclusively from scene-$s$'s own
subproblems, so by Theorem O2 (induction over phases within the fixed
scene) the master value $V_s^{(0)} \le p_s C_s^{\text{opt}}$, the
folded optimal cost of scene $s$'s deterministic path problem.  Sum
over feasible scenes; infeasible scenes contribute $0 \le p_s
C_s^{\text{opt}}$ under A2 (non-negative costs) — with A2 relaxed, the
statement holds over the feasible subset only (the bounds are
$E[\text{cost} \cdot 1_{\text{feasible}}]$; ledger U6).
$\qquad\blacksquare$

*Remark (identical scenes).*  When all scenes realize the same sample
path and probabilities are equal, every scene's value function
coincides, and `max`/`broadcast_mean`/`multicut` all degenerate to
duplicating valid cuts — the strict `CHECK`s in
`test_sddp_bounds_sanity.cpp` (identical-scene fixture) are the
correct pins.  `accumulate` remains structurally wrong even there
(summed RHS); it passes those fixtures only because the aggregated
cut's α-coefficient also sums (K·α ≥ K·rhs reduces to the average
when all cuts coincide — the coefficient sum rescales the row).  Do
not read that as validity for non-coinciding cuts.

The legacy `accumulate` / `broadcast_mean` / `max` verdicts were
settled in the 2026-04-30 audit
(`sddp_cut_sharing_fix_plan_2026-04-30.md`); they stay WARN-only
regression pins and production runs use `none` or `multicut`.

---

## 8. Multicut: the Resampled-Process Theorem

Architecture (verified against
`sddp_method_alpha.cpp:136-265`,
`sddp_method_iteration.cpp:593-631`,
`sddp_cut_sharing.cpp:235-342`):

- every scene-LP at phase $t$ carries $\varphi_0..\varphi_{N-1}$, each
  priced $1/N$;
- scene-$S$'s backward cut (Theorem O1, built from scene-$S$'s
  phase-$(t{+}1)$ LP with $z^\*$ = **full** objective including its
  own $(1/N)\sum_r \varphi_r$ term) lands on $\varphi_S$ at phase $t$
  and is broadcast to $\varphi_S$ in *every* scene-LP;
- PLP parity: `plp-agrespd.f:94` source indexing, `defprbpd.f:812-818`
  pricing `(ScalePhi/ScaleObj)/NVarPhi`.  (PLP leaves $\varphi$ free,
  $\pm\infty$; gtopt adds the §10 floors.)

The scene-LP recursion is therefore, with cuts converging to their
envelopes,

$$
V_s^{(t)}(x) \;=\; \min_u\Big[\, p_s\, c_s(u)
  \;+\; \tfrac1N \textstyle\sum_r \varphi_r \,\Big],
\qquad \varphi_r \approx V_r^{(t+1)}(x') .
$$

**Theorem M1 (uniform probabilities).**  *Let $p_s = 1/N$ for all
$s$.  Substitute $\tilde V_s := V_s / p_s = N V_s$.  Then*

$$
\tilde V_s^{(t)}(x) \;=\; \min_u\Big[\, c_s(u)
  + \tfrac1N \textstyle\sum_r \tilde V_r^{(t+1)}(x') \Big]
  \;=\; \min_u\Big[\, c_s(u) + \mathbb E_{r \sim \mathrm{Unif}}
  \tilde V_r^{(t+1)}(x') \Big],
$$

*which is exactly the Bellman recursion of the **stagewise-independent
resampled process**: scene data redrawn uniformly at every phase
boundary.  Consequently the multicut LB
$\sum_s V_s^{(0)} = \frac1N\sum_s \tilde V_s^{(0)}$ is a valid lower
bound on the optimal expected cost of that resampled process (with
the phase-0 scene drawn uniformly), degraded only by the ε-tolerances
of §4 and the finiteness of the cut collection.*

*Proof.*  Direct substitution; each $\varphi_r$ is bounded by cuts
that underestimate $V_r^{(t+1)}$ (Theorem O2, induction), and the
Bellman operator is monotone, so LP values underestimate the
resampled-process value functions phase by phase.
$\qquad\blacksquare$

**Corollary M2 (process mismatch, not a cut bug).**  *The forward UB
simulates **persistent** per-scene paths (no resampling).  The
resampled-process optimum and the persistent-process optimum are not
ordered in general for heterogeneous scenes.  Hence LB > UB under
multicut on heterogeneous scenes with uniform probabilities does not
indicate cut invalidity — the two bounds refer to different stochastic
processes.  The WARN-only pin on the heterogeneous multicut test is
therefore permanent; the correct strict test compares the multicut LB
against the extensive form of the resampled process (oracle harness,
deliverable 2).*

**Theorem M3 (non-uniform probabilities — unsound).**  *For general
$p_s$, the substitution gives*

$$
\tilde V_s^{(t)}(x) = \min_u\Big[\, c_s(u)
  + \tfrac{1}{N p_s} \textstyle\sum_r p_r\, \tilde V_r^{(t+1)}(x')
  \Big].
$$

*The future term carries weight $\frac{1}{Np_s}\sum_r p_r = \frac{1}
{N p_s}$, which exceeds 1 whenever $p_s < 1/N$.  The recursion is then
the Bellman operator of no proper stochastic process (transition
weights fail to sum to 1), and the fixed point can exceed every
process value; the resulting LB can overshoot the true optimum of both
the persistent and any resampled process.  Multicut with non-uniform
scene probabilities is therefore **not certified** — the 0.6/0.4
heterogeneous WARN case in `test_sddp_bounds_sanity.cpp` exercises
exactly this configuration.*

**Proposition M4 (corrected pricing — not yet implemented).**  *Price
every $\varphi_r$ in scene-$s$'s LP at $w_r = p_s$ (the probability of
the scene owning the LP, uniform across the $N$ columns).  Then*

$$
\tilde V_s^{(t)}(x) = \min_u\Big[\, c_s(u)
  + \textstyle\sum_r p_r\, \tilde V_r^{(t+1)}(x') \Big],
$$

*the Bellman recursion of the process resampled with measure
$q_r = p_r$; the LB $\sum_s V_s^{(0)} = \sum_s p_s \tilde V_s^{(0)}$
is valid for that process with the phase-0 scene drawn from $p$.
Under uniform probabilities $w_r = p_s = 1/N$ reproduces the current
pricing.*

*Derivation.*  With pricing $w_r$ and $\varphi_r \approx V_r^{(t+1)} =
p_r \tilde V_r^{(t+1)}$, the future term is
$\sum_r w_r p_r \tilde V_r^{(t+1)}$; dividing the LP by $p_s$ requires
$w_r p_r / p_s = q_r$.  Choosing $q_r = p_r$ forces $w_r = p_s$.
$\qquad\blacksquare$

> **Do not implement** M4 until this document is reviewed and the
> oracle harness (deliverable 2) has a failing-then-passing test for
> it.  The change itself is one line in `register_alpha_variables`
> (`unit_cost`), plus the terminal-α analogue.

**Known defect (UB strip).**  `sddp_forward_pass.cpp:884-895` strips
only $\varphi_0$ at full weight from the realised opex; under multicut
the objective's future term is $(1/N)\sum_s \varphi_s$.  The two
coincide only while all $\varphi_s$ are equal (iteration 0); afterwards
`forward_objective`, the UB, and the reported gap are biased by
$(\overline{\varphi} - \varphi_0)\cdot \text{scale\_alpha}$ per cell
(either sign).  Confirmed via the offset-0 semantics of
`find_alpha_state_var` (`sddp_method.cpp:57-74`).  Fix (after the
oracle exists): strip $\frac1N \sum_s \varphi_s\,
\text{scale\_alpha}$ using `alpha_cols_on_cell`.  Single-α modes are
unaffected.

---

## 9. Terminal α, Boundary Cuts, and the α-Rebase

**Terminal α.**  With no boundary data, the terminal α stays pinned
$[0,0]$: the model *defines* the post-horizon cost as zero.  With
boundary cuts (`load_boundary_cuts_csv`,
`sddp_boundary_cuts.cpp:42+`), rows

$$
\alpha \;+\; \sum_i (-\gamma_i\, d)\, x_i \;\ge\; \rho\, d,
\qquad d = \texttt{bc\_discount},
$$

encode the supplied terminal FCF's supports, scaled by the boundary
discount.  Their validity is **assumption A5**: the CSV rows are
supports of a convex terminal value function (they originate from
PLP's `planos` data, which is such an FCF by construction).  Scaling
both $\rho$ and $\gamma$ by $d$ encodes the discounted FCF $d \cdot
\Phi(x)$ — consistent as long as the same discounting convention is
used in the UB accounting (it is: the UB never adds a terminal term;
the LB carries it through α).

Terminal α under `boundary_cut_sharing = multicut` routes each
scenario's cuts to its own terminal $\varphi_s$
(`sddp_boundary_cuts.cpp:540-575`) — same semantics as §8 at the
horizon.

**Terminal α ≥ 0 floor** (`sddp_method_alpha.cpp:277-297`): valid iff
stage costs are non-negative (assumption A2).  Deliberately violated
by design features: the *user* α is released to go negative
(`sddp_method_alpha.cpp:108-114`), and mean-shifted boundary cuts make
negative α legitimate — under A2's failure the 0-floor becomes an
over-tightening; see also the §10 fallback caveat.

**α-rebase (mean shift)** (`sddp_boundary_cuts.cpp:664-1000`,
opt-in `boundary_cuts_mean_shift`):

**Lemma B1.**  *Subtracting a per-scene constant $\bar c$ from every
boundary-cut RHS and adding $\bar c$ to the scene's first-phase master
LP native objective offset (`add_obj_constant`) is an exact change of
variables $\alpha' = \alpha - \bar c$: every LP optimum, dual, and the
reported LB/UB are unchanged.*

*Proof.*  In each terminal LP, the feasible set of $(\alpha', x)$ is
the $\bar c$-translate of that of $(\alpha, x)$.  With single-α priced
$1.0$ the objective drops by exactly $\bar c$; under terminal multicut
each of the $N$ $\varphi_s$ is priced $1/N$ and *every* cut on the LP
is shifted by the same $\bar c$, so each $\varphi_s^{\min}$ drops by
$\bar c$ and the future term $\frac1N\sum_s \varphi_s$ again drops by
exactly $\bar c$ (this corrects the "cost = 1.0" argument in the code
comment; the claim survives, the argument needs the sum).  Restoring
$+\bar c$ via the native offset makes `get_obj_value()`
algebraically identical.  `compute_iteration_bounds` reads the master
objective directly and adds nothing (
`sddp_method_iteration.cpp:1833-1837`) — no double count.
$\qquad\blacksquare$

The per-scene $\bar c$ is surfaced as the `FutureCost/rebase` output
stream (`future_cost_lp.cpp:149-156`) so `alpha + rebase` reconstructs
the un-rebased FCF.

---

## 10. α Floors and Ceilings

`apply_alpha_floor` (`sddp_method_alpha.cpp:421-640`) pins each
$\varphi$'s column bounds from the cuts that reference it.  For a cut
$\alpha + \sum_j \tilde g_j x_j \ge b$ (so $\tilde g_j = -g_j$) and the
physical state box $[x_j^{\min}, x_j^{\max}]$:

$$
\text{floor}_k = b_k - \sum_j \max(g_j x_j^{\min},\, g_j x_j^{\max}),
\qquad
\text{ceil}_k = b_k - \sum_j \min(g_j x_j^{\min},\, g_j x_j^{\max}).
$$

**Proposition F1 (floor).**  *Each installed cut $k$ implies
$\alpha \ge \text{floor}_k$ at every box-feasible state; hence pinning
the column lower bound at any value $\le \max_k \text{floor}_k$ leaves
the LP optimum unchanged.  The code pins at $\min_k \text{floor}_k$
(over cuts with bounded box-suprema) — weaker than the admissible
$\max_k$, hence safe.*  (The accumulator variable is named
`tightest_floor_phys` but accumulates the loosest — naming only.)

*Proof.*  Within the box, $\sum_j g_j x_j \le \sum_j \max(\cdot)$, so
the cut row forces $\alpha \ge b_k - \sum_j\max(\cdot)$.  A column
bound implied by existing rows never cuts feasible points.
$\qquad\blacksquare$

**Caveat (zero fallback).**  When *no* referencing cut yields a
bounded box-floor, the code writes floor $= 0.0$
(`sddp_method_alpha.cpp:568, 622-628`) — this is *not* implied by the
rows and is valid only under A2/A5-style non-negativity of the true
cost-to-go.  With a rebased FCF (negative legitimate α) and unbounded
coupled columns, it can cut the optimum.  Ledger C7; candidate
follow-up fix.

**Proposition F2 (ceiling, `bound_above`).**  *Suppose α appears only
in $\ge$-cut rows and in the objective with a positive price, and the
LP optimum $x^\*$ lies in the state box.  Then
$\alpha^\* \le \max_k \text{ceil}_k$, so pinning the column upper
bound there never cuts the optimum.*

*Proof.*  At a minimum with positive α price, α sits on its highest
binding cut $k^\*$:
$\alpha^\* = b_{k^\*} + \sum_j g_j x_j^\* \le b_{k^\*} - \sum_j \min
(g_j x_j^{\min}, g_j x_j^{\max}) = \text{ceil}_{k^\*} \le \max_k
\text{ceil}_k$. $\qquad\blacksquare$

Floors are recomputed per-$\varphi$ from that column's own cuts
(`:485-496`) — a scenario-$s$ cut never floors $\varphi_d$, matching
the §8 routing.

---

## 11. Feasibility Cuts

### 11.1 Aggregated form

`build_feasibility_cut_physical` (`benders_cut.cpp:312-471`): after
the elastic (Chinneck Phase-1) solve — objective zeroed, slacks
$s^+, s^-$ priced on the fixing rows $dep + s^+ - s^- = \hat v$ — the
cut is

$$
\sum_i \pi_i\, x_i \;\ge\; \sum_i \pi_i\,
\mathrm{clamp}\big(\hat v_i + \delta_i,\; [x_i^{\min}, x_i^{\max}]\big),
\qquad \pi_i = -\text{dual}_i,\;\;
\delta_i = s^-_i - s^+_i \text{ (rescaled)} .
$$

**Theorem FC1.**  *Let $\mu(\hat v) > 0$ be the elastic optimum
(minimum total slack penalty) and $\pi$ the fixing-row duals.  Every
master state $x$ whose subproblem is feasible satisfies the exact
Benders feasibility cut
$\sum_i \pi_i x_i \ge \sum_i \pi_i \hat v_i + \mu(\hat v)$.
The code's RHS $\sum_i \pi_i(\hat v_i + \delta_i)$ equals the exact RHS
whenever each activated slack's dual sits at its penalty bound
(complementarity at a vertex optimum — guaranteed by the crossover
requirement on the elastic solve), and the per-link clamp only ever
*weakens* the RHS in the reachable sign configurations ($\pi_i > 0$
with $\hat v_i + \delta_i > x_i^{\max}$; symmetric for $\pi_i < 0$),
preserving validity.*

*Proof sketch.*  Convexity of $\mu$ and the subgradient property of
row duals give $0 = \mu(x) \ge \mu(\hat v) + \sum_i
\text{dual}_i (x_i - \hat v_i)$ for feasible $x$, which rearranges to
the exact cut.  At a vertex optimum with active slacks nonbasic at
bound, $\mu(\hat v) = \sum_i \pi_i \delta_i$ by complementary
slackness.  Clamping moves each RHS contribution toward the box, i.e.
downward for $\pi_i > 0$ clamping from above (and analogously),
weakening the inequality.  The unreachable direction
($\pi_i > 0$, $\hat v_i + \delta_i < x_i^{\min}$) would tighten; it
requires the clone to have *lowered* dep while its dual asks for more
— excluded at a complementary vertex.  A fully rigorous case analysis
belongs to the oracle harness's randomized checks.
$\qquad\blacksquare$

Filters on this path: non-finite duals skipped; $|\pi| <
\varepsilon$ dropped (Theorem-O3-style bound); the PLP-parity $|
\delta|$-relative drop (`:392-394`); RHS bump **disabled**
(`kFactEps = 0.0`, `:467`) — the aggregated cut is *not* deliberately
over-tightened.

### 11.2 Per-link form (`build_multi_cuts`)

`benders_cut.cpp:1170-1578` mirrors PLP `plp-agrespd.f::AgrElastici`:
one single-variable cut $\pi_i\, x_i \ge \pi_i\,
(\text{clone dep value})_i$ per relaxed link.

**Example FC2 (over-cut by substitution).**  Two reservoirs $A, B$
feed one deficit; the subproblem is feasible iff $x_A + x_B \ge R$.
At $\hat v = (0, 0)$ the elastic optimum picks one mix, say
$(a^\*, b^\*)$ with $a^\* + b^\* = R$, and (with equal penalties)
$\pi_A = \pi_B = \pi > 0$.  The aggregated cut asserts
$\pi x_A + \pi x_B \ge \pi R$ — exact.  The per-link cuts assert
$x_A \ge a^\*$ **and** $x_B \ge b^\*$, which excludes the feasible
master point $(R, 0)$: **over-cut** whenever the relaxed links are
substitutable in the failed constraint set.

**Separability condition.**  Per-link cuts are exact when the
infeasibility decomposes per link: the failed constraints partition
into groups each involving a single relaxed link (e.g. each
reservoir's own balance is independently infeasible).  Then each
$\pi_i x_i \ge \pi_i d_i$ is the aggregated cut of its own group.
Cross-link substitutability (shared deficit rows, cascade
downstream coupling) breaks exactness.

**Statement.**  Per-link feasibility cuts are **heuristic separators**,
not certified cuts: in addition to Example FC2, the escalating outward
perturbation `kFactEps` $= 0.01\,\varepsilon\,\text{niter}$
(`:1225`) *deliberately* over-tightens to escape cycling, with error
growing linearly in the repeat count.  Guards that bound the damage:
the BoxEdgeStats family-drop, the per-cut saturation drop
(`:1511-1516`), and the $|\pi| < \varepsilon$ drop.  The theorem-level
guarantee is therefore: per-link cuts never *mislabel* a
subproblem-feasible trial point as requiring more state than the
aggregated cut plus the quantified perturbations — i.e. they are
sound only up to (a) substitutability over-cuts and (b) the
$k$FactEps schedule.  Runs where exactness matters should use the
aggregated form (`multi_cut` off) — this is the configuration the
oracle harness certifies.

---

## 12. Persistence: Save, Reload, Recover, Cascade Inheritance

**Physical round-trip.**  `StoredCut` holds the pre-compose *physical*
row (`sddp_cut_store.hpp:97-117`); `save_cuts_parquet` writes RHS +
typed `(cls, var, uid, val)` coefficients as-is
(`sddp_cut_parquet.cpp:166-364`).  The `scale_objective` schema
metadata (`:119-126`) is **provenance-only** — never read at load.
This is correct by design: on reload, `add_row`'s compose path (§2)
re-applies whatever scaling the *reloading* run uses, so cuts survive
a change of `scale_objective`, `scale_alpha` (via the α column's own
`col_scale`), and equilibration method (Lemma S1).

**Name-based coefficient resolution.**  Coefficients rebind by
`(class, var, uid)` against the destination's state-variable registry
(`:872-902`).  Precondition A6: the registry must contain every saved
identity; misses are dropped with a WARN and *unbounded* one-sided
error (§4).  The oracle harness should include a
same-universe-reload invariance test.

**Multicut broadcast reconstruction** (fix `9300306da`;
`sddp_cut_parquet.cpp:669-695, 1013-1020, 1096-1119`): saves are
origin-only; the loader re-installs each inherited optimality cut on
its $\varphi_S$ in every non-origin scene-LP (install + replay buffer,
never stored).  This exactly reproduces the in-run
`share_cuts_for_phase` state, so Theorems M1/M3 apply unchanged to
reloaded cut sets.  Feasibility cuts are per-scene and not broadcast —
correct, they are scene-local statements.

**De-dup and identity.**  Loaded rows dedup on the full 5-tuple
`(type, scene_uid, phase_uid, iteration, extra)` (`:697-706`);
`bound_alpha_for_cut` releases exactly the referenced $\varphi$; the
`cut_store` mirror keeps `forget_first_cuts` consistent
(`:1034-1042`).  These are bookkeeping invariants (no validity
content) but they gate the *replayability* of everything above under
`low_memory = compress`.

**Legacy files** (no `scene` column) broadcast every cut to every
scene as origin (`:1021-1028`) — for heterogeneous scenes this
*persists* what the in-run path deliberately never stores.  Compat
caveat A8; regenerate cut files with a current build for certified
runs.

**Cascade inheritance.**  A level-$k{+}1$ run loading level-$k$ cuts
is a reload with a (possibly) different phase/scene universe; validity
is inherited per Theorem O2 + A6, degraded by the §4 reload filters.
The `cut_coeff_eps` reload filter is *relative* while the emit filter
is absolute — the tolerances compound across level transitions
(ledger §2, E1+E3).

---

## 13. Assumption Registry and Open Defects

Assumptions the theorems rely on (violations void the affected
statements):

| # | Assumption | Where it bites |
|---|------------|----------------|
| A1 | LP subproblems solved to optimality; vertex duals (crossover) on elastic solves | O1, FC1 |
| A2 | Stage costs non-negative | terminal α ≥ 0, §10 zero fallback, N1's infeasible-scene term |
| A3 | Uniform scene probabilities | multicut LB (M1); non-uniform is unsound (M3) until M4 ships |
| A4 | UB and LB compare like processes | Corollary M2 — persistent-path UB vs resampled-process LB are incomparable on heterogeneous scenes |
| A5 | Boundary-cut CSV rows support a convex terminal FCF | §9 |
| A6 | State-variable universe preserved across save/load boundaries | §12, E4 |
| A7 | $\sum_s p_s = 1$ (bounds are plain sums of folded values) | N1, M1 |
| A8 | Cut files written by a current (scene-column) emitter | §12 legacy caveat |

Open defects / follow-ups (all documented, none fixed here by design):

1. **UB strip under multicut** (§8) — confirmed; fix after oracle.
2. **Multicut non-uniform pricing** (M3/M4) — one-line fix specified;
   after oracle.
3. **Zero-fallback α floor** (§10) — over-tightens when A2 fails.
4. **Aperture timeout-as-infeasible** (§6) — conditional-measure bias.
5. **Per-link feasibility cuts** (§11.2) — heuristic; certified runs
   use the aggregated form.
6. Comment corrections queued: `sddp_cut_sharing.cpp:258-260` (U2),
   `sddp_method_alpha.cpp:148-153` (C2), `state_variable.hpp:310-325`
   (C1), `sddp_boundary_cuts.cpp:831-836` (C8),
   `tightest_floor_phys` naming (C6).

## 14. References

1. M. V. F. Pereira, L. M. V. G. Pinto, "Multi-stage stochastic
   optimization applied to energy planning", *Mathematical
   Programming* 52, 359–375 (1991).
2. V. Guigues, V. L. de Matos, A. B. Philpott, "Single cut and
   multicut SDDP with cut selection", 2019 — single-cut vs multicut
   semantics.
3. J. W. Chinneck, "Feasibility and Infeasibility in Optimization",
   Springer (2008) — elastic filter (Phase-1) background.
4. PLP sources: `plp_storage/CEN65/src/defprbpd.f` (φ pricing),
   `plp-agrespd.f` (`AgrElastici`, per-link sensitivity cuts).
5. In-repo prior art:
   `docs/analysis/investigations/sddp/sddp_cut_sharing_fix_plan_2026-04-30.md`;
   `docs/methods/sddp.md` (operational documentation, updated in
   lockstep with this document).
