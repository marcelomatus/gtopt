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

* **A1** `update_lp` re-clamp vs one-sided substitution: a FlowRight
  with `bound_rule` + fcost-only + explicit `fmax > target` must keep
  `uppb = min(fmax, target)` after a bound-rule tick (today it relaxes
  to `rb.fmax`, silently un-doing the substitution and corrupting the
  objective).  Write the test first — it documents the known P1 bug in
  `flow_right_lp.cpp:1081-1097`.
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

* **B1** Zone-formula golden test: for a grid of volumes
  (0, 600, 1200, 1370, 1500, 1900, 2000, 5582), assert
  `evaluate_bound_rule(irr_segments, V) + evaluate_bound_rule(mixed_segments, V)`
  equals the CEN Tabla 1 riego column (this is what exposes the missing
  mixed→riego +30 hm³ transfer above colchón 1 — write it xfail until
  the transfer is implemented).
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
* **D5 Anchoring acceptance (blocked on the anchoring fix)**: once the
  partition is coupled to the physical El Toro flow, assert
  `laja_q_turbinado == turbine flow` per block and that burning rights
  actually drains the reservoir ledger-consistently.  Until then, D1
  documents the absence (no row links `frt_*` to `rsv_*`/waterway
  columns).

## Known-gap ledger (tests intentionally xfail/absent until fixed)

1. Physical anchoring of rights flows (Laja & Maule) — D5.
2. IVGAF debit of the december provision (`genpdlajam.f:234-239`).
3. Mixed→riego +30 hm³ transfer above colchón 1 — B1.
4. `qdrh + qdmh + qgah ≤ qdefm` attribution cap (`genpdlajam.f:290-296`).
5. Maule monthly electric-counter reset (PLP resets EVERY month, not
   january) and compensation recompute at year start.
6. `update_lp` re-clamp vs substitution — A1.
