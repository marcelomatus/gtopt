# Support — Reference Cases for gtopt Validation

This directory holds the **reference data sets** used to validate the
gtopt LP/MIP backend against the two industry tools it converts from:

| Subdirectory | What's there | Reader |
|---|---|---|
| [`plp/`](plp/README.md) | PLP (*Programación de Largo Plazo*) cases — long-term hydro-thermal scheduling, `.dat`/`.csv` files xz-compressed. | [`scripts/plp2gtopt/`](../scripts/plp2gtopt/) |
| [`plexos/`](plexos/README.md) | CEN PCP daily PLEXOS bundles — outer-wrapper unwrapped to `DATOS{date}.zip.xz` + `RES{date}.zip.xz` (inputs + solution). | [`scripts/plexos2gtopt/`](../scripts/plexos2gtopt/) |

Each subdirectory ships its own `README.md` with the file-format
conventions, the source / fetch protocol, and the canonical reference
case used for end-to-end validation.

## Other entries

The rest of `support/` collects ad-hoc artifacts:

| Path | Role |
|---|---|
| `gtopt_*/` | Converted gtopt cases produced from the above PLP / PLEXOS sources — typically built by running `plp2gtopt` / `plexos2gtopt` and committed for fast smoke testing. |
| `gtopt_long_term.zip` | Zipped snapshot of `gtopt_long_term/` for archival; the unpacked dir is what tests read. |
| `lps/` | Standalone LP dumps used for solver-level regression / debugging. |
| `juan/`, `ivan/` | Per-engineer scratch directories holding larger benchmark cases that live outside the canonical reference set; **gitignored** to keep the tree small. |
| `output/` | Default output destination when a `gtopt` invocation does not pass `--output-dir`; **gitignored**. |

`.gitignore` is configured so `gtopt_*/`, `juan/`, `ivan/`, and
`output/` stay out of source control by default — the converted
cases are sized in tens to hundreds of MB and are rebuilt from the
PLP / PLEXOS sources on demand.

## Canonical reference cases

| Reference | Subdir | Notes |
|---|---|---|
| Irrigation-agreement validation | `plp/2_years/` | Laja + Maule configs (`plplajam.dat.xz`, `plpmaulen.dat.xz`); BAT_ALICANTO reproducer. |
| PLP long-horizon validation | `plp/long_term/` | Full hydrology (seepage on ELTORO / CIPRESES / COLBUN). |
| PLEXOS end-to-end validation | `plexos/pcp_2026-04-22/` | The canonical bundle that all `plexos2gtopt` docs, benchmarks, and the doctest pipeline reference. |
| PLEXOS broader date sweep | `plexos/pcp_2025-10-05/` … `plexos/pcp_2026-05-17/` | 14 bundles spanning Oct 2025 – May 2026 for regression / day-to-day variance studies. |

## See also

- [`docs/scripts/plp2gtopt.md`](../docs/scripts/plp2gtopt.md) — PLP
  converter reference.
- [`docs/scripts/plexos2gtopt.md`](../docs/scripts/plexos2gtopt.md)
  — PLEXOS converter reference (every emitted gtopt element, every
  UC family, the validation pipeline).
- [`docs/scripts/sddp2gtopt.md`](../docs/scripts/sddp2gtopt.md) —
  PSR-SDDP converter (no reference cases vendored here yet).
