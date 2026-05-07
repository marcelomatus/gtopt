# LP Numerics Specialist Review — `gtopt_marginal_units` Methodology

**Reviewer**: LP Numerics Expert agent  
**Target**: `/home/marce/git/gtopt-hygiene/docs/scripts/gtopt_marginal_units_plan.md`  
**Date**: 2026-05-05  
**Sections covered**: §2 (entire), §3.2, §4.3, §4.4, §4.5, §4.7, §4.8, §6

---

## P0 — Blocking defects (must fix before any implementation)

### P0.1 — §3.2: `flowp_cost` is the reduced cost on the flow column, not a capacity row dual

**Section/line**: §3.2, table note "Which line dual is the congestion price?"

**Defect**: The plan states that `flowp_cost` is "the bound multiplier on
`-tmax_ba ≤ f_l ≤ tmax_ab`". The phrasing implies a Lagrange multiplier on a
constraint row. The LP assembly in `source/line_lp.cpp:287` writes
`out.add_col_cost(cname, FlowpName, ...)`, which emits the **reduced cost of
the `flowp` column**, not a row dual. The actual capacity constraint rows
(emitted as `CapacitypName` / `CapacitynName` via `add_row_dual` at
`source/line_lp.cpp:295-296`) produce output files `Line/capacityp_dual.csv`
and `Line/capacityn_dual.csv` — but these rows only exist when a capacity
expansion variable is present (see `source/line_losses.cpp:370-377`). In the
standard non-expansion case (all IEEE benchmarks) there are no capacity rows;
`tmax` is enforced as a variable bound on `flowp` / `flown`, and the reduced
cost on `flowp` equals the active bound multiplier at optimality via KKT. So
the numerical signal is correct empirically (verified on ieee_14b congested
blocks: `flowp_cost` uid:14 = 830 $/MWh at blocks 20–23 where `flowp_sol`
saturates at -50 MW), but the mechanism is wrong for the expansion case.

**Consequence**: When a line has an expansion capacity column (`capacity_col`
present), the flow variable bound is replaced by the capacity constraint row,
and the reduced cost on `flowp` is no longer the congestion dual — it is zero
(the flow variable is free between ±capacity). The congestion shadow would
instead be in `capacityp_dual` / `capacityn_dual`, which the plan's canonical
schema does not expose at all.

**Remediation**:
1. Add `Line/capacityp_dual` and `Line/capacityn_dual` to the canonical schema
   §3.3.3 alongside `flow_dual`. The zone-partition logic in §4.3 must prefer
   `|capacityp_dual| + |capacityn_dual|` when `capacityp_dual` is non-null,
   falling back to `|flowp_cost|` for the no-expansion case.
2. Reword the §3.2 note: "`flowp_cost` is the reduced cost of the
   `flowp` column, which equals the capacity bound multiplier when the line
   capacity is enforced as a variable bound (no expansion). When a line has
   an expansion variable, use `capacityp_dual` / `capacityn_dual` instead."
3. Add to `test_tolerances.py`: a 2-bus case with an expansion capacity column
   where `flowp_cost = 0` but `capacityp_dual > 0` at saturation — verify the
   zone partition still fires.

**Sign note**: The empirical data also reveals that `flowp_sol` can be
negative at saturation (uid:14 at blocks 20-23 shows flowp_sol=-50, tmax_ba=50).
The condition `|flow[l]| ≥ tmax_l · (1 − tol_flow)` in §4.3 is correct for
this, but it must use `abs(flowp_sol - flown_sol)` or the raw `flow_sol` if
that composite column is available — not `flowp_sol` alone. The canonical
schema §3.3.3 should record which composite/signed convention the reader uses.

---

### P0.2 — §4.5: unit mismatch in `forced_pmin_marginal` escape hatch

**Section/line**: §4.5, "zone load equals the sum of pmin of those units
within `tol_price`"

**Defect**: `tol_price` is defined as `1e-3 · max(|λ_g|, 1.0)` — a price
tolerance in $/MWh. Zone load and `Σpmin` are both in MW. Comparing
`|zone_load − Σpmin| ≤ tol_price` mixes units and the comparison is
numerically nonsensical. A LMP of 100 $/MWh gives `tol_price = 0.1`, so any
zone load mismatch exceeding 0.1 MW is rejected — that is tight to the point
of being useless in real data where metering noise is O(1 MW). A LMP of 1 $/MWh
gives `tol_price = 0.001 MW` — absurdly tight.

**Remediation**: Replace with a separate `tol_load_mw` tolerance (suggested
default: `1.0 MW`, CLI-overridable), distinct from `tol_price`. The test
condition becomes:
```
|zone_load − Σ pmin[g for g forced_pmin in z]| ≤ tol_load_mw
```
Update §4.5 and the CLI table in §5.

---

### P0.3 — §4.7 R1: PTDF inversion is singular for radial and islanded topologies

**Section/line**: §4.7, Step R1 "PTDF · netload, no optimisation, just linear
algebra over a fixed reference bus"

**Defect**: The PTDF matrix is built from the reduced Laplacian of the network
graph (with one reference bus removed). For a **radial topology** (tree) the
reduced Laplacian is non-singular, but it is extremely ill-conditioned when any
branch has near-zero admittance (reactance → ∞). For a **disconnected topology**
(physical islands — permanent or resulting from `active=false` lines) the
Laplacian is singular by construction: the rank is `n_buses − n_islands`. The
plan provides no guard against this.

**Consequence**: In `real-reconstruct` mode with a missing-flows input, the
PTDF solve silently produces garbage flows and wrong zone partitions for the
many Chilean dates when the north-south interconnection trips (a known
operational event). The reconstruction will report the wrong marginal unit and
the wrong zone LMP for every cell in that date range, with no warning.

**Remediation**:
1. Before constructing PTDF, detect connected components of the topology graph
   (all lines with `active=true` and `tmax > 0`). For each component build a
   separate block-diagonal PTDF (one reference bus per component). If any
   component has fewer than 2 buses, skip PTDF for it and use the single-bus
   fallback.
2. After the PTDF solve, verify residual balance: `|B θ − p|_∞ / |p|_∞ ≤
   1e-6`. If violated, log a warning and downgrade the cell to
   `confidence=fallback` with reason `ptdf_residual_large`.
3. Guard against ill-conditioning: if `κ(B_reduced) > 1e8` (estimated cheaply
   via `np.linalg.cond` on the small reduced Laplacian), warn and fall back to
   the single-bus zone.
4. Add a test in `test_reconstruct.py` covering a 3-island topology where the
   PTDF is block-diagonal — verify the zone partition is correct per island.

---

### P0.4 — §4.7 R3: all-pmax-bound zone has LMP strictly above any unit MC

**Section/line**: §4.7 Step R3 corner case "use cheapest pmax-pinned unit's
MC, flag degenerate"

**Defect**: LMP theory (Hogan 1992, CAISO Tariff) says: when every unit in a
zone is at `pmax` (aggregate supply < demand), the zone is in **involuntary
curtailment**. The LMP in that case equals `demand_fail_cost` — it is bounded
above by the penalty, not below by any unit MC. Setting `λ_z` to the
cheapest pmax-pinned unit's MC gives a value that is typically far below
`demand_fail_cost`, producing a nonsensical reconstruction. Moreover, the
"cheapest pmax-pinned unit" being the marginal unit contradicts the standard
theory: the last unit dispatched to its cap is an inframarginal unit that
happens to also be capacity-constrained; it does not set the price.

**Remediation**: Replace the corner-case rule with the correct one:
```
if zone_load > Σ pmax[g in z]:
    λ_z = demand_fail_cost
    emit __demand_fail__ as marginal "unit"
elif no interior unit in z:
    # Every unit is at pmax but load is cleared: degenerate case.
    # The LMP is above the highest unit MC (by complementary slackness
    # on the demand-fail variable: demand_fail ≈ 0 but its reduced cost
    # could be nonzero). Set λ_z = demand_fail_cost only if load ≈ Σpmax;
    # otherwise flag as truly degenerate and emit __unattributed__.
    if |zone_load − Σ pmax| ≤ tol_load_mw:
        λ_z = demand_fail_cost  # boundary condition
    else:
        emit __unattributed__ with reason "no_interior_unit_and_load_mismatch"
```
This is consistent with §2.1 (demand-fail as the marginal "unit") and avoids
emitting a unit MC that is below the correct price.

---

## P1 — Should-fix before v1

### P1.1 — §4.4 classifier: `extramarginal_interior` fires on legitimate
degenerate LP bases

**Section/line**: §4.4, rule row "`extramarginal_interior` (should not occur)"

**Defect**: The plan flags `extramarginal_interior` as "should not occur" and
attributes it entirely to LP-degeneracy or segment mismatch. But this status
fires legitimately in two well-known LP-stable situations:

1. **Piecewise-linear cost above LMP from a cheaper segment**: a unit with
   two segments `[0, 50MW at $30/MWh]` and `[50, 100MW at $80/MWh]` running
   at 70 MW in a zone with LMP=$50 has its active-segment MC=$80 > λ_z=$50.
   The unit is economically interior (earning rents on the first segment) but
   the segment lookup returns the wrong MC because the segment boundary
   coincides with — or the dispatch is just above — the break.
2. **Degenerate optimal basis** (multiple optima): the solver chooses a
   basis where a $90/MWh unit is interior and a $50/MWh unit is at pmin,
   even though both are individually optimal. This is valid LP behaviour.

In case 1 the rule is over-strict (the unit is correctly inframarginal, not
extramarginal). In case 2 the flag is useful but should not abort or error —
it should emit a `degenerate=True` row.

**Remediation**:
- Add a `segment_boundary` reason: when the dispatch is within `ε` of a
  segment break, mark `active_segment=-1` and do not flag `extramarginal_interior`
  (§2.5 already describes this; the classifier must apply it before the MC
  comparison).
- For the remaining true `extramarginal_interior` cases (after segment boundary
  correction), emit with `degenerate=True` and reason `extramarginal_interior`;
  do not raise. Add a `--strict` flag that turns it into an error for
  debugging runs.

---

### P1.2 — §4.4 classifier: `forced_pmin` vs degenerate-interior-at-pmin
distinction requires reduced-cost sign check

**Section/line**: §4.4, rule row "`forced_pmin`": `pmin_g − ε < disp[g] < pmin_g + ε AND pmin_g > 0`

**Defect**: The plan uses only bound proximity to classify a unit as
`forced_pmin`. In LP terms, a unit is *forced* at pmin (positive reduced
cost at the lower bound) when its reduced cost `rc_g = MC_g − λ_b > 0`.
A degenerate optimal basis can place a unit interior at pmin with `rc_g = 0`
(the unit is marginal but coincidentally dispatched at pmin). The plan does
not distinguish these cases.

In `simulated` mode (LP duals available), the reduced cost is directly
available as `generation_sol`'s corresponding `generation_cost` column (the
`add_col_cost` output). The plan's §3.2 table notes `Generator/generation_cost`
as "optional" but does not specify that it carries the **reduced cost**, not
the operating cost. This creates a naming ambiguity in the canonical schema.

**Remediation**:
- In `simulated` mode: if `disp ≈ pmin` AND `rc_g` (from `generation_cost`
  output, i.e. the reduced cost) > `tol_price`, classify as `forced_pmin`.
  If `rc_g ≈ 0`, classify as `marginal` (the unit is both at pmin and at
  the merit-order price — boundary-marginal). This is the standard
  complementary-slackness interpretation.
- In `real` / `real-reconstruct` mode (no LP duals): the ambiguity is
  irresolvable. Flag with `degenerate=True` and reason
  `pmin_boundary_no_dual`.
- In §3.2 rename the row `Generator/generation_cost` to
  `Generator/generation_cost` (reduced cost) with an explicit note that
  this file carries `rc_g`, not `MC_g · disp` — the naming is confusing.

---

### P1.3 — §4.2 / §6.1: `balance_dual` scale assertion is insufficient

**Section/line**: §4.2 note "scale_objective is not applied to LMPs"; §6 item 1

**Defect**: The plan states that `Bus/balance_dual` is "already in $/MWh"
because gtopt scales the objective, not the output. This is approximately
correct but incomplete. From `output_context_icost_unfolding` memory: the
reported row dual is `get_row_dual() × block_icost_factors` where
`block_icost_factors = scale_obj / (prob × discount × duration)`. In a
multi-scenario SDDP run, `prob` and `discount` vary per scene, meaning two
buses in different scenes can have the same physical LMP but different raw
`balance_dual` values unless the unfolding is done correctly. The assertion
`λ_z ≤ 1.05 × demand_fail_cost` only catches gross scale errors; it does not
catch per-scenario unfolding inconsistencies.

Additionally, the `scale_objective` sanity check is one-sided: it only checks
if `λ_z` is *above* the cap, not if it's anomalously small (e.g. missing the
`block_icost_factors` multiplication entirely would yield LMPs near zero without
triggering the cap test).

**Remediation**:
- Add a two-sided sanity check: assert `λ_z ≥ 0` (LMPs are non-negative for
  a standard minimisation with non-negative costs) and `λ_z ≤ 1.05 ×
  demand_fail_cost`. The lower bound catches the sign-flip that would result
  from a missing `scale_obj` in the dual recovery.
- For multi-scenario runs, verify that `balance_dual / block_icost_factors`
  is consistent across scenes for an identical dispatch — or at minimum,
  document that the assertion fires per cell, not across scenes.

---

### P1.4 — §4.7 R3: hydro/battery interior with no declared MC breaks λ_z
computation

**Section/line**: §4.7 Step R3

**Defect**: The R3 rule computes `λ_z = max{ declared_MC[g] }` over interior
units. For hydro / battery units, `declared_MC` is `None` (the plan
acknowledges this in §2.4: "we use their discharge price from the reservoir
dual"). In real/real-reconstruct mode, reservoir duals are absent. So if the
only interior unit in a zone is hydro, `max{}` is over an empty set and
`λ_z` is undefined.

The plan's §2.4 says "for v1 we simply flag hydro/battery as `hydro_marginal`
when interior, and report the bus LMP side-by-side". This is internally
consistent in `simulated` mode (LMP is available from the LP). But in
`real-reconstruct` mode, when hydro is the only interior unit, R3 produces no
λ_z and the entire reconstruction fails silently.

**Remediation**:
- In R3, when the only interior units are `kind ∈ {hydro, battery}`, set `λ_z
  = None` and emit `confidence=fallback, reason=hydro_no_water_value`.
  Append a note in the summary: "zone Z: LMP unrecoverable without water
  value — hydro/battery is the only interior unit."
- Add a test in `test_reconstruct.py` covering a zone where the only
  dispatched unit is hydro with no declared_MC. Verify exit-3 is NOT raised
  (it's not a missing-minimum-data error) but the output row has
  `confidence=fallback` and `zone_lmp=None`.

---

### P1.5 — §4.3: dual-only saturation test (`tol_mu = 1e-2`) may be
too tight when scale_objective differs from default

**Section/line**: §4.3 saturation condition `|mu[l]| > tol_mu`; §4.4
tolerance table

**Defect**: `tol_mu = 1e-2` is defined as an absolute threshold on
`flowp_cost`, which is in physical $/MWh (after `block_icost_factors`
unfolding). For a case with `demand_fail_cost = 10` (hydro-dominated system,
plausible for Chilean irrigation runs), a congestion shadow of 1 $/MWh is
economically meaningful but fails `tol_mu = 1e-2` by two orders of magnitude
— no, wait, 1 > 0.01, this would pass. Actually `tol_mu = 1e-2 = 0.01` passes
any congestion dual ≥ 0.01 $/MWh. The concern is the reverse: for a case
with `demand_fail_cost = 10000`, a 1 $/MWh shadow is a 0.01% signal — is it
real congestion or LP noise? The same absolute threshold applied to both
cases produces inconsistent sensitivity.

**Remediation**: Define `tol_mu` as a **relative** threshold:
`tol_mu = 1e-3 · max(|λ_z|, 1.0)` — the same relative form used for
`tol_price`. This automatically adapts to the price scale. Update the §4.4
tolerance table and §4.3. The absolute floor `max(…, 1.0)` prevents the
threshold collapsing to zero on near-zero-LMP zones.

---

### P1.6 — §4.8.1: "three artifacts is enough" claim hides a reactance
dependency in R1

**Section/line**: §4.8.1 strict minimum, §4.8.3 required-by-mode table

**Defect**: The plan claims that topology + dispatch + demand is sufficient
for `real-reconstruct`. But Step R1 uses PTDF when line flows are absent, and
PTDF requires **line reactances** (or susceptances). The canonical topology
schema §3.3.3 shows `line(uid, bus_a_uid, bus_b_uid, tmax_ab, tmax_ba,
active)` — no reactance field. The plan mentions a "uniform reactance fallback"
but does not say where the reactance comes from when present. A uniform
reactance PTDF is geometrically a node-degree-weighted average that is wrong
whenever the real network is even mildly non-uniform (which is always true for
a real grid).

A uniform-reactance PTDF will misidentify zone boundaries because it assigns
the wrong fraction of injection to each line, potentially marking a lightly
loaded line as saturated and a heavily loaded line as uncongested. This
propagates directly into wrong zone partitions and wrong LMP attributions.

**Remediation**:
1. Add `reactance: float|None` to the canonical `line` schema in §3.3.3.
   When populated, PTDF uses it. When absent, the script uses uniform
   reactance AND emits a one-time `WARN: PTDF built with uniform reactance;
   zone partition may be inaccurate` — **it must not silently proceed**.
2. For `cen2gtopt` (Phase 2): reactance should be sourced from SIP `lineas`
   when available. Add an explicit data-dependency note to §9.
3. The `test_reconstruct.py` test for the 3-bus PTDF case must include a
   non-uniform reactance fixture to verify the result differs from uniform.

---

## P2 — Nice-to-have before v1.1

### P2.1 — §4.6 confidence: exact single-zone, single-unit case should upgrade
to `lp_dual`

**Section/line**: §4.6, §4.7 R4

The plan always emits `confidence=merit_order` for real-reconstruct mode
regardless of how deterministic the attribution is. When there is exactly one
zone, exactly one interior unit, and `|MC_g − λ_z| < tol_price`, the
reconstruction is as unambiguous as an LP dual read-out. Staying at
`merit_order` understates the reliability. Propose: when the attribution is
unique (one candidate, no degenerate flag), upgrade to
`confidence=merit_order_unique` to let downstream consumers distinguish the
unambiguous case from the one-of-many case.

---

### P2.2 — §6 edge cases: missing items worth documenting

**Section/line**: §6

The following edge cases are not addressed:

- **DC HVDC links / asynchronous interconnections**: a DC link between two
  AC zones is not part of the AC network Laplacian and must be excluded from
  the PTDF computation. Its flow is a fixed injection (controllable), not
  governed by Kirchhoff. The zone partition must treat it as a scheduled
  generator/load, not as a transmission line. Add a `kind: hvdc` flag to the
  line topology schema.

- **Phase-shifting transformers**: a phase shifter controls flow
  independently of the natural KVL, making the PTDF model wrong for that
  branch. Flag `kind: phase_shifter`; exclude from PTDF; treat controlled
  flow as a "line load shedding" boundary injection.

- **Batteries treated as load during charging**: the classifier rule for
  `kind ∈ {hydro, battery}` covers generators but not batteries in charging
  mode (negative injection). A battery at `pmin < 0` (charging) that is
  dispatched at its charging lower bound should be classified as
  `capped_charge` (analogous to `capped_pmax` but on the charging bound).
  The current table only handles the discharge side.

- **Unit commitment startup costs / no-load costs**: in a MIP-solved dispatch,
  a committed unit's effective MC includes amortised startup cost, which may
  be above the declared `gcost`. The merit-order R3 rule using `declared_MC`
  alone will produce wrong λ_z when startup costs are significant. Flag
  with `confidence=merit_order_approximate` if the feed indicates UC-level
  dispatch (commitment status column present in canonical feed).

- **Reserve constraints**: when a spinning reserve constraint binds, the
  marginal unit for reserve is a different unit than the marginal unit for
  energy. The current classifier conflates the two. For v1 this is acceptable
  (note it explicitly); for v1.1 a `reserve_dual` column in the canonical
  schema would disambiguate.

---

### P2.3 — §3.2: `theta_dual` role needs clearer documentation

**Section/line**: §3.2, table row `Line/theta_dual`

The plan says `theta_dual` is "the KVL Lagrangian (informational)". From the
code (`kirchhoff_node_angle.hpp`, `source/line_lp.cpp:303`), `theta_rows`
holds the KVL equality row indices for the node-angle formulation
`-θ_a + θ_b + x_τ · f_p - x_τ · f_n = -φ`. The dual of this equality is
the "KVL multiplier", which is nonzero for all lines in the Kirchhoff network
(not just congested ones) and carries units of `$/rad` (physical dual of an
angle-constrained equality). It is NOT the congestion shadow price. The
empirical evidence confirms this: in the ieee_14b case, `theta_dual` is
nonzero for all 20 lines at all blocks, including uncongested blocks, while
`flowp_cost` is zero for uncongested lines.

The plan should explicitly warn users not to use `theta_dual` as a congestion
indicator. Its correct use is: verify KVL consistency (`θ_b - θ_a = x_τ · f`)
and diagnose KVL-row ill-conditioning.

---

## Invariant test for §3.2 dual identification (proposed)

The plan mentions "an invariant test in §7 will pin this down on a
deliberately-congested 2-bus case." Here is the concrete invariant to implement
in `test_tolerances.py`:

```python
# 2-bus case: bus A (cheap gen $20/MWh, pmax=100MW) + bus B (expensive gen
# $90/MWh, pmax=50MW) connected by a line with tmax=40MW.
# Load at B = 60 MW. Expected:
#   - flowp_sol ≈ 40 MW (line saturated, A→B direction)
#   - flowp_cost > 0 (congestion shadow = LMP_B − LMP_A = 70 $/MWh)
#   - theta_dual ≠ 0 (KVL row always active)
#   - capacityp_dual ≡ absent (no expansion variable → no capacity row)
# Assertion: flowp_cost ≈ lmp_B − lmp_A − tcost (transfer cost, if any)
assert abs(flowp_cost_line1 - (lmp_B - lmp_A)) < tol_price
assert theta_dual_line1 != 0     # always nonzero for KVL
assert 'capacityp_dual' not in output_files  # no expansion column
```

This pindown test validates the congestion-identification claim independently
of the IEEE benchmark cases.

---

## Summary table

| ID | Priority | Section | One-line issue |
|---|---|---|---|
| P0.1 | P0 | §3.2 | `flowp_cost` is reduced cost, not capacity row dual; breaks for expansion lines |
| P0.2 | P0 | §4.5 | `forced_pmin_marginal` uses $/MWh tolerance to compare MW loads |
| P0.3 | P0 | §4.7 R1 | PTDF is singular for islands; no guard, silent wrong zones |
| P0.4 | P0 | §4.7 R3 | All-pmax zone: cheapest pmax-unit MC is wrong LMP; should be demand_fail_cost |
| P1.1 | P1 | §4.4 | `extramarginal_interior` fires on segment-break units; too strict |
| P1.2 | P1 | §4.4 | `forced_pmin` needs reduced-cost sign check, not just bound proximity |
| P1.3 | P1 | §4.2/§6.1 | `balance_dual` scale assertion is one-sided; misses small-LMP errors |
| P1.4 | P1 | §4.7 R3 | Hydro-only interior zone: `max{}` over empty set, λ_z undefined |
| P1.5 | P1 | §4.3 | `tol_mu` absolute threshold inconsistent across price scales |
| P1.6 | P1 | §4.8.1 | "3 artifacts enough" hides reactance dependency for PTDF; uniform fallback must warn |
| P2.1 | P2 | §4.6 | Single-zone/single-unit reconstruction should upgrade confidence label |
| P2.2 | P2 | §6 | Missing edge cases: HVDC, phase shifters, battery charging side, UC startup costs |
| P2.3 | P2 | §3.2 | `theta_dual` is not a congestion indicator; needs explicit documentation warning |
