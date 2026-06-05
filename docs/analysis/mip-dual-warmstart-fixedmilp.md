# MIP → dual-LP warm start via `CPXPROB_FIXEDMILP`

**Status:** proposal / analysis (no code changed)
**Scope:** CPLEX backend dual pass after a MIP solve (LMPs / row duals on the
committed unit-commitment solution).
**Observed cost:** the fixed-integer LP re-solve on the 10-05 CEN PCP MIP takes
**≈101 s** (barrier 46 s + crossover 55 s), ~7 % of the 1,477 s solve — repeated
once per case (×14 in a MIP-loop run, ×scene·phase in SDDP).

---

## 1. What the dual pass does today

After `CPXmipopt`, gtopt computes row duals / reduced costs on the *committed*
solution by pinning the integers and re-solving as an LP.  The caller is
`SystemLP` (monolithic) and the SDDP forward pass:

- `source/system_lp.cpp:1622` → `li.fix_integers_and_resolve(lp_opts)`
- `source/sddp_forward_pass.cpp:270` → `li.fix_integers_and_resolve(effective_opts)`

`LinearInterface::fix_integers_and_resolve` (`source/linear_interface.cpp:3096`)
does three things:

1. **Snapshot** the MIP-optimal primal (`get_col_sol_raw()`, `:3104`).
2. **Pin** every integer column to its rounded incumbent value
   (`pinned = std::round(mip_sol[c])`, `:3136`) via one bulk
   `set_col_bounds_raw(idx, 'B', vals)` — `'B'` pins **both** lower and upper
   (`:3138`, `:3148`). → integers are fixed to the incumbent.
3. **Relax** integrality (`relax_integers()`, `:3154`) then **resolve**
   (`resolve(opts)`, `:3160`).

`relax_integers()` (`:3062`) dispatches to the backend bulk relaxation
`relax_all_integers()`; the CPLEX implementation
(`plugins/cplex/cplex_solver_backend.cpp:969`) is:

```c
CPXchgprobtype(env, lp, CPXPROB_LP);   // MILP → LP relaxation
```

and `resolve()` (`:1263`) calls `CPXlpopt`, which respects
`CPXPARAM_LPMethod = 4` (barrier) from `solvers/cplex.prm`.

## 2. Dual semantics: already correct

The earlier open question — *does the current path give committed-solution duals
or pure-relaxation duals?* — is settled by step 2: integers are **pinned to the
rounded incumbent** with `'B'` bounds before the LP solve.  So the row duals are
the **committed-solution LMPs**, which is what we want.  **Switching to
`CPXPROB_FIXEDMILP` does not change the dual semantics** — it fixes the same
integers to the same incumbent. The change is purely about *how fast* the LP
that produces those duals is solved.

## 3. The inefficiency

The MIP and the dual LP run on the **same** `m_env_lp_.lp()` object, so the
incumbent node's optimal simplex basis is present right after `CPXmipopt`. But:

- `CPXchgprobtype(env, lp, CPXPROB_LP)` (`:990`) converts the integer columns to
  continuous and **drops the advanced basis** — CPLEX does not carry the MIP
  start into the relaxation problem type.
- With the basis gone, `CPXPARAM_LPMethod = 4` solves **cold barrier**, which
  starts from the analytic center and then must **crossover** to a vertex basis.

That crossover (55 s of the 101 s) is **reconstructing from scratch the very
vertex basis the MIP already produced** at the incumbent node. The `.prm`
comment documents *why* barrier was forced — "dual simplex did not converge
cold-start in 10 min" — but that benchmark is **cold**; it does not apply to a
warm start.

## 4. The fix: `CPXPROB_FIXEDMILP` + warm simplex

CPLEX has a purpose-built problem type for exactly this:

```c
// after CPXmipopt, on the same lp object:
CPXchgprobtype(env, lp, CPXPROB_FIXEDMILP);   // fix integers to incumbent +
                                              //   install incumbent-node basis
CPXsetintparam(env, CPXPARAM_LPMethod, CPX_ALG_DUAL);  // or PRIMAL
CPXlpopt(env, lp);                            // warm-starts → few pivots
```

`CPXPROB_FIXEDMILP` (a) fixes every discrete variable to its incumbent value
(handling the rounding internally — replacing the manual `std::round` + `'B'`
pin) **and** (b) hands the LP solver the incumbent node's optimal basis. A
primal/dual simplex off that basis is a handful of pivots: **no barrier, no
crossover** — seconds instead of ~100.

## 5. Implementation shape (fits the plugin architecture)

`fix_integers_and_resolve()` / `relax_all_integers()` are the **solver-agnostic**
API (CLP/CBC/HiGHS/Gurobi/MindOpt all implement the bulk relax). `FIXEDMILP` is
CPLEX-specific, so isolate it:

1. Add a backend capability query + method, e.g.
   `bool supports_fixed_mip_duals()` and
   `int fix_mip_duals(LPMethod warm_method)`.
2. **CPLEX**: `CPXchgprobtype(CPXPROB_FIXEDMILP)`; temporarily set
   `CPXPARAM_LPMethod` to `CPX_ALG_DUAL`/`CPX_ALG_PRIMAL` for the warm solve,
   restore afterward (the global `LPMethod 4` must stay for the **cold** root /
   relaxation solves — only the warm dual pass overrides it); `CPXlpopt`.
3. **`fix_integers_and_resolve`**: if `supports_fixed_mip_duals()`, take the fast
   path; else fall back to the current snapshot → pin → relax → resolve (barrier)
   for the other backends, which is correct, just slower.

### LPMethod must stay context-dependent
Do **not** flip the global `cplex.prm` `LPMethod` to simplex — the cold MIP-root
LP relaxation genuinely needs barrier (the `.prm` benchmark). Only the *warm*
`FIXEDMILP` dual pass should request simplex, locally and restored after.

## 6. Expected impact & caveats

- **Impact:** the ~101 s fixed-LP solve collapses to seconds (no barrier, no
  crossover). ≈100 s × 14 cases ≈ **20+ min** off a MIP-loop run; per
  scene·phase in SDDP the aggregate is larger.
- **Numerics:** worth recording `kappa` from `solution.csv` before/after — the
  warm simplex basis is the MIP's, so conditioning should be comparable or
  better (no barrier ill-conditioning, no crossover noise).
- **Precondition:** `FIXEDMILP` requires the MIP to have an incumbent
  (optimal/feasible). When there is none the current path is also meaningless;
  guard identically.
- **Rounding parity:** `FIXEDMILP` fixes to CPLEX's stored incumbent integers;
  the manual `std::round(mip_sol[c])` (`:3136`) becomes unnecessary on the CPLEX
  path (it stays for the fallback).
- **No dual-semantics change** (§2) — output LMPs are unchanged; only wall-clock.

## 7. Citations

| Element | Location |
|---|---|
| Dual-pass caller (monolithic) | `source/system_lp.cpp:1622` |
| Dual-pass caller (SDDP) | `source/sddp_forward_pass.cpp:270` |
| `fix_integers_and_resolve` | `source/linear_interface.cpp:3096` |
| Pin integers to incumbent (`'B'`) | `source/linear_interface.cpp:3136,3148` |
| `relax_integers` → backend | `source/linear_interface.cpp:3062,3077` |
| `CPXchgprobtype(CPXPROB_LP)` | `plugins/cplex/cplex_solver_backend.cpp:990` |
| `CPXlpopt` (dual solve) | `plugins/cplex/cplex_solver_backend.cpp:1263` |
| `LPMethod 4` (barrier, forced) | `solvers/cplex.prm` |
