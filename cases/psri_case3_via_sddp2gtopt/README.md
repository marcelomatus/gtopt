# `psri_case3_via_sddp2gtopt` — multi-system PSR SDDP fixture (case3 variant)

End-to-end integration fixture exercising the **multi-system**
converter path through `scripts/sddp2gtopt/` on the upstream PSR
sample case3 — a variant of `psri_case2` with a smaller `Vazao`
(inflow) series.

## Source

`scripts/sddp2gtopt/tests/data/psri_case3/psrclasses.json` — vendored
from
[`psrenergy/PSRClassesInterface.jl`](https://github.com/psrenergy/PSRClassesInterface.jl)
under MPL-2.0 (see `scripts/sddp2gtopt/tests/data/LICENSE-PSRClassesInterface`).

Composition is structurally identical to `psri_case2`:

* 2 `PSRSystem` (one populated, one empty).
* 3 thermal + 2 hydro plants on system 1.
* 1 demand on system 1.

The only difference upstream is the size of the `PSRGaugingStation.Vazao`
historical series — v0 of the converter does not yet consume the
inflow series, so the produced gtopt JSON is byte-identical to
`psri_case2_via_sddp2gtopt`.  We still vendor it as a separate
e2e case so a future converter that *does* consume inflow series
(planned for v2 hydro) immediately produces a distinguishable
fixture and we can pin the new behaviour independently of case2.

## Conversion path

```
scripts/sddp2gtopt/tests/data/psri_case3/psrclasses.json
    │
    │  python -m sddp2gtopt --input-dir … --output-file system_psri_case3_via_sddp2gtopt.json
    ▼
cases/psri_case3_via_sddp2gtopt/system_psri_case3_via_sddp2gtopt.json
```

## Expected results (analytical)

Same reasoning as `psri_case2_via_sddp2gtopt` — all thermals fall
back to `gcost = 0.0` (no PSR cost segments) and hydros are zero-
cost in v0, so the optimal dispatch cost is **exactly 0** for the
populated system; the empty system is trivially feasible.

## CTest coverage

`add_e2e_case(psri_case3_via_sddp2gtopt, system_psri_case3_via_sddp2gtopt.json)`
— same trio (solve / validate_solution / compare_solution) as
`psri_case2_via_sddp2gtopt`.
