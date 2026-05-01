# sddp2gtopt test fixtures

Each subdirectory is a standalone PSR SDDP case used by the test
suite. Cases are vendored or hand-crafted to exercise specific code
paths in the parsers and writer.

| Case                  | Source       | What it covers                                               |
|-----------------------|--------------|--------------------------------------------------------------|
| `case0/`              | upstream PSR | Real PSR sample (1 system, 3 thermal, 1 hydro, 2 stations)   |
| `case_min/`           | hand-crafted | Smallest valid case (1 thermal, 1 demand, 1 stage, 1 block)  |
| `case_thermal_only/`  | hand-crafted | Thermals only, multi-fuel, 3 stages, 2 blocks/stage          |
| `case_two_systems/`   | hand-crafted | Two PSRSystem — must trigger a clear conversion error        |
| `case_bad_no_study/`  | hand-crafted | Missing PSRStudy — must fail `--validate`                    |
| `case_bad_truncated/` | hand-crafted | Truncated JSON — must surface a parse error                  |

`case0/` was copied from `psrenergy/PSRClassesInterface.jl/test/data/`
under the Mozilla Public License 2.0 (see
`LICENSE-PSRClassesInterface`). Hand-crafted cases use the same
collection schema PSR writes natively, so adding a new fixture only
requires authoring a `psrclasses.json` — no PSR SDDP install needed.
