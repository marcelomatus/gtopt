# SDDP Autoregressive Inflow States — AR(1) Design

> **Status**: design + v1 implementation record (2026-07-08), opt-in
> feature.  Companion to `sddp-cut-validity.md` (the theorem document —
> §3 there applies unchanged to the new state) and
> `mathematical-formulation.md` §5.10 (junction water balance).
>
> **Rendering**: GFM math — `$$...$$` display, `$...$` inline.

## 1. Motivation and model

NEWAVE / PSR-SDDP model hydro inflows as a periodic autoregressive
process so that the future-cost function learns the *hydrological
memory*: a dry month predicts a dry next month.  gtopt's scenes are
persistent sample paths — cuts built without an inflow state value
water as if next phase's inflow were independent of this phase's.

The v1 model is AR(1) per element:

$$
q_t \;=\; \mu_t \;+\; \phi\,\bigl(q_{t-1} - \mu_{t-1}\bigr)
\;+\; \varepsilon_t ,
$$

where $q_t$ is the inflow of the `Flow` element at stage $t$, $\mu_t$
is the *mean* series, $\phi \in (-1, 1)$ the lag-1 coefficient, and
$\varepsilon_t$ the residual.  **In gtopt, reservoir inflows enter the
LP exclusively through `Flow` elements** (a fixed $Q_f$ term in the
junction balance of §5.10), so the feature lives on `Flow`; reservoirs
gain AR inflows by attaching the model to their inflow `Flow`.

**v1 key simplification** — *the historical series IS the
realization*: $\mu_t$ is taken from the existing `discharge` schedule
and the forward pass realizes exactly that schedule, i.e.
$\varepsilon_t = 0$ on the forward path.  The AR structure changes
only what the **cuts** see: the backward pass gains a subgradient
$\partial V/\partial q_{t-1}$, so cuts price hydrological memory.
Backward apertures (alternative hydrologies $a_t$) enter as residuals
$\varepsilon_t^a = a_t - \mu_t$.  With $\phi = 0$ and
$\varepsilon = 0$ the model degenerates to today's data exactly.

## 2. LP formulation

Today `FlowLP` emits, per block $b$, a column $q_{t,b}$ **pinned by
bounds** to the schedule ($lo = up = \mu_{t,b}$) with coefficient
$\pm 1$ in the junction balance row.  With `inflow_model` set the
inflow becomes *fixed-in-forward, variable-in-cut-space*:

1. **Free inflow column.**  $q_{t,b}$ keeps its junction-balance
   coefficient but its bounds are released to $(-\infty, +\infty)$ —
   the AR row below pins it instead.  (Unbounded rather than
   $[0,\infty)$: the AR extrapolation $\mu + \phi\,\Delta$ can cross
   zero and v1 does **not** truncate negative inflows, unlike
   NEWAVE's $\max(0,\cdot)$; the cuts are valid for the untruncated
   process.  Documented trade-off: the α box-floor of
   `sddp-cut-validity.md` §10 sees an unbounded state box and falls
   back to the $\alpha \ge 0$ floor — valid under non-negative stage
   costs, assumption A2.)

2. **Lagged-inflow state.**  The *last block* of each stage's inflow
   column is registered as a `StateVariable`
   (`class = Flow, col = inflow`).  At a **cross-phase** boundary the
   consuming stage adds one dependent column $\ell_t$ (`inflow_lag`)
   and queues the standard `PendingStateLink` to the previous phase's
   `inflow` state — the exact `efin → eini` machinery
   (`state_variable.hpp`, `storage_lp.hpp`).  At build time $\ell_t$
   is pinned to the previous stage's schedule reference
   $\mu_{t-1}^{\text{ref}}$ (the last block value), so every non-SDDP
   path solves the identical LP; the SDDP forward pass re-pins it to
   the realized trial value via `propagate_trial_values`, exactly as
   reservoir `sini`.  **Within** a phase (shared LP) the lag is simply
   the previous stage's last-block column — no state variable needed.

3. **AR row** (equality, one per block, all blocks of stage $t$ share
   the stage's lag):

$$
q_{t,b} \;-\; \phi\,\ell_t \;=\;
\mu_{t,b} \;-\; \phi\,\mu_{t-1}^{\text{ref}} \;+\; \varepsilon_t ,
$$

   with $\varepsilon_t$ **data** folded into the RHS ($= 0$ forward).
   With $\ell_t$ pinned at $\mu_{t-1}^{\text{ref}}$ this yields
   $q_{t,b} = \mu_{t,b}$ — LP values identical to today for any
   $\phi$.  The very first stage of the horizon has no lag: its
   columns stay bound-pinned as today (initial inflow condition,
   analogous to `eini`).

**Cut construction is automatic.**  The backward solve pins $\ell_t$
to the trial value $\hat q_{t-1}$; the reduced cost of $\ell_t$ is
$\phi\,\pi^{AR}$ plus the chain through the balance rows — a valid
subgradient $\partial V^{(t)}/\partial q_{t-1}$ by LP sensitivity.
`sddp-cut-validity.md` §3 (Theorems O1/O2) is stated for *any*
registered state variable and applies unchanged: the emitted cut
$\alpha + (-rc_e)\,e + (-rc_q)\,q \ge b$ underestimates the tail value
as a function of **both** the reservoir energy and the lagged inflow.
Within a multi-stage phase the intra-phase chaining propagates the
shift with $\phi^k$, so the tail seen by a cut probed at lag $\ell$ is
the explicit deterministic tail with inflows
$\mu_{t+k} + \phi^k(\ell - \mu_t^{\text{ref}})$ — the exact oracle
used by `test_sddp_ar_inflow.cpp`.

**Apertures.**  Today `FlowLP::update_aperture` re-pins the column
bounds to the aperture hydrology $a_{t,b}$.  With AR active the same
entry point instead rewrites the **AR row RHS**:

$$
\text{rhs}^a_{t,b} \;=\; a_{t,b} \;-\; \phi\,\hat q_{t-1}^{\,a},
$$

where $\hat q_{t-1}^{\,a}$ is the aperture's own previous-stage
reference when the lag is an in-LP column (same phase), and the
forward trial pin ($= \mu_{t-1}^{\text{ref}}$ in v1) at a cross-phase
lag.  The aperture subproblem's primal is therefore unchanged versus
today ($q = a$ at the trial point) while its cut gains the
$\partial V/\partial q$ slope — Theorem AP1 (§6 of the theorem doc)
applies with the enlarged state.

## 3. Configuration

Opt-in per element on `Flow`:

```json
{
  "uid": 1, "name": "inflow_j1", "junction": "j1",
  "discharge": "inflow",
  "inflow_model": { "type": "ar1", "phi": 0.62, "sigma": 14.3 }
}
```

- `type`: only `"ar1"` in v1 (build aborts on anything else).
- `phi`: lag-1 coefficient (default 0.0 — structure present, no
  memory).
- `sigma`: residual std-dev, **carried for tooling only** (the LP
  never reads it; future sampled-eps modes will).

Absent `inflow_model`, every code path is byte-identical to the
pre-feature build: no new columns, no new rows, no state registration
(hard requirement, pinned by the row/column-count parity test).
`inflow_model` with a per-(stage,block) `fcost` on the same element is
unsupported in v1 (the soft-band slack semantics conflict with the AR
equality); `FlowLP` logs a warning and ignores the model there.

## 4. plp2gtopt estimation

`plp2gtopt --inflow-model ar1` estimates, per central with
`plpaflce.dat` data, the residual lag-1 autocorrelation of the
historical hydrology ensemble it already parses:

- $\mu_t$ = cross-hydrology mean per stage (this is the *existing*
  schedule mean; the emitted `discharge` schedule itself is untouched
  — v1 realizes the schedule, per §1);
- $\phi$ = pooled lag-1 autocorrelation of the per-hydrology
  stage-mean residuals $x^h_t = q^h_t - \mu_t$:
  $\phi = \sum_{h,t} x^h_t x^h_{t-1} \big/ \sum_{h,t} (x^h_{t-1})^2$,
  clamped to $[-0.99, 0.99]$;
- $\sigma$ = std-dev of the AR(1) residuals
  $x^h_t - \phi\, x^h_{t-1}$.

The result is emitted as the `inflow_model` object on each generated
`Flow` element.  Elements with degenerate series (constant flow, a
single stage) are left without a model.

## 5. Roadmap: PAR(p) (stretch, not in v1)

PAR(p) generalizes to per-calendar-month coefficients
$\phi_{m(t),1..p}$.  The LP shape is unchanged — the AR row gains
$p$ lag terms and each phase must export the last $p$ stage inflows
as states (a vector of `inflow`, `inflow_2`, … state variables).
Estimation is the per-month Yule–Walker fit over the hydrology
ensemble.  Deliberately **punted** from v1: the single-lag row already
exercises every new mechanism (pinned column, cross-phase link, cut
coefficient, aperture RHS), and PAR(p)'s only new machinery is the
$p$-deep state export.

## 6. References

1. M. E. P. Maceira, J. M. Damázio, "Use of the PAR(p) model in the
   stochastic dual dynamic programming optimization scheme used in
   the operation planning of the Brazilian hydropower system",
   *Probability in the Engineering and Informational Sciences* 20,
   143–156 (2006) — the NEWAVE PAR(p) inflow model.
2. M. V. F. Pereira, L. M. V. G. Pinto, "Multi-stage stochastic
   optimization applied to energy planning", *Math. Programming* 52
   (1991).
3. In-repo: `docs/formulation/sddp-cut-validity.md` (cut validity for
   arbitrary registered states); `docs/formulation/
   mathematical-formulation.md` §5.10 (junction water balance).
