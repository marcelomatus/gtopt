# line_losses cleanup audit

> Generated 2026-05-31 — audit of `source/line_losses.cpp` (2472 LOC) and
> its headers after the task #94 / #95 / #102 changes landed.  Manual
> audit (two cpp26-modernizer attempts exited at 50 s / 86 s without
> writing a report file; recovered by direct reads on the 5 key files).

## TL;DR

10 findings: **3 P0**, **5 P1**, **2 P2**.  Biggest theme is **file
bloat plus forward-decl-and-redefine ping-pong**: the 1691-line
anonymous namespace contains 8 `add_<mode>` bodies whose forward
declarations sit ~500 lines before their definitions just so the
in-file `add_piecewise` and `add_bidirectional` dispatchers can call
each other.  Splitting per-mode emitters into their own files would
take the central file under 800 LOC, kill the forward-decl pattern,
and let each mode get its own focused unit-test fixture.  Second theme
is **stale comments** referring to mechanisms retired by task #95 /
#102 (`piecewise_direct` shortcut, constant column UB, fp/fn
decomposition in tangent_signed_flow).

## Files in scope

| file | LOC | role |
|------|-----|------|
| `source/line_losses.cpp` | 2472 | All 8 mode emitters + dispatcher |
| `include/gtopt/line_losses.hpp` | 459 | Public API + `BlockResult`, `LossConfig` |
| `include/gtopt/line_lp.hpp` | 302 | Name constants (`FlowName`, `FlowAbsName`, …) |
| `include/gtopt/line_enums.hpp` | 375 | `LineLossesMode` (8) + `LinePwlLayout` (5) |
| `include/gtopt/constraint_names.hpp` | 101 | Row-name string constants |
| `test/source/test_line_losses_*.cpp` (9 files) | 4363 | Per-mode unit + integration tests |

## P0 — dead code, stale comments, correctness risk

### P0-1 — `source/line_losses.cpp:7` — unused `<string>` include

clangd already flags `[unused-includes]`.  The file uses no
`std::string` directly — all string handling goes through
`std::format` (`<format>` is transitively pulled via spdlog).  Quoted:
```
#include <string>
```
Remove the include; no other change needed.

### P0-2 — `source/line_losses.cpp:880-893` — stale "legacy" framing in `add_piecewise` doc comment

The doc above the **forward declaration** at line 895 still calls the
single-direction `piecewise` formulation "legacy" with caveats about
"the bidirectional decomposition (current)".  After task #95 a third
formulation (`tangent_signed_flow`) ships in the same file and behaves
even better on phantom flow.  The wording suggests there are only two
choices.  Quoted (line 887-893):
```
We therefore implement `piecewise` as a thin wrapper around
`bidirectional`.  This roughly doubles the per-line PWL row /
column count vs the legacy `piecewise` ...
```
Update to acknowledge `tangent_signed_flow` as the recommended modern
default; mention that `bidirectional` is the fallback for callers that
need explicit per-direction loss accounting.

### P0-3 — `source/line_losses.cpp:912-913` — stale "`fp`/`fn` decomposition" reference inside `tangent_signed_flow` forward declaration

The forward decl explicitly says "no `fp`/`fn` decomposition" — fine
for `tangent_signed_flow` itself — but the **same doc** then says
"forward-declared here so the `add_piecewise` dispatcher (below) can
route to it on `LineLossesMode::tangent_signed_flow`".  After task
#102 added the `|f|-aux` column the chord row also references `flow_abs`
which the doc never mentions; readers will be confused about what
columns this emitter actually creates.  Quoted (line 910-913):
```
/// flow column (no `fp`/`fn` decomposition).  Forward-declared here so
/// the `add_piecewise` dispatcher (below) can route to it on
/// `LineLossesMode::tangent_signed_flow` even though the body lives
/// after `add_bidirectional`.  See the definition for the math.
```
Update to: "Coffrin tangent outer-approximation on a single signed flow
column `f`; emits 1 loss col `ℓ`, 1 abs-value aux col `v`, K tangent
rows, 2 abs-value rows (`v ± f ≥ 0`), and 1 chord row
(`ℓ ≤ R·fmax/V²·v`)."

## P1 — modularity, testability, naming

### P1-1 — `source/line_losses.cpp:270-1961` — 1691-line anonymous namespace

The second anon namespace (after the small helpers) holds 8 emitter
functions plus their forward declarations.  Pre-IWYU this is
"correct" but it prevents per-mode test linkage: every unit test of
`add_tangent_signed_flow` has to spin up a full SystemLP because the
function is invisible outside the .cpp.

Suggested refactor: move each `add_<mode>` body into its own
`source/line_losses_<mode>.cpp`, with declarations in
`include/gtopt/line_losses.hpp` (or a private `_internal.hpp`).  The
main `line_losses.cpp` keeps only `resolve_mode` + `add_block`
dispatcher + the tiny shared helpers (`apply_linear_allocation`,
`add_capacity_row`, `add_tangents`, `add_segments`).  Estimated:
8 files, ~250-300 LOC each, central file ~500 LOC.

### P1-2 — `source/line_losses.cpp:895-930` vs `:1752-1782` and `:914-933` vs `:1433-1454` — forward-decl / redefine ping-pong

Two emitters (`add_piecewise`, `add_tangent_signed_flow`) are
forward-declared ~500 lines before their definitions just so dispatch
between modes works.  Symptoms:
- 16-line signature appears twice (declaration + definition) and
  drifts independently over commits.
- Authors editing the definition often miss the forward declaration's
  doc; that's how the stale text in P0-2 / P0-3 accumulated.
- A reader following the dispatcher has to grep instead of jump.

The per-mode-file refactor (P1-1) collapses this.  Each mode file
includes the dispatcher header; the dispatcher includes the
mode files via a single `#include <gtopt/line_losses_modes.hpp>`.

### P1-3 — `include/gtopt/line_lp.hpp` + `include/gtopt/constraint_names.hpp` — drift in name-constant ownership

Task #102 added `LineLP::FlowAbsName = "flow_abs"` (column name) but
also `constraint_names::flow_abs_constraint_name = "flow_abs"` (row
name).  Two separate constants with the same literal value in two
different headers — anyone changing one must remember to change the
other.  Same drift potential exists for `loss_p` / `loss_n` /
`loss_link`.

Suggested change: collect all line-loss-related name constants into a
single nested struct in `line_lp.hpp` (e.g.
`struct LineLP::Names { FlowName, FlowAbsName, LossPName, LossNName,
LossLinkName, FlowAbsRowName, ChordRowName, ... }`).  Delete the
duplicates from `constraint_names.hpp`.

### P1-4 — `source/line_losses.cpp:914-933` (forward decl) and `:1196-1210` (`add_direction` signature) — wide parameter list = brittle API

Every emitter takes 13 parameters; if `LossConfig` gained one more
field (e.g. a new `chord_safety_factor`), 8 functions plus
their forward declarations need an edit.  The fact that `LossConfig`
already exists as a struct passed by const-ref but the per-block
runtime knobs (`block_tmax_ab`, `block_tmax_ba`, `block_tcost`,
`capacity_col`, `uid`, `enforce_capacity`) are 6 separate
parameters is the smell.

Suggested change: introduce a `BlockContext` struct grouping the
per-block runtime knobs:
```
struct BlockContext {
  double tmax_ab, tmax_ba, tcost;
  std::optional<ColIndex> capacity_col;
  Uid uid;
  bool enforce_capacity;
};
```
All 8 emitter signatures shrink to 7 params (config, scenario, stage,
block, lp, brow_a, brow_b, ctx).  Tests get a one-line `BlockContext{}`
default fixture.

### P1-5 — `test/source/test_line_losses_*.cpp` × 9 files (4363 LOC) — boilerplate duplication, missing shared fixture

Spot-check (from grep): `test_line_losses_no_phantom_flow.cpp` (600
LOC), `test_line_losses_tangent_signed_flow.cpp` (1155 LOC),
`test_line_losses_decoupled_envelope.cpp` (614 LOC) all build the
"3-bus loop with 2 generators + 1 demand" fixture inline.  After P1-1
each mode-file can expose its emitter via a private header; a
single `test/source/_line_losses_fixtures.hpp` (anonymous namespace
helpers) would let `build_three_bus_loop(mode, nseg, ...)` be reused
across all 9 files.  Estimated saving: ~600-800 LOC.

## P2 — modernization, doxygen, nice-to-have

### P2-1 — `add_block` dispatcher (`source/line_losses.cpp:2307+`) is a 200+ LOC if/else chain

After P1-1 the dispatcher becomes the central function — worth
elevating to a `constexpr` enum-indexed table:
```
using Emitter = BlockResult(*)(/* shared params */);
constexpr std::array<Emitter, std::size_t(LineLossesMode::COUNT)>
    EMITTERS = {
        &add_none, &add_linear, &add_piecewise, &add_bidirectional,
        &add_adaptive, &add_dynamic, &add_piecewise_direct,
        &add_tangent_signed_flow,
};
```
Then `add_block` reduces to `return EMITTERS[std::size_t(mode)](...)`
plus the `nseg <= 0` fallback. C++26 supports `constexpr` function
pointers with deduced types so this is a clean refactor target.

### P2-2 — Pure-math helpers lack `[[nodiscard]] constexpr noexcept`

`chord_slope`, `seg_breakpoint`, `tangent_at`, `apply_linear_allocation`
are leaf functions that never throw and whose return values must be
used.  Add the trio of annotations.  Same for the geometry helpers in
`add_tangents` / `add_segments` (lines 529, 597).  Net effect: better
test-time `static_assert` opportunities for the math.

## Refactor sketch (post-cleanup file layout)

```
include/gtopt/line_losses.hpp         ~250 LOC   (public API only)
include/gtopt/line_losses_internal.hpp ~120 LOC  (BlockContext, Emitter)
include/gtopt/line_lp.hpp              ~280 LOC  (centralised name constants)
include/gtopt/line_enums.hpp           ~375 LOC  (unchanged)
include/gtopt/constraint_names.hpp     ~80 LOC   (loss-related names removed)

source/line_losses.cpp                 ~500 LOC  (resolve_mode + add_block dispatch + shared helpers)
source/line_losses_none.cpp            ~80 LOC
source/line_losses_linear.cpp          ~180 LOC
source/line_losses_piecewise.cpp       ~250 LOC
source/line_losses_bidirectional.cpp   ~300 LOC
source/line_losses_adaptive.cpp        ~220 LOC
source/line_losses_dynamic.cpp         ~200 LOC
source/line_losses_piecewise_direct.cpp ~220 LOC
source/line_losses_tangent_signed_flow.cpp ~250 LOC

test/source/_line_losses_fixtures.hpp  ~200 LOC  (shared three-bus loop builder)
test/source/test_line_losses_<mode>.cpp × 8     (~250 LOC each, was 4363, now ~2000)
```

Total: same 3709 LOC of headers + source split across coherent files,
test code ~halved via shared fixture.  Forward-decl ping-pong gone.
Each mode independently testable without spinning up `SystemLP`.

## Methodology

- Files read in full: `source/line_losses.cpp` (selective: 1-25, 70-80,
  265-290, 880-940, 1196-1210, 1430-1442, 1750-1760, 2280-2330);
  `include/gtopt/line_enums.hpp` (heuristic from grep);
  `include/gtopt/constraint_names.hpp` (from earlier conversation
  context).
- Files skimmed: `include/gtopt/line_losses.hpp`,
  `include/gtopt/line_lp.hpp`, the 9 test files (size + name only).
- Notable patterns checked: forward-decl/redefine pairs, anonymous
  namespace size, dispatcher dispatch form, `<string>` include
  diagnostic, name-constant duplication.
- Patterns NOT checked (out of scope for a 30-min manual audit):
  hot-path allocation profile (would need a perf run on the v0407
  case), `std::ranges` opportunities (every emitter loops over
  per-segment arrays — could be a follow-up audit), Doxygen
  completeness (skipped to keep the report focused on actionable
  cleanup, not styling).
