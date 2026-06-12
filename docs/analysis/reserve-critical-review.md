#Critical Review : gtopt Reserve Model vs PLP and Industry Tools

## 1. Executive Summary

This document critically reviews the gtopt reserve constraint implementation,
comparing it against the PLP Fortran model (CEN's production tool for Chile's
National Electric System) and industry-standard tools (PLEXOS, PSR SDDP,
PyPSA). The review identifies architectural strengths, gaps, and
recommendations for achieving feature parity and beyond.

## 2. Architecture Comparison

### 2.1 Reserve Service Types

| Feature | PLP | gtopt | PLEXOS | PyPSA |
|---------|-----|-------|--------|-------|
| Primary up (CPF+) | Yes | Yes (generic) | Yes | Yes |
| Primary down (CPF-) | Yes | Yes (generic) | Yes | Yes |
| 10-second reserve | Yes (dedicated) | Via zone config | Yes | No |
| 5-minute reserve (up/down) | Yes (dedicated) | Via zone config | Yes | No |
| Secondary up/down (CSF) | Yes (dedicated) | Via zone config | Yes | No |
| Tertiary up/down (CTF) | Yes (dedicated) | Via zone config | Yes | No |
| Inertia | Partial (flag + endogenous formulation) | **Not implemented** | Yes | Partial |
| Synthetic inertia | No | No | Yes | No |

**Assessment**: PLP has 9 hardcoded reserve service types with dedicated
variables and constraints for each. gtopt uses a generic `ReserveZone` +
`ReserveProvision` model where zones are user-defined. This is architecturally
**superior** — the PLP approach of hardcoding 9 types leads to massive code
duplication (the `genpdreserva.f` file is >2000 lines of nearly identical
blocks). gtopt's generic model can represent any number of service types
through zone configuration.

### 2.2 Zone Model

| Feature | PLP | gtopt |
|---------|-----|-------|
| Multiple zones | Yes (up to 26, char ID) | Yes (unlimited, UID-based) |
| Zone activation per stage range | Yes (`etaini`/`etafin`) | No (always active if defined) |
| Dynamic requirements (f(generation)) | Yes (Eq. 40: `R0 + Σ AtrG·AtrZ·AtrCen·RC·g`) | No |
| Fixed requirements | Yes | Yes (`urreq`/`drreq` schedules) |
| Scarcity cost per zone | Yes (`CE*`) | Yes (`urcost`/`drcost`) |
| Maximum scarcity per zone | Yes (`ME*`) | No (bounded by requirement) |
| Zone-specific inertia flag | Yes (`ZonaInercia`) | No |

**Gap 1 — Dynamic requirements**: PLP's most powerful feature is Eq. (40),
where the reserve requirement is a function of generator dispatch:

```
zr(z,b) = R0(z,b) + Σ_c (AtrG[group(c),z] + AtrZ[z]·AtrCen[c,z]) · RC(c) · g(c,b)
```

This makes the requirement grow with renewable generation (e.g., solar/wind
groups have high `AtrG`, so more solar dispatch → more reserve needed). gtopt
currently only supports fixed requirements. This is a **significant gap** for
realistic Chilean system studies where reserve needs scale with variable
renewable penetration.

**Gap 2 — Stage activation**: PLP allows enabling reserves only for a
stage range (`etaini` to `etafin`). gtopt zones are always active. Minor gap,
easily addressed with schedule-based activation.

### 2.3 Generator-Zone Linkage

| Feature | PLP | gtopt |
|---------|-----|-------|
| Multi-zone participation | Yes (char string, e.g. "RI") | Yes (colon-separated UIDs) |
| Technology groups | Yes (3 groups with per-group attributes) | No |
| Per-generator effectiveness factors | Yes (9 factors: `FE10s`, `FE5mp`, etc.) | Yes (`ur/dr_provision_factor`) |
| Per-generator capacity factors | No (implicit via Pmax) | Yes (`ur/dr_capacity_factor`) |
| Per-generator max provision | Yes (per zone, per block) | Yes (`urmax`/`drmax`) |
| Per-generator min provision | Yes (per zone, per block) | No |
| Provision cost per generator | Yes (per stage, overridable) | Yes (`urcost`/`drcost`) |
| Rendimiento (efficiency) coupling | Yes (hydro head-dependent) | No (implicit in generation) |

**Gap 3 — Technology groups**: PLP groups generators by technology (thermal,
solar, wind) with per-group attributes that feed into the dynamic requirement
formula. This enables "solar generation requires X% more reserve" policies.
gtopt has no group concept. This could be implemented via PAMPL user
constraints or as a first-class feature.

**Gap 4 — Minimum provision**: PLP supports per-generator, per-zone, per-block
minimum provision bounds (`MinP*`). gtopt only has maximum bounds. Minimum
provision is used to enforce mandatory ancillary service participation.

### 2.4 LP Constraint Structure

| Constraint | PLP | gtopt |
|------------|-----|-------|
| Requirement satisfaction | `Σ FE·r(c,z) + ze(z) - zr(z) ≥ 0` | `Σ γ·r(c) - req ≥ 0` |
| Upward capacity coupling | `Pmax - RC·g ≥ Σ_z (rpp + rsp + rtp)` | `g + r ≤ C` (per zone) |
| Downward capacity coupling | `RC·g - Pmin ≥ Σ_z (rpn + rsn + rtn)` | `g - r ≥ pmin` (per zone) |
| Cross-service capacity sharing | Yes (all up services share headroom) | No (independent per zone) |
| Water consumption tracking | Yes (`qrp`, `qrn` variables + Laja) | No |
| Variable requirement equation | Yes (`zr = R0 + Σ attr·RC·g`) | No |

**Gap 5 — Cross-service capacity sharing**: PLP enforces that the total
upward reserve across all services (primary + secondary + tertiary) for a
generator cannot exceed `Pmax - g`. Similarly for downward. In gtopt, each
zone's provision is independently coupled to the generator. If a generator
provides reserves in multiple zones, the total could exceed its headroom.

This is a **critical correctness gap**. If a generator with Pmax=100 MW
dispatching at 80 MW provides 15 MW of primary up-reserve and 15 MW of
secondary up-reserve, PLP correctly enforces 15+15 ≤ 100-80=20 (infeasible).
gtopt would allow it because each zone's constraint is independent.

**Recommendation**: Add a "capacity sharing" constraint that aggregates
provisions across zones per generator:

```
g(c,b) + Σ_z r_up(c,z,b) ≤ Pmax(c,b)
g(c,b) - Σ_z r_dn(c,z,b) ≥ Pmin(c,b)
```

This is exactly what `ReserveProvisionLP::add_provision` does per-zone, but
the constraint must be a single row aggregating all zones.

**Gap 6 — Water consumption**: PLP tracks the water consumed by hydro
generators providing reserves (`qrp`/`qrn` variables) and feeds this into the
reservoir water balance. This is critical for the Laja convention and hydro
systems. gtopt does not account for the water implications of hydro reserve
provision.

### 2.5 Output and Diagnostics

| Feature | PLP | gtopt |
|---------|-----|-------|
| Per-generator provision by zone | Yes (`plpresc.csv`) | Yes (output context) |
| Zone requirement/scarcity/duals | Yes (`plpresz.csv`) | Yes (output context) |
| Provision cost output | Yes (`plpcostpres.csv`) | Yes (col_cost) |
| Water consumed by reserves | Yes (in `qrp`/`qrn`) | No |

## 3. Comparison with Industry Tools

### 3.1 PLEXOS (Energy Exemplar)

PLEXOS has the most comprehensive reserve model in the industry:

- **Reserve types**: Regulation up/down, spinning, non-spinning,
  replacement, and user-defined
- **Inertia**: Dedicated "Inertia" property per generator (H constant in
  seconds, MVA rating). System-wide or regional minimum inertia constraints
- **Synthetic inertia**: Supported via "Virtual Inertia" property on
  batteries and inverter-based resources
- **Co-optimization**: Full co-optimization of energy and reserves
- **Unit commitment**: Binary variables for on/off status, min up/down times

**Key difference**: PLEXOS uses binary commitment variables (`u_g ∈ {0,1}`),
making inertia contribution = `2·H_g·S_g·u_g`. This requires MIP, which is
appropriate for short-term scheduling but expensive for long-term planning.

### 3.2 PSR SDDP

PSR's SDDP model has reserve constraints but with limited public documentation:

- **Reserve types**: Spinning, non-spinning, by zone
- **Inertia**: Not directly modeled in the public LP formulation
- **Approach**: LP-based, consistent with SDDP decomposition
- **Zones**: Regional reserve requirements

**Key similarity**: Like PLP and gtopt, PSR SDDP maintains an LP formulation
compatible with Benders decomposition / SDDP. This constrains the modeling
to continuous variables.

### 3.3 PyPSA

PyPSA has a simpler reserve model:

- **Reserve types**: Up/down margins per carrier
- **Implementation**: Via `GlobalConstraint` with carrier-level aggregation
- **Inertia**: Can be modeled as a custom constraint on synchronous capacity
- **Limitation**: No built-in zone model; requires manual constraint setup

### 3.4 Summary Matrix

| Capability | PLP | gtopt | PLEXOS | PSR SDDP | PyPSA |
|------------|-----|-------|--------|----------|-------|
| Generic zone model | Partial | **Best** | Good | Good | Weak |
| Dynamic requirements | **Best** | Missing | Good | Unknown | Weak |
| Cross-service coupling | **Yes** | **Missing** | Yes | Yes | No |
| Inertia (binary) | Heuristic | Missing | **Best** | No | No |
| Inertia (continuous/LP) | **Yes** | Missing | No | No | No |
| SDDP compatibility | **Yes** | **Yes** | N/A | **Yes** | N/A |
| Scalability | Poor (memory) | **Good** | Good | Good | Good |
| Configurability | Poor (hardcoded) | **Good** | **Best** | Good | Good |

## 4. Strengths of gtopt's Current Model

1. **Generic architecture**: The `ReserveZone` + `ReserveProvision` pattern
   is cleaner than PLP's 9-type hardcoded approach. Any reserve service can
   be modeled by creating zones with appropriate parameters.

2. **Capacity factor**: gtopt supports `ur/dr_capacity_factor` (fraction of
   installed capacity available for reserves), which PLP lacks.

3. **Expansion integration**: Reserve provision can reference expansion
   `capacity_col`, so new generators can provide reserves. PLP has no
   expansion planning.

4. **PAMPL integration**: User constraints can reference reserve variables,
   enabling custom policies without code changes.

5. **Scalability**: gtopt's memory model (block-level LP construction,
   optional `low_memory` mode with LZ4 compression) avoids PLP's 53 TB
   memory problem.

## 5. Critical Gaps (Priority Order)

### P0 — Cross-service capacity sharing (correctness)

Without this, a generator can over-commit reserves across zones. Must add
an aggregate capacity constraint per generator.

### P1 — Dynamic requirements (feature parity)

Required for realistic Chilean system studies. The requirement should be
expressible as `R0 + Σ attr·g(c)` where attributes can be per-group or
per-generator.

### P1 — Inertia constraint (new feature)

See companion document: `inertia-constraint-proposal.md`

### P2 — Minimum provision bounds

Add `urmin`/`drmin` to `ReserveProvision` for mandatory participation.

### P2 — Water consumption tracking for hydro reserves

Track water consumed by hydro reserve provision and integrate with reservoir
water balance. Important for systems like Laja.

### P3 — Stage activation range

Add optional `first_stage`/`last_stage` to `ReserveZone` for temporal
scoping.

### P3 — Technology groups

Add a group/carrier concept for dynamic requirement attribution. Could
leverage existing PAMPL infrastructure.

## 6. Recommendations

1. **Fix P0 immediately**: The cross-service capacity sharing gap is a
   correctness issue that could produce infeasible real-world solutions.

2. **Implement inertia as a natural extension**: The existing `ReserveZone`
   architecture can represent inertia zones with appropriate parameters.
   See the companion proposal for details.

3. **Dynamic requirements via PAMPL**: Consider using the existing user
   constraint / PAMPL framework to express dynamic requirements as
   `zr = R0 + Σ attr·g`, avoiding hardcoded attribute structures.

4. **Do not replicate PLP's 9-type architecture**: gtopt's generic model is
   superior. Specific reserve services should be configured via zone
   parameters, not hardcoded.
