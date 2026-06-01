# Support — PLEXOS Reference Bundles

Daily PCP (Programa de Coordinación de Predespacho) bundles from the
**Coordinador Eléctrico Nacional (CEN, Chilean ISO)** — the ground-
truth daily unit-commitment LP that the Chilean system runs every
operating day.  Each bundle pairs the LP inputs (`DATOS{date}.zip`)
with the PLEXOS-solved outputs (`RES{date}.zip`) so `plexos2gtopt`
can be driven against a stable archive and `compare_with_plexos` can
read the reference solution back without re-fetching from the CEN
portal.

See [`docs/scripts/plexos2gtopt.md`](../../docs/scripts/plexos2gtopt.md)
for the converter's full input ledger and end-to-end pipeline.

## Layout

```
support/plexos/
├── README.md                          ← this file
├── pcp_2025-10-05/
│   ├── DATOS20251005.zip.xz   (~3.4 M)  ← LP inputs (DBSEN_PRGDIARIO.xml + per-class CSVs)
│   └── RES20251005.zip.xz     (~28  M)  ← PLEXOS solution database (.accdb)
├── pcp_2025-10-19/
├── pcp_2025-11-09/
├── pcp_2025-11-23/
├── pcp_2025-12-07/
├── pcp_2025-12-21/
├── pcp_2026-01-04/
├── pcp_2026-01-18/
├── pcp_2026-02-15/
├── pcp_2026-03-15/
├── pcp_2026-04-07/                    ← reference bundle (per-bundle README inside)
├── pcp_2026-04-12/
├── pcp_2026-04-22/                    ← canonical reference (per-bundle README inside)
└── pcp_2026-05-17/
```

Currently 14 bundles, ~310 MB total compressed.  The directories are
named `pcp_{YYYY-MM-DD}` (calendar date of the operating day the
bundle covers, the day the `PLEXOS{YYYYMMDD}.zip` outer wrapper from
CEN was generated for).

## File-format conventions

The PCP archive ships as `PLEXOS{YYYYMMDD}.zip` from the CEN portal —
an outer wrapper containing the two inner zips
`DATOS{date}.zip` (LP inputs, 1 400+ files) and `RES{date}.zip`
(PLEXOS solution database, a single `.accdb` + sibling logs).  We
unwrap the outer once, **xz-compress** each inner zip with `xz -6
-T0`, and store the result here.  Rationale:

* The inner zips compress further under xz (3.4 MB DATOS,
  ~20 MB RES) because their content is mostly already-zipped CSVs +
  one large MasterDataSet XML that xz handles better than zip's
  per-entry DEFLATE.
* `plexos2gtopt.plexos_loader.locate_bundle` reads either form
  transparently: pass it the `DATOS{date}.zip.xz`, the outer
  `PLEXOS{date}.zip`, a directory containing either, or an
  already-extracted directory.
* The xz form keeps git history readable and saves ~30 % vs.
  storing the raw outer wrapper.

If you have a fresh CEN download in `PLEXOS{date}.zip` form, unwrap it
into this layout with:

```bash
mkdir -p support/plexos/pcp_${DATE_DASH}
TMP=$(mktemp -d)
unzip -q PLEXOS${D8}.zip -d "$TMP"
xz -6 -T0 -c "$TMP/DATOS${D8}.zip" > support/plexos/pcp_${DATE_DASH}/DATOS${D8}.zip.xz
xz -6 -T0 -c "$TMP/RES${D8}.zip"   > support/plexos/pcp_${DATE_DASH}/RES${D8}.zip.xz
rm -rf "$TMP"
```

(where `DATE_DASH=2026-04-22` and `D8=20260422`).

## Source

- **Portal:** <https://programa.coordinador.cl/operacion/pcp/bases-modelo>
- **API:** `administracion.api.coordinador.cl/programa-operacion/`
- **Fetcher:** [`cen2gtopt.pcp_archive`](../../scripts/cen2gtopt/pcp_archive.py) —
  see its module docstring for the full discovery protocol (public
  browse-only USERKEY → presigned S3 URLs).

The bucket is geo/IP-restricted; downloads succeed only from Chilean
IPs (or via an authorised relay).  Once a bundle is in this directory
the rest of the workflow runs offline.

## Refreshing or fetching another day

```bash
# List recent definitive bundles
python -m cen2gtopt.pcp_archive list --pattern '^PLEXOS' --since 2026-05-01

# Download a specific bundle into the local cache
python -m cen2gtopt.pcp_archive download --name PLEXOS20260518.zip

# The downloader leaves it in ~/.cache/gtopt/cen2gtopt/pcp_archive/PCP/;
# unwrap + xz-compress into support/plexos/ via the snippet above.
```

If the connection fails with `Connection reset by peer`, the bucket
is rejecting non-Chilean clients — retry from a permitted network and
copy the resulting zip here.

## Using a bundle

```bash
# Convert (uses pcp_2026-04-22 as the canonical reference)
plexos2gtopt support/plexos/pcp_2026-04-22/DATOS20260422.zip.xz \
  -o gtopt_PLEXOS20260422

# Compare against the PLEXOS solution (RES*.zip.xz)
plexos2gtopt --compare gtopt_PLEXOS20260422/DATOS20260422.json \
  support/plexos/pcp_2026-04-22/DATOS20260422.zip.xz

# Solve with CPLEX
gtopt gtopt_PLEXOS20260422/DATOS20260422.json --solver cplex
```

The converter auto-discovers the sibling `RES{date}.zip.xz` for the
PLEXOS block-layout (`t_phase_3`, 111 variable-duration blocks per
CEN PCP week) under the default `--horizon-mode plexos`.

## Canonical reference bundle

`pcp_2026-04-22/` is the reference bundle for end-to-end validation
of the converter and the gtopt LP/MIP backend.  Its README documents
the conversion results on that specific day; tests, doctests, and
benchmarks all point at it.  Pick a different bundle only when
diagnosing a date-specific issue.

## See also

- [`../plp/README.md`](../plp/README.md) — sibling PLP (Programación
  de Largo Plazo) cases.
- [`docs/scripts/plexos2gtopt.md`](../../docs/scripts/plexos2gtopt.md)
  — converter reference (every emitted gtopt element, every UC
  family, CLI, validation pipeline).
- [`scripts/plexos2gtopt/`](../../scripts/plexos2gtopt/) — converter
  source.
