# PLEXOS Reference Bundle — CEN PCP Daily LP Inputs & Solution

This directory holds **one daily** PLEXOS bundle from the Coordinador
Eléctrico Nacional (CEN, Chilean ISO) "Programa de Coordinación de
Predespacho" (PCP) — the **ground-truth daily unit-commitment LP** that
the Chilean system runs every operating day. The artefact lives here so
a future `plexos2gtopt` tool can be developed against a fixed, known
input set without needing to re-fetch from the CEN portal.

## Source

- Portal: <https://programa.coordinador.cl/operacion/pcp/bases-modelo>
- API: `administracion.api.coordinador.cl/programa-operacion/`
- Bundle: `PLEXOS20260422.zip` — published 2026-04-21, state
  *Definitivo* (final).
- Fetcher: [`cen2gtopt.pcp_archive`](../../scripts/cen2gtopt/pcp_archive.py).
  See its module docstring for the full discovery protocol (public
  browse-only USERKEY → presigned S3 URLs).

The bucket is geo/IP-restricted; downloads succeed only from Chilean
IPs (or via an authorised relay). Once the bundle is in this directory
the rest of the workflow runs offline.

## Refreshing or fetching another day

From the project root:

```bash
# List recent definitive bundles
python -m cen2gtopt.pcp_archive list --pattern '^PLEXOS' --since 2026-05-01

# Download a specific date into support/plexos
python -m cen2gtopt.pcp_archive download \
       --name PLEXOS20260518.zip \
       --output support/plexos
```

If the connection fails with `Connection reset by peer`, the bucket is
rejecting non-Chilean clients — retry from a permitted network and copy
the resulting zip here.

## Layout

```
support/plexos/
├── README.md                          ← this file
├── DATOS20260422.zip.xz       (3.3 M) ← LP inputs (1421 files)
└── RES20260422.zip.xz          (18 M) ← LP solution (3 files)
```

The two inner zips have been extracted from the CEN outer wrapper
`PLEXOS20260422.zip` (32 M) and recompressed with `xz -9` for repo
storage — the outer wrapper is dropped because it just bundles the two
inner zips with `store` compression and adds nothing beyond a single
download artifact. Total disk footprint shrank 80 M → 22 M while
keeping the original CEN file identities byte-exact and recoverable.

### Reconstructing the original CEN files

```bash
# Recover the inner zips verbatim
xz -dk DATOS20260422.zip.xz RES20260422.zip.xz   # produces .zip files

# Re-bundle into the outer wrapper if needed (rare)
zip -0 PLEXOS20260422.zip DATOS20260422.zip RES20260422.zip
```

### Extracting the payloads directly

```bash
# Inputs (1421 files: PLEXOS XML + per-class CSVs)
xz -dkc DATOS20260422.zip.xz | funzip > /tmp/datos_first_file  # single-file
xz -dk  DATOS20260422.zip.xz && unzip DATOS20260422.zip        # all files

# Solution (PLEXOS Access DB + log)
xz -dk RES20260422.zip.xz && unzip RES20260422.zip
```

## What is inside `DATOS20260422.zip` — the LP inputs

The 1421-file payload pairs a PLEXOS XML object database with per-class
CSV time-series. The XML is the schema; the CSVs are the data.

| File | Size | Role |
|---|---|---|
| `DBSEN_PRGDIARIO.xml` | 36 M | **PLEXOS XML object database** (4 500+ objects across 96 classes; generators, fuels, lines, nodes, reservoirs, reserves, constraints) |
| `PLEXOS_Param.xml` | 3 KB | Run parameters (horizon, solver settings) |
| `PLEXOS_SolverParam.xml` | 0.6 KB | Solver tuning |
| `Gen_Rating.csv` | 10 M | Per-unit hourly Pmax (rating, in MW) |
| `Gen_UnitsOut.csv` | 10 M | Forced-outage schedule per unit per hour |
| `Gen_MinStableLevel.csv` | — | Per-unit Pmin |
| `Gen_HeatRate.csv` | — | Per-unit heat rate |
| `Gen_StartCost.csv`, `Gen_ShutDownCost.csv`, `Gen_VOMCharge.csv` | — | Cost components |
| `Fuel_Price.csv` | — | Monthly fuel prices |
| `Fuel_MaxOfftakeWeek.csv` | — | TOP / take-or-pay |
| `Hydro_StoWaterValues.csv` | 0.4 KB | Water value per reservoir |
| `Hydro_WaterFlows.csv` | 60 KB | Natural inflow time series |
| `Hydro_MaxVolume.csv`, `MinVolume.csv`, `InitialVolume.csv` | — | Reservoir bounds + initial state |
| `Hydro_EfficiencyIncr.csv`, `Hydro_MaxRampDay.csv`, `Hydro_AntucoBounds.csv` | — | Cascade-specific constraints |
| `Lin_MaxRating.csv`, `Lin_MinRating.csv`, `Lin_Units.csv` | — | Line topology + flow bounds |
| `Nod_Load.csv` | 500 KB | Per-bus per-hour load (wide CSV) |
| `Res_Requirement.csv` | 45 KB | Reserve requirements |
| `Res_Timeslice.csv` | — | Reserve time-slice mapping |
| `SSCC_Activation_BESS.csv`, `ReserveUsageTxCompensation.csv` | — | Ancillary services |

The XML format is PLEXOS native:

```xml
<MasterDataSet xmlns="http://tempuri.org/MasterDataSet.xsd">
  <t_attribute><attribute_id>1</attribute_id>…</t_attribute>
  …
</MasterDataSet>
```

Tables of interest for `plexos2gtopt`:

- `t_class` — object classes (e.g. *Generator*, *Fuel*, *Line*, *Node*)
- `t_object` — concrete instances
- `t_collection` — relationships (e.g. `Generator → Fuel`)
- `t_property`, `t_attribute` — schema metadata
- `t_data`, `t_band`, `t_text` — actual values + multi-band tariffs
- `t_memo_data` — per-time-slice or per-hour overrides
- `t_membership` — owner→collection→child triples
- `t_action` — operational events (outages, derates)
- `t_message`, `t_unit` — units (MW, m³/s, $, etc.)

## What is inside `RES20260422.zip` — the solution

| File | Size | Role |
|---|---|---|
| `…/Solution/Model … Log.txt` | 58 KB | Solver log (CPLEX, kappa, gap) |
| `…/Solution/Model … Solution.accdb` | 190 M | Microsoft Access DB — **all primal + dual + auxiliary outputs** |
| `…/Solution/Model … Solution.zip` | 3 M | Solution metadata (sparse) |

The `.accdb` is the main artefact. PLEXOS post-processes via Access /
SQL Server; for cross-platform reads use [`mdbtools`](https://github.com/mdbtools/mdbtools)
(`mdb-tables`, `mdb-export`) — already a project apt dependency.

```bash
mdb-tables "Model PRGdia_Full_Definitivo Solution.accdb"
mdb-export "Model …Solution.accdb" Generation
mdb-export "Model …Solution.accdb" Price
```

## Notes for `plexos2gtopt`

A future converter should ingest, at minimum:

1. **Topology** — `t_class` + `t_object` + `t_membership` → gtopt
   `Bus`/`Line`/`Generator`/`Fuel`/`Reservoir`/`Junction`/`Waterway`/
   `Turbine` arrays.
2. **Schedules** — the per-class CSVs in `DATOS*.zip` map to gtopt's
   `OptTBRealFieldSched` per-(stage, block) schedules. The PCP horizon
   is 24 hourly blocks × 1 stage; gtopt's block/stage decomposition is
   a direct match.
3. **Reserves** — `Res_Requirement.csv` + `Res_Timeslice.csv` → gtopt
   `ReserveZone` + `ReserveProvision`.
4. **Hydro cascade** — `Hydro_*.csv` → `Reservoir` + `Waterway` +
   `Turbine` + `FlowRight` entries (irrigation-style rights are
   embedded in `Hydro_AntucoBounds.csv` and similar).
5. **Costs** — `Fuel_Price.csv` + `Gen_HeatRate.csv` →
   `Generator.fuel` + `Generator.heat_rate`; ancillary cost columns
   → `Generator.gcost` and the new `EmissionSource` entity.

The naming-dialects registry (`share/gtopt/naming_dialects.json`)
already has the PLEXOS column-name aliases for most fields — see
entries with `"dialect": "plexos"`.

## Bundle metadata

```
PLEXOS20260422.zip
  category:    PCP
  date:        2026-04-22
  state:       Definitivo
  published:   2026-04-21 18:15 UTC-4
  total size:  50 681 914 bytes (uncompressed inner zips)
```
