# Task: small MIP-start round-trip prototype + per-solver unit test + fix the loading

Repo: `/home/marce/git/gtopt` (C++26, gtopt GTEP LP/MIP solver, pluggable
solver backends). Build: `cmake -S all -B build -G Ninja -DCMAKE_BUILD_TYPE=CIFast
-DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 -DCMAKE_C_COMPILER_LAUNCHER=ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` then `cmake --build build -j20` and
`cd build && ctest --output-on-failure -j20`. Default solver CPLEX.

## Goal & invariant

Build a SMALL, self-contained prototype and a doctest unit test that validate the
MIP-start (warm-start) LOADING mechanism end to end, for EVERY MIP-capable backend
(cplex, highs, gurobi, mindopt, cbc, scip):

> Solve a tiny MIP, capture its COMPLETE optimal solution (integer AND continuous
> column values, plus the basis), reload that exact solution as a MIP start, and
> verify the backend ACCEPTS it and re-solves to the same optimum with ~zero extra
> work (0 B&B nodes / ~0 simplex iterations, wall ≤ the cold solve).

**Invariant to enforce:** reloading a complete, consistent, OPTIMAL solution as a
MIP start MUST be accepted and cost ≤ the cold solve. If a backend reports
"No solution found from N MIP starts" (CPLEX) when handed the EXACT optimal, the
loading path is broken. Loading a complete, near-feasible start must be *repaired
easily* by the solver — the user's premise is that "cargar toda la solución no
debería tener problema; CPLEX la repara fácil."

## Background: why (a real bug found while debugging the CEN PLEXOS UC MIP)

On the real 04-12 CEN case (~860k cols, ~120k integers) the round trip FAILS:
1. Solve cold → optimal (obj ≈ 9.73e8).
2. Dump integers via `--set monolithic_options.mip_start.dump_file=sol.dump`
   (writes header `# gtopt mip_start integer solution (index value)` / `ncols N` /
   `nint K` then `<idx> <val>` per INTEGER col; wrote 120,670 integers correctly).
3. Reload via `--set monolithic_options.mip_start.from_file=sol.dump
   --set monolithic_options.mip_start.inject.effort=check_feasibility` →
   **"No solution found from 1 MIP starts"** (instant, before presolve) then solves
   cold. Reloading the EXACT optimal should be trivially accepted.

Root-cause hypotheses to confirm/fix (code pointers):
- `FileMipStart::generate` (`source/mip_start.cpp` ~L160-254) reads the dumped
  integers and OVERLAYS them onto `ctx.li.get_col_sol_raw()` — the RAW **relaxation**
  primal (continuous = LP-relaxation dispatch). So the reconstructed start =
  {optimal integers} ∪ {relaxation continuous} → MUTUALLY INCONSISTENT.
- It returns `std::nullopt` early at `if (sol.empty()) return std::nullopt;`
  (~L185). Suspect `get_col_sol_raw()` is EMPTY at that point → FileMipStart yields
  no start → the pipeline SILENTLY falls back to the round candidate (rounded
  relaxation), which is what actually gets injected and rejected. The
  "MIP-start[file]: replayed N (M skipped)" info line (~L246) never appeared in the
  logs — verify whether generate() even reaches it.
- The CPLEX plugin `set_mip_start` (`plugins/cplex/cplex_solver_backend.cpp`
  ~L1540-1590) ALREADY filters to a SPARSE **integer-only** start (drops continuous,
  because a dense start with mismatched continuous makes CHECKFEAS reject). So the
  continuous-mismatch is *supposed* to be handled — yet the optimal is still
  rejected. So either `get_col_sol_raw()` is empty (round fallback), or the integer
  indices/values reaching CPLEX are wrong. The tiny prototype will disambiguate.
- CPLEX effort levels (symbolic constants, version-safe): CHECKFEAS *fails* if the
  start is infeasible; SOLVEFIXED/SOLVEMIP/REPAIR complete/repair; NOCHECK trusts
  verbatim. The plugin maps gtopt `inject.effort` → these correctly.

## The small prototype

Extend the closed-form fixture already in `test/source/test_mip_start_per_solver.cpp`:
```
min 10 x + 1 y   s.t. x + y >= 5,  x >= 0 continuous,  y in [0,3] integer
optimum: y=3, x=2, obj=23
```
Make it slightly richer so the round trip is non-trivial but still solves in
microseconds: a 2-generator / 2-block unit-commitment mirrors the real structure —
u binaries (commitment) + generation continuous + a demand-balance row + Pmin·u ≤ p
≤ Pmax·u links. 3-6 integer cols is plenty. Build it directly with
`LinearInterface`/`SparseCol`/`SparseRow` (see the existing test), OR as a tiny
`from_json<Planning>` case — either is fine; the LinearInterface route is simpler
and backend-uniform.

## The per-solver round-trip test (`test/source/test_mip_start_roundtrip.cpp`)

For each `gtopt::solver_test::exact_mip_solvers()` (drops LP-only + thread-unsafe
cuOpt; see helper), `CAPTURE(name)`:
1. Build + `initial_solve` the tiny MIP → REQUIRE optimal. Capture `obj_cold`, the
   COMPLETE solution `sol = get_col_sol_raw()` (integers + continuous), and
   `basis = get_basis()`.
2. Fresh `LinearInterface` with the SAME model. Inject `set_mip_start(sol, effort)`
   with the COMPLETE solution; `set_basis(basis)`; `resolve`.
3. ASSERT: accepted, same obj (`doctest::Approx`), and ~0 extra work — a
   backend-agnostic proxy: solve reaches optimality with the injected objective as
   the first/only incumbent (0 improving B&B nodes). If the backend exposes an
   iteration/node count, assert it is ≪ cold.
4. Perturbation case: flip ONE non-critical binary in the complete start → the
   backend must REPAIR it back to the optimum (exercises "repair a near-feasible
   complete start"). CPLEX effort `solve_fixed`/`repair` as appropriate.
5. Also add a `dump_file` → `from_file` ROUND-TRIP at the gtopt_main / SystemLP
   level if feasible (a tiny JSON case), to cover the file path that actually
   broke on CEN — solve, dump, reload, assert accepted + ≤ cold.

Follow `test_mip_start_per_solver.cpp` conventions: SPDX header, `#include
<doctest/doctest.h>` first, `using namespace gtopt;` at file scope (no NOLINT —
`test/.clang-tidy` covers it), `doctest::Approx` for FP, trailing commas in
brace-init lists, gate on `exact_mip_solvers()` non-empty.

## The fix (make the round trip pass)

1. Persist+restore the COMPLETE, CONSISTENT solution: the dump should carry the
   continuous column values too (or all columns), and `FileMipStart` should
   reconstruct the start from the DUMPED continuous, NOT overlay optimal integers on
   `get_col_sol_raw()` (the relaxation). Consistent start ⇒ CHECKFEAS accepts.
2. `FileMipStart` must NOT silently fall back to the round candidate when
   `get_col_sol_raw()` is empty — surface it (warn) and prefer the dumped values.
3. Verify the CPLEX sparse-integer-only `set_mip_start` accepts the exact optimal
   integers on the tiny case (fixing optimal integers ⇒ optimal LP ⇒ feasible). If
   it still rejects on the prototype, the bug is index-mapping / effort, not the
   continuous mismatch — fix there.
4. Keep it backend-uniform: whatever the fix, the per-solver test must pass for
   cplex/highs/gurobi/mindopt/cbc/scip (skip a backend cleanly if no license).

## Deliverables
- `test/source/test_mip_start_roundtrip.cpp` (new, per-solver, gated).
- Source changes in `source/mip_start.cpp` (dump/from_file complete-solution) and,
  only if needed, `plugins/cplex/cplex_solver_backend.cpp`.
- `ctest -R mip_start -j20` green; pre-commit clang-format + clang-tidy clean on
  touched `.cpp` (never `.hpp`).

## Do NOT
- Do not "fix" it by loosening the acceptance test — the invariant (reload optimal
  ⇒ accepted, ≤ cold) is the point.
- Do not touch the huge CEN case; the prototype is the unit of work.
