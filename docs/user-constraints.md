# User-Defined Constraints — Syntax Reference

gtopt supports **user-defined linear constraints** that are added directly
to the LP formulation. Constraints are expressed in an AMPL-inspired syntax
that references power system elements and their LP variables, with optional
domain restrictions over scenarios, stages, and blocks.

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Syntax Overview](#2-syntax-overview)
3. [Element Types and Attributes](#3-element-types-and-attributes)
4. [Element Identification](#4-element-identification)
5. [Aggregation with sum()](#5-aggregation-with-sum)
6. [Domain Specifications](#6-domain-specifications)
7. [Comments](#7-comments)
8. [Examples](#8-examples)
9. [External Constraint Files](#9-external-constraint-files)
10. [Formal Grammar (BNF)](#10-formal-grammar-bnf)
11. [Comparison with AMPL](#11-comparison-with-ampl)
12. [Best Practices](#12-best-practices)
13. [See Also](#13-see-also)

---

## 1. Quick Start

Add a `user_constraint_array` to the `system` section of your JSON case file:

```json
{
  "system": {
    "bus_array": [...],
    "generator_array": [...],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "gen_pair_limit",
        "expression": "generator('G1').generation + generator('G2').generation <= 300"
      }
    ]
  }
}
```

This adds a constraint to the LP: the sum of generation from G1 and G2
must not exceed 300 MW in every scenario, stage, and block.

---

## 2. Syntax Overview

A constraint expression has three parts:

```text
<linear_expression> <operator> <rhs> [, for(<domain>)]
```

| Part | Description | Example |
|------|-------------|---------|
| Linear expression | Sum of `coefficient * element.attribute` terms | `2 * generator('G1').generation - demand('D1').load` |
| Operator | Comparison: `<=`, `>=`, or `=` | `<=` |
| RHS | Right-hand side: number or another linear expression | `300` |
| Domain (optional) | Index restriction: `for(stage in ..., block in ...)` | `for(stage in {1,2,3}, block in 1..24)` |

### Range constraints

Range constraints bound an expression from both sides:

```text
100 <= generator('G1').generation <= 500
```

This creates a single LP row with both lower and upper bounds.

### Element references

Elements are referenced by type and identifier (name or UID):

```text
generator('TORO').generation    -- by name
generator('uid:23').generation  -- by UID
```

---

## 3. Element Types and Attributes

| Element type | Attributes | LP variable meaning |
|-------------|------------|---------------------|
| `generator` | `generation` | Generator power output (MW) |
| `generator` | `cost` | Generation cost contribution ($/h) |
| `demand` | `load` | Served demand (MW) |
| `demand` | `fail` | Unserved demand / load curtailment (MW) |
| `line` | `flow` | Active power flow on transmission line (MW) |
| `line` | `flowp` | Positive-direction power flow (MW) |
| `line` | `flown` | Negative-direction power flow (MW) |
| `line` | `lossp` | Positive-direction line losses (MW) |
| `line` | `lossn` | Negative-direction line losses (MW) |
| `battery` | `energy` | Battery state of energy (MWh); scaled by energy scale from `variable_scales` |
| `battery` | `charge` | Battery charging power (MW) |
| `battery` | `discharge` | Battery discharging power (MW) |
| `battery` | `spill` | Battery energy spillway / curtailment (MW); also accepts `drain` |
| `converter` | `charge` | Converter charging power (MW) |
| `converter` | `discharge` | Converter discharging power (MW) |
| `reservoir` | `volume` | Reservoir water volume (dam³); also accepts `energy`; scaled by energy scale from `variable_scales` |
| `reservoir` | `extraction` | Water extraction from reservoir (m³/s); scaled by energy scale from `variable_scales` |
| `reservoir` | `spill` | Reservoir spillway discharge (m³/s); also accepts `drain`; scaled by energy scale from `variable_scales` |
| `bus` | `theta` | Voltage angle at bus (radians); also accepts `angle`; scaled by `1/scale_theta` |
| `waterway` | `flow` | Water flow through waterway (m³/s) |
| `turbine` | `generation` | Turbine power output (MW) |
| `junction` | `drain` | Junction drain/spill variable (m³/s) |
| `flow` | `flow` | Water discharge into junction (m³/s); also accepts `discharge` |
| `filtration` | `flow` | Filtration flow variable (m³/s); also accepts `filtration` |
| `reserve_provision` | `up` | Up-reserve provision variable (MW reserved up); also accepts `uprovision`, `up_provision` |
| `reserve_provision` | `dn` | Down-reserve provision variable (MW reserved down); also accepts `dprovision`, `dn_provision`, `down` |
| `reserve_zone` | `up` | Up-reserve requirement variable (MW of up-reserve); also accepts `urequirement`, `up_requirement` |
| `reserve_zone` | `dn` | Down-reserve requirement variable (MW of down-reserve); also accepts `drequirement`, `dn_requirement`, `down` |

### Variable Scaling

Some LP variables are internally scaled to improve solver numerical conditioning.
User constraints are written in **physical units**; the constraint resolver
automatically applies the appropriate scale factor so that the LP constraint
is dimensionally correct.

| Variable | Scale factor (physical = LP × scale) | Default |
|----------|--------------------------------------|---------|
| `reservoir.volume` / `reservoir.energy` | energy scale (from `variable_scales`) | 1000 |
| `reservoir.extraction` | flow scale (from `variable_scales`) | 1000 |
| `reservoir.spill` / `reservoir.drain` | flow scale (from `variable_scales`) | 1000 |
| `battery.energy` | energy scale (from `variable_scales`) | 1.0 |
| `battery.spill` / `battery.drain` | flow scale (from `variable_scales`) | 1.0 |
| `bus.theta` / `bus.angle` | `1 / scale_theta` | 1/1000 |
| All other variables | 1.0 (no scaling) | — |

For example, `reservoir("R1").volume >= 5000` (in dam³) is automatically
translated to the LP constraint `scale × volume_LP ≥ 5000`, accounting
for the fact that the LP variable stores `volume_physical / scale`.

---

## 4. Element Identification

Elements can be referenced by **name** (single-quoted string) or by **numeric UID**
(bare integer):

```text
# By name (single-quoted string)
generator('TORO').generation
demand('D1').load
line('L1_2').flow

# By explicit UID prefix (single-quoted string)
generator('uid:23').generation

# By bare numeric UID (integer — automatically treated as uid:N)
generator(3).generation        -- equivalent to generator('uid:3')
demand(7).load                 -- equivalent to demand('uid:7')
battery(1).energy              -- equivalent to battery('uid:1')
```

**Mixing name and UID references** in the same expression is allowed:

```text
generator('G1').generation + generator(5).generation <= 300
```

---

## 5. Aggregation with `sum()`

The `sum()` function aggregates a variable across multiple elements of the
same type, inspired by AMPL's `sum{...}` syntax. This avoids listing each
element individually.

### Syntax

```text
sum( element_type ( id_list ) . attribute )
```

Where `id_list` is one of:
- **Explicit list**: `'G1', 'G2', 'G3'` or `1, 2, 3` or mixed
- **All elements**: `all`

### Examples

```text
# Sum generation over specific generators (by name)
sum(generator('G1', 'G2', 'G3').generation) <= 500

# Sum generation over specific generators (by UID)
sum(generator(1, 2, 3).generation) <= 500

# Mixed name and UID references
sum(generator('G1', 2, 'uid:3').generation) <= 500

# Sum over ALL generators in the system
sum(generator(all).generation) <= 1000

# Sum with a coefficient
0.5 * sum(demand('D1', 'D2').load) <= 200

# Combined: sum + individual elements
sum(generator('G1', 'G2').generation) + demand('D1').load <= 1000

# Balance constraint: total generation minus total demand
sum(generator(all).generation) - sum(demand(all).load) = 0
```

### AMPL comparison

| gtopt | AMPL equivalent |
|-------|-----------------|
| `sum(generator('G1','G2').generation)` | `sum{g in {"G1","G2"}} generation[g]` |
| `sum(generator(all).generation)` | `sum{g in GENERATORS} generation[g]` |
| `0.5 * sum(demand(all).load)` | `0.5 * sum{d in DEMANDS} load[d]` |

---

## 6. Domain Specifications

By default, a constraint applies to **every** scenario, stage, and block.
Use a `for(...)` clause to restrict the domain:

### Dimension names

| Dimension | Meaning |
|-----------|---------|
| `scenario` | Scenario index |
| `stage` | Stage (investment period) index |
| `block` | Block (operating hour) index |

### Index set forms

| Form | Meaning | Example |
|------|---------|---------|
| `all` | Every index | `stage in all` |
| `N` | Single value | `stage = 3` |
| `N..M` | Range (inclusive) | `block in 1..24` |
| `{N, M, ...}` | Explicit set | `stage in {1, 3, 5}` |
| `{N, M..P, Q}` | Mixed values and ranges | `block in {1, 5..10, 20}` |

### Syntax variants

Both `in` and `=` are accepted:

```text
for(stage in {1,2,3})     -- using 'in'
for(stage = 1)            -- using '=' (single value)
```

Unspecified dimensions default to `all`:

```text
for(block in 1..24)       -- all scenarios, all stages, blocks 1-24
```

---

## 7. Comments

Expressions support line comments using `#` or `//`. Everything after the
comment marker to the end of the line is ignored:

```text
generator('G1').generation <= 100   # limit gen output

generator('G1').generation          // first gen
+ generator('G2').generation        // second gen
<= 300
```

Multi-line expressions with comments are useful for documenting complex
constraints:

```text
# Total system generation capacity constraint
sum(generator(all).generation)    # MW total
<= 1000                          # system-wide limit
, for(block in 1..24)             # applies to all 24 blocks
```

---

## 8. Examples

### Example 1 — Simple generation cap

Limit generator G1 to 100 MW:

```json
{
  "uid": 1,
  "name": "g1_cap",
  "expression": "generator('G1').generation <= 100"
}
```

### Example 2 — Joint generation limit

The sum of two generators must not exceed 300 MW:

```json
{
  "uid": 2,
  "name": "gen_pair_limit",
  "expression": "generator('TORO').generation + generator('uid:23').generation <= 300, for(stage in {4,5,6}, block in 1..30)"
}
```

### Example 3 — Minimum generation requirement

Generator G1 must produce at least 50 MW:

```json
{
  "uid": 3,
  "name": "min_gen",
  "expression": "generator('G1').generation >= 50"
}
```

### Example 4 — Line flow limit

Restrict flow on line L1_2 to 200 MW:

```json
{
  "uid": 4,
  "name": "flow_limit",
  "expression": "line('L1_2').flow <= 200"
}
```

### Example 5 — Generation-load balance

Generator G1 output must equal demand D1 load:

```json
{
  "uid": 5,
  "name": "gen_demand_balance",
  "expression": "generator('G1').generation = demand('D1').load"
}
```

### Example 6 — Range constraint

Generator output must be between 50 and 250 MW:

```json
{
  "uid": 6,
  "name": "gen_range",
  "expression": "50 <= generator('G1').generation <= 250"
}
```

### Example 7 — Weighted sum with coefficients

Partial contributions from two generators:

```json
{
  "uid": 7,
  "name": "weighted_cap",
  "expression": "0.8 * generator('G1').generation + 0.5 * generator('G2').generation <= 200"
}
```

### Example 8 — Cross-element CHP coupling

Model combined heat-and-power relationship (generation proportional to
load):

```json
{
  "uid": 8,
  "name": "chp_coupling",
  "expression": "generator('CHP').generation - 1.5 * demand('HeatLoad').load = 0"
}
```

### Example 9 — Battery energy limit during peak hours

Limit battery state of energy during peak blocks:

```json
{
  "uid": 9,
  "name": "bess_peak_limit",
  "expression": "battery('BESS1').energy <= 400, for(block in {18, 19, 20, 21})"
}
```

### Example 10 — Scenario-specific constraint

Different limit in scenarios 1 and 2 only:

```json
{
  "uid": 10,
  "name": "scenario_limit",
  "expression": "generator('G1').generation <= 150, for(scenario in {1, 2})"
}
```

### Example 11 — Inactive constraint (disabled)

A constraint that is defined but not active:

```json
{
  "uid": 11,
  "name": "maintenance_limit",
  "active": false,
  "expression": "generator('G1').generation <= 10"
}
```

### Example 12 — Zero unserved energy requirement

Force no load curtailment on demand D1:

```json
{
  "uid": 12,
  "name": "no_curtailment",
  "expression": "demand('D1').fail = 0"
}
```

### Example 13 — Generator referenced by numeric UID

Use the bare integer syntax instead of `'uid:3'`:

```json
{
  "uid": 13,
  "name": "gen_uid_limit",
  "expression": "generator(3).generation <= 200"
}
```

### Example 14 — Sum over all generators (budget constraint)

Limit total system generation using `sum()`:

```json
{
  "uid": 14,
  "name": "total_gen_cap",
  "expression": "sum(generator(all).generation) <= 1000"
}
```

### Example 15 — Sum over specific generators

Constrain a subset of generators:

```json
{
  "uid": 15,
  "name": "thermal_limit",
  "expression": "sum(generator('G1', 'G2', 'G3').generation) <= 500, for(block in 1..12)"
}
```

### Example 16 — Sum with coefficient (weighted budget)

Weighted sum of demand served:

```json
{
  "uid": 16,
  "name": "weighted_demand",
  "expression": "0.5 * sum(demand('D1', 'D2').load) <= 200"
}
```

### Example 17 — Balance: total generation equals total demand

System-wide power balance using two `sum()` terms:

```json
{
  "uid": 17,
  "name": "system_balance",
  "expression": "sum(generator(all).generation) - sum(demand(all).load) = 0"
}
```

### Example 18 — Reservoir volume constraint

Limit reservoir volume during dry season:

```json
{
  "uid": 18,
  "name": "reservoir_min_vol",
  "expression": "reservoir('RES1').volume >= 1000, for(stage in {3, 4})"
}
```

### Example 19 — Converter charge/discharge limit

Limit total converter throughput:

```json
{
  "uid": 19,
  "name": "converter_limit",
  "expression": "converter('CV1').charge + converter('CV1').discharge <= 100"
}
```

### Example 20 — Expression with comments

Use `#` or `//` for inline documentation (useful in external files):

```json
{
  "uid": 20,
  "name": "documented_limit",
  "expression": "generator('G1').generation + generator('G2').generation <= 300 # peak capacity"
}
```

### Example 21 — Reserve provision limit

Limit up-reserve provision of a specific provider:

```json
{
  "uid": 21,
  "name": "up_reserve_limit",
  "expression": "reserve_provision('RP1').up <= 50"
}
```

### Example 22 — Reserve zone total up-reserve

Constrain total up-reserve in a zone across all provisions:

```json
{
  "uid": 22,
  "name": "zone_up_reserve_min",
  "expression": "reserve_zone('RZ1').up >= 100"
}
```

---

## 9. External Constraint Files

When there are many constraints, store them in a separate file.

### JSON format

```json
{
  "system": {
    "bus_array": [...],
    "user_constraint_file": "constraints.json"
  }
}
```

External JSON file (`constraints.json`):

```json
[
  {
    "uid": 1,
    "name": "gen_limit",
    "expression": "generator('G1').generation <= 100"
  },
  {
    "uid": 2,
    "name": "flow_limit",
    "expression": "line('L1').flow <= 200"
  }
]
```

### PAMPL format

PAMPL (pseudo-AMPL) files provide a more readable syntax with named
constraints, parameters, and comments:

```pampl
# System constraints
param pct_elec = 35;
param seasonal[month] = [0,0,0,100,100,100,100,100,100,100,0,0];

constraint gen_limit "Combined generation limit":
  generator('G1').generation + generator('G2').generation <= 300;

constraint seasonal_limit:
  generator('G1').generation <= pct_elec * seasonal[month];
```

PAMPL files are loaded automatically when referenced:

```json
{
  "system": {
    "user_constraint_file": "constraints.pampl"
  }
}
```

### Multiple external files

Use `user_constraint_files` (plural, array) to load multiple files
independently. Each file is parsed with auto-incremented UIDs to avoid
collisions:

```json
{
  "system": {
    "user_constraint_files": [
      "laja_agreement.pampl",
      "maule_agreement.pampl"
    ]
  }
}
```

This keeps each constraint set self-contained and avoids combining
files. Both `user_constraint_file` (singular) and
`user_constraint_files` (plural) can coexist — all sources are
accumulated.

### Combining inline and external

Both `user_constraint_array` and external files can be used
simultaneously. Constraints from all sources are accumulated:

```json
{
  "system": {
    "user_constraint_array": [
      {"uid": 1, "name": "inline_limit", "expression": "..."}
    ],
    "user_constraint_files": ["more_constraints.pampl"]
  }
}
```

### Multi-file merge

When loading from multiple JSON files (the standard gtopt pattern),
constraints from all files are accumulated via `System::merge()`:

```bash
gtopt base.json overrides.json
```

---

## 10. Formal Grammar (BNF)

```text
constraint     := expr comp_op expr [',' for_clause]
               |  number comp_op expr comp_op number [',' for_clause]

expr           := term (('+' | '-') term)*

term           := [number '*'] element_ref
               |  [number '*'] sum_expr
               |  ['-'] number

element_ref    := element_type '(' element_id ')' '.' IDENT

sum_expr       := 'sum' '(' element_type '(' id_list ')' '.' IDENT ')'

id_list        := 'all'
               |  element_id (',' element_id)*

element_id     := STRING          -- name: 'G1' or 'uid:3'
               |  number          -- bare UID: 3 → uid:3

element_type   := 'generator' | 'demand' | 'line' | 'battery'
               |  'converter' | 'reservoir' | 'bus'
               |  'waterway' | 'turbine'
               |  'junction' | 'flow' | 'filtration'
               |  'reserve_provision' | 'reserve_zone'

comp_op        := '<=' | '>=' | '='

for_clause     := 'for' '(' index_spec (',' index_spec)* ')'

index_spec     := index_dim ('in' | '=') index_set

index_dim      := 'scenario' | 'stage' | 'block'

index_set      := 'all'
               |  '{' index_values '}'
               |  number '..' number
               |  number

index_values   := index_value (',' index_value)*

index_value    := number
               |  number '..' number

comment        := ('#' | '//') <anything to end of line>

STRING         := '"' <characters> '"' | "'" <characters> "'"
IDENT          := [a-zA-Z_][a-zA-Z0-9_]*
number         := [0-9]+ ('.' [0-9]+)?
```

---

## 11. Comparison with AMPL

### AMPL equivalents

```ampl
# ── gtopt: simple capacity constraint ──
# generator('G1').generation <= 100
# AMPL:
subject to g1_cap:
  generation["G1"] <= 100;

# ── gtopt: sum over element group ──
# sum(generator('G1','G2','G3').generation) <= 500, for(block in 1..12)
# AMPL:
subject to thermal_limit {b in 1..12}:
  sum{g in {"G1","G2","G3"}} generation[g,b] <= 500;

# ── gtopt: budget constraint (sum over all) ──
# sum(generator(all).generation) <= 1000
# AMPL:
subject to budget_constraint:
  sum{g in GENERATORS} generation[g] <= 1000;

# ── gtopt: cross-element balance ──
# sum(generator(all).generation) - sum(demand(all).load) = 0
# AMPL:
subject to balance:
  sum{g in GENERATORS} generation[g] - sum{d in DEMANDS} load[d] = 0;

# ── gtopt: weighted sum with domain ──
# 0.8 * generator('G1').generation + 0.5 * generator('G2').generation <= 200,
#     for(stage in {4,5,6}, block in 1..30)
# AMPL:
subject to weighted_cap {s in STAGES, b in BLOCKS: s in {4,5,6} and b >= 1 and b <= 30}:
  0.8 * generation["G1",s,b] + 0.5 * generation["G2",s,b] <= 200;
```

### Key differences from AMPL

| Aspect | AMPL | gtopt |
|--------|------|-------|
| Element access | `generation["G1",s,b]` | `generator('G1').generation` |
| Element by UID | Not applicable | `generator(3).generation` |
| Sum syntax | `sum{g in SET} expr` | `sum(element_type(list).attr)` |
| Index sets | `{s in STAGES: s >= 4}` | `for(stage in {4,5,6})` |
| Constraint name | `subject to name:` | `"name": "..."` field |
| Comments | `#` only | `#` and `//` |
| File format | `.mod` text file | JSON field or external file |
| Scope | Full modeling language | LP constraints only |
| Set definitions | Explicit `set GENERATORS;` | Implicit from system model |

### Design philosophy

The gtopt constraint language is intentionally **narrower** than AMPL:

- **No set definitions needed**: Element sets are implicit from the system
  model. `sum(generator(all).generation)` automatically sums over all
  generators in the system, without requiring a `set GENERATORS;` declaration.

- **Element-centric**: Variables are accessed via element references
  (`generator('G1').generation`) rather than indexed arrays
  (`generation["G1",s,b]`). This is more natural for power system engineers.

- **JSON-native**: Constraints live in JSON files alongside the rest of the
  case definition, enabling programmatic generation from scripts and GUIs.

---

## 12. Best Practices

1. **Name constraints meaningfully**: use descriptive names like
   `gen_pair_limit` or `night_battery_reserve`, not `c1` or `test`.

2. **Start without domain restrictions**: let the constraint apply to all
   time steps first, then narrow with `for(...)` as needed.

3. **Use UIDs for stability**: `generator('uid:5')` or `generator(5)` is
   stable across name changes; `generator('TORO')` breaks if the generator
   is renamed.

4. **Prefer `sum()` over manual expansion**: use
   `sum(generator(all).generation)` instead of listing every generator
   individually — it's shorter, self-documenting, and auto-adapts when
   generators are added or removed.

5. **Use comments to document intent**: add `# ...` or `// ...` comments
   to explain *why* a constraint exists, not just what it does.

6. **Set `active: false` for debugging**: disable a constraint without
   removing it from the file.

7. **Use external files for large constraint sets**: when you have more than
   ~10 constraints, move them to a separate file referenced by
   `user_constraint_file`.

8. **Validate with `use_single_bus: true` first**: check that your
   constraints are feasible in a simple model before adding network
   constraints.

9. **Check LP feasibility**: if adding user constraints makes the problem
   infeasible (`status != 0`), check `output/Demand/fail_sol.csv` for
   unserved demand.

---

## 13. See Also

- **[Irrigation Agreements](irrigation-agreements.md)** — Laja and Maule
  agreement modeling, FlowRight/VolumeRight entities, PLP comparison
- **[Input Data Reference](input-data.md)** — Full JSON input format specification
  (§3.18 for UserConstraint fields)
- **[Mathematical Formulation](formulation/mathematical-formulation.md)**
  — LP/MIP formulation details
- **[Planning Guide](planning-guide.md)** — Step-by-step planning guide
- **[Usage Guide](usage.md)** — Command-line options and output interpretation
