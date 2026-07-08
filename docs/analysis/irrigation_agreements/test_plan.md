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


## Fictitious irrigation generators — topology recovery audit (2026-07)

PLP models the irrigation reaches with FICTITIOUS generators
(``Barra 0, Rendi 1.0`` in plpcnfce.dat) whose "generation" is water,
not electricity.  Their arc semantics and the gtopt image:

| PLP pattern | Centrals | gtopt image |
|---|---|---|
| ``SerHid=0, SerVer>0`` — gen exits (the irrigation offtake, PotMax = canal capacity), spill continues the river | RIEGZACO (→43), RieSaltos (→LAJA_I), RieSur123SCDZ (→SAN_IGNACIO) | junction + ``{name}_irrigation_right`` FlowRight (fmax = PLP PotMax, e.g. 70 for Zañartu-Collao) + ``{name}_ver_*`` continuation arc; Saltos' offtake is CARRY mode returning at LAJA_I (the convenio's ±1 injection pair) |
| ``SerHid=SerVer=0`` — both arcs exit free; the convenio's l_qri rides on top | RieTucapel + RieMelado / RieCMNA / RieCMNB / RieMauleSur / RieMaitenes / RieMolinosOtros | junction with ``drain: true`` (the free exits) + the agreement's retiro categories attached via ``anchor_junction`` (the accounted withdrawal) |

Verified on the converted 2-year case: every node keeps its junction,
its upstream feed arcs (C_Melado_gen, CANAL_LAJA_ver/El Diuto chain,
ANTUCO/MACHICURA outflows), and its exit accounting; nothing is
dropped as isolated and nothing double-counts (the diversions are
free physical offtakes; only the agreement categories carry demands
and fail costs).  Pinned by
``test_fictitious_irrigation_topology_recovered`` so converter
changes cannot silently erase the irrigation reaches again.


## PLP vs gtopt comparison — KPIs and findings (2026-07-08)

`test_plp_gtopt_comparison.py` (equal tier always-on; behavioral/KPI
tiers opt-in via `GTOPT_PLP_COMPARE=1`, run with `-n0`).  KPI set:

| KPI | Probe for | Observed |
|---|---|---|
| Initial volumes / inflow passthrough | conversion factors | equal ✓ |
| Seepage piecewise vs PLP `EmbQFil` | filtration model | 1.3% (ELTORO) ✓ |
| Laja partition ≡ El Toro turbinado | anchor integrity | exact ✓ |
| LMP spatial rank corr (241 buses) | network/congestion model | **0.975** ✓ |
| LMP level ratio | marginal-unit calibration | 1.5 (band 0.5–2) |
| Water-value ratio per reservoir | storage valuation | PEHUENCHE 1.00, RALCO 0.95 ✓; **ELTORO 12.6×, COLBUN 0.35×, RAPEL 0.20× — flagged** |
| Cut states | FCF composition | Reservoir efin ✓ + **347 VolumeRight coeffs** (irrigation buckets participate) ✓ |
| Convenio states (`plplajam.csv` vdrf/vdef/vdmf/vgaf, `plpmaule.csv` vmg*/vmdcef) vs `laja_vol_*`/`maule_vol_*` efin | agreement state machines | ready — skips until a PLP run's CSVs are provided (PLP_OUT_DIR) |

**FINDING — largely RETRACTED after units correction (2026-07-08,
user-caught)**: the original claim compared gtopt's volume-balance
dual against PLP's `EmbPsom`, which is the **$/MWh** representation
(CMg × FPhi / FactRendim — divided by the downstream chain's summed
rendimiento, plp-gdbdemb.f:115-119).  The correct counterpart is
`EmbPsom2` = CMg × FPhi, the raw **volume shadow price**
(plp-gdbdemb.f:121-123).  Against Psom2 the per-reservoir ratios
tighten to a median of 1.22 — COLBUN 0.66, RAPEL 1.18, LMAULE 0.80,
PEHUENCHE 0.89, RALCO 1.25 all healthy; the 12.6×/0.35×/0.18×
scatter was the chain-yield units artifact.  Per-volume water values
differing 10× across reservoirs is PHYSICS (value per hm³ ∝ chain
energy yield; El Toro's cascade extracts far more MWh/hm³), not an
inconsistency.  **Residual finding**: ELTORO still ~5× (gtopt dual ≈
383 k$/hm³, efin_cost 411 k$/hm³, vs PLP Psom2 ≈ 74 k$/hm³);
CANUTILLAR 2.6× / PILMAIQUEN 3.1× milder cousins.

VERIFIED (2026-07-09, user request): every reservoir's `efin_cost`
comes from the BOUNDARY CUTS file — planos `plpplem1/2` cut
lower-bound water value (−max GradX per reservoir) divided by the
last stage's discount factor (0.9091 = 1/1.1), all 10 exact
(ELTORO: 374,000 / 0.9091 = 411,400 ✓) — NOT from the
ANCHOR×lost_pf fail-cost estimate (that auto path only covers
reservoirs absent from the planos).  Pinned by
`test_efin_cost_from_boundary_cuts`.  The mode is `combined`: the
cut hyperplanes are ALSO loaded (α′ re-centered at the efin target,
sddp_boundary_cuts.cpp:675-717) while efin_cost prices only the
last-stage soft-efin slack — complementary, not double-counted.
So the ELTORO 5× is a property of the planos data itself: even the
SHALLOWEST cut gradient prices El Toro at 374 k$/hm³ while PLP's
own simulation prices interior water at ~74 k$/hm³ — i.e. the
vendored cuts value the El Toro boundary far above the operating
water value (steep FCF tail at the 2017-Acuerdo recovery volumes).
gtopt faithfully consumes them; whether PLP's planos SHOULD be that
steep is a question for the cut-generation run, not the converter.
Everything else validated.

**FINDING (PLP semantics)**: PLP's filtration is an LP variable on
the segment envelope, not a curve lookup — during extreme refills it
legitimately sits off the curve (CIPRESES).  Matches our
ReservoirSeepage semantics; equality asserted in the binding regime.


## Live PLP run — convenio state comparison (2026-07-08)

PLP CEN65 (CPLEX 22.1.1) was run on the 2-year case with
`PLP_CONVLAJA_MODE=2` / `PLP_CONVMAULE_MODE=2` (userstop after 15
Benders iterations, gap 0.41 plateaued; 19 simulations, 59 min).  The
convenio outputs are vendored: `support/plp/2_years/plplajam.csv.xz`
/ `plpmaule.csv.xz`.  Comparison results (Sim 1, shape-based — PLP
writes eta states as volume-per-stage x VolScale, genpdlajam.f:986):

* **Reset timing EXACT**: PLP's TipoEtaGM=3 (INICIOTEMP) and =4
  (INICIOANTIC) stamps select precisely gtopt's december/september
  reset stages (`test_laja_reset_timing_matches_plp`).
* **Balance semantics 1:1**: genpdlajam.f:227-257 rows are literally
  our design — `vdrf_t = vdrf_(t-1) − qdrh·dt` (depleting bucket),
  `+vgaf` debit folded into the December provision row, provision =
  `min(DerRiego, DerRiegoMax)`, vgaf up-counter with `−qgah·dt`.
* **Compensation machinery correlates** (vmdcef vs
  maule_vol_compensacion_elec: norm-corr 0.68) — the january credit
  moves the same way in both models.
* Laja partition buckets correlate positively (vdrf 0.31, vdef 0.35).
* The flat-vs-moving cases (vdmf/vgaf: PLP moves, gtopt flat at 0;
  vmgeaf/vmgrtf: gtopt moves, PLP ~flat) are the KNOWN dispatch-policy
  divergence (El Toro hoarding / LMAULE usage under the efin_cost
  vector), not agreement-machinery differences — no state
  anti-correlates below −0.5 (inverted-semantics gate).

PLP quirks recorded: F9.3 overflow prints `*********` for vdef >= 1e6
(coerced to NaN); `userstop` file with `P` stops the policy loop and
jumps to simulation (plp-pdconvrg.f:51).


## Terminal-volume policy: soft-slack vs hard/cuts (2026-07-09, user-driven)

Reviewed the two mechanisms that price a reservoir's end-of-horizon
volume, prompted by the ELTORO ~5× water-value residual.

**PLP (volfinem.f)**: the terminal volume is a HARD lower bound —
`EmbVMin(NEtapa) = EmbVFin`, i.e. `vol_end >= EmbVFin` — gated by
`FVolFinEmb .OR. .NOT. EmbCFUE`.  CFUE reservoirs (those carrying a
future-cost function = boundary cuts; ELTORO is `EmbCFUE=T`) are
governed by the FCF cuts and do NOT get the hard bound unless the
global `FVolFinEmb` forces it.  So PLP prices ELTORO's terminal
value with the cuts alone; its simulated end floats to 982 hm³ (well
below the configured `EmbVFin`=3777).

**gtopt (before)**: `combined` boundary-cut mode loads the cut
hyperplanes AND additionally imposes a SOFT `vol_end + slack >= efin`
row priced at `efin_cost`, with `efin = EmbVFin = 3777`.  For a CFUE
reservoir whose target is unreachable this double-counts the terminal
value and injects a one-sided penalty.  Measured on the 2-year case
(1y horizon, first hydrology):

| variant | ELTORO end | water_value | note |
|---|---|---|---|
| soft (before) | 1255 | 383,226 | over-hoards; slack penalty ≈ 9.4e8 $ ≈ PLP's whole objective |
| **cuts-govern** | **1039** | 17,890 | cuts sole terminal mechanism |
| PLP (simulated) | 982 | 74,000 | CFUE → cuts only |

**Fix**: new `--cuts-govern-terminal` converter flag (default OFF —
shifts results for every cut-priced reservoir, so opt-in per case).
When set with `boundary_cuts_mode=combined`, it drops `efin`/
`efin_cost` from every cut-covered reservoir so the loaded cuts are
the sole terminal-value mechanism — PLP's CFUE behaviour.  Non-cut
reservoirs keep their hard `efin` bound (gtopt already uses hard
`>=` when `efin_cost` is unset — PLP's non-CFUE branch).  End-to-end
through the real converter: ELTORO 1039 (matches the hand-edit),
removing both the hoarding and the objective pollution.

**Discount division (checked)**: the writer divides the cut water
value by the last stage's discount factor, and gtopt's LP RE-applies
that discount via `scenario_stage_ecost` (`cost_helper.hpp:139`,
`cost × cost_factor` where cost_factor carries the discount).  The
two exactly cancel — the LP objective coefficient equals the raw
discounted cut gradient `GradX` either way.  The division is not a
correctness lever; it only keeps the JSON `efin_cost` value in the
same undiscounted "face-value" frame as auto-estimated reservoirs
(which the LP then discounts identically).  It does NOT drive the
ELTORO residual (374k vs 411k is a 10% cosmetic shift; the residual
is the soft-slack, now addressed by --cuts-govern-terminal).
