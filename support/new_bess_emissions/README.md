# `new_bess_emissions` — PLP 2-year case + PLEXOS overlay + new BESS

A composite gtopt planning case that exercises three converter
features at once:

1. **PLP-base hydrology + topology** — converted from
   `support/plp/2_years/` via `plp2gtopt`.
2. **PLEXOS overlay** — heat-rate + Fuel attachments + per-fuel
   emission factors (CEN-Chile IPCC defaults applied via
   `share/gtopt/emissions/cen_chile.json`) lifted from the most
   recent PLEXOS bundle (`PLEXOS20260517.zip`) and merged onto the
   PLP-base generators by name.
3. **13 new stand-alone BESS projects** — the COD-known subset of
   `Consolidado_de_proyectos_BESS_Stand_Alone_.xlsx` (712.5 MW /
   3 880 MWh), with bus assignments resolved against the
   PLP 2-year bus catalogue (3 direct PLP matches + 2 BFS-resolved via
   Infotecnica `/v1/lineas` + 8 regional proxies; each entry's
   `description` records the rationale).

## Layout

```
support/new_bess_emissions/
├── build.sh                          # end-to-end builder (steps 1-4)
├── run.sh                            # invoke gtopt with both files
├── README.md                         # this file
├── plp_overlay_report.json           # report of what step-3 touched
├── gtopt_plp_plexos_2_years/         # PLP base + PLEXOS overlay applied
│   ├── gtopt_plp_plexos_2_years.json
│   ├── input/                        # per-class Parquet time-series
│   ├── boundary_cuts.csv
│   └── …
├── bess_battery_array.json           # 13 new BESS overlay (separate file)
└── reference/                        # full PLEXOS conversion (NOT merged)
    ├── PLEXOS20260517.json
    ├── input/
    └── …
```

## Build pipeline (`build.sh`)

```
PLEXOS bundle ──plexos2gtopt──▶ reference/PLEXOS20260517.json
                                          │
PLP 2-year case ──plp2gtopt──▶ gtopt_plp_plexos_2_years.json
                                          ▲
                              apply_plexos_overlay()
                                  (1666 gens matched,
                                   211 fuels added,
                                   per-fuel emission_factors carried)

bess_gtopt_battery_array_plp_buses.json  ──renumber─▶  bess_battery_array.json
   (~/tmp source, UIDs 1..13,                          (UIDs 10009..10021,
    bus names as strings)                               bus refs as int uids)
```

UIDs in the BESS overlay are **renumbered above the PLP-merged max
(10008)** so `System::merge`'s append semantic (`source/system.cpp:822`)
doesn't produce duplicate-uid collisions.  Bus references are converted
from string names (`"DonGoyo220"`) to integer uids (`30`) at build
time — matching the PLP-side battery convention.

## Run

```bash
./run.sh                                # default solve
./run.sh --no-mip                       # LP-relax (relaxation lower bound)
./run.sh --lp-only -l my_lp             # build LP, don't solve, write my_lp.lp
./run.sh --json-file merged.json        # write merged JSON for inspection
```

`run.sh` chains both planning files via repeated `-s` flags:

```bash
gtopt -s gtopt_plp_plexos_2_years.json -s ../bess_battery_array.json ...
```

The working directory must be `gtopt_plp_plexos_2_years/` so gtopt
finds the per-class Parquet inputs + `boundary_cuts.csv` next to the
main planning file.

## What the PLEXOS overlay did

Inspect `plp_overlay_report.json` for the full audit.  Headline
counters (from a successful build):

* **1666 generators matched** (PLP ↔ PLEXOS by name).
* **211 fuels added** to the PLP planning (PLP carries no fuels
  natively; PLEXOS contributes the per-Fuel taxonomy +
  `emission_factors`).
* **emission_array** lifted from PLEXOS so gtopt's
  `expand_fuel_emission_sources` has something to attach.

Generators that didn't match (`unmatched_plp_only`,
`unmatched_plexos_only`) are listed in the report; mismatch is
expected and not an error — PLP's renewable / RoR catalogue differs
from PLEXOS's, and PLEXOS adds the CCGT-mode-variant artifacts.

## Regenerating

The build is idempotent — re-run `./build.sh` to rebuild from
upstream changes (new PLP/PLEXOS bundle, updated emissions overlay,
updated BESS spreadsheet, etc.).  All inputs are read by path
(no in-place mutation of the case dir), so re-running cleans up
before regenerating.

## Notes

* **PLEXOS overlay** uses `plp2gtopt._plexos_overlay.apply_plexos_overlay`
  directly (the `--plexos-overlay` CLI flag is referenced in code but
  not yet registered in argparse, so `build.sh` calls the Python
  helper instead).
* **BESS bus assignment confidence**: each Battery entry's
  `description` field carries `bus_resolution=direct|bfs-h1|bfs-h2|regional`
  + the rationale.  See `bess_battery_array.json` for details.
* **`reference/PLEXOS20260517.json`** is NOT in the merge chain —
  it sits there as a self-contained PLEXOS planning for manual
  cross-checks.
