# integration_test/data/

Pre-staged compressed bundles consumed by the
[`scripts/sienna_to_gtopt/`](../../scripts/sienna_to_gtopt/) converter.
Each `*.tar.zst` is extracted on the fly by the converter (one
extraction per process, cached to a tempdir; the OS reclaims it at
process exit). The bundled approach keeps the repo small while letting
the integration tests run **fully offline / air-gapped** — no network
access required at CI time.

## Inventory

| File | Compressed | Extracted | Source | Consumer |
|---|---:|---:|---|---|
| `sienna_5bus.tar.zst` | 10.7 KB | ~70 KB | NREL-Sienna `PowerSystemsTestData/5-Bus/` (12 files: branch.csv, bus.csv, gen.csv, generator_mapping.yaml, modifier.jl, reserves.csv, storage.csv, Hydro_Upstream_Input.csv, user_descriptors{,_var_cost}.yaml, timeseries_pointers_{da,rt}.json, data_5bus_pu.jl) | `scripts/sienna_to_gtopt/_bundle.py::_extract_bundle("sienna_5bus")` |
| `sienna_14bus.tar.zst` | 2.9 KB | ~20 KB | NREL-Sienna `PowerSystemsTestData/psy_data/data_14bus_pu.jl` | `scripts/sienna_to_gtopt/_bundle.py::_extract_bundle("sienna_14bus")` |

The 6.4 MB `PJM_5_BUS_7_DAYS.h5` time-series file from the upstream
`5-Bus/` directory is **intentionally excluded** — none of the three
sienna_to_gtopt variants (cascading_hydro, monitored_line, hvdc) use
the dense 7-day SCADA data; the cascade adjacency + bus/branch topology
+ generator catalog are sufficient.

## Sister converters that do NOT bundle data

Two parallel emission ports download their data on first run rather
than ship pre-bundled — both cache to `~/.cache/gtopt/<port>/`:

| Converter | Data origin | Cache location |
|---|---|---|
| `scripts/nrel118_to_gtopt/` | [NREL-Sienna/PowerSystemsTestData/118-Bus](https://github.com/NREL-Sienna/PowerSystemsTestData/tree/master/118-Bus) (several MB of CSVs + annual hourly profiles) | `~/.cache/gtopt/nrel118/` |
| `scripts/rts_gmlc_to_gtopt/` | [GridMod/RTS-GMLC/RTS_Data/SourceData](https://github.com/GridMod/RTS-GMLC/tree/master/RTS_Data/SourceData) (~MB CSVs) | `~/.cache/gtopt/rts_gmlc/` |

These don't ship pre-bundled because (a) the data is larger and (b) the
integration tests use small hand-coded subsets of the fleet (5-8 named
gens) — the full converter is exercised by Python tests that tolerate
the one-time download. If air-gapped CI ever needs them, pre-stage them
to this directory using the same `tar --zstd -cf` approach.

## How to regenerate a bundle

Each bundle was produced from the canonical NREL-Sienna source via the
GitHub API (no `git clone` required). To regenerate
`sienna_5bus.tar.zst` from scratch:

```bash
cd "$(mktemp -d -p ~/tmp sienna-regen-XXXX)"
mkdir sienna_5bus
cd sienna_5bus
for f in branch.csv bus.csv gen.csv generator_mapping.yaml modifier.jl \
         reserves.csv storage.csv Hydro_Upstream_Input.csv \
         user_descriptors.yaml user_descriptors_var_cost.yaml \
         timeseries_pointers_da.json timeseries_pointers_rt.json; do
  gh api repos/NREL-Sienna/PowerSystemsTestData/contents/5-Bus/$f \
    --jq '.content' | base64 -d > "$f"
done
gh api repos/NREL-Sienna/PowerSystemsTestData/contents/psy_data/data_5bus_pu.jl \
  --jq '.content' | base64 -d > data_5bus_pu.jl
cd ..
tar --zstd -cf sienna_5bus.tar.zst sienna_5bus/
mv sienna_5bus.tar.zst /home/marce/git/gtopt/integration_test/data/
```

And `sienna_14bus.tar.zst`:

```bash
mkdir sienna_14bus
gh api repos/NREL-Sienna/PowerSystemsTestData/contents/psy_data/data_14bus_pu.jl \
  --jq '.content' | base64 -d > sienna_14bus/data_14bus_pu.jl
tar --zstd -cf sienna_14bus.tar.zst sienna_14bus/
mv sienna_14bus.tar.zst /home/marce/git/gtopt/integration_test/data/
```

## Why `zstd` and not gzip / xz

Per the project's compression policy
([`feedback_use_zstd_lz4` memory + CLAUDE.md "Compression codecs"
section](../../CLAUDE.md)): zstd is the canonical archival codec across
gtopt (Parquet output, in-memory LP snapshots use lz4 for hot paths,
zstd for cold archive). Reusing `zstd` here keeps the dependency
surface to one decompressor (`zstandard` Python package, already in
`scripts/pyproject.toml`).

The bundles compress 5-7× because the source files are
high-redundancy human-edited CSVs / YAMLs — typical ratios for tabular
data.

## Caveats

- **Do NOT delete or modify the `.tar.zst` files in place** — they are
  the canonical bundled data the sienna_to_gtopt converter consumes at
  test time. Regenerate them via the commands above if upstream
  PowerSystemsTestData changes.
- **`data_5bus_pu.jl` is mostly dynamic-simulation data** (generator
  rotor inertia, AVR / governor parameters) — out of scope for gtopt's
  LP framework. The sienna_to_gtopt converter pulls only the
  steady-state subset (per-unit voltage, base MVA) needed for the
  monitored_line and hvdc variants.
- **The bundle includes a tiny `Hydro_Upstream_Input.csv`** (79 bytes,
  4 rows) — that's the entire cascade adjacency for the 5-bus
  benchmark. The converter pairs its `HydroUnit{1..3}` rows to
  `HydroDispatch{1..3}` rows from `gen.csv` by ORDINAL POSITION
  (Sienna's documented convention; no explicit join key in the CSVs).
