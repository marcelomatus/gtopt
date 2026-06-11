# Hydro Topology — Forced-Flow Wiring Review (corrected)

**Date:** 2026-06-07
**Scope:** forced hydro obligations (`Riego_*` irrigation, `Caudal_Eco_*`
ecological, `Filt_*` filtration/seepage, `Ext_*` external, `Vert_*` spill) on
the CEN PCP weekly bundle (`20251005`).

> **Correction note.** An earlier draft of this doc claimed "irrigation not
> modelled / FlowRights silently dropped." **That was wrong** — caused by two
> author errors: (1) reading the JSON under the wrong key (`flowright_array`
> instead of `flow_right_array`), and (2) misreading `pinned_count = 9` as
> "9 targets, 5 dropped." The actual state is below: **all active forced
> flows are correctly wired, by an intentional topology-based split.**

---

## Finding: the wiring is complete and intentional

The converter picks the LP primitive per forced flow **by topology** — where
the water goes in the PLEXOS graph. This split is **intentional**:

| forced flow | water destination | gtopt primitive |
|---|---|---|
| `Riego_*` (irrigation) | leaves basin (→ `_sink`) | **FlowRight** (consumptive, debits source) |
| `Filt_Colb` | leaves to sink | **FlowRight** |
| `Caudal_Eco_Ralco` (eco) | stays in river (→ PANGUE) | **pinned waterway** (`fmin=fmax`) |
| `Filt_Laja`, `Filt_Inv` | → downstream reservoir | **pinned waterway** |
| `Ext_Maule` | → downstream | **pinned waterway** |
| `Vert_*` (spill) | → ocean | **junction drain** |

**Rule:** consumptive (water leaves the basin) → FlowRight; pass-through
(water stays in the river) → pinned waterway; operator spill → junction drain.

### Coverage on `20251005` — nothing dropped
- **4 FlowRights** emitted (`soft_Riego_NoGen_Colbun`, `soft_Riego_RUCUE`,
  `soft_Riego_SANIGNACIO`, `soft_Filt_Colb`), **0 dropped** in
  `_synthesise_pinned_flow_rights`.
- **9 pinned waterways** (eco / filt-to-downstream / ext).
- **junction drains** for the `Vert_*` spills.
- **All 3 filtrations/seepage wired** (`Filt_Colb` FlowRight, `Filt_Inv` +
  `Filt_Laja` waterways).
- Every candidate that *looked* missing (`Riego_B_Maule`, `Vert_LMaule`,
  `Vert_ANTUCO`, `Vert_ELTORO`, …) has **PLEXOS flow = 0** this period — no
  obligation to model. (`Vert_ELTORO` is also `never_drain` by design.)

So **everything PLEXOS actually routes, gtopt models.**

---

## What *was* worth keeping from the review

`_synthesise_pinned_flow_rights` (`parsers.py:5990`) used a silent
`continue` when a forced obligation's source junction was not emitted — a
latent bug that would make a *future* obligation vanish without trace. Added a
`logger.warning` so any such drop is visible (it fires **0** times today).
Defensive only; no behaviour change.

---

## The real residual: RALCO/Biobío dispatch, not topology

With the wiring confirmed correct, the residual **+9 % op-cost gap** is **not**
a forced-flow / seepage issue. It is the **RALCO over-conservation**:

- RALCO stores ~87 % of its 35,861 inflow (`extraction` = net fill) and
  turbines only 151 MWh; PLEXOS turbines the same inflow for 26,119 MWh.
- The water value (3,441) is **correct / PLEXOS-matched** — the lever is its
  **translation to $/MWh** via `production_factor` (1.489) and the CMD
  storage-unit scaling, and how the single flat terminal value propagates
  in-week. This is the open item; it is independent of the (correct)
  forced-flow topology.
