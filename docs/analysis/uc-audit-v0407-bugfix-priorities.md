# UC audit v0407 — bugfix priorities (tangent_signed_flow)

Audit input: `/tmp/uc_audit_v0407.json` (922 PLEXOS UCs vs 1080 gtopt UCs,
662 intersection, 260 missing-from-gtopt, 418 synthetic-in-gtopt).

PLEXOS bundle: `~/.cache/gtopt/cen2gtopt/pcp_archive/PCP/PLEXOS20260407.zip`.
gtopt converted output: `/home/marce/tmp/gtopt_pcp_v0407_LIFT_CAP/`.

## TL;DR

After cross-referencing the audit JSON against the PAMPL files, the
converted planning JSON and the C++ LP code, **only one bucket carries a
genuine data-loss / encoding bug**, and it accounts for 4 of the 6 B2
RHS-mismatch findings:

1. **P0 — ANTUCO/ELTORO discharge UCs emitted twice** (B2 RHS bucket,
   `ANTUCOmin` / `ANTUCOmax`; the `discharge_*` mirror is also still
   present). When the matching hydro generator already has a Commitment
   (CEN PCP ships one for every hydro unit), `_auto_promote_hydro_min_max_
   to_commitments` skips promotion **and does not drop the duplicate raw
   `<plant>min/max` UC nor its `discharge_<plant>min/max` mirror**.
   Result: the LP gets both encodings —
   `1 * ANTUCO_U1.gen >= 137` (raw MW, wrong unit interpretation, ONLY
   ANTUCO_U1) AND `0.625 * (ANTUCO_U1+U2).gen >= 83.30` (correct m³/s).
   The two encodings are not equivalent. Bug locus:
   `scripts/plexos2gtopt/parsers.py:8952` (`if gen_name in committed:
   continue` short-circuits both the new Commitment AND the drop of the
   dual rows). Fix: drop the duplicates regardless of whether a new
   Commitment was synthesised.

2. **P0 — All 33 B3 "BatMaxCycDay_BAT_*", 5 "GenMaxStartsWeek_*", the
   B7 "fuel_offtake_week" (82) + B7 "gas_maxopday" (78) + B7
   "transmission_security" (57) entries are by-design native-primitive
   promotion**, not data loss. Each is verified end-to-end (parser →
   writer → JSON → C++ LP):
   * `Battery.max_cycles_day` is read at
     `scripts/plexos2gtopt/parsers.py:2577`, emitted in the JSON
     `battery_array` (36/36 batteries carry it in v0407), and
     materialised as the LP `cycle_limit` row at
     `include/gtopt/storage_lp.hpp:957`.
   * `Commitment.max_starts` + `starts_scope` are populated at
     `scripts/plexos2gtopt/parsers.py:4573-4587` for the
     NEHUENCO/TOCOPILLA family, emitted at
     `scripts/plexos2gtopt/gtopt_writer.py:2528-2533` and enforced by
     the C++ commitment LP (`include/gtopt/commitment.hpp:302`).
   * `Fuel.max_offtake` is written at
     `scripts/plexos2gtopt/parsers.py:8253`.
   * `Line.tmax_ab` is set at `scripts/plexos2gtopt/parsers.py:1956`.

   No fix needed; the audit just has no normalisation between
   UC-name and native-primitive promotion. Recommend documenting the
   promoted families in the audit's bucket comment.

3. **P1 — Audit tool does not sanitise PLEXOS UC names before
   matching**, producing ~62 "missing" entries that are pure
   identifier-collision noise (e.g. `Kelar_GNL_SSCC CA_CC` PLEXOS vs
   `Kelar_GNL_SSCC_CA_CC` gtopt; `MutuallyExclusive_Andes2B-3` vs
   `MutuallyExclusive_Andes2B_3`). Bug locus: `uc_audit.py` has no call
   to `_pampl_ident` (defined at `gtopt_writer.py:3493`). Fix: normalise
   PLEXOS names through the same regex in `uc_audit.py` before forming
   `intersection` / `missing_from_gtopt`.

4. **P2 — B9 "5 UCs inactive in gtopt active in PLEXOS"** are
   tautological `battery.charge >= 0` / `battery.discharge >= 0` rows
   that gtopt correctly marks inactive (per the rationale of commit
   `bfa2f817e`). The PLEXOS "price" the audit reports is the dual on
   the natural non-negativity bound — not a real constraint violation.
   No bug; audit should filter trivially-satisfied LHS before flagging.

5. **P2 — B2 CSF_LW_Def / CSF_RS_Def / CSF_MinUnits / Inertia_Calculation_e1**
   show "RHS mismatch" but the underlying difference is that PLEXOS
   *reformulates* the constraint internally (move requirement variable
   off LHS onto RHS, normalise so PLEXOS-sol RHS == 0). gtopt's
   encoding is the raw-DB form. The PLEXOS RHS the audit reads is the
   *post-reformulation* RHS, not the input one. Whether gtopt's
   formulation actually binds the same way is a separate (deeper)
   question that would need to compare LMP / shadow-price between the
   two solves on the same case; it is **not** a converter bug visible
   from the audit alone.

## P0 — Real data-loss bugs

### P0.1 — Duplicate ANTUCO/ELTORO discharge UCs

**Audit bucket:** B2_rhs_mismatch (6 items, 4 of which are ANTUCO/ELTORO
duplicate-emission).

| PLEXOS UC name | gtopt sees as | Issue |
|---|---|---|
| `ANTUCOmin` | `gen_n_terms=1`, `rhs=137`, op `>=` | raw MW interpretation, ANTUCO_U1 only |
| `ANTUCOmax` | `gen_n_terms=1`, `rhs=137`, op `<=` | same |
| `ELTOROmax` | (not in audit per_row_diff B2, but PAMPL has both raw + discharge_ form) | same |
| `ANTUCOmin` (discharge mirror) | `0.625 * (ANTUCO_U1+U2) >= 83.30`, op `>=` | correct m³/s |
| `ANTUCOmax` (discharge mirror) | `0.625 * (ANTUCO_U1+U2) <= 85.30`, op `<=` | correct m³/s |

PAMPL evidence
(`/home/marce/tmp/gtopt_pcp_v0407_LIFT_CAP/uc_operational.pampl`):

```
constraint ANTUCOmax penalty soft_floor_penalty:
  1 * generator("ANTUCO_U1").generation <= 137;          # raw (WRONG)
constraint ANTUCOmin penalty soft_floor_penalty:
  1 * generator("ANTUCO_U1").generation >= 137;          # raw (WRONG)
constraint discharge_ANTUCOmin penalty soft_floor_penalty:
  0.625 * gen("ANTUCO_U1") + 0.625 * gen("ANTUCO_U2") >= 83.3046;   # CORRECT
constraint discharge_ANTUCOmax:
  0.625 * gen("ANTUCO_U1") + 0.625 * gen("ANTUCO_U2") <= 85.3046;   # CORRECT
```

**Root cause** (file:line):

`scripts/plexos2gtopt/parsers.py:8952` — in
`_auto_promote_hydro_min_max_to_commitments`:

```python
if gen_name in committed:
    continue
```

The function builds two side-effects:
* synthesise a Commitment for the gen (via `pmin_by_gen` / `pmax_by_gen`)
* schedule the corresponding raw UC name AND its `discharge_<name>min/max`
  mirror for drop (`drop_base_uc_names` + `drop_discharge_names`)

The `if gen_name in committed: continue` guard short-circuits BOTH side
effects together. When CEN PCP ships a pre-existing `uc_ANTUCO_U1`
Commitment (the case for every hydro generator in v0407), the function
skips the entire generator → neither the raw `ANTUCOmin/max` UC nor its
`discharge_*` mirror is dropped → both end up in the PAMPL.

**Proposed fix:**

Split the side effects. Always do the duplicate-drop pass, but only
synthesise a new Commitment when one doesn't already exist. Concretely:

```python
# inside the inner loop (parsers.py:~8968)
for gen_name, op, rhs, uc_name in rows:
    if gen_name not in hydro_names:
        continue
    # ALWAYS schedule both names for drop — the discharge_<plant><kind>
    # mirror is the authoritative encoding (multi-unit, m³/s units).
    # The raw plant<kind> UC is the duplicate that must go away
    # regardless of whether we synthesise a new Commitment or rely on
    # the existing one.
    drop_base_uc_names.add(uc_name)
    drop_stems.add(stem)
    if gen_name in committed:
        # Already has a Commitment — accumulate pmin/pmax onto the
        # EXISTING Commitment via a separate post-pass instead of
        # creating a new one.  For now (CEN PCP v0407) the existing
        # Commitments carry the correct pmin from PLEXOS Min Stable
        # Level, so we just need to drop the duplicate UC.
        continue
    # ...existing pmin/pmax synthesis...
```

Alternative: drop the `if gen_name in committed: continue` line and let
`_auto_promote_hydro_min_max_to_commitments` always emit the duplicate-
drop set. The new-Commitment synthesis loop downstream needs to skip
already-committed gens (it can use `committed` instead).

Either approach removes the redundant `ANTUCOmin/max`, `ELTOROmax` raw
UCs. The correct `discharge_*` encodings stay.

**Note on `discharge_*` mirror — DO NOT drop the mirror, drop the raw**:
The current `drop_discharge_names` logic at lines 8987-8996 drops the
*mirror*, not the raw. That is correct when a Commitment is newly
synthesised (the pmin/pmax floor on the Commitment now enforces what
the mirror used to enforce, so the mirror becomes redundant). It is
**not** correct when an existing Commitment is reused without
absorbing the new pmin/pmax: the existing Commitment's pmin is the
PLEXOS Min Stable Level (53 MW for ANTUCO_U1), NOT the hydro
discharge bound (137 MW / 1.6 ≈ 86 MW). So when reusing an existing
Commitment, the mirror MUST stay, and the raw must go. Adjust the fix
accordingly:

* `gen_name in committed` (reuse existing) → drop raw `<plant>min/max`
  UC, **keep** `discharge_<plant>min/max` mirror.
* `gen_name not in committed` (new Commitment) → drop raw AND mirror,
  add new Commitment with `pmin=137` / `pmax=137` (current behaviour).

### P0.2 — B3 BatMaxCycDay_BAT_* (33 items) — VERIFIED NO BUG

The 33 missing `BatMaxCycDay_BAT_*` UCs are correctly promoted to the
native `Battery.max_cycles_day` field. Full chain verified:

| Stage | File:line |
|---|---|
| PLEXOS prop read | `scripts/plexos2gtopt/parsers.py:2577` (`db.static_property("Battery", batt.object_id, "Max Cycles Day")`) |
| Spec field | `scripts/plexos2gtopt/entities.py:348` (`max_cycles_day: float`) |
| Writer emit | `scripts/plexos2gtopt/gtopt_writer.py:1444-1446` (writes `capacity` + `max_cycles_day` keys) |
| JSON binding | `include/gtopt/json/json_battery.hpp:128` (`json_number_null<"max_cycles_day", OptReal>`) |
| Spec field (C++) | `include/gtopt/battery.hpp:302` (`OptReal max_cycles_day {}`) |
| LP row emit | `include/gtopt/storage_lp.hpp:957-1016` (`if (opts.max_cycles_day > 0.0 && stage_capacity < DblMax)` → adds `cycle_limit` row `Σ flow·duration ≤ N · capacity`) |

Confirmed in the converted JSON:
`/home/marce/tmp/gtopt_pcp_v0407_LIFT_CAP/PLEXOS20260407.json`:
all 36 batteries carry `"max_cycles_day": 1.0` and `"capacity": <emax>`.

**Auditor implication:** the audit should treat any UC whose name
starts with `BatMaxCycDay_` as "promoted to native primitive" and
*not* list it under B3 `missing_from_gtopt`. A simple regex-based
allowlist in `uc_audit.py` analogous to `UC_FAMILY_PATTERNS` would do.

### P0.3 — B3 GenMaxStartsWeek_* (5 items) — VERIFIED NO BUG

The 5 missing `GenMaxStartsWeek_*` UCs (NEHUENCO_2_TG_DIE,
NEHUENCO_2_TG_GNL, NEHUENCO_2_TV, TOCOPILLA_U16_CA_GNL,
TOCOPILLA_U16_CC_GNL) are promoted to `Commitment.max_starts` +
`Commitment.starts_scope`. Full chain verified:

| Stage | File:line |
|---|---|
| PLEXOS scope priority | `scripts/plexos2gtopt/parsers.py:4573-4587` (Week > Day > Hour > Horizon > Month > Year) |
| Spec field | `scripts/plexos2gtopt/entities.py:807-809` (`max_starts: int`, `starts_scope: str`) |
| Writer emit | `scripts/plexos2gtopt/gtopt_writer.py:2528-2533` (writes when `max_starts > 0 and starts_scope`) |
| LP enforcement | `include/gtopt/commitment.hpp:302` + the LP `min_starts ≤ Σ startup ≤ max_starts` row |

Confirmed in JSON: all 5 NEHUENCO/TOCOPILLA UCs surface as
`commitment_array` entries with `max_starts=1000`, `starts_scope="week"`.

Same auditor implication: regex `^GenMaxStarts(Hour|Day|Week|Month|Year)_`
should be exempted from B3.

### P0.4 — B7 family-level promotions (3 families, 217 UCs total) — VERIFIED

| Family | Count | Native primitive | Verified at |
|---|---|---|---|
| transmission_security (`SD_*`, `_PAzucar_*`, etc.) | 57 | `Line.tmax_ab` + `overload_penalty` | `parsers.py:1956`, `parsers.py:1822-1829` |
| fuel_offtake_week (`FueMaxOff*`) | 82 | `Fuel.max_offtake` + `max_offtake_cost` | `parsers.py:8253` (`_apply_native_fuel_offtake_caps`) |
| gas_maxopday (`Gas_MaxOpDay*`) | 78 | Consolidated into `Gas_MaxOpDay_<group>` UC (NOT a primitive); high-soft tier | `parsers.py:6097-6253` (`_consolidate_gas_maxopday`) |

The first two are clean primitive promotions; `gas_maxopday` is a
consolidation that still emits UCs but with a renamed key. Both are
intentional. Auditor should pre-categorise these names away from
B7_missing_uc_family.

## P1 — Cosmetic mismatches (audit-tool improvements)

### P1.1 — Identifier sanitisation not applied in audit

**Audit bucket:** B3_missing_uc (3 Kelar_GNL_SSCC + 2 MutuallyExclusive
items) and many B8_synthetic_in_gtopt entries.

PLEXOS source names contain characters illegal in PAMPL identifiers
(`+`, `>`, `-`, `.`, space). `gtopt_writer.py:_pampl_ident` (line 3493)
maps every such character to `_`. The audit's `intersection / missing
/ synthetic` computation works on raw strings, so the same constraint
appears on both sides under different names.

Examples:
* `Kelar_GNL_SSCC CA_CC` (PLEXOS) ↔ `Kelar_GNL_SSCC_CA_CC` (PAMPL) —
  3 such Kelar UCs.
* `MutuallyExclusive_Andes2B-3` ↔ `MutuallyExclusive_Andes2B_3`.
* `MutuallyExclusive_Tocopilla-Tamaya` ↔ `MutuallyExclusive_Tocopilla_Tamaya`.
* `SD_*_Guacolda-Maitencillo` ↔ `SD_*_Guacolda_Maitencillo`.

**Proposed fix** (audit-only, no production code change):

In `scripts/plexos2gtopt/uc_audit.py:run_audit`, around line 451 where
`plexos_names = set(plexos.keys())` is built, normalise both keys via
`_pampl_ident` before set-arithmetic. Maintain a reverse-map
`{sanitised → original_plexos_name}` so the per-row report still shows
the original PLEXOS name for traceability.

Concretely:

```python
from .gtopt_writer import _pampl_ident
plexos = {_pampl_ident(n): rec for n, rec in plexos.items()}
# (and add: rec["original_plexos_name"] = n)
```

This will resolve ~62 false-positive "missing" entries with no
converter-side changes.

### P1.2 — Audit's `plexos_active` heuristic over-flags tautological UCs

**Audit bucket:** B9_inactive_gtopt_active_plexos (5 items).

The 5 UCs flagged are tautological non-negativity constraints
(`battery.charge >= 0` or `battery.discharge >= 0`, RHS=0, single
positive coefficient). gtopt correctly marks them inactive per commit
`bfa2f817e`'s rationale ("PLEXOS itself drops the whole family from the
ST schedule"); the audit's `plexos_active` flag is true here only
because PLEXOS reports an `activity` dual on the natural
non-negativity bound of the battery column, not because PLEXOS
solves a real constraint row.

**Proposed audit fix:** filter B9 candidates by structural inertness
before flagging. A constraint with RHS=0, a single positive
coefficient and op `>=` is trivially satisfied and need not appear in
B9.

## P2 — Design-validated observations

### P2.1 — CSF / Inertia constraints have different formulation

**Audit bucket:** B2_rhs_mismatch (`CSF_LW_Def`, `CSF_RS_Def`,
`CSF_MinUnits`, `Inertia_Calculation_e1`).

gtopt and PLEXOS encode these constraints differently but they are
algebraically related. Examples:

* `CSF_LW_Def` — gtopt PAMPL:
  `−1·CSF_LW_Requirement.value + Σ provision.dn >= 0` with per-block
  RHS profile `[163..333]`. PLEXOS sol RHS=0. The per-block profile in
  gtopt is `max(scalar_rhs, Σ reserves' requirement)` (parsers.py:8040)
  — the requirement-side that PLEXOS keeps as a separate variable.
  Both formulations are equivalent **provided** the LP treats
  `CSF_LW_Requirement.value` as a non-negative variable bounded above
  only by the reserves' requirement; in PLEXOS that variable is in
  fact bounded by the reserve-zone aggregation. The gap is whether the
  Decision Variable bounds in gtopt match.
* `Inertia_Calculation_e1` — gtopt `... >= -622.54`; PLEXOS sol RHS=0.
  gtopt's bound is the raw DB value; the constraint never binds in
  gtopt because LHS easily exceeds -622.54. PLEXOS, by contrast, binds
  this for 111/168 h. This suggests PLEXOS internally
  reformulates `−1000·Inertia_SEN + Σ inertia·commit_status >= 622.54`
  (right-side positive), with `Inertia_SEN` minimised by the
  objective. In gtopt the same value pattern is `>= −622.54` which is
  satisfied by `Inertia_SEN = 0, commit_status = 0` (degenerate).

These are not converter bugs in the strict sense — the raw DB values
are read correctly — but the **runtime effect differs from PLEXOS**.
Listed P2 because validating the equivalence (or fixing the
reformulation) requires solving both LPs and comparing LMP/shadow
prices, beyond the scope of this audit.

### P2.2 — `CSF_MinUnits` sign flip (-10 vs +3)

PLEXOS sol reports RHS=+3, gtopt PAMPL shows `>= -10`. The PLEXOS DB
ships RHS=-10 (read correctly by gtopt). The +3 in the PLEXOS sol is
the post-pre-solve effective RHS after PLEXOS shifts commit_status
columns that are fixed (forced on/off) onto the RHS. gtopt does not
perform this shift; instead it relies on `Commitment.fixed_status`
producing equivalent LP-equality rows. Same observation as P2.1: not a
read-bug, but a runtime-equivalence question.

### P2.3 — B8 synthetic-in-gtopt — overwhelmingly cosmetic

418 entries; majority are identifier sanitisation (per P1.1). The
truly-synthesised group (no PLEXOS counterpart) is small:

* `discharge_ANTUCOmin/max`, `discharge_ELTOROmax` — correct multi-unit
  m³/s encoding of the PLEXOS hydro discharge bounds. The PLEXOS
  counterparts (`ANTUCOmin/max`, `ELTOROmax`) are the duplicate-emission
  source per P0.1.
* `MutuallyExclusive_*` — sanitised from PLEXOS `MutuallyExclusive_X-Y`.
* `Kelar_GNL_SSCC_*` — sanitised from `Kelar_GNL_SSCC X_Y` (space).
* `FCF_future_cost` — gtopt's terminal-value boundary cut from
  `boundary_cuts.csv`; intentional (no PLEXOS equivalent).
* `uc_*` prefixed names — sanitised from PLEXOS leading-digit names
  (e.g. `2024084134_…` → `uc_2024084134_…`).

No real synthesises (apart from the boundary-cut FCF entry) materialise
new LP rows without a PLEXOS counterpart.

## Files to read in full

| File | Why |
|---|---|
| `scripts/plexos2gtopt/parsers.py` | UC extractors + duplicate-drop logic |
| `scripts/plexos2gtopt/_uc_policy.py` | soft/hard penalty classification |
| `scripts/plexos2gtopt/gtopt_writer.py` | JSON emission + `_pampl_ident` sanitisation |
| `scripts/plexos2gtopt/uc_audit.py` | audit tool (needs name normalisation) |
| `scripts/plexos2gtopt/entities.py` | Battery, Commitment, Fuel, Line specs |
| `include/gtopt/battery.hpp` | C++ Battery options (max_cycles_day, capacity) |
| `include/gtopt/commitment.hpp` | C++ Commitment options (max_starts, starts_scope) |
| `include/gtopt/storage_lp.hpp` | C++ cycle_limit LP row construction |

## Summary table

| Priority | Bucket | Count | Real bug? | Fix locus | Effort |
|---|---|---|---|---|---|
| P0 | B2 ANTUCO/ELTORO duplicates | 4 | YES | `parsers.py:8952` | 1-2 h + tests |
| P0 | B3 BatMaxCycDay_BAT_* | 33 | NO (native promotion verified) | audit allowlist only | 30 min in `uc_audit.py` |
| P0 | B3 GenMaxStartsWeek_* | 5 | NO (native promotion verified) | audit allowlist only | 15 min in `uc_audit.py` |
| P0 | B7 transmission_security / fuel_offtake / gas_maxopday | 217 | NO (native + consolidation) | audit allowlist only | 30 min in `uc_audit.py` |
| P1 | B3/B8 identifier sanitisation | ~62 | NO (audit normalisation only) | `uc_audit.py:451` | 30 min |
| P1 | B9 tautological non-neg dups | 5 | NO (audit false-positive) | `uc_audit.py` (B9 filter) | 30 min |
| P2 | B2 CSF/Inertia formulation | 4 | UNCERTAIN (runtime LP comparison needed) | n/a — investigate separately | unknown |
| P2 | B6 soft-in-PLEXOS-hard-in-gtopt | 328 | UNCERTAIN (no clear over-constraint evidence; defensible-hard is the default) | n/a — case-by-case | unknown |
