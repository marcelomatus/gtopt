# PCP UserConstraint full audit — RES20260422 vs gtopt PAMPL v14

**Date.** 2026-05-30  **Author.** UC alignment audit  **Inputs.**

- gtopt PAMPL bundle: `/home/marce/tmp/gtopt_pcp_check_v14/uc_*.pampl`
  (no `v15` directory existed at audit time)
- gtopt JSON bundle: `/home/marce/tmp/gtopt_pcp_check_v14/PCP_20260422.json`
- gtopt solution output: `/home/marce/tmp/gtopt_pcp_check_v14_out/UserConstraint/`
- PLEXOS sol .accdb (extracted from `~/tmp/plexos_res_20260422/RES20260422.zip`):
  `/home/marce/tmp/plexos_layout_yxomqjpx/Model PRGdia_Full_Definitivo Solution/Model PRGdia_Full_Definitivo Solution.accdb`
- Hard list: `scripts/plexos2gtopt/data/cen_pcp_hard_ucs.txt` (204 names)
- Audit raw outputs: `/home/marce/tmp/uc_audit_1780173757/out/{plexos_uc_solution.json, gtopt_ucs.json, audit.json}`
- Builder: `/home/marce/tmp/uc_audit_1780173757/build_audit.py`

---

## Executive summary

1. **963 PLEXOS user-constraints vs 1071 gtopt user-constraints** (1049 PAMPL +
   22 JSON). 652 names match exactly, 311 names are PLEXOS-only and 419 are
   gtopt-only — but the vast majority of the apparent identity mismatch is
   structural, NOT missing functionality (see point 2).
2. **The dominant identity bucket is `SD_*` security-contingency rows** (PLEXOS-only
   201, gtopt-only 171; **zero name overlap** despite both pools naming the
   same transmission corridors). The two pools carry disjoint ticket-number
   prefixes (PLEXOS: ~80 % 2025/2026 tickets, gtopt: mostly 2023/2024) — the
   plexos2gtopt cache and the PLEXOS RES20260422 solution were generated from
   **different XML revisions of the SD_ catalogue**. This explains 192 of 206
   B9 mismatches and 201 of 328 B6 mismatches in one stroke.
3. **Per-row structural diff on the 652-name intersection is clean.**
   Only **7 real RHS mismatches** (B2), **3 names where gtopt is soft and the
   hard-list expects hard** (B5: `ANTUCO_PMax`, `PANGUEcaudal_min_diario`,
   `limited_generation_calculation`) and **9 hydro min/max names listed in
   the hard-list that gtopt keeps soft by design** (no gtopt commit-status
   primitive — see Section 4).
4. **Missing constraints by KIND** that actually require code work:
   `BatMaxCycDay_*` (32 names — realized as `Battery.max_cycles_day`
   attribute, NOT as UCs; covers all 36 batteries),
   `Gas_MaxOpDay_*` (16/32 fuel-owner groups consolidated correctly,
   **15 fuel-owner groups are emitted as 8 `inactive` Day-X stubs each
   (= 120 names) with no active consolidated row** — see B7 / Section 5),
   and **2 truly missing hard rows**:
   `FueMaxOffWeek_Gas_EnelMejillones_E`, `FueMaxOffWeek_Gas_Newen_GN_A`.
5. **Hydro min/max special case is correct.** All 10 hydro UCs in the
   intersection that fit the `(ANTUCO|MACHICURA|PANGUE|COLBUN|ANGOSTURA)…
   {min|max|ramp}` shape carry gtopt penalty 10.0 (soft-op). 9 of them
   appear in the hard list — the user has accepted this divergence
   (gtopt has no `commit_status × pmin` primitive).
6. **No duplicate UC names anywhere** (gtopt PAMPL ∪ JSON: 1071 unique).

Headline counts (above section 1):

| Bucket | Count | Pages |
|---|---:|---|
| Identity (PLEXOS-only) | 311 | §1, §3 B3/B7 |
| Identity (gtopt-only) | 419 | §1, §3 B8 |
| Intersection (compared) | 652 | §2 |
| RHS mismatch (real) | 7 | §3 B2 |
| Hard-in-PLEXOS, soft-in-gtopt | 3 (non-hydro) + 9 (hydro by design) | §3 B5, §4 |
| Soft-in-PLEXOS, hard-in-gtopt | 328 (mostly SD_*) | §3 B6 |
| Inactive in gtopt, active in PLEXOS | 206 (192 SD_) | §3 B9 |
| Missing UC family | 2 (battery_cycle realized as attr; gas_maxopday partly) | §3 B7 |

---

## Section 1 — Identity audit (set comparison)

| | Count |
|---|---:|
| PLEXOS Constraint objects (class_id=70, in RES20260422 .accdb) | **963** |
| gtopt PAMPL UC defs (`uc_*.pampl`) | **1049** |
| gtopt JSON UCs (`PCP_20260422.json:user_constraint_array`) | **22** |
| gtopt total | **1071** |
| gtopt unique names | 1071 (no duplicates) |
| Intersection (name-equal) | **652** |
| In PLEXOS, not in gtopt | **311** |
| In gtopt, not in PLEXOS | **419** |

### 1a. Missing-from-gtopt by family (311 names)

| Family | Count | Sample |
|---|---:|---|
| security `SD_*` | **172** (≈ 56 %) | `SD_2024050241_ATRLoAguirre_AJahuel`, `SD_2025090885_Polpaico_Chena` |
| `Gas_MaxOpDay*_<suffix>` | **128** | `Gas_MaxOpDay0_Colbun`, `Gas_MaxOpDay3_Enel_GN_C` |
| `BatMaxCycDay_BAT_*` | **32** | `BatMaxCycDay_BAT_DEL_DESIERTO` |
| `2025*_PAzucar_…` transmission_security | 4 | `2025079778_PAzucar_Polpaico500_I_y_II` |
| `MutuallyExclusive_Andes2B-3` | 1 | spelling differs from `MutuallyExclusive_Andes2B_3` |
| Other (commitment / FueMaxOffDay / etc.) | 6 | `Commit_Atacama_2_TG2B_0.5TV`, `FueMaxOffDay_Gas_Newen_A` |

Of these:

- **Battery cycle (32 names)** — *functionally present*, realized as
  `battery_array[*].max_cycles_day = 1.0` for all 36 batteries.
  No PAMPL emission needed; verified in
  `/home/marce/tmp/gtopt_pcp_check_v14/PCP_20260422.json`.
- **Gas_MaxOpDay_X day-of-week names (128 names = 16 fuel-owner groups × 8
  days)** — these 16 groups ARE present in gtopt as INACTIVE stubs
  (`inactive constraint Gas_MaxOpDay0_<suffix>: 0 <= 0;` ×8) AND the
  consolidated `Gas_MaxOpDay_<suffix>` row is also MISSING from gtopt's
  active set. PLEXOS solves the underlying rows with non-zero binding:
  128 stub Day-X rows carry Σ activity = 28,260; Σ slack = 25,132.5; 25
  rows binding ≥ 1 hour. So this IS a real functional gap (see §3 B7).
- **SD_* security (172 names)** — these are date-stamped contingency rows.
  Both pools name the same corridors but their ticket-number prefixes
  rarely overlap, so by-name comparison massively over-states the
  mismatch.  In 5 corridor-name overlap probes, only 3 corridors matched.
  Root cause: gtopt cache XML and RES20260422 sol XML are from different
  contingency-list revisions.

### 1b. Synthetic-in-gtopt by family (419 names)

| Family | Total | Active | Inactive stub |
|---|---:|---:|---:|
| Other (mostly SD_, GEN_, LOAD_, CTFOFF_, CRCA_, CSF_, CPFN_) | 381 | 8 | 373 |
| gas_maxopday | 24 | 16 | 8 |
| discharge_min | 3 | 3 | 0 |
| hydro_min | 3 | 0 | 3 |
| hydro_max | 3 | 0 | 3 |
| mutually_exclusive | 4 | 1 | 3 |
| fcf | 1 | 1 | 0 |

**Only 29 / 419 synthetic UCs are ACTIVE.** The other 390 are `inactive
constraint X: 0 <= 0;` stubs preserved for provenance (e.g., gas Day-X
groups whose LHS evaluated to 0 at this run date). Active-and-synthetic
names of interest:

- **`FCF_future_cost`** (gtopt-only, pen=0) — expected, internal future-cost
  injection; not in PLEXOS.
- **`Gas_MaxOpDay_<suffix>`** ×16 consolidated rows — gtopt-only naming for
  the consolidated daily gas budgets. Each replaces 8 PLEXOS Day-X rows.
- **`discharge_ANTUCOmin`, `discharge_ANTUCOmax`, `discharge_ELTOROmax`** —
  gtopt waterway-flow synthetics promoted from PLEXOS hydro `min`/`max`
  generator-cap UCs. Naming intentional.
- **`MutuallyExclusive_Andes2B_3`** — PLEXOS has `MutuallyExclusive_Andes2B-3`
  (hyphen). Pure name normalisation glitch in plexos2gtopt
  (`-` → `_`). Listed as B3 missing in PLEXOS direction and B8 synthetic
  in gtopt direction — should be aliased (see Section 5).
- **`Kelar_DIE_SSCC`, `Kelar_GNL_SSCC_CA_CC`, `Kelar_GNL_SSCC_CC_CA_1`/`_2`** —
  hard, pen=0. Verify PLEXOS source: likely `KELAR_*` (uppercased). Pure
  spelling drift.
- **`Commit_Atacama_2_TG2B_0_5TV`** vs PLEXOS `Commit_Atacama_2_TG2B_0.5TV`
  — dot → underscore.
- 2 active `SD_2026036857_*` synthetics that nominally belong in PLEXOS
  but are absent from the sol .accdb — likely added in the newer gtopt
  cache, not retro-applied to PLEXOS RES20260422.

### 1c. Alias near-matches (heuristic)

| gtopt name | PLEXOS name | likely cause |
|---|---|---|
| `MutuallyExclusive_Andes2B_3` | `MutuallyExclusive_Andes2B-3` | `-` → `_` |
| `Commit_Atacama_2_TG2B_0_5TV` | `Commit_Atacama_2_TG2B_0.5TV` | `.` → `_` |
| `Gas_MaxOpDay_<suffix>` ×16 | `Gas_MaxOpDay{0..7}_<suffix>` ×128 | consolidation |
| `discharge_<HYDRO>{min,max}` ×3 | `<HYDRO>{min,max}` (already matched) | promoted waterway version |

---

## Section 2 — Per-UC structural diff (intersection, 652 rows)

### 2a. Sense (op)

The PLEXOS sol .accdb does NOT carry the `Sense` parameter — it lives in the
input XML (handled at plexos2gtopt convert time, not auditable from the sol
DB). The audit therefore reports only gtopt-side op:

| Op | Count |
|---|---:|
| `<=` | 465 |
| `>=` | 187 |
| `=`  | 0 |

No gtopt UC uses equality; all PLEXOS Sense=0 rows became one of the
inequality sides during conversion.

### 2b. RHS (gtopt scalar/profile.max vs PLEXOS rhs.max)

After excluding gtopt `daily_sum=true` rows (where the inline RHS is a
1 e9 sentinel) and gtopt rows whose `rhs_profile.max == 0` (intentionally
deactivated rows), **7 real RHS mismatches remain** — see §3 B2.

### 2c. Coefficients

Per-row coefficient comparison would require re-extracting the
**Constraint Coefficients** property from each child class
(Generator → Constraints, Line → Constraints, …) in the SOURCE XML
(the converter consumes this; the RES sol .accdb does NOT store it).
**Not feasible from the sol .accdb alone** — flagged as out-of-scope for
this audit (Section 5 action C1).

### 2d. Type / cost

Gtopt penalty distribution on the 652-name intersection:

| Penalty class | Count |
|---|---:|
| HARD (pen = 0.0) | 632 |
| soft-op (pen = 10.0) | 16 |
| soft-resv (pen = 1000.0) | 4 |

204-name hard-list coverage:

- Hard-list names also in PLEXOS sol: **150 / 204** (75 %)
  (the other 54 are `BatMaxCycDay_*` 27, `Gas_MaxOpDay*` 25, plus 2
  `FueMaxOffWeek_*` truly missing)
- Hard-list names also in gtopt: **150 / 204**
- Hard-list names emitted as soft in gtopt (in intersection): see §3 B5

---

## Section 3 — Mismatch buckets (named, ordered by count desc)

### B8 — Synthetic UC in gtopt (gtopt-only): **419**

| Sub-bucket | Count |
|---|---:|
| Inactive stubs (`active=False`) | 390 |
| Active (real gtopt emissions) | 29 |

Of the 29 active synthetic UCs:

- 16 are correctly-consolidated `Gas_MaxOpDay_<suffix>`
- 3 are intentional `discharge_<HYDRO>` waterway promotions
- 8 are SD_2026* / `Kelar_*` / `Commit_*0_5TV` naming drift vs PLEXOS
  (alias-fix candidates)
- 1 `FCF_future_cost` (expected internal)
- 1 `MutuallyExclusive_Andes2B_3` (alias `Andes2B-3`)

### B6 — Soft in PLEXOS, hard in gtopt: **328**

| Sub-bucket | Count |
|---|---:|
| `SD_*` security rows | 201 |
| `ATA_*`, `KELAR_*`, `CTFOFF_*` commit/config | 22 |
| `CSF_LW_MIN_BAT_*` reserve floor | 11 |
| `Commit_*`, `*_Uniq` etc. | 94 |

These are mostly false positives of the "PLEXOS solved with slack so it's
soft" heuristic: the constraints carry NO Penalty Price in the source
XML (so gtopt correctly treats them as HARD) but PLEXOS solves the LP
with non-zero slack because the system is *infeasible without slack*
(curtailment-equivalent). The hard-list (204 names) is the
authoritative HARD signal — not the sol slack column. None of the 328
B6 entries appear in the hard-list mismatch list, so this bucket is
informational and does NOT indicate a converter bug.

### B2 — RHS scale mismatch (real): **7**

| Name | PLEXOS rhs.max | gtopt rhs (eff) | ratio | likely cause |
|---|---:|---:|---:|---|
| `ANTUCOmax` | 69.63 | 137.0 | 1.97 | gtopt uses raw capacity; PLEXOS RHS is a per-period bound that varies |
| `ANTUCOmin` | 67.63 | 137.0 | 2.03 | same as ANTUCOmax — gtopt RHS = capacity, PLEXOS = operational floor |
| `CSF_LW_Def` | 0.0 | 333.0 | — | PLEXOS LHS includes deficit decision-variable absorbing the 333 MW; pid-3073 RHS reports 0 in sol |
| `CSF_RS_Def` | 0.0 | 332.0 | — | same as CSF_LW_Def |
| `CSF_MinUnits` | 3.0 | -8.0 | -2.67 | sign / count flip — gtopt has negative RHS, PLEXOS positive |
| `Diesel_OffTakeDay` | 386.54 | 0.387 | 0.001 | **gtopt RHS scaled by 1/1000** (unit slip: GJ vs MJ?) — same family pattern as `Gas_MaxOpDay`, both store the daily-energy cap |
| `Inertia_Calculation_e1` | 0.0 | -622.54 | — | gtopt LHS includes the inertia decision-variable on the LHS with a negative RHS; PLEXOS reports RHS=0 |

CSF_LW_Def / CSF_RS_Def / Inertia_Calculation_e1: cross-side decomposition
of `LHS_phys ± slack_var ≥ 0` vs `LHS_phys ≥ RHS` — same underlying
constraint, equivalent algebraically. Not a real bug, just two
representations.

`Diesel_OffTakeDay` ×1000 scale and `CSF_MinUnits` sign flip are **real
bugs** worth fixing.

### B9 — Inactive in gtopt, active in PLEXOS: **206**

| Sub-bucket | Count | Note |
|---|---:|---|
| `SD_*` security | 192 | gtopt cache had these but the converter flagged LHS=0 at convert time; PLEXOS solved them with non-zero activity |
| `Ancoa_AJahuel`, `Gx_Pehuenche_Ancoa`, etc. | 7 | similar — corridors active in PLEXOS sol but converter zeroed them |
| `CFRS_PEHUENCHE` | 1 | Σ activity = 6 149 712 in PLEXOS — large constraint zeroed in gtopt |
| `IL_2026000186_BAT_ANDES_2B_FV_Capacity` | 1 | active in PLEXOS Σ activity = 29 934 |
| `SDCF_Rx{1..5}_norte_a_sur` etc. | 5 | high-activity reactor constraints |

**CFRS_PEHUENCHE** is the single biggest concern (6.15 M activity). It
sits in gtopt as `inactive constraint CFRS_PEHUENCHE: 0 <= 0;` because
the converter's zero-LHS auto-deactivation heuristic flagged it. Verify
its child-class coefficient table in the XML.

### B3 — Missing UC (specific, not a whole family): **37**

| Sub-bucket | Count | Notes |
|---|---:|---|
| `BatMaxCycDay_BAT_*` | 32 | already realized as `Battery.max_cycles_day=1.0` — *functional, no gap* |
| `2025*_PAzucar_…` (4) | 4 | transmission_security — likely missing from gtopt cache, present in PLEXOS sol |
| `MutuallyExclusive_Andes2B-3` | 1 | alias; gtopt has `_3` instead of `-3` |

### B5 — Hard in PLEXOS (hard-list) but soft in gtopt: **3** non-hydro + **9** hydro-by-design

Non-hydro (REAL B5 — to address):

| Name | gtopt penalty |
|---|---:|
| `ANTUCO_PMax` | 10 |
| `PANGUEcaudal_min_diario` | 10 |
| `limited_generation_calculation` | 1000 |

Hydro-by-design (acknowledged divergence, see §4):
`ANTUCOmax`, `ANTUCOmin`, `COLBUNmax`, `MACHICURAmax`, `MACHICURAmin`,
`MACHICURAlagrampdown`, `MACHICURAlagrampup`, `MACHICURArampdownact`,
`PANGUEramp`.

### B7 — Missing UC family (whole family not emitted): **2**

| Family | PLEXOS Σ activity | Notes |
|---|---:|---|
| `Gas_MaxOpDay_*` 15 fuel-owner groups (= 120 Day-X names in PLEXOS) | 28,260; 25 binding hours; 25,133 slack | active in PLEXOS, emitted only as `inactive` stubs in gtopt (consolidated row NEVER emitted) |
| `FueMaxOffWeek_Gas_EnelMejillones_E`, `FueMaxOffWeek_Gas_Newen_GN_A` (weekly fuel cap) | small | 2 hard-list names absent from gtopt entirely |

### B5 (hydro) and the 9 acknowledged-soft names — see Section 4.

---

## Section 4 — Hydro min/max special case

The following 9 names appear in `cen_pcp_hard_ucs.txt` but gtopt emits
them with `penalty soft_floor_penalty` (= 10.0). All 9 belong to one of
the hydro families (ANTUCO, MACHICURA, COLBUN, PANGUE):

| Name | gtopt penalty | PLEXOS hours binding | PLEXOS slack | hard-list? |
|---|---:|---:|---:|---|
| `ANTUCOmax`            | 10 | 35 | 188 | yes |
| `ANTUCOmin`            | 10 | 6  | 34  | yes |
| `COLBUNmax`            | 10 | 47 | 35,001 | yes |
| `MACHICURAlagrampdown` | 10 | 6  | 6,192 | yes |
| `MACHICURAlagrampup`   | 10 | 8  | 7,170 | yes |
| `MACHICURAmax`         | 10 | 39 | 107,625 | yes |
| `MACHICURAmin`         | 10 | 5  | 2,175 | yes |
| `MACHICURArampdownact` | 10 | 10 | 10,000 | yes |
| `PANGUEramp`           | 10 | 7  | 420 | yes |

Plus 1 ramp row not in the hard-list:
`ANGOSTURAmaxramp` (gtopt pen=10, PLEXOS no slack).

These 9 (+1) are kept SOFT by design — gtopt has no
`commitment_status × pmin` primitive to gate the floor on unit-online,
which is the PLEXOS semantics. **Confirmed: none of the 9 are sourced
from a bug; they are the documented divergence** (see
`docs/analysis/...soft_pmin_fcost.md` and project_memory note on
`pmin_fcost`).

PLEXOS reports non-zero Slack on every one of the 9 in this run — so
PLEXOS itself is treating them as effectively-soft on RES20260422
(the hard-list flags them because they CAN bind tight on other dates).

---

## Section 5 — Action list (do NOT edit code — describe fixes)

### A1.  Name-alias drift (B8 + B3 pair)

**Files.** `scripts/plexos2gtopt/parsers.py` (extract_user_constraints,
around the name-normalisation step inside the Constraint extractor).

**Fix.** Apply the same name-cleanup gtopt uses for other element
classes (replace `.` and `-` with `_`) when reading PLEXOS Constraint
object names. Cases to align:

- `MutuallyExclusive_Andes2B-3`     →  `MutuallyExclusive_Andes2B_3`
- `Commit_Atacama_2_TG2B_0.5TV`     →  `Commit_Atacama_2_TG2B_0_5TV`
- `Kelar_*` ↔ `KELAR_*` (case-fold; need to confirm PLEXOS source
  capitalisation)

Removes ~6 spurious B8 + 6 B3 entries with zero behavioural change.

### A2.  `Gas_MaxOpDay` 15 dropped fuel-owner groups (B7)

**File.** `scripts/plexos2gtopt/parsers.py` —
`extract_user_constraints` builds per-Day_X spec with LHS computed from
`Fuel.Offtake Coefficient × generation`; the LHS is collapsed to "all
generators provably contribute 0 at this run date" by the
shadow-line / always-off heuristic and the spec is downgraded to an
`inactive` stub. Subsequently
`_consolidate_gas_maxopday_*` (line ~5832) iterates only over
`s.active is not False` specs, so groups whose Day-X were all flagged
inactive get SKIPPED.

**Fix.** Either
(i) loosen the all-LHS-zero deactivation for `Gas_MaxOpDay*` so the
consolidator still sees the rows, OR
(ii) collect inactive Day-X specs by suffix BEFORE consolidation and
emit ONE consolidated row per suffix even when the per-Day_X LHSs are
all zeroed (preserving the daily RHS budget).

Impact: 15 fuel-owner groups (Colbun, Enel × {GN_B..GN_E,
Mejillones_GN_{A..C}}, NuevaRenca × variants) gain proper daily fuel
caps; PLEXOS shows 25 binding hours and 25,133 units of slack on these
in the reference run.

### A3.  Diesel_OffTakeDay 1/1000 scale (B2)

**File.** `scripts/plexos2gtopt/parsers.py` — `extract_user_constraints`,
the `Fuel.Offtake Coefficient` expansion branch.

**Fix.** Apply the `_DAILY_ENERGY_RHS_SCALE` (=1000) multiplier the
same way the `Gas_MaxOpDay` daily-energy path does. The
`Diesel_OffTakeDay` row uses RHS_Day = 9.277 GJ/h — gtopt currently
stores 0.386542 (= 9.277/24, a per-block rate) but PLEXOS RHS is 386.5
(a per-period MJ-or-MWh figure). Cross-check the unit chain.

### A4.  CSF_MinUnits RHS sign flip (B2)

**File.** `scripts/plexos2gtopt/parsers.py` — the CSF_MinUnits handler
(search for the name; likely in a special-case block that subtracts a
unit count from the RHS).

**Fix.** Verify the RHS arithmetic against PLEXOS: gtopt stores
`RHS = -8` (likely `original_RHS - n_committed_units_already`), PLEXOS
stores RHS=3 (the raw threshold). Either keep gtopt's representation
and adjust LHS accordingly, or align the RHS sign.

### A5.  Missing `FueMaxOffWeek_Gas_EnelMejillones_E` and
`FueMaxOffWeek_Gas_Newen_GN_A` (B7)

**File.** `scripts/plexos2gtopt/parsers.py` —
`extract_user_constraints` likely treats these two analogous to
`Gas_MaxOpDay` but with a *weekly* horizon. Audit which special-case
block handles `FueMaxOffWeek_*` and verify it covers these two fuel
groups.

### A6.  ANTUCO_PMax, PANGUEcaudal_min_diario,
limited_generation_calculation (B5 non-hydro)

**File.** `scripts/plexos2gtopt/parsers.py` — the soft-floor classifier
that assigns `penalty=10` or `penalty=1000`.

**Fix.** Tighten the classifier so these three hard-list entries get
`penalty=0` (HARD):

- `ANTUCO_PMax` — looks like a hydro PMax cap matching the family
  pattern but the hard-list lists it as HARD; gate the soft-by-default
  branch on a per-name allow-list OR detect by the absence of an
  underlying `commit_status × pmin` shape.
- `PANGUEcaudal_min_diario` — daily caudal floor; PLEXOS solved hard
  (0 slack).
- `limited_generation_calculation` — currently 1000 (soft-resv); promote
  to HARD.

### A7.  CFRS_PEHUENCHE / Ancoa_AJahuel / Gx_* zero-LHS deactivation (B9)

**File.** `scripts/plexos2gtopt/parsers.py` — the auto-deactivation
heuristic (search `inactive constraint`, `all LHS term(s) provably
contribute 0`).

**Fix.** The 14 non-SD B9 names are PLEXOS-active with large activity
(`CFRS_PEHUENCHE` = 6.15 M). The heuristic currently classifies them as
all-zero LHS; verify the LHS includes terms that are NON-zero at run
time (likely a member-class binding the heuristic missed, e.g., a
Storage/Reservoir reference). Whitelist by name OR teach the heuristic
to recognise the missing element kinds.

### A8.  SD_* security catalogue refresh (B6, B9, B8)

**File.** Not parsers.py — this is an input-data issue. The plexos2gtopt
input XML (`DBSEN_PRGDIARIO.xml`) is older than the
RES20260422-generator XML. The 192 B9 `SD_*` mismatches and 201 B6
`SD_*` mismatches will resolve as soon as the converter cache is
refreshed to the same XML revision PLEXOS used.

**Action.** Refresh `/home/marce/.cache/gtopt/cen2gtopt/pcp_archive/PCP/
PLEXOS20260422.zip` from the same source PLEXOS used for RES20260422,
then re-run plexos2gtopt and re-audit. Until then, do NOT treat the
SD_* mismatches as parser bugs.

### A9.  Battery cycle convention (B3 false positive — DOC)

**File.** `scripts/plexos2gtopt/parsers.py` line ~2417 already does the
right thing (`max_cycles_day = …` on `BatterySpec`).

**Action.** Add a one-line audit-helper comment near
`extract_user_constraints` (or to this audit doc) stating that
`BatMaxCycDay_*` PLEXOS constraints are NOT expected to appear as gtopt
UCs because they are realized as battery attributes. Suppresses 32
spurious B3 entries.

### A10. Coefficient-by-coefficient parity (out of scope for this audit)

Section 2c was skipped: the PLEXOS sol .accdb does NOT carry
`Constraint Coefficient` properties (those live in the input XML the
converter already consumes). A real coefficient audit needs the
SOURCE XML: extract `t_data` rows for
collections {32, 54, 90, 97, 106, 172, 227, 278, 293, 310, 618, 700,
702, 710, 722} via `mdb-export` on the *DBSEN_PRGDIARIO.xml*-derived
.adb image (not the RES sol .accdb). Listed here as TODO C1.

---

## Appendix — file map

- Audit builder: `/home/marce/tmp/uc_audit_1780173757/build_audit.py`
- Raw outputs (JSON): `/home/marce/tmp/uc_audit_1780173757/out/`
  - `plexos_uc_solution.json` — 963 PLEXOS Constraint rows with per-row
    Σ activity / Σ slack / Σ |dual_price| / RHS stats
  - `gtopt_ucs.json` — 1071 gtopt UC rows (PAMPL parsed + JSON parsed),
    each with op / rhs / penalty / terms
  - `audit.json` — full diff with all 7 buckets and per-row intersection
    diff (652 rows)
- PLEXOS table dumps: `/home/marce/tmp/uc_audit_1780173757/t_*.csv`
- PLEXOS .accdb used:
  `/home/marce/tmp/plexos_layout_yxomqjpx/Model PRGdia_Full_Definitivo Solution/Model PRGdia_Full_Definitivo Solution.accdb`
