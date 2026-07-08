# Markov-Chain SDDP (`cut_sharing_mode = markov`)

> **Status**: opt-in, experimental (2026-07-08).  Design + derivation for
> the Markov-chain cut-sharing mode.  Generalizes `multicut`
> (`docs/formulation/sddp-cut-validity.md` §8): multicut is the
> degenerate `M = N` case with one scene per state and transition rows
> equal to the normalized scene probabilities.  Notation follows the
> cut-validity theorem document §1 (folded value functions
> $V_s = p_s \tilde V_s$, cost folding via `cost_factor`).

## 1. Configuration

- `markov_states`: array of $N$ integers — scene $s$ is assigned the
  Markov state $m(s) \in \{0, \dots, M-1\}$.  Static per scene (v1);
  per-phase assignments are future work.
- `markov_transition`: row-major $M \times M$ row-stochastic matrix
  $P$ ($P_{m m'} \ge 0$, $\sum_{m'} P_{m m'} = 1$ per row, tolerance
  $10^{-6}$).

Validation at SDDP setup (`validate_markov_config`): $M \ge 1$, the
matrix is square and matches $M$, every assignment is in range, every
row sums to $\approx 1$ with non-negative finite entries, and **every
state has at least one assigned scene** (an empty state would make its
mass $\pi_{m'} = 0$ and the pricing below divides by it).  This forces
$M \le N$.

Let $p_s$ be scene-$s$'s probability (sum of its scenarios'
`probability_factor`s; scale-invariant below, so no normalization is
required) and

$$
\pi_{m} \;=\; \sum_{r \,:\, m(r) = m} p_r
$$

the state mass.

## 2. LP layout and pricing (the derivation)

Every non-terminal (scene $s$, phase $t$) LP carries $M$ future-cost
columns $\varphi_0, \dots, \varphi_{M-1}$
(uid $= \texttt{sddp\_alpha\_uid} + m$, same uid-offset scheme as
multicut).  Scene-$S$'s backward cut lands on $\varphi_{m(S)}$ and is
broadcast to $\varphi_{m(S)}$ in **every** scene-LP
(`share_cuts_for_phase` mechanics unchanged).  The terminal phase is
unchanged (governed by `boundary_cut_sharing_mode`).

**The Markov-modulated resampled process.**  At phase $t$ in Markov
state $m$: draw a scene $r$ from the within-state conditional
$p_{r|m} = p_r / \pi_m$ (over $m(r) = m$), realize scene-$r$'s stage
data, then transition to $m' \sim P_{m\cdot}$.  Define the post-draw
value $U_r^{(t)}$ and the pre-draw state value $W_m^{(t)}$:

$$
U_r^{(t)}(x) = \min_u \Big[ c_r(u)
  + \sum_{m'} P_{m(r)\,m'}\, W_{m'}^{(t+1)}(x') \Big],
\qquad
W_m^{(t)}(x) = \sum_{r : m(r)=m} p_{r|m}\, U_r^{(t)}(x).
$$

**What $\varphi_{m'}$ stands for.**  A scene-$r$ backward cut supports
scene-$r$'s **folded** LP value $\widehat V_r^{(t+1)}$ (Theorem O1),
which by induction underestimates $p_r U_r^{(t+1)}$.  Define the
folded state aggregate

$$
\Phi_{m'}^{(t+1)}(x) \;=\; \sum_{r : m(r)=m'} p_r\, U_r^{(t+1)}(x)
  \;=\; \pi_{m'}\, W_{m'}^{(t+1)}(x).
$$

$\varphi_{m'}$ is the LP's estimator of $\Phi_{m'}^{(t+1)}$.  The
Bellman recursion above, folded by $p_s$ on scene-$s$'s LP, is

$$
V_s^{(t)}(x) = p_s U_s^{(t)}(x)
  = \min_u \Big[ p_s c_s(u)
  + \sum_{m'} \frac{p_s\, P_{m(s)\,m'}}{\pi_{m'}}\,
    \Phi_{m'}^{(t+1)}(x') \Big],
$$

which fixes the pricing weight of $\varphi_{m'}$ in scene-$s$'s LP:

$$
\boxed{\; w_{s,m'} \;=\; \frac{p_s \, P_{m(s)\,m'}}{\pi_{m'}} \;}
$$

(scale-invariant in the total probability mass: one $p$ factor above,
one below).

**Theorem MK1 (validity).**  *Assume A2 (non-negative stage costs and
terminal FCF, so $U \ge 0$) whenever some state contains more than one
scene; no extra assumption for singleton states.  Then every installed
cut on $\varphi_{m'}$ underestimates $\Phi_{m'}$, every scene-LP value
satisfies $\widehat V_s^{(t)} \le p_s U_s^{(t)}$, and*

$$
LB \;=\; \sum_s \widehat V_s^{(0)}(x_0)
  \;\le\; \sum_s p_s\, U_s^{(0)}(x_0),
$$

*the expected optimum of the Markov-modulated process with the initial
(scene, state) drawn from $p$.*

*Proof (induction over phases, mirroring Theorems O2/M1).*  Base: the
terminal $\varphi$ is pinned at $0$ (the model's definition of zero
post-horizon cost) or bounded by boundary cuts (A5).  Step (LP side):
with all cuts underestimating $\Phi_{m'}^{(t+1)}$ and $w_{s,m'} > 0$,
relaxing the future term only lowers the LP optimum, so
$\widehat V_s^{(t)} \le \min_u [ p_s c_s + \sum_{m'} w_{s,m'}
\Phi_{m'}^{(t+1)} ] = p_s U_s^{(t)}$.  Step (cut side): the fresh cut
from scene $r$'s phase-$(t{+}1)$ solve supports
$\widehat V_r^{(t+1)} \le p_r U_r^{(t+1)}$; under A2,
$p_r U_r^{(t+1)} \le \sum_{r' \in m(r)} p_{r'} U_{r'}^{(t+1)} =
\Phi_{m(r)}^{(t+1)}$ (equality for a singleton state — A2 not needed
there), and the cut lands on $\varphi_{m(r)}$. $\qquad\blacksquare$

## 3. Within-state multiplicity — the v1 decision

When state $m'$ contains $k > 1$ scenes, all their cuts land on the
**same** column $\varphi_{m'}$; the LP drives $\varphi_{m'}$ down to
the highest binding cut, i.e. the cut family converges to
$\max_{r \in m'} p_r \tilde V_r$ — **not** the within-state
expectation.  Consequences (do not hand-wave these):

- **Validity is kept** (Theorem MK1): each cut underestimates the state
  *sum* $\Phi_{m'} = \sum_{r \in m'} p_r U_r \ge \max_r p_r U_r$ —
  but only **under A2**.  With negative stage costs (or a rebased FCF
  making $U < 0$) the within-state domination step fails and the mode
  is uncertified for multi-scene states.
- **Exactness is lost**: $\max_r p_r \tilde V_r < \sum_r p_r \tilde
  V_r$ whenever $k > 1$ scenes carry positive folded value, so the
  recursion is systematically loose (the future term is under-counted
  by up to the multiplicity factor) and the gap cannot close to zero.
  Treating within-state scenes as the within-state expectation would
  require an averaging combinator at cut-landing time (Lemma A1) —
  deliberately not shipped in v1.

**v1 verdict**: the *certified and exact* configuration is
$|\{s : m(s) = m'\}| = 1$ for every state ($M = N$ up to relabeling).
Multi-scene states are accepted but WARN at SDDP setup: the LB is
valid-but-loose under A2 only, and within-state scenes should be
either split into their own states or understood as indistinguishable
realizations whose common tail the shared column approximates from
below.

## 4. Degenerate equivalences (tested)

1. **$M = N$, one scene per state, $P$ rows $=$ normalized scene
   probabilities** ($P_{m(s)\,m(r)} = p_r$): $\pi_{m(r)} = p_r$, so
   $w_{s,m'} = p_s p_r / p_r = p_s$ for every column — exactly the
   corrected multicut pricing of Proposition M4
   (`sddp-cut-validity.md` §8), with identical cut routing
   ($\varphi_{m(S)} = \varphi_S$).  Under uniform probabilities
   $w = 1/N$, byte-identical to the current multicut objective
   coefficients (pre- and post-M4).  Pinned by the oracle equivalence
   test.
2. **$M = 1$**: $\pi_0 = \sum_r p_r$, $P = [1]$, so
   $w_{s,0} = p_s$ — a single future-cost column priced $p_s$
   (single-cut expected SDDP layout).  Every scene's cut lands on the
   one column and, under A2, underestimates the probability-weighted
   mean tail $\Phi_0 = \sum_r p_r \tilde V_r$ (per-scene folded tails
   dominate from below; see §3 for why the family is loose).  Pinned
   at the final transition by the oracle.

## 5. Process mismatch (bounds usage)

**Corollary MK2** (mirror of Corollary M2, `sddp-cut-validity.md` §8):
the forward UB simulates **persistent** per-scene sample paths, while
the markov LB bounds the **Markov-modulated resampled** process; the
two optima are not ordered for heterogeneous scenes.  `LB > UB` under
markov on heterogeneous scenes is a process mismatch, not a cut bug —
the bounds-sanity pin stays WARN-only permanently, and strict
certification is oracle-based (extensive form of the Markov process /
the degenerate cases above).  For identical scenes every process
coincides and the strict `LB ≤ UB` invariant applies at every
iteration.

## 6. Mechanics shared with multicut

- **Cut landing**: `sddp_method_iteration.cpp` targets
  $\varphi_{m(S)}$ via `find_alpha_state_var(..., source_scene =
  SceneIndex{m(S)})` — the state index rides the same uid-offset
  scheme.
- **Broadcast**: `share_cuts_for_phase` installs each cut on every
  scene-LP unchanged (the α column identity is baked into the row).
- **Floors**: `apply_alpha_floor` / `bound_alpha_for_cut` enumerate
  `alpha_cols_on_cell` and floor each $\varphi_m$ from its own cuts —
  the $M$-column layout is picked up generically.
- **Persistence**: saves stay origin-only; the parquet loader's
  broadcast reconstruction (`sddp_cut_parquet.cpp`) fires for markov
  exactly as for multicut.  The stored α coefficient's typed identity
  (`Sddp:alpha:` uid $= \texttt{sddp\_alpha\_uid} + m(S)$) resolves the
  scene→state routing at load time.
- **Forward UB strip**: the realized-opex strip removes the full
  priced future term $\sum_{m} w_{s,m}\, \varphi_m \cdot
  \text{scale\_alpha}$ (same shape as the multicut
  $\sum_r w_r \varphi_r$ strip).  Both the strip and the column
  pricing read the weights from the single mode-aware accessor
  `alpha_col_weights` (which delegates to `markov_alpha_weights` for
  non-terminal markov phases and to the M4 `alpha_unit_cost` rule
  everywhere else — terminal phases included, since the terminal
  layout follows `boundary_cut_sharing`), so the two are structurally
  unable to diverge.

## 7. Future work

- Per-phase scene→state assignment (seasonal Markov chains).
- Within-state expected-cut aggregation (restores exactness for
  multi-scene states via Lemma A1).
- Terminal-α Markov analogue (boundary cuts routed by state).
