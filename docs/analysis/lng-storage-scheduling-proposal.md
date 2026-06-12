# LNG Storage and Scheduling: Critical Analysis and gtopt Proposal

## 1. Introduction

Liquefied Natural Gas (LNG) storage and scheduling is a key constraint in
power systems with gas-fired generation fed by LNG terminals rather than
pipeline gas.  The PLP Fortran solver includes a GNL (Gas Natural Licuado)
module that models LNG terminal storage as a volume-balance problem with
delivery schedules and generator coupling.  This document critically reviews
the PLP implementation, compares it with industry practice and academic
literature, and proposes an LNG model for gtopt that leverages the existing
`StorageLP` framework.


## 2. PLP GNL Model Review

### 2.1 Architecture Overview

The PLP GNL model is controlled by the `PLP_RESTGNL_MODE=si` environment
variable and reads data from `plpcnfgnl.dat`.  The implementation spans:

| File | Role |
|------|------|
| `pargnl.f` | Data structures (`TermGNL`, `PAR_GNL`) |
| `genpdgnl.f` | LP assembly, data reader, output writer |
| `getopts.f` | Activation via environment variable |
| `plp-faseprim.f` | Primal phase: sets RHS, propagates state |
| `plp-fasedual.f` | Dual phase: passes GNL state |
| `plp-agrespd.f` | SDDP cuts: GNL volume as state variable |

### 2.2 Mathematical Formulation

The PLP GNL module defines **three LP variables** per terminal per stage:

| Variable | Symbol | Description |
|----------|--------|-------------|
| `IVGNLF` | $V^f$ | Final LNG storage volume |
| `IVGNLE` | $V^e$ | Net LNG consumed in stage |
| `IVGNLV` | $V^v$ | LNG vented/spilled in stage |

**Constraint 1 — Volume balance (stage-level):**

$$V^f - V^e + V^v = V^{prev}$$

Where $V^{prev}$ is the final volume from the previous stage, set as RHS.

**Constraint 2 — Gas consumption coupling (block-level):**

$$V^e = \sum_{i \in \text{blocks}} \frac{\Delta t_i \cdot P_i}
       {\eta_{gnl} \cdot \eta_{cen,i} \cdot 3600}$$

Where $P_i$ is the block generation (MW), $\Delta t_i$ the block duration
(hours), $\eta_{gnl}$ the LNG-to-gas efficiency, and $\eta_{cen,i}$ the
per-generator gas-to-power efficiency.

**Objective function:**

$$\min \quad C_{alm} \cdot V^f \cdot \frac{1000 \cdot \text{edur}}{\Phi}
      + C_{ver} \cdot V^v \cdot \frac{1000}{\Phi}$$

Where $C_{alm}$ is the storage holding cost ($/m³/day, ÷24 for hourly) and
$C_{ver}$ is the venting penalty cost.

**Bounds:**

- $0 \leq V^f \leq V_{max}$
- $-\infty \leq V^e \leq +\infty$ (free: sign determined by delivery vs
  consumption)
- $0 \leq V^v \leq +\infty$

**SDDP state coupling:** The final volume $V^f$ is registered as an SDDP
state variable.  Benders cuts propagate the marginal value of stored LNG
backward through stages, exactly as reservoir volumes do.

### 2.3 Data File Format (`plpcnfgnl.dat`)

```
# header
# header
NumTerminals
# header (per terminal)
Id  Name  VMax  Vini  CGnl  CVer  CReg  CAlm  GnlRen
# header
NumLinkedGenerators
# header (per generator)
GeneratorName  Efficiency
# header
NumDeliveryEntries
# header (per delivery)
StageIndex  DeliveryVolume
```

### 2.4 Gas Network Model (`pargn.f`)

PLP also contains a separate `PAR_GN` structure defining a full gas transport
network with nodes, pipelines, compressors, regasification points, demands,
and generators.  This model appears experimental and decoupled from the main
GNL module — the two are not integrated in the LP assembly.


## 3. Critical Analysis

### 3.1 Strengths

1. **Simplicity and tractability.**  The storage-balance formulation is
   structurally identical to the well-understood reservoir model.  It adds
   only 3 variables and 2 constraints per terminal per stage, keeping the LP
   manageable.

2. **SDDP integration.**  GNL volume participates as a state variable in the
   Benders decomposition, which is correct: the marginal value of stored LNG
   should influence dispatch decisions in earlier stages.

3. **Direct generator coupling.**  The block-level consumption constraint
   correctly links thermal dispatch to gas volume via heat-rate conversion
   factors, creating an endogenous gas demand signal.

### 3.2 Weaknesses

1. **No boil-off gas (BOG) modeling.**  Real LNG terminals lose 0.05–0.15%
   of tank volume per day to boil-off [1].  PLP ignores this entirely.
   In a weekly-stage model with large tanks, this can represent several
   hundred m³ of unmodeled losses per stage.  By contrast, the gtopt
   `Reservoir` model already supports `annual_loss` for evaporation — an
   analogous mechanism.

2. **Fixed delivery schedule.**  LNG deliveries (`VolInpEta`) are exogenous
   deterministic inputs.  There is no stochastic modeling of cargo delays,
   spot market availability, or the option to schedule additional cargoes.
   In practice, LNG delivery uncertainty is a major operational risk,
   especially for systems dependent on spot LNG (e.g., Chile).

3. **No take-or-pay contracts.**  The model has no minimum offtake
   constraints.  Real LNG contracts typically impose annual or seasonal
   minimum purchase obligations with financial penalties for shortfall [2].
   PSR SDDP and PLEXOS both model these as rolling-window constraints.

4. **No regasification capacity limit.**  The consumption variable $V^e$ is
   unbounded ($\pm\infty$), meaning the model assumes infinite
   regasification send-out capacity.  Real terminals have physical limits
   (typically 2–15 MMSCFD per train) that can bind during peak demand.

5. **Single fuel source per generator.**  PLP hardwires each generator to
   one GNL terminal.  In reality, generators may switch between pipeline
   gas, LNG, and diesel depending on availability and price.  The model
   cannot represent dual-fuel capability or gas/LNG arbitrage.

6. **No storage holding-cost time structure.**  The `CAlm` cost is a fixed
   scalar.  Seasonal LNG price differentials and carrying costs vary
   significantly — winter storage is more valuable than summer storage in
   southern hemisphere systems.

7. **Crude unit conversion.**  The factor `3.6×10³` in the coupling
   constraint conflates the MJ→GJ conversion with the MWh→GJ conversion.
   The actual conversion should be $3.6$ GJ/MWh, with additional
   efficiency factors clearly separated.  The code divides by
   `GnlRen × CenRen × 3600`, which encodes `GnlRen × CenRen × 3.6 × 1000`
   — the `×1000` is a unit scaling artifact from the m³→km³ internal
   conversion.  This mixing of unit conversions with physics makes the code
   error-prone and hard to validate.

8. **No gas network integration.**  The `PAR_GN` gas network model exists
   in `pargn.f` but is not connected to the GNL module.  Pipeline
   constraints, gas node balances, and compressor costs are absent from the
   optimization.

9. **Variable cost override.**  `LeeTermGnl` overwrites the variable cost
   of linked generators with `CReg/Rendim` (line 363 of `genpdgnl.f`).
   This is a side-effect that silently changes generator economics when the
   GNL module is activated — a fragile coupling that makes it impossible to
   validate generator costs independently.


## 4. Literature and Industry Practice

### 4.1 Standard Formulations

The academic consensus models LNG terminal storage as a multi-commodity
inventory problem [3, 4]:

$$V(t) = (1 - \alpha) \cdot V(t-1) + D(t) - S(t) - R(t)$$

Where:
- $\alpha$ = BOG rate (fraction per period)
- $D(t)$ = cargo delivery
- $S(t)$ = send-out (regasification)
- $R(t)$ = re-liquefaction or BOG recovery

### 4.2 Key Features in Commercial Tools

| Feature | PLEXOS | PSR SDDP | PLP |
|---------|--------|----------|-----|
| Tank volume balance | Yes | Yes | Yes |
| BOG losses | Yes | Partial | **No** |
| Regasification limits | Yes | Yes | **No** |
| Take-or-pay contracts | Yes | Yes | **No** |
| Multiple fuel sources | Yes | Yes | **No** |
| Gas network | Full | Partial | Separate (unused) |
| SDDP state variable | — | Yes | Yes |
| Stochastic deliveries | Yes | Yes | **No** |
| Seasonal cost variation | Yes | Yes | **No** |

### 4.3 Key References

1. Fodstad, M. et al. (2010). "LNG Shipping and Inventory Management."
   *Computers & Chemical Engineering*, 34(10), 1602–1614.
2. Rakke, J.G. et al. (2011). "A rolling horizon heuristic for creating a
   liquefied natural gas annual delivery program."  *Transportation Research
   Part C*, 19(5), 896–911.
3. Correa-Posada, C.M. & Sanchez-Martin, P. (2015). "Integrated Power and
   Natural Gas Model for Energy Adequacy in Short-Term Operation."  *IEEE
   Trans. Power Systems*, 30(6), 3313–3324.
4. Zlotnik, A. et al. (2017). "Coordinated Scheduling for Interdependent
   Electric Power and Natural Gas Infrastructures."  *IEEE Trans. Power
   Systems*, 32(1), 600–610.
5. Chaudry, M. et al. (2014). "Combined gas and electricity network
   expansion planning."  *Applied Energy*, 113, 1171–1187.


## 5. Proposal: LNG Storage in gtopt

### 5.1 Design Principles

1. **Reuse `StorageLP`.**  The LNG tank is structurally identical to a
   `Reservoir` or `Battery` — a volume balance with inflows (deliveries),
   outflows (send-out), losses (BOG), and state coupling (SDDP).  The
   existing `StorageLP` template provides all of this.

2. **Separate fuel consumption from generation.**  Instead of hardwiring
   generators to an LNG terminal, model the coupling through a new
   `FuelNode` concept: generators consume fuel at a rate determined by their
   heat rate, and the fuel node balances supply from multiple sources
   (LNG terminal send-out, pipeline gas, diesel).

3. **Explicit regasification limits.**  The send-out flow must be bounded
   by the terminal's physical regasification capacity.

4. **Support contract constraints via `UserConstraint`.**  Take-or-pay and
   seasonal minimum offtake can be expressed as `UserConstraint` instances
   that reference LNG terminal variables, avoiding hard-coded logic.

### 5.2 Proposed Data Model

```cpp
struct LngTerminal
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  // ── Storage parameters ──
  OptTRealFieldSched emin {};       ///< Min tank level [m³]
  OptTRealFieldSched emax {};       ///< Max tank level [m³]
  OptReal eini {};                  ///< Initial tank level [m³]
  OptReal efin {};                  ///< End-of-horizon min level [m³]

  // ── Loss / boil-off ──
  OptTRealFieldSched annual_loss {};  ///< BOG rate [p.u./year]
                                      ///< Treated like Reservoir annual_loss

  // ── Send-out (regasification) ──
  OptTRealFieldSched sendout_max {};  ///< Max regasification rate [m³/h]
  OptTRealFieldSched sendout_min {};  ///< Min regasification rate [m³/h]

  // ── Delivery schedule ──
  OptTRealFieldSched delivery {};     ///< Scheduled LNG arrival [m³/stage]

  // ── Costs ──
  OptTRealFieldSched ecost {};        ///< Storage holding cost [$/m³]
  OptReal spillway_cost {};           ///< Venting penalty [$/m³]
  OptReal spillway_capacity {};       ///< Max venting rate [m³/h]

  // ── SDDP ──
  OptBool use_state_variable {};      ///< Propagate tank level across stages
  OptReal mean_production_factor {};  ///< For scost computation [MWh/m³]
  OptTRealFieldSched scost {};        ///< State penalty [$/m³]

  // ── Fuel node linkage ──
  SingleId fuel_node {unknown_uid};   ///< Connected FuelNode for send-out
};
```

### 5.3 Proposed FuelNode Concept

A `FuelNode` is a nodal balance for fuel (gas, LNG, diesel) analogous to
how `Bus` is a nodal balance for electricity:

```cpp
struct FuelNode
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  Name fuel_type {};                ///< "gas", "lng", "diesel", etc.

  // Balance: supply_in - demand_out = 0  (per block)
  // Supply from: LngTerminal sendout, pipeline supply, spot purchases
  // Demand from: Generator fuel consumption (via heat rate)
};
```

Generators would gain an optional `fuel_node` field:

```json
{
  "uid": 42,
  "name": "NEHUENCO_1-TG",
  "bus": 5,
  "fuel_node": 1,
  "heat_rate": 8.5,
  "...": "..."
}
```

The LP assembly would add a coefficient to the `FuelNode` balance row:

$$\text{fuel\_consumption}_{g,b} =
    \text{heat\_rate}_g \times P_{g,b} \times \Delta t_b$$

Where $P_{g,b}$ is the generator output (MW) and $\Delta t_b$ the block
duration (hours).

### 5.4 LP Formulation

**Variables per LNG terminal per (scenario, stage):**

| Variable | Description | Bounds |
|----------|-------------|--------|
| $V^f_s$ | Final tank volume at stage $s$ | $[V_{min}, V_{max}]$ |
| $Q_{s,b}$ | Send-out flow in block $b$ | $[Q_{min}, Q_{max}]$ |
| $V^v_s$ | Vented LNG | $[0, V^v_{max}]$ |

**Energy balance (per block $b$ in stage $s$):**

$$V^f_{b} = V^f_{b-1} \cdot (1 - \alpha \cdot \Delta t_b / 8760)
            + D_s / N_b - Q_{s,b} \cdot \Delta t_b - V^v_s / N_b$$

Where $\alpha$ is the annual BOG rate, $D_s$ the stage delivery, and $N_b$
the number of blocks in the stage.  This maps directly to the `StorageLP`
energy balance with:

- `finp` ← delivery (fixed RHS or scheduled inflow)
- `fout` ← send-out flow (coupled to `FuelNode`)
- `drain` ← venting (with penalty cost)
- `annual_loss` ← BOG rate

**FuelNode balance (per block $b$):**

$$\sum_{t \in \text{terminals}} Q_{t,b} +
  \text{pipeline}_b - \sum_{g \in \text{generators}}
  \text{HR}_g \cdot P_{g,b} = 0$$

**Objective contributions:**

$$\sum_s \sum_b \left[
    C_{ecost} \cdot V^f_{s,b} \cdot \Delta t_b
  + C_{vent} \cdot V^v_s
\right]$$

### 5.5 StorageLP Integration

The `LngTerminalLP` class would inherit from `StorageLP<ObjectLP<LngTerminal>>`
exactly as `ReservoirLP` does:

```cpp
class LngTerminalLP : public StorageLP<ObjectLP<LngTerminal>>
{
public:
  static constexpr LPClassName ClassName {"LngTerminal"};
  static constexpr std::string_view SendoutName {"sendout"};

  // ...
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);
};
```

Key mapping to `StorageLP`:

| StorageLP concept | LNG interpretation |
|-------------------|--------------------|
| `eini` / `efin` | Initial / final tank volume |
| `emin` / `emax` | Tank level limits |
| `annual_loss` | BOG rate |
| `drain` | Venting (with spillway_cost) |
| `energy_cols` | Tank volume per block |
| `energy_rows` | Volume balance rows (for FuelNode coupling) |
| State variable | Tank volume for SDDP |

The send-out flow would use a pattern similar to `ReservoirLP`'s extraction
columns, coupled to the `FuelNode` balance row instead of a `Junction` flow
balance.

### 5.6 Contract Constraints via UserConstraint

Take-or-pay and minimum offtake are naturally expressed as `UserConstraint`:

```json
{
  "uid": 100,
  "name": "GNL_Quintero_min_annual_offtake",
  "sense": ">=",
  "rhs": 500000,
  "terms": [
    {"class_name": "LngTerminal", "variable": "sendout", "uid": 1, "scale": 1}
  ],
  "axis": {"stage": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]}
}
```

This avoids hard-coding contract logic and allows arbitrary contractual
structures (monthly minimums, seasonal windows, annual totals) to be defined
in the input data.

### 5.7 Backward Compatibility with PLP

The `plp2gtopt` converter would read `plpcnfgnl.dat` and generate:

1. One `LngTerminal` per `TermGNL` entry
2. One `FuelNode` per terminal (or shared if multiple terminals feed the
   same generators)
3. `fuel_node` references on the linked generators
4. Delivery schedules from `VolInpEta` mapped to the `delivery` field

The generator variable cost override (`CReg/Rendim`) would become
unnecessary — the fuel cost is endogenously determined by the LNG terminal
economics and heat-rate coupling.

### 5.8 Implementation Phases

**Phase 1 — LNG terminal as storage (minimal viable):**
- `LngTerminal` struct + JSON contract
- `LngTerminalLP` inheriting `StorageLP`
- Delivery as fixed inflow schedule
- Send-out as bounded outflow coupled directly to generator heat rates
  (without a full `FuelNode`, use direct coefficient injection into
  `energy_rows` similar to how `Waterway` injects into `Reservoir`)
- SDDP state variable for tank volume
- Unit tests following the test ladder pattern

**Phase 2 — FuelNode and multi-source coupling:**
- `FuelNode` struct + `FuelNodeLP` balance row
- Generator `fuel_node` field + heat-rate coupling
- Pipeline gas supply as a bounded variable on the `FuelNode`
- Dual-fuel generators selecting cheapest source endogenously

**Phase 3 — Contract constraints and advanced features:**
- Take-or-pay via `UserConstraint` (already supported by the framework)
- Seasonal delivery profiles with stochastic scenarios
- BOG recovery / re-liquefaction option
- Multiple tanks per terminal


## 6. Summary

The PLP GNL model is a reasonable first approximation: it captures the
essential storage balance and SDDP coupling.  However, it lacks
regasification limits, boil-off losses, contract constraints, and multi-fuel
capability — all features that modern tools like PLEXOS and PSR SDDP
support.

The gtopt `StorageLP` framework is a natural fit for LNG terminal modeling.
The proposed design reuses the existing storage pattern (volume balance,
drain/spill, annual loss, SDDP state) and adds a `FuelNode` concept for
clean generator-fuel coupling.  Contract constraints leverage the existing
`UserConstraint` infrastructure.  The phased implementation path starts with
a minimal viable LNG terminal and progressively adds multi-fuel and
contract features.
