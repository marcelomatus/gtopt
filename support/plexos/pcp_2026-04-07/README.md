# PLEXOS Reference Bundle — CEN PCP Daily LP Inputs & Solution (2026-04-07)

This directory holds **one daily** PLEXOS bundle from the Coordinador
Eléctrico Nacional (CEN, Chilean ISO) "Programa de Coordinación de
Predespacho" (PCP) — the **ground-truth daily unit-commitment LP** that
the Chilean system runs every operating day. It is a **second reference
day** alongside [`../plexos_pcp_2026-04-22`](../plexos_pcp_2026-04-22),
so `plexos2gtopt` can be validated across more than one operating day
without re-fetching from the CEN portal.

The payload layout, XML schema tables, and per-class CSV documentation
are identical to the 04-22 bundle — see that sibling's `README.md` for
the full reference. Only the date-specific facts differ and are recorded
below.

## Source

- Portal: <https://programa.coordinador.cl/operacion/pcp/bases-modelo>
- API: `administracion.api.coordinador.cl/programa-operacion/`
- Bundle: `PLEXOS20260407.zip` — state *Definitivo* (final).
- Fetcher: [`cen2gtopt.pcp_archive`](../../scripts/cen2gtopt/pcp_archive.py).
  This day was recovered from the local fetcher cache
  (`~/.cache/gtopt/cen2gtopt/pcp_archive/PCP/PLEXOS20260407.zip`); the
  CEN S3 bucket is geo/IP-restricted to Chilean clients.

## Refreshing or fetching another day

From the project root:

```bash
python -m cen2gtopt.pcp_archive download \
       --name PLEXOS20260407.zip \
       --output support/plexos_pcp_2026-04-07
```

## Layout

```
support/plexos_pcp_2026-04-07/
├── README.md                          ← this file
├── DATOS20260407.zip.xz       (3.5 M) ← LP inputs
└── RES20260407.zip.xz          (18 M) ← LP solution
```

The two inner zips were extracted from the CEN outer wrapper
`PLEXOS20260407.zip` (32 M) and recompressed with `xz -9` for repo
storage — the outer wrapper just bundles the two inner zips with `store`
compression. Total disk footprint shrank ~50 M → 22 M while keeping the
original CEN file identities byte-exact and recoverable.

### Reconstructing the original CEN files

```bash
# Recover the inner zips verbatim
xz -dk DATOS20260407.zip.xz RES20260407.zip.xz   # produces .zip files

# Re-bundle into the outer wrapper if needed (rare)
zip -0 PLEXOS20260407.zip DATOS20260407.zip RES20260407.zip
```

### Extracting the payloads directly

```bash
# Inputs (PLEXOS XML object DB + per-class CSV time-series)
xz -dk DATOS20260407.zip.xz && unzip DATOS20260407.zip

# Solution (PLEXOS Access DB + log)
xz -dk RES20260407.zip.xz && unzip RES20260407.zip
```

## Bundle metadata

```
PLEXOS20260407.zip
  category:    PCP
  date:        2026-04-07
  state:       Definitivo
  inner mtime: 2026-04-06 17:54 UTC-4
  inner zips:  DATOS20260407.zip  6 433 372 bytes
               RES20260407.zip   44 119 805 bytes
  total size:  50 553 177 bytes (uncompressed inner zips)
```
