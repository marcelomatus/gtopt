# Irrigation Agreements — Test Plan (Laja & Maule)

Bottom-up ladder: each tier assumes the tiers below it are green.  Tiers
A–B are implemented; C–D list concrete proposed cases with the file they
belong in.  Source-of-truth references: `~/git/plp_storage/CEN65/src`
(`leelajam.f`, `genpdlajam.f`, `genpdmaule.f`) and the CEN informe
*Actualización Convenio del Lago Laja e Implementación en Modelo PLP*
(Dec 2018, `Actualizacion_Convenio_Laja_PLP.pdf`, esp. Tabla 1 and
annexes 13.2/13.3).

## Tier A — LP primitives (C++, implemented)

Covered by `test_flow_right*.cpp`, `test_right_bound_rule.cpp`,
`test_irrigation_bounds.cpp`, `test_irrigation_user_constraints.cpp`:

* Kink algebra: fcost-only / uvalue-only substitution, full kink,
  slack caps (unbounded-ray guard), `fail_sol_at` reconstruction.
* **New (2026-07)**: target-0 encodings — negative `uvalue` = per-unit
  usage cost over the full `[fmin, fmax]` band; positive `uvalue` =
  per-unit reward; no-cost target-0 = plain band (back-compat reset).
  (`test_flow_right_substitution.cpp`, Invariant 6.)
* `RightBoundRule` segment selection, cap/floor clamping, axis
  dispatch, stage-month guard.
* VolumeRight storage balance, reset-month provisioning (mechanism is
  month-agnostic; fixtures use april), `skip_state_link` at cross-phase
  resets.

Gaps to add at this tier:

* **A1** `update_lp` re-clamp vs one-sided substitution — FIXED
  2026-07: the re-clamp now preserves the cached-target clamps
  (`uppb = min(fmax, target)` under fcost-only, `lowb = max(fmin,
  target)` under uvalue-only) at both block and qeh scope (a new
  stage-scope substitution cache was added for the latter).  Three
  regression tests in `test_flow_right_substitution.cpp` (Invariant
  7) — note each needs its own fixture: bound edits invalidate the
  solved state, so only the first element updated after a solve sees
  the moved volume axis.
* **A2** VolumeRight `bound_rule` dimensional guard: the rule value is
  an hm³ provision but is also applied as an m³/s per-block extraction
  cap (`volume_right_lp.cpp:124-126`).  Pin current behaviour, then
  split the two uses (e.g. `bound_rule_caps_extraction: false`).

## Tier B — encoding: PLP data → canonical JSON → entities (Python, implemented)

Covered by `plp2gtopt/tests/test_laja.py`, `test_maule.py`,
`gtopt_expand/tests/test_round_trip.py`, `test_water_value_resolver.py`:

* Cost-line read order pinned to `leelajam.f:202-207` against the real
  2-year fixture: `cost_irr_ns=1100`, `cost_irr_uso=0`,
  `cost_elec_uso=1150`, `cost_mixed_uso=0.1`, `cost_antic_uso=1.0`;
  `cost_elec_ns` must NOT exist.
* Usage costs emitted as negative `uvalue` schedules with the correct
  monthly factors (e.g. electric April = −1265 = −1150 × 1.1); no
  `fcost` on the main rights flows.
* Reset months: december (rights) / september (anticipado), derived
  from `MesIniTempRiego`/`MesIniTempAntic`.
* District `fcost` = `cost_irr_ns × cost_factor × FactMenCRiegoNS(mes)`
  schedules; resolver override applies to the retiro base only.
* Maule: Invernada `costo_embalsar`/`costo_no_embalsar`/`costo_canelon`
  emitted negative; `maule_gasto_normal_elec` has no fcost.

Gaps to add at this tier:

* **B1** Zone-formula golden test — IMPLEMENTED 2026-07:
  `test_zone_formula_matches_cen_tabla1` asserts
  `irr(V) + mixed(V)` equals the CEN Tabla 1 riego column and
  `elec(V)` the generation column, across all four colchones.
* **B2** tampl render smoke test: `generate_pampl` on the real 2-year
  config renders without `StrictUndefined` errors and contains the
  corrected `cost_*_uso` params.

## Tier C — single-agreement LP structure (C++, proposed)

New file `test_irrigation_laja_lp_structure.cpp` — build a reduced Laja
system **from the actual gtopt_expand output** (render a small config
through the real templates, load with `from_json<Planning>`), 12 monthly
stages starting in April, one reservoir:

* **C1 December provisioning**: with reservoir volume V₀, the december
  stage fixes `vrt_sini(laja_vol_der_riego)` to the colchón formula at
  V₀; an off-reset month does not.  Repeat for two V₀ values in
  different colchones (e.g. 900 → 570; 1500 → 690 today, 720 once B1's
  transfer lands).
* **C2 Electric usage price**: objective coefficient of
  `frt_qeh(laja_der_electrico)` is `+1150 × FactMen(mes) × dur/scale`
  in Dec–Apr and 0 in May–Nov; the sign is positive (a charge).
* **C3 Season gating**: `fmax` of `laja_der_riego` qeh is 0 in May–Nov
  (FactMenMaxRiego), so the flow columns are inactive off-season.
* **C4 Partition identity**: after solve, `laja_q_turbinado` flow
  equals the sum of the four rights flows in every block (UserConstraint
  `laja_particion_derechos` binding).
* **C5 Ledger monotonicity**: `vrt_vol` is non-increasing between
  resets and `≥ 0` (extraction cannot exceed the provision).

New file `test_irrigation_maule_lp_structure.cpp` (same recipe):

* **C6 Zone switching**: `update_lp` with Colbún volume 700/300/100
  drives `maule_gasto_normal_elec` fmax to 30/0/0 and
  `maule_gasto_ordinario_elec` to 0/30/0 (2- and 3-segment step rules).
* **C7 Ordinary split**: with both ordinario flows free, the LP cannot
  exceed elec ≤ 20 % and irr ≤ 80 % of their sum (constraints bind).
* **C8 Invernada balance + penalties**: `invernada_balance` holds and
  the objective charges (not rewards) embalsar at 1500 and no-embalsar
  at 1000 per unit.
* **C9 Res105**: served flow ≤ target, deficit priced at
  `costo_riego_ns_res105`.

## Tier D — whole-agreement integration (proposed)

* **D1 Golden-LP diff vs PLP** (Python, `scripts/gtopt_check_lp`):
  convert the PLP 2-year support case (`plp2gtopt` → `gtopt_expand`),
  run `gtopt --lp-debug` for stage 1, and diff the irrigation columns /
  rows / objective coefficients against a checked-in expectations file
  derived from `genpdlajam.f` (the §9.7/§9.8 comparison, automated).
  This is the test that would have caught the cost-line mis-parse.
* **D2 Wet/dry provisioning scenario test**: two scenarios with
  different initial volumes; assert the december provision differs per
  the colchón formula and that the dry scenario delivers less to
  districts with the deficit priced at the retiro fcost.
* **D3 Electric-deferral economics**: with the 1150 in-season usage
  cost active, a two-stage (December + June) case should charge
  electric-rights water in June (cost 0) rather than December when
  storage permits — assert the December `laja_der_electrico` flow is 0.
* **D4 SDDP state coupling** (extend
  `test_sddp_boundary_cuts_volume_right.cpp` /
  `test_irrigation_reset_cross_phase.cpp`): december reset inside a
  phase vs at a phase boundary; backward duals must not propagate
  through the reset (mirrors `IsVarCorteLajaM`).
* **D5 Anchoring acceptance (implemented 2026-07)**:
  `test_irrigation_anchoring.cpp` (Tier 9.1-9.3) covers the full
  chain — anchor binds (`qgt == waterway flow`), usage-cost steering,
  and ledger caps forcing category switches.  Emission is pinned in
  `test_water_rights_integration.py::test_anchor_constraints_emitted`
  (gen arcs referenced, `_ver` arcs asserted ABSENT — spills must
  never enter the rights accounting).  Remaining: assert dual
  consistency of the anchor row against the reservoir water value.

## Known-gap ledger (tests intentionally xfail/absent until fixed)

1. ~~Physical anchoring of rights flows (Laja & Maule)~~ — done
   2026-07 (gen-arc-only; see D5).  ~~District anchoring~~ — done
   2026-07: districts whose physical diversion exists in the topology
   (the JunctionWriter `{central}_irrigation_right` offtakes — PLP's
   per-retiro `+l_qriK` node term) get `dist_anclaje_*` constraints
   equating the offtake to the sum of their category FlowRights; the
   categories drop their direct junction refs; RieSaltos' diversion is
   switched to CARRY mode returning at LAJA_I (PLP `ICenInyRiego`).
   Districts without a diversion in the topology (RieTucapel — see the
   leelajam.f:292 question for the CEN) stay soft sinks.  The netted
   primary targets are in too: 1o_reg targets = pct × min(gross,
   hoya + filtration) per GetQsLajaM's QPRiego re-set, and the qdefm
   carrier is scenario-dimensioned when the conversion emits multiple
   forward scenarios (one series per hydrology).
2. ~~IVGAF debit of the december provision~~ — done 2026-07: the
   anticipado bucket is a PLP-style up-counter (`reset_value: 0` at
   september, fills via `saving = qga`) and the riego december reset
   emits the PLP row `eini + vgaf = provision` via the new
   `reset_debit_right` field (Tier 9.4/9.5 in
   `test_irrigation_anchoring.cpp`).
3. ~~Mixed→riego +30 hm³ transfer above colchón 1~~ — done 2026-07:
   mixed segments are per-zone steps (PLP selector semantics) and the
   riego segments carry the `DerMixtoBase×(1−FMixto)` transfer; the
   B1 golden test (`test_zone_formula_matches_cen_tabla1`) pins the
   emitted rights to the 2017 Acuerdo's Tabla 1.
4. ~~`qdrh + qdmh + qgah ≤ qdefm` attribution cap~~ — done 2026-07:
   the cap now uses the NETTED per-stage requirement (GetQsLajaM):
   plp2gtopt computes the duration-weighted hoya-intermedia inflow
   means from the aflce data and the agreement emits the `laja_qdefm`
   carrier FlowRight (fixed column) that the cap references; gross
   district deliveries remain the fallback for configs without
   inflow data.  Scenario-dimensioned schedules are in (2026-07: one
   qdefm series per forward-scenario hydrology, mapped through the
   scenarios' `hydrology` field; dry hydrologies correctly get a
   HIGHER cap).  STATIC-FILTRATION approximation remains (QFiltLaja =
   QFiltHist, so QDefAbanico = 0 — the same role QFiltHist plays in
   PLP vs its dynamic FiltVals estimate).

   Volume-dependent filtration — DESIGN CONSTRAINTS (2026-07, per
   user guidance): the filtration is modeled by SEPARATE first-class
   gtopt elements — one ReservoirSeepage per relevant reservoir
   (e.g. `ELTORO_seepage_2` driving the `filt_ELTORO_37_38` arc into
   ABANICO, from plpfilemb.dat).  These are PHYSICAL elements of the
   base hydro topology that exist with or without the irrigation
   agreements; the agreements must never create, modify, or assume
   them — only READ them, referenced directly as gtopt elements and
   resolved defensively (emit the refinement only when the element
   exists, like the anchor refs).  The delivery side already needs
   nothing: districts receive the real volume-dependent seepage water
   through the junction network.  For the operational-rule constants:

   * the seepage element is PAMPL-visible as `seepage('<name>').flow`
     (reservoir_seepage_lp.cpp registers the canonical `flow` attr),
     so the cap's two linear pieces can be expressed IN-LP:
     `rights <= (QP - hoya) - seepage(X).flow + QN + QS + QE` and
     `rights <= QFiltHist - seepage(X).flow + QN + QS + QE`;
   * the outer `max(., 0)` is the remaining wrinkle (a negative RHS
     with rights >= 0 is infeasible, not a relaxed cap) — handle via
     a numeric update_lp path that evaluates the SEEPAGE ELEMENT's
     own piecewise at the current lake volume (never a duplicate
     curve), or prove the RHS stays non-negative in-season under the
     fmax gating before going in-LP.
5. ~~Maule monthly electric-counter reset~~ — done 2026-07: new
   `VolumeRight.reset_monthly` re-provisions at every month start
   (PLP TipoEtaDE != INTRAETA); the annual bucket resets in january
   (ENEROHID = hydro 10, plpmod.f:21) and the seasonal riego bucket
   at the data-derived season start (PLP MesRiegoIni).  Also fixed in
   the same batch: the Maule zone driver is LAGUNA DEL MAULE (PLP
   IVMUTIL, genpdmaule.f:326-329 — was Colbun), the electric/riego
   ledgers accumulate normal+ordinario flows, plpeta stage months are
   HYDRO-indexed and now convert to calendar in stage_parser (the
   February=672h discriminator), and the Maule monthly arrays index
   by hydro month.  The compensation recompute at year start is done
   too: new `VolumeRight.reset_credit_right` — at the january reset
   the provision is `min(emax, own_incoming + credit_incoming)`,
   evaluated numerically per stage in `update_lp` exactly like PLP's
   FijaMaule (Tier 9.7 pins 40 -> 30 across a solve+update; the e2e
   solve shows the january conversion live: annual 250 -> 112.4
   remaining becomes the compensation provision).  The same batch
   fixed the in-phase reset chain (`StorageOptions.break_stage_chain`:
   a reset stage now gets a FRESH eini column instead of pinning the
   previous stage's shared efin — Tier 9.6 now solves and proves the
   prior balance stays intact).
6. ~~`update_lp` re-clamp vs substitution~~ — fixed 2026-07 (A1).


## SDDP cut soundness — irrigation state variables (reviewed 2026-07)

The irrigation buckets are Tilmant-style "dummy reservoirs": their
`efin` state variables enter Benders cuts exactly like reservoir
states.  The reset machinery interacts with cut validity as follows,
with PLP's `IsVarCorteLajaM` / `IsVarCorteMaule` as the reference:

| Situation | gtopt mechanism | Cut coefficient | PLP reference |
|---|---|---|---|
| Non-reset stage | state-linked sini (cross-phase) / shared efin column (in-phase) | real gradient | cut variable kept |
| Plain reset (december rights, monthly/annual electric, seasonal riego) | incoming state DECOUPLED: `skip_state_link` severs the cross-phase link; `break_stage_chain` allocates a fresh in-phase eini | **zero** — the incoming state touches no row, so its dual vanishes naturally | PLP EXCLUDES the variable from the cut at the reset stage (`IsVarCorteLajaM` at INICIOTEMP, `IsVarCorteMaule` for IVMGEMF/IVMGEAF/IVMGRTF/zone reserves) |
| December debit (riego provision − anticipado) | the antic bucket's incoming column appears IN the debit row `eini + vgaf = provision` | **real, negative gradient** — early spending reduces the provision and the duals flow into the cuts | PLP keeps IVGAF as the ONLY cut variable at INICIOTEMP (`IsVarCorteLajaM:1034-1040`) — same semantics |
| January credit (compensation) | provision recomputed NUMERICALLY in update_lp from own + annual incoming states | zero (procedural) | PLP also computes VCompElecN numerically from `VarMaulePrev` AND excludes IVMGEAF from the cuts at INICIOANO — parity |
| Provision-vs-reservoir-volume gradient | December provision = rule(V) pinned numerically per iteration | zero (`∂provision/∂V` not in cuts) | identical PLP limitation (procedural FijaLajaM/FijaMaule) |

Cross-scene cut sharing: the irrigation states are scene-uniform (the
same provisioning rules and LP structure per scene), so shared cuts
remain valid — the zero coefficients at reset boundaries are zero in
every scene alike.  Registered-but-decoupled state variables carry
zero coefficients, which is equivalent to PLP's explicit exclusion.

Feature switches: `--no-irrigation-couplings` (plp2gtopt) reverts the
expansion to the legacy uncoupled shape; per-feature keys in the
canonical laja/maule JSON: `enable_physical_anchoring`,
`enable_ledger_linkage`, `enable_attribution_cap`,
`enable_netted_targets` (all default true; toggle tests in
test_laja.py / test_maule.py).
