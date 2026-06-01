# Commitment Layout & the `IntegerVariable` choke-point

**Status:** draft · **Owner:** marcelo · **Last updated:** 2026-05-31
**Tracks:** UC modernisation, PLEXOS / SDDP parity, AMPL/user-constraint
integration

---

## 1. One-sentence summary

Make the integer grid of every UC-style variable in gtopt (commitment
`u/v/w`, converter mode, expansion-module build, future battery / pump
on/off, future binary reserves) **first-class, configurable per element,
and synchronisable system-wide** via a single `commitment_layout`
description. All integer columns route through one
`SystemContext::add_integer_variable` choke-point — no `is_integer = true`
sneaks into `lp.add_col` anywhere else.

The continuous grid (`block_array`, dispatch resolution) stays
untouched.

---

## 2. Why now

| Driver | Today | Pain |
|---|---|---|
| PLEXOS parity | `Commitment.commitment_period` is a per-generator scalar (`include/gtopt/commitment.hpp:160`) | No CSV, no CLI, no per-block group definition; can't mirror PLEXOS *Periods per Day* / *Commitment Granularity* per unit. |
| Sub-hourly dispatch | block grid drives both continuous and integer | 15-min dispatch implies 15-min UC — combinatorially infeasible at week-long horizons. |
| Multiple integer producers | 4 unrelated sites (`commitment_lp`, `simple_commitment_lp`, `converter_lp`, `capacity_object_lp`) each call `add_col({.is_integer=...})` directly | Solver-side relaxation, per-(scene, phase) bookkeeping, AMPL exposure, and integer-feasibility audits each have to special-case 4 callers. |
| SDDP cuts on UC | Backward pass relaxes everything to LP; no central place that knows "this was integer" | Cut interpretation, fixation traces, and `--ampl-state` checks all need to recover the integer set by grep. |
| AMPL exposure | UC vars are registered as `AmplVariable` per block | A `commitment("X").status` query lies about resolution when the underlying col is group-shared. |

A single registry-backed abstraction fixes the choke-point problem
cleanly, and `commitment_layout` rides on top of it.

---

## 3. Prior art (precise analogies, with caveats)

| Tool | Mechanism | What gtopt actually borrows / where the analogy breaks |
|---|---|---|
| **PLEXOS — `Generator.Commit Periods`** | Per-unit commitment granularity in hours; applies at the generator class. | **Closest analogy.** gtopt's per-element `commit_period` / `commit_layout` mirrors this directly. Note: PLEXOS `Commit Periods` is set at the class level (every unit in the class shares it); gtopt's per-element field is finer (per generator). The system-level binding stack closes the gap by allowing `by_class` defaults. |
| **PLEXOS — `Periods per Day`** | Daily aggregation knob; says "use N commitment periods per 24 h day". | Captured by the `stride` / `hours` shorthand in §6.1 when combined with a daily stage. Applies to the MT schedule; the ST schedule always runs chronologically. **Caveat:** PLEXOS `Periods per Day` is a CONTAINER property, not per-unit — our binding stack handles that via `by_class`. |
| **PLEXOS LT / MT / ST phase chain** | Multi-resolution problems with integer states FIXED downstream (LT → MT → ST). | gtopt cascade carries an analogous `level_array[*].commitment_layout` per level. **Caveat:** PLEXOS passes FIXED integer values downstream; gtopt's cascade today passes `initial_status` scalars between phases. The layout knob coarsens within a level, not across levels. |
| **PowerSimulations.jl — `feedforward`** | Passes integer SOLUTIONS from a coarser problem as FIXED PARAMETERS into a finer problem. | gtopt already does this via `initial_status` propagation in cascade. **Caveat:** PowerSim's feedforward is integer-fixing across resolutions; gtopt's `commitment_layout` is coarsening WITHIN a resolution. They are orthogonal capabilities — not directly equivalent. |
| **GenX — `operational subperiod weighting`** | Representative-day collapse with weights; UC and dispatch share the representative day at hourly resolution. | **Not equivalent.** GenX collapses time via clustering / representative days, leaving dispatch and commitment at the same resolution. `commitment_layout` keeps every continuous block intact and coarsens ONLY the integer grid. The two address different problems. |
| **MISO / ISO-NE real-time** | Sub-hourly economic dispatch with hourly UC look-ahead. | Directly analogous: sub-hourly `block_array` + hourly `commitment_layout`. |
| **Literature** — Garver 1962 (count-form UT), Rajan–Takriti 2005 (tight UT), Morales-España 2013 (tight + ramp), Knueven 2020 (formulation survey UT1–UT5), Hua 2018 (multi-interval pricing), Palmintier 2014 (reduced-resolution UC) | Three-bin UC, tight formulations, multi-resolution UC. | Existing `commitment_lp.cpp` uses Garver / UT2 count form (NOT the Rajan–Takriti tight form); see §4 LP-relaxation tightness note. Tightening to UT3 is deferred. |

Naming convention chosen: **`commitment_layout`** for the system-level
table (mirrors `block_array`), **`IntegerVariable`** for the
runtime choke-point (mirrors `StateVariable`).

---

## 4. Concept

Two orthogonal grids over the same `Stage`:

```
Block grid       ─┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐  168 blocks · 1 h
                  │ b1│ b2│ b3│ b4│ b5│ b6│ b7│ b8│ b9│b10│b11│b12│  …
                  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                  continuous vars: P_b, F_b, SoC_b, reserves_b, …

Commit layout    ─┬───────────┬───────────┬───────────┬───────────┐  42 groups · 4 h
                  │    g1     │    g2     │    g3     │    g4     │  …
                  └───────────┴───────────┴───────────┴───────────┘
                  integer vars: u_g, v_g, w_g, expmod_g, mode_g, …
```

Each commitment group `G` is a contiguous, non-overlapping set of blocks
**within a single stage**. Per-stage, every block belongs to exactly one
group. Layouts do not cross stage boundaries.

For every block `b ∈ G`:

```
0 ≤ p_b ≤ Pmax · u_G                     gen upper bound
        p_b ≥ Pmin · u_G                       gen lower bound
        u_G − u_{G−1} = v_G − w_G              logic (per group)
        v_G + w_G ≤ 1                          exclusion
        cumulative-duration min-up / min-down windows (Σ v_G, Σ w_G)
        p_b ramps: per-block (continuous), startup-ramp/shutdown-ramp on
          the first/last block of G
        no-load cost: c_NL · u_G · Σ_{b∈G} dur(b)
        startup cost: per v_G transition
```

This is exactly the existing `commitment_lp.cpp` formulation generalised
from "greedy uniform period" to "arbitrary group map".

**LP-relaxation tightness note.** The min-up / min-down windows are the
**count-based aggregate form** (Knueven 2020 UT2 / Garver), `Σ_{q∈W} u_q ≥ |W| · v_p`,
not the LP-tight endpoint form (Knueven 2020 UT3 / Rajan–Takriti 2005),
`u_p − u_{p−1} ≤ u_q ∀ q ∈ W`. UT2 admits fractional LP solutions that
UT3 cuts. The existing code (`commitment_lp.cpp:710-834`) is UT2;
the design does not change this. For heterogeneous group durations the
relaxation looseness compounds (a fractional `u_G = 0.5` on a 4 h group
contributes the same "count" as on a 1 h group despite representing 4×
the committed capacity). Tightening to UT3 is a Phase-3 candidate; for
Phase 0–2 the documented behaviour is UT2.

---

## 5. The `IntegerVariable` abstraction

Mirrors `StateVariable` (`include/gtopt/state_variable.hpp`).

### 5.1 Scope: chronological vs non-chronological

Not every integer variable lives on the block grid. Some are
"singletons" with respect to time:

| Scope | Cardinality | Examples |
|---|---|---|
| **`Block`** | One col per (stage, block) — degenerate group. | Today's identity layout. |
| **`Group`** | One col per (stage, group ∈ layout). | Generalised UC: `Commitment.status`, `.startup`, `.shutdown`. |
| **`Stage`** | One col per stage, layout-immune. | Hypothetical per-stage AGC binary; per-stage maintenance flag. |
| **`Phase`** | One col per phase, layout-immune, scenario-invariant. | `CapacityObject.expmod` — expansion decision is per-phase regardless of any block / group structure. |

`Phase` and `Stage` scopes **ignore the `commitment_layout`**: no
binding table entry can move them onto a block grid (and a binding
that tries to is a hard error). They are still registered through
`add_integer_variable`, so the "no integer column without
`IntegerVariable`" invariant holds uniformly.

### 5.2 Header sketch

```cpp
// include/gtopt/integer_variable.hpp  (new)
namespace gtopt {

enum class IntegerDomain : std::uint8_t {
  Binary,        // {0, 1}
  Integer,       // [lowb, uppb] integer
  Relaxed,       // continuous (user / global relax)
};

enum class IntegerScope : std::uint8_t {
  Block,         // 1:1 with blocks (identity layout)
  Group,         // grouped via commitment_layout
  Stage,         // one per stage, layout-immune
  Phase,         // one per phase, layout-immune
};

class IntegerVariable : public LPVariable {
public:
  using LPKey = gtopt::LPKey;          // (scene, phase, kind)

  struct Key {
    LPClassName class_name {};         // "Commitment", "Converter", "CapacityObject", …
    Uid element_uid {unknown_uid};
    std::string_view col_name {};      // "status", "startup", "shutdown", "expmod", "mode", …
    ScenarioUid scenario_uid {};       // unknown_uid_of<Scenario>() for scenario-invariant scopes
    StageUid stage_uid {};             // unknown_uid_of<Stage>()    for Phase scope
    GroupUid group_uid {};             // unknown_group              for Block / Stage / Phase scope
    LPKey lp_key {};
    auto operator<=>(const Key&) const noexcept = default;
  };

  ColIndex col() const noexcept;
  IntegerDomain domain() const noexcept;
  IntegerScope  scope() const noexcept;
  GroupUid      group_uid() const noexcept;  // valid only when scope == Group

  // Blocks this col fans out across at row-stamp time.
  // Returns std::nullopt for Phase scope (no per-block fan-out exists);
  // callers that need per-block iteration must check the optional and
  // either branch on scope() or assert the scope they require. The
  // empty-span sentinel was rejected at design time — it silently
  // turned Phase-scope iteration into a no-op when callers forgot to
  // check scope first.
  //
  // - Block:  exactly one block         → non-empty span (size 1)
  // - Group:  every block of the group  → non-empty span (size K)
  // - Stage:  every block of the stage  → non-empty span (size n_blocks)
  // - Phase:  no per-block fan-out      → std::nullopt
  std::optional<std::span<const BlockUid>> blocks() const noexcept;

  // Strict accessor: throws if called on Phase scope. Use at sites that
  // KNOW the scope is chronological (e.g. CommitmentLP after the layout
  // resolver has refused Phase-scope bindings).
  std::span<const BlockUid> blocks_or_throw() const;
};

using IntegerVariableMap = flat_map<IntegerVariable::Key, IntegerVariable>;
}
```

### 5.3 Registration entry point

```cpp
class SystemContext {
public:
  IntegerVariable& add_integer_variable(
      LPClassName       class_name,
      Uid               element_uid,
      std::string_view  col_name,           // CommitmentLP::StatusName, etc.
      const ScenarioLP&,
      const StageLP&,
      IntegerScope      requested_scope,    // caller asserts the scope it wants
      IntegerDomain     default_domain,     // Binary / Integer
      LpColSpec         spec);              // lowb, uppb, cost, fixed_value
};
```

`requested_scope` is the caller's truth claim — `CommitmentLP` always
asks for `Group`, `CapacityObjectLP` always asks for `Phase`. The
binding resolver may refine *within* a scope (`Group` → which named
layout) but may **not** cross scopes — if a binding sets
`CapacityObject.expmod → 4h`, the resolver rejects it at load time.

### 5.4 Invariants enforced inside `add_integer_variable`

1. **Single source of integrality.** `lp.add_col({.is_integer = ...})`
   is private to this function (enforced by clang-tidy custom check +
   a single whitelisted call site).
2. **Domain decision precedence**, highest first:
   - per-element `relax` flag (e.g. `Commitment.relax`)
   - global `planning_options.relax_integers`
   - default domain at the call site
3. **Scope precedence** (highest first; cannot lift to a finer grid):
   - `requested_scope` at the call site is a hard upper bound on
     coarseness — `Phase < Stage < Group < Block` (coarser → finer).
   - A binding may refine within `Group` (picks the layout) but may
     not move `Phase`/`Stage` callers onto a block grid.
4. **Group → ColIndex memoised** per `(stage, class, element_uid, col_name, group_uid)`
   — every producer for the same group/element hits the same col.
5. **AMPL exposure** is auto-registered: `add_integer_variable` also
   calls `add_ampl_variable(...)` with `block_cols` fanned out across
   `blocks()`, so `commitment("X").status` resolves identically on
   every block in a group, on every block of a `Stage`-scoped column,
   and is **not** registered per-block for `Phase`-scoped columns
   (PAMPL gets a `stage_col`-shaped registration instead).
6. **State-variable bridging.** Independent of integrality. When the
   variable is also a cross-phase boundary (mostly `expmod`), the
   caller additionally registers it via `add_state_variable` —
   `IntegerVariable` complements `StateVariable`, doesn't replace it.
   Same `LPKey`.

This is the "**no integer column without `IntegerVariable`**" rule the
user asked for, made enforceable.

---

## 6. `commitment_layout` data model

Three orthogonal concepts: **layouts** (the named grids),
**bindings** (which integer variable uses which grid), **scopes**
(whether the grid even applies — see §5.1). The same shape is
expressible in JSON (in `planning_options`) and in CSV (input dataset).
The CLI offers only the simplest use case: a single global default.

### 6.1 Layout definitions

A **layout** is a named map from `(stage → list of groups → blocks)`.
Defined once, referenced by name from any number of bindings. Identical
to the §6.2 schema in the previous revision; reproduced here as the
**`commitment_layout_table`** input file (CSV/Parquet, same loader as
`block_array`):

```csv
# commitment_layout_table.csv
layout_name,stage_uid,group_uid,first_block,last_block
hourly,*,1,1,1
hourly,*,2,2,2
...
4h,*,1,1,4
4h,*,2,5,8
...
mixed,1,1,1,1
mixed,1,2,2,2
mixed,1,7,7,12
mixed,1,8,13,16
...
```

Rules (hard errors at load):

- `layout_name` is a `Name` (any token).
- `stage_uid = "*"` expands to every stage of the simulation.
- For every (`layout_name`, `stage_uid`) pair, groups must be
  contiguous, non-overlapping, and cover every block of the stage
  exactly once.

Equivalent JSON (inside `planning_options`):

```yaml
commitment_layout:
  layouts:
    hourly: { type: stride, blocks_per_group: 1 }
    4h:     { type: stride, blocks_per_group: 4 }
    daily:  { type: hours,  hours: 24.0 }
    mixed:  { type: table,  table: commitment_layout_table.csv, name: mixed }
```

`stride` and `hours` are pure shorthands the loader expands into the
canonical group table at LP-build time.

The reserved layout name **`identity`** is always available and
means "one group per block". The reserved name **`none`** means "this
integer variable doesn't go on a block grid"; only legal for `Stage`
and `Phase` scopes.

### 6.2 Bindings — layered specialisation

A **binding** is a tuple `(class, element, attribute) → layout`
saying "this integer variable uses this layout". Bindings stack: more
specific overrides less specific. The five specificity levels, lowest
to highest:

| Level | Selector | Use case |
|---|---|---|
| 0 — global default | `*.*.*` | Whole-system default ("everything runs hourly"). |
| 1 — class | `Commitment.*.*` | "All commitments coarse, all converters hourly". |
| 2 — class + attribute | `Commitment.*.startup` | PLEXOS-style: `status` coarse, `startup`/`shutdown` fine. |
| 3 — element | `Commitment:G42.*` | One unit gets a different granularity. |
| 4 — element + attribute | `Commitment:G42.startup` | Finest grain; per-unit, per-binary tuning. |

Resolution at LP-build time, per (class, element, attribute):

```
chosen_layout = first non-empty binding from levels 4 → 3 → 2 → 1 → 0
              → falls back to `identity` if none.
chosen_layout must agree with the call-site scope (§5.3):
  - Group scope:  any layout (including `identity`) is valid.
  - Stage scope:  only `none` is valid.
  - Phase scope:  only `none` is valid.
A scope mismatch is a hard error at load.
```

#### 6.2.1 JSON form

```yaml
commitment_layout:
  layouts:
    hourly: { type: stride, blocks_per_group: 1 }
    4h:     { type: stride, blocks_per_group: 4 }
    daily:  { type: hours,  hours: 24.0 }

  bindings:
    # Level 0 — global default for chronological integers
    default: hourly

    # Level 1 — per class
    by_class:
      Commitment:    4h
      Converter:     hourly
      CapacityObject: none   # Phase scope — layout-immune

    # Level 2 — per (class, attribute)
    by_class_attribute:
      "Commitment.startup":  hourly
      "Commitment.shutdown": hourly

    # Level 3 — per element (all of that element's integer attributes)
    by_element:
      "Commitment:G42": daily

    # Level 4 — per (element, attribute)
    by_element_attribute:
      "Commitment:G42.startup": hourly
```

`Commitment:G42` uses `Uid` (`G42 → 42`); `Commitment:"name"` is the
named form, resolved through the same `AmplElementNameMap` PAMPL uses.

#### 6.2.2 CSV form

Two tables — layouts in §6.1, bindings in a separate file. A wildcard
`*` in any selector field matches all values. Most-specific match wins
(non-`*` cells outrank `*` cells column-by-column from
`element_uid` → `attribute` → `class_name`).

```csv
# commitment_bindings.csv
class_name,element_uid,attribute,layout_name
*,*,*,hourly                          # Level 0 — global default
Commitment,*,*,4h                     # Level 1 — class default
Commitment,*,startup,hourly           # Level 2 — class.attribute
Commitment,*,shutdown,hourly
Commitment,42,*,daily                 # Level 3 — per-element
Commitment,42,startup,hourly          # Level 4 — per-(element, attribute)
CapacityObject,*,*,none               # Phase scope: no block grid
```

Rules (hard errors at load):

- `(class_name, element_uid, attribute)` is unique across the table.
- `layout_name` must exist in the layout table, or be the reserved
  `identity` / `none`.
- The chosen layout must respect the call-site `IntegerScope`
  (§5.3) — e.g. `CapacityObject.expmod → 4h` is rejected because
  `expmod` is `Phase` scope.
- Wildcard cells must be the literal `*`; empty cells are not
  permitted (keeps the precedence rule unambiguous).

**Precedence — explicit level mapping, not column-by-column.** Every CSV
row is scored by its specificity level using a single rule that mirrors
§6.2 exactly:

| Row pattern (class, element, attr) | Level |
|---|---|
| `*`, `*`, `*` | 0 — global default |
| `Class`, `*`, `*` | 1 — class |
| `Class`, `*`, `attr` | 2 — class + attribute |
| `Class`, `Uid`, `*` | 3 — element |
| `Class`, `Uid`, `attr` | 4 — element + attribute |

Higher level wins. Within a level, every row is uniquely keyed by the
tuple, so no further tie-break is needed. The previously vague
"column-by-column wildcard ranking" is replaced by this explicit table.
Mixed wildcards such as `(*, Uid, *)` (element without class) are
illegal — element identifiers are class-scoped in gtopt, so the loader
rejects them at load time.

### 6.3 Per-element shorthand fields (PLEXOS parity)

Every UC-bearing element still carries the same optional trio. These
are read **after** the binding table and add Level-3 / Level-4 entries
on the fly — they are syntactic sugar over the binding mechanism, not a
parallel channel.

| Field | Type | Specificity | Semantics |
|---|---|---|---|
| `commit_layout` | `OptName` | Level 3 | Name of a layout from the table. |
| `commit_period` | `OptReal` | Level 3 | Hours; greedy cumulative-duration grouping. Loader expands to an anonymous layout `__autoG<period>h`. |
| `commit_stride` | `OptInt` | Level 3 | Blocks per group, uniform stride. Loader expands to `__autoS<stride>`. |
| `commit_layout_<attr>` | `OptName` | Level 4 | Per-attribute override; e.g. `commit_layout_startup`. |

When both a field on the element and a binding-table row target the
same (class, element, attribute) tuple, the binding-table row wins
(explicit beats implicit). A debug-build warning flags the override.

### 6.4 Worked example — PLEXOS-style mixed UC

> "Coal units commit on 4-hour windows; their startups/shutdowns on
> the underlying hourly grid; one specific peaker `G42` commits
> daily; expansion modules are per-phase; everything else hourly."

```yaml
commitment_layout:
  layouts:
    hourly: { type: stride, blocks_per_group: 1 }
    4h:     { type: stride, blocks_per_group: 4 }
    daily:  { type: hours,  hours: 24.0 }

  bindings:
    default: hourly
    by_class:
      Commitment:     4h
      CapacityObject: none
    by_class_attribute:
      "Commitment.startup":  hourly
      "Commitment.shutdown": hourly
    by_element:
      "Commitment:G42": daily
    by_element_attribute:
      "Commitment:G42.startup":  hourly
      "Commitment:G42.shutdown": hourly
```

`G42.status` → `daily`, `G42.startup` → `hourly` (Level-4 wins over
Level-3); every other commitment → `4h` status, `hourly` start/stop;
every converter mode and any future binary reserve → `hourly`;
`expmod` → `Phase` scope, unaffected by any binding.

### 6.5 CLI surface — global default only

The CLI is restricted to the simplest possible use: set a single
**global default layout** that becomes Level 0 of the binding stack.
Per-class, per-element, per-attribute specialisation requires the
JSON/CSV channels of §6.1–§6.3. This keeps the CLI surface small and
unambiguous.

```
--commitment-layout       PATH            # commitment_layout_table file (also enables bindings.csv beside it)
--commitment-stride       K               # global default: every K blocks → 1 group
--commitment-hours        H               # global default: ≥H hours of cumulative duration per group
--commitment-layout-name  NAME            # global default: pick a named layout from the table
--commit-relax            true|false      # global LP relaxation
```

`--commitment-stride`, `--commitment-hours`, and
`--commitment-layout-name` are mutually exclusive (use only one);
each becomes the Level-0 `default` binding. They do not write Level-1+
entries — anything finer-grained must come from the bindings table or
JSON. `--commitment-layout PATH` enables the input-dataset table but
does not by itself set a default.

Precedence (highest → lowest):
JSON `bindings` > CSV `commitment_bindings.csv` > CLI Level-0 default >
dataset table > built-in `identity`.

---

## 7. Coverage — every integer producer

| Producer | Scope | Today | After this change | Notes |
|---|---|---|---|---|
| `commitment_lp.cpp` (status / startup / shutdown) | `Group` | Greedy `commitment_period` + `is_integer = !is_relax` | Three `add_integer_variable("Commitment", uid, {"status","startup","shutdown"}, …, IntegerScope::Group, …)` registrations. Keep relax + tier-startup logic untouched. | Largest diff; smallest risk — the formulation is unchanged. |
| `commitment_lp.cpp` startup tiers (`hot_start` / `warm_start` / `cold_start`) | `Group` | `is_integer = !is_relax` per period at `commitment_lp.cpp:906/920/934` | Three additional `add_integer_variable("Commitment", uid, {"hot_start","warm_start","cold_start"}, …, IntegerScope::Group, IntegerDomain::Binary, …)` registrations. **Required at Phase 0 — without them the clang-tidy choke-point ban breaks the build.** | The C8 equality `v_p = y_hot_p + y_warm_p + y_cold_p` still pins `v` to {0,1} at integer optima via the tier columns. |
| `simple_commitment_lp.cpp` (status) | `Group` | `is_integer = !is_relax` per block | Same registration, single column. | Trivial. |
| `converter_lp.cpp` (mode, dir) | `Group` | `is_integer = true` per block, lines 228 + 270 | Two registrations, `default_domain = Binary`. Add `OptBool relax` field to `Converter` for consistency with `Commitment`. | |
| `capacity_object_lp.cpp` (`expmod`) | **`Phase`** | `is_integer = m_integer_expmod_` once per phase | Registration with `IntegerScope::Phase`, `default_domain = Integer`. **Layout-immune** — any binding that targets `CapacityObject.expmod` with a chronological layout is rejected at load (§5.3, §6.2). | Already a single col per phase; scope encodes that explicitly. |
| `battery_lp.cpp` (future on/off) | `Group` | n/a | Same path the day the column is added. | No retrofit needed once the choke-point exists. |
| `pump.hpp` (future on/off) | `Group` | n/a | Same. | |
| `reserve_provision_lp.cpp` (future binary AGC reserves) | `Group` (or `Stage` for system-wide AGC slot) | n/a | Same; both scopes supported. | |
| Hypothetical per-stage maintenance flag | `Stage` | n/a | One col per stage, layout-immune. | Reserved scope for future use. |

Switching one producer at a time is safe because the choke-point is
created first (Phase 0) with the binding stack defaulting to
`identity` for `Group` scope — bit-identical LP. `Phase`-scope
producers (`expmod`) are already one-col-per-phase today, so the
scope tag is metadata only at Phase 0.

---

## 8. Edge cases & cross-cutting concerns

| Concern | Resolution |
|---|---|
| **`relax` precedence** | per-element `relax` > global `planning_options.relax_integers` > default domain. Resolved inside `add_integer_variable`. |
| **`fixed_status` schedule** | **Conflict rule per (stage, group):** (a) NO block has an entry → group is free. (b) ALL entried blocks agree → that value pins `u_G`. (c) Two entried blocks DISAGREE → **hard error** with a diagnostic naming both blocks. (d) Mixed entried + unentried → entried blocks must agree; unentried blocks inherit the pin (logged at INFO). Today's code at `commitment_lp.cpp:240` uses the representative-block value only; the new rule replaces that with the explicit four-case decision so user-intent mismatches surface immediately. |
| **`pmin` per-block schedule** | Block grid (continuous coupling) — `p_b ≥ pmin_b · u_G` — **but** the layout resolver SPLITS any group at points where `pmin_b` changes value within it. Otherwise a 4 h group with `pmin = [0, 0, 150, 150]` MW could be served by `u_G = 0.5` plus `p = [0, 0, 75, 75]` in the LP relaxation — producing duals that misprice commitment. Splitting preserves physical fidelity; if a split makes the effective group shorter than the user-requested granularity, INFO-log the schedule-induced split. |
| **Min-up / min-down times** | Translated to cumulative-duration windows on the group grid. **Approximation bound:** when a constraint duration falls inside a single group (`min_*_time ≤ dur(G)`), the constraint is trivially satisfied by the group itself and effectively dropped (`commitment_lp.cpp:746` `continue`). For constraints longer than one group, the per-group rounding error is bounded by `max_dur(G)`. E.g. `min_down_time = 25 h` on 24 h groups enforces 24 h of rest, not 25 h. This is a known PLEXOS-equivalent trade-off (Palmintier 2014 §3.2); the doc surfaces it explicitly so users don't read T7 as "exactly respected". |
| **Startup-cost tiers (hot/warm/cold)** | Offline counter measured in group-duration sum. Same logic; lifted to groups. Tier variables `y_hot`/`y_warm`/`y_cold` go through `add_integer_variable` (§7) — they are integer columns and must respect the choke-point. |
| **Ramps** | Per-block (continuous). Startup-ramp/shutdown-ramp attach to the FIRST / LAST block of the group only. **LP-relaxation looseness:** at `u_G = 0.5` (relaxed), inner group blocks 2…N have only `p_b ≥ pmin · u_G = 0.5 · pmin`, with no startup-ramp adjustment. The integer solution is correct; the LP relaxation that the SDDP backward pass sees is looser than necessary. Tighter inner-block bounds (Morales-España 2014) are a Phase-3 candidate. |
| **Reserves** | Continuous reserves → block grid, unchanged. Binary AGC reserves → integer choke-point. |
| **SoC / storage** | Block grid. Unchanged. |
| **SDDP backward pass** | Cuts are on continuous state variables. Integer columns are relaxed in the backward LP per the existing path; `IntegerVariable::domain()` makes the relaxation site explicit. **For `Phase`-scope integers (e.g. `expmod`):** the relaxation produces valid LP-lower-bound cuts on the INTEGER value function but they may be loose (inner approximation of the integer convex hull). When `max_modules` is large (≥ ~10) convergence can stall — a per-phase L-shaped / Benders layer for `expmod` is the textbook fix and is tracked in §10. |
| **Cascade levels** | Each level's `planning_options.commitment_layout` overrides the global one. Coarse → fine across levels, like PLEXOS LT/MT/ST. |
| **Stage-boundary link under mismatched layouts** | The C1 logic link between the last group of stage S and the first group of stage S+1 is ALWAYS mediated by the scalar `initial_status` (and `initial_hours`, `initial_power` for ramps and tiers), NOT by a cross-stage `u_G` column reference. The current code at `commitment_lp.cpp:202-203,327` resets `prev_ucol = {}` at the start of every stage; the inter-stage link goes through scalar boundary conditions. **Implementers must not attempt a direct cross-stage column link** — the column lives in the previous stage's LP-build call and is not reachable. Layouts on adjacent stages are therefore decoupled at LP-build time; no cross-stage layout compatibility check is needed. |
| **AMPL `commitment("X").status` at sub-group block** | Resolves to the shared group col automatically via the fan-out in `add_integer_variable`. PAMPL never sees a different shape. **Per-row stamping is one-stamp-per-row, NOT K-stamps-per-row:** the user-constraint builder at `user_constraint_lp.cpp:796-827` creates a fresh `SparseRow` per block, so K blocks in a group produce K identical rows (each `... = 1.0 · u_G`), not one row with coefficient `K · u_G`. Regression-pinned by `test/source/test_user_constraint_commitment_period.cpp` (28 assertions). The K identical rows are LP-correct (presolve collapses them); duals and objective are unaffected. |
| **Kink-row coefficient density** | Piecewise heat-rate kink rows at `commitment_lp.cpp:556-575` add a `−breakpoint_k` coefficient on `u_G` per (block, segment). With K-block groups and S segments, a single `u_G` column carries `K × S` non-zeros across the constraint matrix. For 24 h groups with 4 segments that's 96 non-zeros on a binary column — atypical and degrades Markowitz fill-in. When all blocks in the group have identical `pmax_segs` (the common case — segments don't vary per block), Phase-3 can collapse the K per-block kink-row gating into one aggregated row, dropping `K − 1` non-zeros per (col, segment). Tracked in §10. |
| **State-variable bridging** | `IntegerVariable` ≠ `StateVariable`; producers that need both (e.g. expmod across phases) register both. Same `LPKey`. |
| **Group across non-chronological stages** | UC is already skipped on `chronological=false` stages (`commitment.hpp:13–15`). Layout is consulted only on chronological stages. |
| **Cross-scenario** | Layout is **stage-keyed only**, identical across scenarios — exactly the user's "same for all scenarios" constraint. Holds for every scope. |
| **Loaded but unused** | A layout or binding defined in the table but not referenced by any registered integer variable is a warning, not an error. |
| **Solver round-trip** | `LinearInterface::is_integer(col)` matches `IntegerVariable::domain() ≠ Relaxed` post-build — a debug assert verifies it. |
| **Scope mismatch in a binding** | A binding pointing `CapacityObject.expmod` (Phase scope) at a chronological layout (`4h`, `hourly`, `identity`) is a hard error at load. Symmetric for `Stage` scope. The only legal layout for non-chronological scopes is the reserved `none`. |
| **Phase-scope AMPL exposure** | `Phase`-scope columns are registered as `stage_col` (broadcast) in the AMPL registry — `capacity_object("X").expmod` resolves identically on every (stage, block). |
| **Phase-scope + state-variable bridge** | `expmod` keeps using `add_state_variable` exactly like today; the two registrations share the same `LPKey` and reference the same `ColIndex`. |

---

## 9. Migration & backward compatibility

| Step | Behaviour |
|---|---|
| Phase 0 (choke-point only) | Every existing call rewritten to `add_integer_variable` with `group_uid = block_uid` and per-block fan-out of size 1. LP is bit-identical. |
| Phase 1 (per-element shorthand) | `Commitment.commitment_period` aliased to `commit_period`. Old field name accepted by the JSON loader with a deprecation note. |
| Phase 2 (system layout) | New `commitment_layout` table + CLI. Default 1-block-per-group → existing inputs are bit-identical. |
| Phase 3+ | Other integer producers ported one PR at a time. |

No on-disk format changes are required for existing fixtures.

---

## 10. Open design points (deliberately deferred)

- **Scenario-conditioned layouts** (different granularity per scenario).
  Defer — user constraint says "same for all scenarios".
- **Layouts that cross stage boundaries** (e.g. weekend group spanning
  Sat + Sun of two adjacent stages). Defer — adds a state variable. Not
  needed for PLEXOS / PLP parity in the CEN cases.
- **Per-block `commit_stride` override on `Generator`** (rather than on
  the `Commitment` row). Defer — would split the source of truth.
- **Heterogeneous groups inferred from price-band changes** (PLEXOS
  *Fitted Chronological*). Defer — orthogonal to this feature; can be
  driven by a script that emits a `commitment_layout_table.csv`.
- **Pattern-based bindings** (regex / glob on element names instead of
  exact `Uid` / `Name`). Defer — explicit `Uid` listing is unambiguous
  and the CSV form already collapses well in practice.
- **Tight min-up / min-down formulation** (Knueven 2020 UT3 / Rajan–Takriti
  endpoint form). The current `commitment_lp.cpp` uses the count-based
  UT2 form, which is looser in the LP relaxation. Tightening to UT3 is
  a Phase-3 candidate; deferred so Phases 0–2 don't bundle a formulation
  rewrite with the layout infrastructure.
- **Tighter inner-block LP bound** (Morales-España 2014 inner-block
  startup-ramp correction). Phase-3 candidate; quantify the LP gap
  on real cases before committing.
- **Aggregated kink-row gating across a group** (replace K per-block
  kink rows by one aggregated row when `pmax_segs` is constant in the
  group). Phase-3 optimisation; eliminates `(K − 1) × S` non-zeros per
  group `u_G` column.
- **Per-phase L-shaped / Benders for `expmod`**. The LP relaxation of
  the integer expansion count produces valid-but-loose cuts in SDDP
  (§8 SDDP backward pass row). Worth revisiting if cascade convergence
  stalls on cases with `max_modules ≥ ~10`.
- **Reserve provision "consumer pulls" refactor** (`commitment_lp.cpp:618-632`
  ARCHITECTURAL DEBT comment). Prerequisite for Phase 5 (binary AGC
  reserves through the choke-point) — the current `CommitmentLP`-as-
  consumer pattern mutates `ReserveProvisionLP`-owned rows after they
  are built, which is fragile under reordering. Tracked separately from
  this design; cannot ship binary reserves through `IntegerVariable`
  until that refactor lands.

---

## 11. Implementation plan

Each phase is one PR. Phase N+1 depends only on Phase N landing.

### Phase 0 — `IntegerVariable` choke-point (no behavioural change)

| Item | File | Notes |
|---|---|---|
| New header | `include/gtopt/integer_variable.hpp` | Mirrors `state_variable.hpp`. Includes `IntegerDomain` and `IntegerScope` enums. |
| Registry on `SimulationLP` | `include/gtopt/simulation_lp.hpp`, `source/simulation_lp.cpp` | `integer_variables(scene_idx, phase_idx)` accessor (same shape as `state_variables`). |
| `add_integer_variable` API | `include/gtopt/system_context.hpp`, `source/system_context.cpp` | Routes through `lp.add_col`; also calls `add_ampl_variable` (per-block fan-out for Group/Block scope, `stage_col` broadcast for Stage/Phase scope). |
| Rewrite producers (no semantic change) | `source/commitment_lp.cpp` (`status`, `startup`, `shutdown` AND `hot_start` / `warm_start` / `cold_start`), `source/simple_commitment_lp.cpp`, `source/converter_lp.cpp`, `source/capacity_object_lp.cpp` | Group-scope callers pass `requested_scope = Group, layout = identity`; `capacity_object_lp.cpp` passes `requested_scope = Phase`. **All six integer columns in `commitment_lp.cpp` must be routed** — the three startup-tier columns (`commitment_lp.cpp:902-939`) are easy to miss and would trip the clang-tidy ban if left raw. |
| Pre-existing `simple_commitment` bug fix | `source/simple_commitment_lp.cpp` | Add the same `stage.is_chronological()` early return that `commitment_lp.cpp:48-51` has. Today `simple_commitment_lp.cpp:31-60` only checks element / generator activity; on a non-chronological stage it would build binary cols against a layout that has no chronological ordering guarantee. Surfaces during Phase 0 via the scope check. |
| Clang-tidy custom check | `tools/clang-tidy/IntegerColumnCheck.cpp` (new) | Bans `is_integer = true` outside `system_context.cpp`. |
| Tests | `test/source/test_integer_variable.cpp` (new) | Registry round-trip per scope; domain precedence; AMPL fan-out (Group/Block per-block, Phase/Stage broadcast); scope-mismatch rejection. |
| Bit-identical check | `test/source/test_lp_names_and_fingerprint.cpp` (extend) | Compare LP fingerprint AND the column / row registration ORDER before and after Phase 0 on `ieee_14b` and `c0`. Ordering matters because solver presolve / branching strategies use column indices; reordering would change the LP fingerprint and may shift CPLEX / HiGHS solve paths even when the math is equivalent. The choke-point must register cols in the same `(u, v, w, hot, warm, cold)` order as today, per (period, generator). |
| Risk | LOW | LP must be bit-identical; tested via `gtopt_check_lp` AND fingerprint+order check on `ieee_14b`, `c0`. |

### Phase 1 — Per-element shorthand on every UC bearer

| Item | File | Notes |
|---|---|---|
| Rename `commitment_period → commit_period` (alias) | `commitment.hpp`, JSON loader | Old field accepted with deprecation log; igtopt sync metadata updated. |
| Add `commit_period`, `commit_stride`, `commit_layout` (Name), and per-attribute `commit_layout_<attr>` to | `commitment.hpp`, `simple_commitment.hpp`, `converter.hpp` | `CapacityObject` deliberately omitted — `Phase` scope makes block-grid fields meaningless. All optional. |
| `LayoutResolver` helper | `include/gtopt/commitment_layout.hpp` + `source/commitment_layout.cpp` (new) | Given (class, element, attribute, scope, stage, blocks) → `LayoutChoice` (group spans or `none`). Implements the 5-level precedence of §6.2 with per-element fields as auto-generated Level-3/Level-4 bindings. |
| Wire resolver into the choke-point | `source/system_context.cpp` | `add_integer_variable` calls the resolver; rejects scope-mismatched bindings. |
| Tests | `test/source/test_commitment_layout.cpp` (new) | Per-element override precedence; per-attribute override; identity default; greedy hours; stride; scope-mismatch rejection; named layout (Phase 2 placeholder). |
| Risk | LOW–MEDIUM | Behaviour change scoped to elements that actually set the new fields. |

### Phase 2 — System layout table, bindings table, JSON, and (global-only) CLI

| Item | File | Notes |
|---|---|---|
| `commitment_layout_table.csv` loader | `include/gtopt/commitment_layout_table.hpp`, `source/commitment_layout_table.cpp` (new) | Same path discipline as `block_array`. |
| `commitment_bindings.csv` loader | Same files | Five-specificity-level resolution per §6.2.2. |
| `options.commitment_layout` JSON (`layouts:` + `bindings:`) | `include/gtopt/planning_options.hpp`, `scripts/igtopt/_options_meta.py` | Same shape as §6.2.1. Updates the Python sync (project memory: `project_cpp_python_option_sync`). |
| Per-attribute binding nodes | `include/gtopt/planning_options.hpp` | `by_class_attribute`, `by_element_attribute`. |
| CLI flags — global default only | `include/gtopt/main_options.hpp`, `source/main.cpp`, `scripts/run_gtopt/main.py` | `--commitment-layout PATH`, `--commitment-stride K`, `--commitment-hours H`, `--commitment-layout-name NAME`, `--commit-relax`. Each writes only Level 0 (§6.5). Mutually exclusive. |
| Phase-2 resolver integration | `source/commitment_layout.cpp` | Adds Level 0 (CLI / global default) and the bindings table at every precedence level. |
| Tests | `test/source/test_commitment_layout_table.cpp` and `test_commitment_bindings.cpp` (new); `integration_test/` shorthand sanity on `c0` | All 5 precedence levels; scope-mismatch rejection at every level; `c0` with `--commitment-stride 4` solves and matches relaxed identity within tolerance. |
| Risk | MEDIUM | New input file paths; loaders must error on shape mistakes; bindings table parser must be airtight on wildcard precedence. |

### Phase 3 — Converters

| Item | Notes |
|---|---|
| `plexos2gtopt` | Emit `commitment_layout.csv` derived from `Generator.Commit Periods`. Per-unit `commit_period` written when distinct from the system default. |
| `plp2gtopt` | If a PLP run carries non-hourly UC granularity, emit named layouts and per-element references. |
| Tests | Round-trip check: bit-identical LP when converter is told to use the existing behaviour. |
| Risk | LOW | Converters are well-tested; the data wiring is mechanical. |

### Phase 4 — Cascade & SDDP integration

| Item | Notes |
|---|---|
| `cascade_options.level_array[*].planning_options.commitment_layout` | Per-level override, same precedence stack. |
| SDDP backward-pass relaxation site uses `IntegerVariable::domain()` | Replace the grep-for-`is_integer` style audits. |
| Cut audits (`gtopt_check_lp`) | Report integer relaxation set by class/element instead of "all integer cols". |
| Tests | Cascade smoke run: L0 coarse layout, L1 finer, L2 hourly. |
| Risk | MEDIUM | Backward-pass code already handles relaxation; the change is informational. |

### Phase 5 — Future producers (Battery / Pump / Reserve binaries)

No retrofit; each new commit-bearing entity gets `commit_*` fields and
calls `add_integer_variable`. The day this PR series lands, the
choke-point is the only path to integrality.

---

## 12. Test ladder

| Tier | Scope | Acceptance |
|---|---|---|
| T0 | `IntegerVariable` registry round-trip | Domain precedence; scope round-trip (Block/Group/Stage/Phase); group→col memoisation; AMPL fan-out (per-block for Block/Group, stage broadcast for Stage/Phase). |
| T1 | Identity layout on `ieee_14b`, `c0` | Bit-identical LP fingerprint vs main. Phase-scope `expmod` still produces one col per phase. |
| T2 | Uniform-hours layout (4 h) on `ieee_9b` via CLI `--commitment-hours 4` | Solve, gap ≤ tolerance vs hourly baseline; `expmod` unaffected. |
| T3 | Per-element override: 1 generator with `commit_period=4`, others hourly | Verifies the Level-3 override path. |
| T4 | Per-(class, attribute) binding: `Commitment.status → 4h`, `.startup → hourly` | Verifies Level-2 binding; per-attribute layout resolution. |
| T5 | Per-(element, attribute) binding: `Commitment:G42.status → daily`, `.startup → hourly` | Verifies Level-4 wins over Level-3, Level-3 over Level-2, etc. |
| T6 | Named-layout table on `c0` | Multiple layouts in one CSV; named reference resolves per element. |
| T7 | Mixed heterogeneous groups (hourly weekday, 4h weekend) | Min-up/down windows still respected. |
| T8 | `fixed_status` conflict inside a group | Hard error at LP build. |
| T9 | `--commit-relax true` global + per-element override `relax=false` | Element is integer, others continuous. |
| T10 | Converter / capacity-object producers | Each still produces the expected LP after routing through the choke-point; converter binds to a chronological layout, `expmod` is layout-immune. |
| T11 | Scope-mismatch error: binding `CapacityObject.expmod → 4h` | Hard error at load with a clear diagnostic. |
| T12 | CLI surface limited to global default | `--commitment-stride` etc. only write Level 0; CLI cannot write per-class or per-element entries. |
| T13 | Cascade L0 coarse / L1 fine / L2 hourly | Each level solves; no cross-level state-variable corruption; `expmod` decisions are per-phase consistent. |
| T14 | clang-tidy custom check | Reintroducing `is_integer = true` outside `system_context.cpp` fails the build. |
| T15 | Phase-0 column-order preservation | `ieee_14b` + `c0`: identical LP fingerprint AND identical `(class, attribute)` ordering of columns vs main, after every producer routes through `add_integer_variable`. |
| T16 | Intra-group `pmin` split | 4 hourly blocks with per-block `pmin = [0, 0, 150, 150]` and `commit_period = 4h`: layout resolver splits the group at block 2 → effective layout is `[2h, 2h]` not `[4h]`; INFO log emitted. |
| T17 | `fixed_status` four-case rule | Per-block `fixed_status = [1, 1, 1, 0]` in a single 4-block group → hard error (case c). Per-block `[1, 1, 1, 1]` → group pinned to 1 (case b). All nullopt → group free (case a). `[1, null, null, 1]` → group pinned to 1, INFO log (case d). |
| T18 | Min-up/down approximation bound | Generator with `min_down_time = 25 h` in 24 h-group layout: enforces 24 h of rest (one full group), not 25 h. Test asserts the bound. |
| T19 | Existing test: per-block UC stamping under `commitment_period` | `test/source/test_user_constraint_commitment_period.cpp` — K identical UC rows, each with coefficient 1.0 on the shared `u_G`, NOT one row with coefficient K. Regression pin against accidental reintroduction of K·coef accumulation during the choke-point refactor. |
| T20 | Reserve provision deferred | Sanity: any attempt to register a `ReserveProvision` integer column via `add_integer_variable` at Phase 0 builds and runs (no clang-tidy violation), but the `commitment_lp.cpp:618-632` "consumer pulls" path stays intact until that refactor lands separately. Tracked rather than tested. |

---

## 13. Risks & mitigations

| Risk | Mitigation |
|---|---|
| LP fingerprint drift in Phase 0 (identity layout) | T1 compares fingerprints; T15 also pins column registration ORDER. |
| Min-up/down semantics on heterogeneous groups | Implement and test the cumulative-duration formulation explicitly (T5, T18). Tightness is UT2 (count form), not UT3 — documented at §4 LP-relaxation note. |
| AMPL resolution at sub-group blocks reports wrong "physical resolution" | Document explicitly: `status` is **group-valued**; per-block lookup returns the same value for every block in the group. Per-row stamping is one-stamp-per-row (verified by `test/source/test_user_constraint_commitment_period.cpp`, 28 assertions); presolve collapses the K identical rows. |
| Converter / `expmod` producers don't fit the per-block fan-out cleanly | Phase 0 lets each producer pass `requested_scope = {Block, Group, Stage, Phase}` and the registry handles the AMPL exposure shape per scope. |
| Layout table validation noisy in real cases | Loader emits one consolidated error per stage with the full gap/overlap diagnostic. |
| Feature creep — scenario-conditioned layouts, cross-stage groups | Explicitly out of scope (§10). |
| Intra-group `pmin` step changes produce LP-relaxation distortion | Layout resolver splits groups at pmin step-change points (T16) — splits emit an INFO log so users see the effective granularity. |
| Implementer attempts a cross-stage `u_G` column link under mismatched layouts | §8 stage-boundary row pins this: `prev_ucol` does not survive the stage boundary; chronology is mediated by the scalar `initial_status` channel. |
| Startup-tier columns (`y_hot`/`y_warm`/`y_cold`) accidentally remain raw `lp.add_col` | §7 lists all six commitment integer columns; T14 (clang-tidy) blocks any unrouted column. The Phase-0 producer rewrite must touch all three tier add-sites at `commitment_lp.cpp:902-939`. |
| SDDP LB stalls under `expmod` integer relaxation in cascades with large `max_modules` | Documented in §8 SDDP row + §10 deferred (per-phase L-shaped). Mitigate operationally by using `max_modules ≤ 10` or by switching the offending phase to integer-fixed feedforward; revisit if real cases stall. |

---

## 14. Glossary

- **Block** — atomic continuous time unit (`Block.duration` hours).
  Carries dispatch, flow, SoC, reserves.
- **Group** — contiguous run of blocks within a stage that share one
  integer variable.
- **Layout** — a (stage → list of groups) map, possibly named.
- **Binding** — a tuple `(class, element, attribute) → layout` that
  picks the layout for an integer variable. Stacks in five
  specificity levels; most specific wins.
- **Scope** — whether an integer column lives on the block grid
  (`Block`, `Group`) or is layout-immune (`Stage`, `Phase`). Set at
  the call site, not by bindings.
- **`commitment_layout`** — the JSON / CSV input that defines named
  layouts; the CLI flag that sets the global Level-0 default.
- **`commitment_bindings`** — the CSV / JSON input that maps
  (class, element, attribute) tuples to layouts.
- **`commit_period` / `commit_stride` / `commit_layout` /
  `commit_layout_<attr>`** — per-element shorthand fields that
  auto-generate Level-3 / Level-4 bindings.
- **`IntegerVariable`** — runtime registry entry for one integer LP
  column shared across the blocks of one group, stage, or phase.
  Mirrors `StateVariable`.
- **`add_integer_variable`** — the only place in gtopt that adds an
  integer column to a solver. Enforced by clang-tidy.

---

## 15. References

- Garver, L. L. (1962). *Power Generation Scheduling by Integer
  Programming — Development of Theory.* IEEE Trans. Power Apparatus
  and Systems. (Origin of the count-based UT2 form used today.)
- Rajan, D., Takriti, S. (2005). *Minimum Up/Down Polytopes of the Unit
  Commitment Problem with Start-Up Costs.* IBM Research Report. (UT3 /
  endpoint form — LP-tight, deferred to Phase 3.)
- Morales-España, G., Latorre, J. M., Ramos, A. (2013). *Tight and
  Compact MILP Formulation for the Thermal Unit Commitment Problem.*
  IEEE TPS 28(4).
- Morales-España, G. (2014). *Tight and Compact MILP Formulation for
  the Thermal Unit Commitment Problem Considering the Ramp-Up and
  Ramp-Down Limits.* IEEE TPS — relevant for inner-block bound
  tightening (§10 deferred).
- Knueven, B., Ostrowski, J., Watson, J.-P. (2020). *On MIP Formulations
  for the Unit Commitment Problem.* INFORMS J. Comput. 32(4). (Survey
  of UT1–UT5 formulations; gtopt's existing code is UT2.)
- Hua, B., Schiro, D. A., Zheng, T., Baldick, R., Litvinov, E. (2018).
  *Pricing in Multi-Interval Real-Time Markets.* IEEE TPS 34(4).
- Palmintier, B. S. (2014). *Flexibility in generation planning:
  Identifying key operating constraints.* PSCC. (§3.2 — group-duration
  rounding error on min-up/down constraints.)
- Energy Exemplar. *PLEXOS Documentation* — `Generator.Commit Periods`,
  `Periods per Day`, *Fitted Chronological*, LT/MT/ST phase chain.
- NREL Sienna / PowerSimulations.jl docs, *Feedforwards and
  Multi-Stage Problems* (orthogonal to layout coarsening; §3 caveat).
- GenX docs — *Operational subperiod weighting* (representative-day
  clustering; §3 caveat).

### Internal artefacts referenced

- `test/source/test_user_constraint_commitment_period.cpp` — regression
  pin for the per-block UC stamping behaviour under `commitment_period`
  (28 assertions; rebuts the K·coef-accumulation reading of §A.1/§A.4).
- `source/commitment_lp.cpp:134-171,202-203,240,307-313,556-575,710-834,
  836-1047,1075-1095` — anchor points for the changes in Phase 0 /
  Phase 1.
- `source/user_constraint_lp.cpp:796-827` — per-block UC row builder.
- `source/element_column_resolver.cpp:226-309` — `stamp_ref` and the
  single-vs-multi-col resolution path.
- `include/gtopt/state_variable.hpp` — pattern that `IntegerVariable`
  mirrors.
- `include/gtopt/ampl_variable.hpp` — AMPL registry surface that
  `add_integer_variable` auto-populates.
