# plp2gtopt vs plexos2gtopt — hydro topology comparison

Read-only analysis. All citations are file:line.

## Critical comparison vs plp2gtopt

### 1. Spillway (`_ver` / `Vert_*`) handling

| Aspect | plp2gtopt | plexos2gtopt |
|---|---|---|
| Default arc | Emit explicit `<central>_ver` Waterway from central → `ser_ver` junction, `fmax = VertMax`, `fcost = CVert` (or `rebalse_cost` for vrebemb) (`scripts/plp2gtopt/junction_writer.py:1125-1132`) | Emit `Vert_*` Waterway redirected to synthetic `<source>_ocean` drain (`scripts/plexos2gtopt/parsers.py:1782-1786`) — never to next reservoir |
| ser_ver=0 / ocean | Encode spill on the source junction itself: `Junction.drain_capacity = VertMax`, `Junction.drain_cost = CVert` (`junction_writer.py:1134-1215, 1383-1388`) | Same ocean redirect via `<source>_ocean` drain junction |
| Cost source | `CVert` from `plpmat.dat` (default 1.0 if absent, `junction_writer.py:1739-1743`); `Costo de Rebalse` from `plpvrebemb.dat` per reservoir | `Max Flow Penalty` from PLEXOS Waterway static prop (`parsers.py:1802-1803`); typical 3.6 $/(m³/s·h) for `Vert_*` |
| Reservoir-level `spillway_cost` | Set on the Reservoir record only as a vestigial `Costo de Rebalse` field, but `spillway_capacity=0` disables the LP drain teleport (`junction_writer.py:1693-1715`). Spill physically flows via the `_ver` arc. | `spillway_cost` only set when PLEXOS ships `Spill Penalty` (currently never on CEN PCP) → reservoir-internal drain is **disabled by default** (`gtopt_writer.py:747-765`) |
| `Vert_ELTORO` (operator-controlled) | Always emitted (PLP wires it via `ser_ver`) | **Dropped entirely** (`parsers.py:1734-1743`) — operator-controlled, PLEXOS keeps at zero via Water Value |

### 2. Forced flows (Filt / Riego / Caudal_Eco)

- plp2gtopt **does not** model PLEXOS Riego / Caudal_Eco at all. Filtrations come from
  `plpfilemb.dat` and are emitted as a `filt_<embalse>` Waterway from reservoir → receiving
  central, with **piecewise-linear, volume-dependent slope/constant** carried by a
  `ReservoirSeepage` record (`junction_writer.py:2201-2230`). The LP updates the
  coefficients dynamically (no fixed pin); not `fmin = fmax = const`.
- plexos2gtopt **pins** every forced-flow waterway as `fmin = fmax = forced_target` (or
  per-block matrix when CSV varies), uniformly for `Filt_*`, `Riego_*`, `Caudal_Eco_*`,
  `Ext_*` (`parsers.py:1804-1850`). Hard constraint, no soft slack.
- Direction: **plp2gtopt waterway-fmin → soft FlowRight** transform happens after the
  fact in `pmin_flowright_writer.py:682-857`: every non-zero `Waterway.fmin` is converted
  to a soft `FlowRight` (`direction = -1`, `fcost = fail_cost`), and the waterway fmin is
  zeroed — i.e. **plp2gtopt softens its own forced flows**, plexos2gtopt keeps them hard.

### 3. Bypass arcs (`B_*`)

- plp2gtopt has **no concept of an explicit bypass arc**. PLP equivalent: pmin on transit
  centrals (`bus=0`) wired via `plpmance.dat` per-stage envelopes onto the gen waterway
  (`junction_writer.py:1265-1289`), then softened in `pmin_flowright_writer.py:298-310`.
- plexos2gtopt's `B_*` bypass: emits the CSV profile as `fmin` only, leaves `fmax`
  unbounded (`pin_fmax_from_profile = False`, `parsers.py:1844-1850`, `gtopt_writer.py:820-825`).
- plp2gtopt also has the `FlowRight.bypass_junction` feature wiring `qriego` to an
  irrigation district sink, but **only** under the laja/maule agreement model
  (`laja_writer.py`, `maule_writer.py`) — not for generic forced flows.

### 4. Penstock `fmax`

- plp2gtopt pins **every** gen waterway: `fmax = PotMax / Rendi` (m³/s)
  (`junction_writer.py:853-869`), independent of whether a turbine entry exists. Sentinel
  `PotMax ≥ 9000 MW` → unbounded.
- plexos2gtopt synthesises a **per-turbine zero-cost penstock** with
  `fmax = pmax_peak / production_factor` (`gtopt_writer.py:891-942`). `pmax_peak` is the
  **max over the horizon** of the generator's pmax_profile — looser than plp2gtopt's
  static cap because plp2gtopt uses `PotMax` (the nameplate), while plexos2gtopt's
  `pmax_peak` is *also* the nameplate when pmax is static, but the per-block fmax is NOT
  enforced. So both approaches yield the same nameplate cap; **plexos2gtopt does not pin
  per-block fmax even when `pmax_profile` is zero at midnight**.

### 5. `Junction.drain_capacity` / `drain_cost` (commit 3d977d57a)

- plp2gtopt **does** use them, exclusively for the ser_ver=0 + vrebemb-or-VertMax>0 path
  (`junction_writer.py:1167-1215, 1383-1388`). Saves one Junction + one Waterway per
  terminal central. Specifically motivated by LMAULE / ELTORO degeneracy (`junction_writer.py:1145-1166`).
- plexos2gtopt **does not** use `drain_capacity` / `drain_cost`. It synthesises ocean
  Junctions + a Waterway to them (`parsers.py:1782-1786`), the legacy / Waterway-based form.

## Why plexos2gtopt over-dispatches hydro by +76%

Putting the differences together against the +76% over-generation (505 GWh gtopt vs
286 GWh PLEXOS):

1. **Vert spillways carry zero flow in gtopt because the LP prefers turbining**. Both
   converters route spill to an ocean drain, but plp2gtopt's `_ver` arc carries a
   per-flow `fcost` (CVert ≈ small) AND the reservoir's effective spill cost via the
   `_ver` arc, while in plexos2gtopt PLEXOS's `Max Flow Penalty` (typically 3.6) is on
   the spillway Waterway *but* `spillway_cost` on the Reservoir is disabled
   (`gtopt_writer.py:747-760`). With `Vert_ELTORO` dropped (`parsers.py:1734-1743`), the
   only Laja-cascade exit is through PEHUENCHE / ELTORO penstocks. Result: LP prefers
   generation (zero cost) over spill (3.6 $/(m³/s·h)).

2. **Per-block penstock fmax is uncapped at low-pmax blocks**. plexos2gtopt's
   `pmax_peak / pf` is the max over the horizon, so when a unit's pmax dips to 0
   (e.g. EL_TORO_U1 at certain hours, comment at `gtopt_writer.py:886-896`), the
   penstock stays open at the peak cap while the generator equality `gen = pf × flow`
   is skipped (zero gen col). Water flows for free through the penstock.

3. **`Reservoir.efin` is pinned**: forces ALL the water to leave by horizon end.
   Combined with #1+#2, the only path for the "extra" water to leave is via the
   turbines because spillways are de-facto free outlets but **with no balance penalty**
   on the source reservoir — the LP picks turbines because they at least generate
   revenue/displace thermal.

### Structural fix candidates (taken from plp2gtopt)

- Pin `Junction.drain_capacity = VertMax` and `drain_cost = Max Flow Penalty` on every
  ocean junction (plp2gtopt's `junction_writer.py:1167-1215` approach), so spillage
  acquires a real cost on the LP balance.
- Add a non-zero `spillway_cost` to the Reservoir (CVert-style symmetry breaker,
  `junction_writer.py:1717-1748`) so the LP isn't indifferent between spill paths.
- Tighten penstock `fmax` per block from `pmax_profile / pf` rather than the horizon
  peak, mirroring plp2gtopt's per-stage `_write_transit_waterway_bounds`
  (`junction_writer.py:2683-2757`).
- Reconsider dropping `Vert_ELTORO`: plp2gtopt always emits its equivalent and lets the
  cost decide. The hard drop is the most aggressive structural difference and is the
  one likely creating the 7,479 m³/s·h missing spill.
