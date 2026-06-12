#Inertia Constraint Proposal for gtopt

## 1. Background and Motivation

### 1.1 What is System Inertia?

Rotational inertia is the kinetic energy stored in the spinning masses of
synchronous generators. After a sudden generation loss (contingency), this
stored energy arrests the frequency drop before governors respond. The key
relationship is the **swing equation**:

```
RoCoF = (f₀ · ΔP_D) / (2 · H_sys)   [Hz/s]
```

where:
- `f₀` = nominal frequency (50 Hz in Chile)
- `ΔP_D` = contingency size (largest single unit loss) [MW]
- `H_sys` = total system inertia [MWs]
- `RoCoF` = Rate of Change of Frequency [Hz/s]

Each synchronous generator contributes `H_g · S_g` [MWs], where:
- `H_g` = inertia constant [seconds] (machine property, 2–4 s hydro,
  4–6 s coal, 3–7 s gas)
- `S_g` = rated apparent power [MVA]

Inverter-based resources (solar, wind, BESS) contribute **zero** physical
inertia unless equipped with grid-forming (GFM) inverters that provide
synthetic/virtual inertia.

### 1.2 Why it Matters for Chile (CEN)

The CEN already developed a reserve+inertia model for PLP (documented in
"Informe Final: Mejoras al modelo PLP. Reserva e Inercia", Coordinador
Eléctrico Nacional). With growing solar and wind penetration, minimum system
inertia constraints are becoming binding — especially during daytime hours
with high solar output displacing synchronous generation.

### 1.3 What PLP Currently Does

PLP implements inertia in two ways:

1. **Exogenous heuristic**: An iterative script (`run_inercia.sh`) that
   forces generators to minimum output, re-runs PLP, checks inertia, and
   repeats. Requires 37+ iterations for realistic cases. **Not recommended
   for production** — non-optimal and slow.

2. **Endogenous formulation as equivalent downward reserve**: The key insight
   from the CEN document (Eq. 41–46) is that the inertia constraint can be
   reformulated as a downward reserve requirement:

   ```
   Σ_c (H_c / Pmin_c) · r_dn(c) ≥ H_req
   ```

   where `r_dn(c) ∈ [0, Pmin_c]` is a downward reserve provision variable,
   and the effectiveness factor `FE_c = H_c / Pmin_c` converts reserve MW
   into inertia MWs. This is a **continuous LP relaxation** — the binary
   "unit is on" variable is relaxed, which is consistent with PLP's overall
   LP approach.

### 1.4 What Industry Tools Do

| Tool | Approach | Variables |
|------|----------|-----------|
| PLEXOS | Binary UC: `Σ H·S·u_g ≥ H_min` | Binary `u_g` |
| PSR SDDP | Generic constraints (no dedicated module) | Continuous |
| PyPSA | Custom constraints (no built-in) | Binary or continuous |
| PLP | Equivalent reserve (endogenous) | Continuous (relaxed binary) |
| Academic MILP | Binary UC + RoCoF + nadir constraints | Binary `u_g` |

The standard MIP formulation is:

```
Σ_g H_g · S_g · u_
{
  g, t
}
≥ H_sys_min(t)    ∀t
```

    where `u_
{
  g, t
} ∈ {0,1}` is the commitment binary. This is linear in u and
straightforward to implement as a MILP constraint.

## 2. Proposed Architecture

### 2.1 Design Principles

1. **Reuse the existing reserve zone/provision architecture** — inertia is
   modeled as a specialized reserve zone type, keeping the codebase unified
2. **Support both MIP and LP formulations** — configurable per zone
3. **Integrate with existing commitment models** — `Commitment` (full UC)
   and `DispatchCommitment` (simplified pmin) from PR #385 already provide
   binary `u` variables that the inertia constraint needs
4. **Zone-based** — support multiple inertia zones (matching PLP's design
   and enabling regional inertia requirements for weakly-coupled systems)

### 2.2 Model B is the LP Relaxation of Model A

Before defining data structures, we prove the central design claim:
**the continuous PLP formulation (Model B) is exactly the LP relaxation
of the binary formulation (Model A)**. This means a single LP structure
handles both — the only difference is whether `u` is integer or continuous.

#### Model A (Binary)

A generator with `DispatchCommitment` has binary `u_g ∈ {0,1}` and:
```
p_g ≤ Pmax_g · u_g         (DC upper)
p_g ≥ Pmin_g · u_g         (DC lower)
```

The inertia contribution is:
```
I_g = H_g · S_g · u_g      [MWs]
```

#### Model A relaxed → Model B

Relax `u_g ∈ [0,1]`. Substitute `r_g = Pmin_g · u_g`:
- `u_g ∈ [0,1]` → `r_g ∈ [0, Pmin_g]`
- `p_g ≥ Pmin_g · u_g` → `p_g ≥ r_g`   (capacity coupling)
- `I_g = H_g · S_g · u_g = H_g · S_g · (r_g / Pmin_g) = FE_g · r_g`

where `FE_g = H_g · S_g / Pmin_g` is PLP's effectiveness factor.

**The requirement constraint transforms identically:**
```
Σ_g H_g · S_g · u_g ≥ H_req          (Model A)
Σ_g FE_g · r_g      ≥ H_req          (Model B, after substitution)
```

**This is not an approximation — it is an exact algebraic equivalence.**
The inertia provision variable `r_g` in Model B is literally `Pmin_g · u_g`
from the relaxed Model A. The continuous model is the LP relaxation.

#### Implications for implementation

Since Model B = relaxed Model A, we implement **one formulation** with a
`relax` flag — exactly like `DispatchCommitment.relax` and
`Commitment.relax` already work:

| Setting | `u` type | Effect |
|---------|----------|--------|
| `relax = false` | Binary `{0,1}` | MIP: unit fully on or off, `r = 0` or `Pmin` |
| `relax = true` | Continuous `[0,1]` | LP: fractional commitment, `r ∈ [0, Pmin]` |
| Phase-level relaxation | Continuous `[0,1]` | Same as `relax=true` for relaxed phases |

The provision variable `r_inertia` and its coupling constraints are
**always** created. When `u` is binary, the LP solver naturally drives
`r_inertia` to `
{
  0, Pmin
}
`.When `u` is continuous, `r_inertia` takes fractional values — the PLP
                              behavior.

                          ## #2.3 Unified Data
                          Structures(Consistent with Reserves)

                              The inertia data model mirrors `ReserveZone`
        / `ReserveProvision` exactly :

    | Reserve concept | Reserve field | Inertia field |
    | -- -- -- -- -- -- -- -- -| -- -- -- -- -- -- --| -- -- -- -- -- -- -- -|
    | Zone requirement | `urreq` / `drreq` [MW] | `inertia_req` [MWs] |
    | Shortage penalty | `urcost` / `drcost` [$ / MW] | `inertia_cost` [$ / MWs]
    | | Generator link | `generator` | `generator` | | Zone membership
    | `reserve_zones` | `inertia_zones` | | Max provision
    | `urmax` / `drmax` [MW]
    | Computed
    : `Pmin` [MW]
    |
    | Provision factor | `ur / dr_provision_factor` [p.u.]
    | Computed : `FE = H·S / Pmin` [MWs / MW] | | Capacity factor
    | `ur / dr_capacity_factor` [p.u.] | Not applicable | | Provision cost
    | `urcost` / `drcost` [$ / MW] | `inertia_provision_cost` [$ / MW](optional)
    |

    #### `InertiaZone` — The requirement side

```cpp struct InertiaZone
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  /// Minimum system inertia requirement [MWs]
  /// Can be stage/block-scheduled for varying requirements
  OptTBRealFieldSched inertia_req {};

  /// Shortage penalty cost [$/MWs] — soft constraint when set
  OptTRealFieldSched inertia_cost {};
};
```

    No `mode` field is needed — the formulation is always the same.The binary
        vs.continuous behavior is controlled by whether the
            generator's associated `DispatchCommitment`
    or `Commitment` has `relax = true
    / false`.

      #### `InertiaProvision` — The generator contribution side

```cpp struct InertiaProvision
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId generator {unknown_uid};
  String inertia_zones {};  // colon-separated zone IDs

  /// Inertia constant H [seconds] — machine property
  OptReal inertia_constant {};

  /// Rated apparent power S [MVA] — for inertia contribution = H·S
  /// If omitted, uses generator Pmax as proxy (common in LP tools)
  OptReal rated_mva {};

  /// Minimum technical output [MW] — overrides generator's pmin for
  /// computing FE = H·S/dispatch_pmin and bounding the provision variable.
  /// If omitted, uses the generator's pmin from the LP column lower bound.
  OptTBRealFieldSched dispatch_pmin {};

  /// Provision cost [$/MW] — optional cost of providing inertia
  OptTRealFieldSched inertia_provision_cost {};

  /// Whether this unit can provide synthetic/virtual inertia
  /// (BESS with grid-forming inverters, wind with SI)
  OptBool synthetic_inertia {};

  /// Maximum synthetic inertia [MWs] — for IBR units
  OptReal max_synthetic_inertia {};
};
```

### 2.4 Unified LP Formulation

**One formulation, always the same LP structure.** The binary/continuous
distinction is external (from `DispatchCommitment.relax`).

#### Variables per generator per zone per block

- `r_inertia(g,z,b)` — inertia provision [MW], bounded `[0, Pmin_g]`

This mirrors `ReserveProvision`'s provision column (`dprovision`).

#### Variables per zone per block

- `slack_inertia(z,b)` — inertia shortage [MWs], penalized at
  `inertia_cost`, bounded `[0, H_req]`

This mirrors `ReserveZone`'s shortage column (`drequirement`).

#### Constraints

**C1 — Requirement satisfaction** (one row per zone per block):
```
Σ_g FE_g · r_inertia(g,z,b) + slack_inertia(z,b) ≥ H_req(z,b)
```
where `FE_g = H_g · S_g / Pmin_g` [MWs/MW].

Mirrors `ReserveZoneLP`'s requirement row with `provision_factor = FE`.

**C2 — Capacity coupling** (one row per generator per zone per block):
```
p(g,b) - r_inertia(g,z,b) ≥ 0
```

Mirrors `ReserveProvisionLP`'s downward provision row:
`p(g,b) - r_dn(g,z,b) ≥ Pmin` → here the RHS is 0 because inertia
provision does not require the generator to be above pmin — only above
the provision amount itself.

**C3 — Commitment integration** (deferred, added by CommitmentLP/
DispatchCommitmentLP):

When a generator has a `DispatchCommitment`, the commitment's `add_to_lp`
modifies the inertia provision row:
```
p(g,b) - r_inertia(g,z,b) ≥ 0                     (before commitment)
p(g,b) - r_inertia(g,z,b) - Pmin_g · u(g,b) ≥ 0   (won't help)
```

Actually, the correct integration is simpler: `DispatchCommitment` already
enforces `p ≥ Pmin·u`. The inertia provision `r_inertia ≤ Pmin` and
`p ≥ r_inertia` together with the commitment's `p ≥ Pmin·u` naturally
couple. When `u=1` (binary), `p ≥ Pmin` and `r_inertia` can reach `Pmin`.
When `u=0`, `p = 0` and `r_inertia = 0` (from `p ≥ r_inertia`).

**No explicit injection of H·S·u into the inertia row is needed.** The
provision variable `r_inertia` already captures this via the algebraic
equivalence proven in Section 2.2. The commitment's `u` acts indirectly
through the generation variable `p`.

This is the key architectural insight: **the inertia LP class does not
need to know about commitment at all.** It creates provision variables
and couples them to generation, exactly like reserves. The commitment
classes separately enforce on/off behavior on the same generation variable.
The two interact purely through the shared `p(g,b)` column.

### 2.5 Structural Comparison: Inertia vs Reserve LP Classes

```
                    ReserveZone                    InertiaZone
                    ───────────                    ───────────
Zone row:           Σ γ·r_prov + slack ≥ req       Σ FE·r_inertia + slack ≥ H_req
Zone column:        slack ∈ [0, req] with cost     slack ∈ [0, H_req] with cost
AMPL name:          reserve_zone("Z").up/dn        inertia_zone("Z").requirement

                    ReserveProvision               InertiaProvision
                    ────────────────               ─────────────────
Provision col:      r_up/r_dn ∈ [0, rmax]         r_inertia ∈ [0, Pmin]
Coupling row (up):  p + r_up ≤ Pmax                (not applicable)
Coupling row (dn):  p - r_dn ≥ Pmin                p - r_inertia ≥ 0
Zone injection:     γ · r_prov into zone row       FE · r_inertia into zone row
AMPL name:          reserve_provision("P").up/dn   inertia_provision("P").provision
```

The LP class implementation follows the same pattern:
- `InertiaZoneLP` parallels `ReserveZoneLP` — one `Requirement` struct
  (instead of `ur`/`dr` pair, just one since inertia has no up/down)
- `InertiaProvisionLP` parallels `ReserveProvisionLP` — one `Provision`
  struct, same `add_provision` helper function

### 2.6 Element Type Ordering

In the `LPCollections` tuple (from `system_lp.hpp`):

```
...,
ReserveZoneLP,        // creates reserve requirement rows
ReserveProvisionLP,   // fills reserve rows with provision terms
InertiaZoneLP,        // creates inertia requirement rows
InertiaProvisionLP,   // fills inertia rows with FE·r_inertia
CommitmentLP,         // full UC — modifies reserve+inertia provision rows
DispatchCommitmentLP, // simplified UC — modifies reserve+inertia provision rows
...
```

**No deferred injection or reordering needed.** Since Model B = relaxed
Model A, the inertia provision variable `r_inertia` is always created by
`InertiaProvisionLP` with its coupling row `p - r_inertia ≥ 0`. Then
`CommitmentLP` / `DispatchCommitmentLP` (processed later) modifies the
provision row to condition on `u`, using the existing reserve-UC integration
pattern from `commitment_lp.cpp:520–558`:

```
p - r_inertia ≥ 0                   (original, from InertiaProvisionLP)
p - r_inertia - 0·u ≥ 0             (commitment adds u coefficient = 0
                                      — or more precisely, the commitment
                                      already constrains p via Pmin·u,
                                      making this coupling implicit)
```

In practice, the commitment classes only need to handle inertia provision
rows the same way they handle reserve provision rows — the pattern at
`commitment_lp.cpp:520–558` already iterates over `ReserveProvisionLP`
elements for the linked generator. Extending this to also iterate over
`InertiaProvisionLP` elements is straightforward.

## 3. Implementation Plan

### Phase 0: DispatchCommitment (prerequisite — from PR #385)

The `DispatchCommitment` element is a prerequisite for the binary inertia
model and useful on its own for enforcing minimum technical output. PR #385
introduced the design; this phase completes it, adds tests, and merges it.

**Why DispatchCommitment before inertia:**
- Inertia's binary mode needs a `u` variable per generator per block
- `DispatchCommitment` provides exactly this: one binary `u` with
  `p ≤ Pmax·u` and `p ≥ Pmin·u`, without chronological stage requirements
- The full `Commitment` (three-bin UC) is too heavy for long-term planning
  and requires chronological stages
- `DispatchCommitment` works on any stage type, making it the natural
  companion for inertia in GTEP studies

**What PR #385 provides (to be completed and merged):**

Data model (`dispatch_commitment.hpp`):
```cpp
struct DispatchCommitment {
  Uid uid;
  Name name;
  OptActive active;
  SingleId generator;  // FK to Generator
  OptTBRealFieldSched dispatch_pmin;  // min output when dispatched [MW]
  OptBool relax;  // LP relaxation: u ∈ [0,1]
  OptBool must_run;  // force u = 1
};
```

LP formulation (`dispatch_commitment_lp.cpp`):
- Creates binary `u` per block (or continuous if `relax = true`)
- C1: `p - Pmax·u ≤ 0` (upper generation limit)
- C2: `p - dispatch_pmin·u ≥ 0` (minimum output when dispatched)
- Integrates with reserve provision rows (same pattern as `CommitmentLP`)

**Steps to complete Phase 0:**

1. **Review and fix PR #385 code** — the PR has known gaps (build not
   verified, no tests, pre-existing build fixes mixed in)
2. **Separate concerns** — extract the `integer_expansion` feature and
   build fixes into their own commits/PRs
3. **Write tests** (`test/source/test_dispatch_commitment.cpp`):
   - Single generator, binary mode — verify u=0 → p=0, u=1 → p≥Pmin
   - LP relaxation (`relax: true`) — verify u continuous in [0,1]
   - Must-run mode — verify u forced to 1
   - Integration with reserves — verify reserve headroom rows are modified
     (same as `commitment_lp.cpp:520–558`)
   - Multi-stage — verify u is per-block, independent across stages
   - JSON roundtrip tests
4. **Run clang-format, clang-tidy, build, and full test suite**
5. **Merge PR #385** (or create a clean replacement PR)

**Deliverable**: `DispatchCommitment` merged into master with full test
coverage, providing the `u` variable infrastructure for Phase 2.

### Phase 1: Continuous Inertia Model (LP-compatible, SDDP-safe)

This is the PLP-equivalent formulation, sufficient for production use in
long-term planning. Does not depend on Phase 0 (no binary variables needed).

**Files to create:**
- `include/gtopt/inertia_zone.hpp` — `InertiaZone` struct
- `include/gtopt/inertia_provision.hpp` — `InertiaProvision` struct
- `include/gtopt/inertia_zone_lp.hpp` / `source/inertia_zone_lp.cpp`
- `include/gtopt/inertia_provision_lp.hpp` / `source/inertia_provision_lp.cpp`
- `include/gtopt/json/json_inertia_zone.hpp`
- `include/gtopt/json/json_inertia_provision.hpp`
- `test/source/test_inertia_zone.cpp`
- `test/source/json/test_inertia_zone_json.cpp`
- `test/source/json/test_inertia_provision_json.cpp`

**Files to modify:**
- `include/gtopt/system.hpp` — add `inertia_zone_array`, `inertia_provision_array`
- `include/gtopt/system_lp.hpp` — add `InertiaZoneLP`, `InertiaProvisionLP`
  to collections tuple
- `include/gtopt/json/json_system.hpp` — add JSON bindings
- `include/gtopt/model_options.hpp` — add `inertia_fail_cost` global fallback
- `source/system.cpp` — add merge support
- `source/system_lp.cpp` — add `make_collection` calls

**Constraints implemented:**
1. Requirement row: `Σ FE_g · r_inertia(g,z,b) + slack(z,b) ≥ H_req(z,b)`
2. Capacity coupling: `p(g,b) - r_inertia(g,z,b) ≥ 0`
3. Provision bounds: `0 ≤ r_inertia(g,z,b) ≤ Pmin_g`

### Phase 2: Commitment Integration (MIP inertia via unified model)

Depends on Phase 0 (DispatchCommitment merged) and Phase 1 (InertiaZone
infrastructure). The inertia LP classes from Phase 1 are **unchanged** —
this phase only extends the commitment classes to handle inertia provision
rows the same way they already handle reserve provision rows.

**Files to modify:**
- `source/commitment_lp.cpp` — extend the reserve-UC integration loop
  (lines 520–558) to also iterate over `InertiaProvisionLP` elements for
  the linked generator, modifying their provision rows to condition on `u`
- `source/dispatch_commitment_lp.cpp` — same extension
- `test/source/test_inertia_zone.cpp` — add tests with DispatchCommitment

**What the commitment classes do:**
The existing pattern at `commitment_lp.cpp:520–558` iterates over
`ReserveProvisionLP` elements and modifies their up/down provision rows:
```
p + r_up ≤ Pmax       →  p + r_up - Pmax·u ≤ 0    (up provision)
p - r_dn ≥ Pmin       →  p - r_dn - Pmin·u ≥ 0    (down provision)
```
For inertia, the same transformation is applied to the capacity coupling
row:
```
p - r_inertia ≥ 0     →  p - r_inertia - 0·u ≥ 0   (already correct!)
```
Since the inertia coupling row has RHS = 0 (not Pmin), no modification
is actually needed — the commitment's `p ≤ Pmax·u` and `p ≥ Pmin·u`
already enforce `u=0 → p=0 → r_inertia=0`. The provision variable
is driven to `{
  0, Pmin}` purely through the shared `p` column.

**In practice**, the only code change needed is to ensure that
`DispatchCommitmentLP` (and `CommitmentLP`) look up associated
`InertiaProvisionLP` elements and verify the coupling is correct.
No coefficient injection is required.

### Phase 3: Advanced Features

- **Synthetic inertia**: BESS/wind with grid-forming inverters. New
  provision variable bounded by energy state and GFM capability.
- **Dynamic requirements**: `H_req = f(largest_online_unit)` — couples
  inertia requirement to dispatch decisions (nonlinear, requires
  linearization or iterative approach).
- **Frequency nadir constraints**: Beyond pure inertia — couples with
  primary frequency response reserves (academic research, complex).
- **Regional inertia security regions**: For weakly-coupled grids with
  inter-area oscillations.

## 4. JSON Configuration Examples

### 4.1 LP-Only Mode (no commitment — PLP-equivalent)

When generators have no `DispatchCommitment`, the provision variable
`r_inertia` is continuous `[0, Pmin]` and acts as the PLP endogenous model.

```json
{
  "inertia_zone_array"
      : [{
        "uid": 1,
        "name": "SEN_inertia",
        "inertia_req": 15000,
        "inertia_cost": 10000
      }]
      , "inertia_provision_array" : [
        {
          "uid": 1,
          "name": "coal1_inertia",
          "generator": "coal1",
          "inertia_zones": "1",
          "inertia_constant": 5.0,
          "rated_mva": 400,
          "dispatch_pmin": 150
        },
        {
          "uid": 2,
          "name": "hydro1_inertia",
          "generator": "hydro1",
          "inertia_zones": "1",
          "inertia_constant": 3.5,
          "rated_mva": 200
        }
      ]
}
```

The LP automatically computes:
- `coal1`: `FE = H·S/Pmin = 5·400/150 = 13.33`, `r_max = Pmin = 150 MW`
  → max inertia contribution = 13.33 × 150 = 2000 MWs ✓ (= H·S = 5·400)
- `hydro1`: `FE = 3.5·200/Pmin`, `r_max = Pmin`
  → max contribution = H·S = 700 MWs

This is the continuous relaxation. Generators can provide fractional
inertia without being fully at minimum output.

### 4.2 MIP Mode (with DispatchCommitment — same inertia config)

Adding `DispatchCommitment` entries makes `u` binary. **The inertia
configuration is identical** — only the commitment entries are added:

```json
{
  "inertia_zone_array"
      : [{
        "uid": 1,
        "name": "SEN_inertia",
        "inertia_req": 15000,
        "inertia_cost": 10000
      }]
      , "inertia_provision_array"
      : [{
        "uid": 1,
        "name": "coal1_inertia",
        "generator": "coal1",
        "inertia_zones": "1",
        "inertia_constant": 5.0,
        "rated_mva": 400,
        "dispatch_pmin": 150
      }]
      , "dispatch_commitment_array" : [{
        "uid": 1,
        "name": "coal1_dc",
        "generator": "coal1",
        "dispatch_pmin": 150,
        "relax": false
      }]
}
```

With `relax: false`:
- `DispatchCommitment` creates binary `u ∈ {0,1}` for coal1
- `u = 1` → `p ≥ 150` → `r_inertia` can reach 150 → contribution = 2000 MWs
- `u = 0` → `p = 0` → `r_inertia = 0` → contribution = 0

With `relax: true` (or omitting `dispatch_commitment_array` entirely):
- Falls back to continuous model — identical to Section 4.1

**The inertia zone and provision configuration never changes.** The
binary/continuous choice is purely a property of the commitment.

### 4.3 Mixed Mode (some generators binary, some continuous)

```json
{
  "inertia_zone_array": [{
    "uid": 1,
    "name": "SEN_inertia",
    "inertia_req": 12000,
    "inertia_cost": 10000
  }],
  "inertia_provision_array": [
    {
      "uid": 1,
      "name": "coal1_inertia",
      "generator": "coal1",
      "inertia_zones": "1",
      "inertia_constant": 5.0,
      "rated_mva": 400
    },
    {
      "uid": 2,
      "name": "hydro1_inertia",
      "generator": "hydro1",
      "inertia_zones": "1",
      "inertia_constant": 3.5,
      "rated_mva": 200
    }
  ],
  "dispatch_commitment_array": [{
    "uid": 1,
    "name": "coal1_dc",
    "generator": "coal1",
    "dispatch_pmin": 150,
    "relax": false
  }]
}
```

Here coal1 has binary commitment (exact inertia on/off), while hydro1
has no commitment (continuous relaxation). Both use the same inertia
provision LP structure — only coal1's `r_inertia` is driven to `{0, Pmin}`
by the MIP solver through the binary `u`.
```

## 5. Interaction with Reserve Constraints

### 5.1 Shared Capacity Headroom

When a generator provides both reserves and inertia, the constraints
interact through the generation variable:

**Continuous mode:**
```
p(g,b) + r_up(g,z_reserve,b) ≤ Pmax(g)     (reserve up headroom)
p(g,b) - r_dn(g,z_reserve,b) ≥ Pmin(g)     (reserve down headroom)
p(g,b) - r_inertia(g,z_inertia,b) ≥ 0       (inertia capacity coupling)
```

**Binary mode:**
The commitment binary `u` links both: `p ≤ Pmax·u` and `p ≥ Pmin·u`.
The inertia constraint `Σ H·S·u ≥ H_req` and reserve constraints
`p + r_up ≤ Pmax·u` are naturally coupled through the same `u`.

### 5.2 PLP's `ZonaInercia` Flag

In PLP, when `ZonaInercia = .TRUE.` for a reserve zone, the shared
downward capacity constraint (`cbajada_fil`) excludes secondary and
tertiary down reserves — only primary down counts. This ensures
inertia-providing units reserve enough headroom for primary frequency
response.

In gtopt, this can be implemented via the PAMPL user constraint framework
or as a `reserve_priority` attribute on `InertiaProvision`.

### 5.3 Cross-Service Capacity Sharing (P0 Gap)

As noted in the reserve critical review (`reserve-critical-review.md`),
gtopt currently lacks cross-service capacity sharing. The inertia
implementation should include the aggregate constraint:

```
p(g,b) + Σ_{z∈reserve_zones} r_up(g,z,b) ≤ Pmax(g,b)
p(g,b) - Σ_{z∈reserve_zones} r_dn(g,z,b) - Σ_{z∈inertia_zones} r_inertia(g,z,b) ≥ 0
```

This ensures a generator does not over-commit across reserve + inertia.

## 6. Comparison: PLP vs Proposed gtopt

| Feature | PLP | gtopt (proposed) |
|---------|-----|------------------|
| Zone-based | Yes (char IDs) | Yes (UID-based) |
| Binary mode | Heuristic only | Unified: `DispatchCommitment(relax=false)` |
| Continuous mode | Equivalent reserve | Unified: no commitment or `relax=true` |
| Model B = relaxed Model A | Implicit (not proven) | **Proven** (Section 2.2) |
| Automatic FE computation | No (manual) | Yes (from H, S, Pmin) |
| Consistent with reserves | Partially (shares `cbajada`) | **Fully** (same Zone/Provision pattern) |
| Synthetic inertia (BESS/wind) | No | Phase 3 |
| Dual values / shadow prices | Yes | Yes |
| SDDP compatible | Yes (continuous) | Yes (no commitment or `relax=true`) |
| Separate inertia output | Via reserve output | Dedicated inertia output |
| Dynamic requirements | No | Phase 3 |
| Memory scalability | Poor (53 TB for large cases) | Good (block-level LP) |

## 7. Test Plan

### Unit Tests (`test/source/test_inertia_zone.cpp`)

1. **Single generator, continuous mode** — verify FE computation and
   constraint structure
2. **Two generators, continuous mode** — verify requirement satisfaction
   with multiple providers
3. **Single generator, binary mode** — verify integration with
   DispatchCommitment
4. **Inertia shortage** — verify slack variable activates with penalty
5. **Multi-zone** — two inertia zones with different requirements
6. **Multi-stage** — verify stage-scheduled requirements
7. **Reserve + inertia** — generator provides both reserve and inertia,
   verify capacity coupling
8. **Expansion + inertia** — new generator expansion provides inertia

### Integration Tests

9. **IEEE 9-bus with inertia** — system-level test with inertia constraint
10. **PLP equivalent** — replicate PLP's CEN test case and compare duals

## 8. Phase Dependency Graph

```
Phase 0: DispatchCommitment ──────────────────────┐
  (PR #385 cleanup, tests, merge)                 │
                                                   ▼
Phase 1: Continuous Inertia ──► Phase 2: Binary Inertia ──► Phase 3: Advanced
  (LP/SDDP-safe, no binaries)   (MIP, uses u from P0)       (synthetic, nadir)
```

Phases 0 and 1 can proceed **in parallel** — they are independent. Phase 2
requires both to be complete.

## 9. References

1. CEN, "Informe Final: Mejoras al modelo PLP. Reserva e Inercia",
   Coordinador Eléctrico Nacional (the `reserva_inercia.docx` document)
2. Paturet et al., "Stochastic Unit Commitment in Low-Inertia Grids",
   IEEE Trans. Power Systems, 2020
3. Badesa et al., "Unit Commitment With Inertia-Dependent and Multispeed
   Allocation of Frequency Response Services", IEEE, 2018
4. Gonzalez-Romero et al., "Assessing the impact of inertia and reactive
   power constraints in generation expansion planning", Applied Energy, 2021
5. NREL, "Frequency Nadir Constrained Unit Commitment for High Renewable
   Penetration Island Power Systems", 2024
6. gtopt PR #385: DispatchCommitment (simplified pmin dispatch) and
   integer expansion modules
