# Guatemala (luis) — gtopt cases

Three case directories here, **one per source archive** in this folder:

| directory | source archive | converter | contents |
|-----------|----------------|-----------|----------|
| **`psse_bdd/`** | `BDD ESO PLP AE MAY26-ABR27 Definitiva.7z` | `psse2gtopt` | 6 PSS®E DC-OPF snapshots (`PAE*`) |
| **`sddp_ncp_week/`** | `26 - Semana del 21 al 27 de junio de 2026 NCP.5.29.b.rar` | `sddp2gtopt` | weekly PSR SDDP/NCP multi-bus DC OPF |
| **`ncp_pdd_22jun/`** | `PDD_22JUN2026_V5.29.b.rar` | `sddp2gtopt` | daily PSR NCP multi-bus DC OPF (water values + inflow + import cap) |

Plus **`reference_data/`** — operator-published data downloaded from AMM/EOR
(hourly spot prices for LMP validation, per-plant CVG, interconnection
transfer limits); see its own `README.md`.

Each case directory also ships a **`src/`** subfolder with the exact
converter inputs **xz‑compressed** (`.raw.xz`, `.dat.xz`, `.csv.xz`,
`.xls.xz`), so every case regenerates **from this repo** — no proprietary
archive needed.  Both converters read `.xz` transparently.

Every `<case>.json` is self-contained (inline `lmax`) — run with
`gtopt -s <path>/<case>.json -d out`.  All solve to optimality with CPLEX.

---

## `psse_bdd/` — PSS®E snapshots

Six DC-OPF snapshots converted from the PSS®E `.raw` network in the 7z
(the AMM "Programación de Largo Plazo 2026-2027" study).  Each is a
runnable case under `psse_bdd/<case>/<case>.json`.

| case          | season           | demand level |
|---------------|------------------|--------------|
| `PAESEPMIN26` | Sep 2026 (wet)   | minimum      |
| `PAESEPMED26` | Sep 2026 (wet)   | medium       |
| `PAESEPMAX26` | Sep 2026 (wet)   | maximum      |
| `PAEMARMIN27` | Mar 2027 (dry)   | minimum      |
| `PAEMARMED27` | Mar 2027 (dry)   | medium       |
| `PAEMARMAX27` | Mar 2027 (dry)   | maximum      |

Single-snapshot multi-bus DC OPF (1 stage / 1 block / 1 scenario):
~871 buses, ~942 lines, ~115 generators, ~255 demands.

Regenerate from the committed **`psse_bdd/src/`** xz inputs (PSS/E v33
`.raw` + AMM spreadsheets), using
[`scripts/psse2gtopt`](../../docs/scripts/psse2gtopt.md):

```bash
psse2gtopt -i psse_bdd/src/PAESEPMED26.raw.xz -o psse_bdd/PAESEPMED26 \
  --nomenclatura "psse_bdd/src/Nomenclatura.xls.xz" \
  --ldm          "psse_bdd/src/LDM Septiembre 2026.xlsx.xz" --rating-set A
gtopt -s psse_bdd/PAESEPMED26/PAESEPMED26.json -d out
```

(SEP cases use *LDM Septiembre 2026*, MAR cases *LDM Marzo 2027*.)

- `--nomenclatura` → human-readable bus names (`AGU-230` → `Aguacapa-230`).
- `--ldm` → rank-based **merit-order** generation costs (SEP cases use
  *LDM Septiembre 2026*, MAR cases *LDM Marzo 2027*).  PSS/E RAW carries
  no costs; the LDM is the only economic signal in the 7z (≈56 of 115
  generators match by name — the converter logs the rate).

Caveats: DC OPF only; **area 8 = Mexico** is an EOR equivalent (~27-35 GW,
not real Guatemalan load ≈ 1.3-2.3 GW in area 1); a recurring ~9.5 MW
unserved at *SID-22* is a real binding transformer rating, not a bug.

---

## `sddp_ncp_week/` + `ncp_pdd_22jun/` — PSR SDDP / NCP

Multi-bus DC OPF converted from the PSR `.dat` collection (the real
economic-dispatch model, **with costs**), single-stage over the dispatch
horizon (1 block per hour).  ~830 buses, ~955 lines, ~220 generators,
~240 demand nodes.

| directory | source | economics applied |
|-----------|--------|-------------------|
| `sddp_ncp_week/` | weekly NCP `.dat` | thermal merit costs + import cap (no water-value file shipped) |
| `ncp_pdd_22jun/` | daily PDD (`PDD_DIARIO`) | thermal merit + **water values** + inflow-limited RoR + **import cap** |

```bash
# regenerate from the committed xz inputs (sddp_ncp_week/src, ncp_pdd_22jun/src)
sddp2gtopt -i sddp_ncp_week/src -o sddp_ncp_week --import-limit 200
sddp2gtopt -i ncp_pdd_22jun/src -o ncp_pdd_22jun --import-limit 200
gtopt -s ncp_pdd_22jun/ncp_pdd_22jun.json -d out
```

**Economics that convert faithfully:** thermal merit costs
(`gcost = CEsp·Custo + CVaria`), per-plant **water values** (`watervcp`
k$/hm³ → $/MWh via `FPMed`; storage priced high, run-of-river free),
inflow-limited run-of-river (`inflow.csv × FPMed`), and the
**interconnection import cap** (`--import-limit 200` — the EOR GUA↔MEX tie
limit; the PSR fuel table prices Mexico imports at $0, so without the cap
~1.8 GW of free imports flood the dispatch).

**LMP cross-check vs PSR `cmgbuscp`** (PDD daily): the import cap is the
decisive lever — it lifts gtopt's LMPs from ~$3 to a **time-mean ≈ 58
$/MWh (peak ≈ 88)**, the right order of magnitude vs PSR's 141 (peak 165)
and the AMM 2025 spot (~80-95).  The residual ~2× gap is the single-stage
limitation: PSR never prices below its **inter-temporal water-value floor**
(~$93 even off-peak), whereas a single-stage snapshot lets free
run-of-river hydro price to ~$0 off-peak.  Closing that needs the
multi-stage reservoir coupling (see `docs/scripts/sddp2gtopt.md`).
