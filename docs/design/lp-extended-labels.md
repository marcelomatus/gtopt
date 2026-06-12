# LP Label Style: Compact vs Extended

**Status:** draft · **Owner:** marcelo · **Last updated:** 2026-06-01
**Tracking issue:** [#508](https://github.com/marcelomatus/gtopt/issues/508)
**Tracks:** LP debug ergonomics, PLEXOS / SDDP traceability

---

## 1. One-sentence summary

Add an `lp_label_style` option with values `compact` (current —
`line_overloadp_307_1_1_111`) and `extended`
(`line_overloadp_jadresic220_ii__montemina220_1_1_111`) so LP file
output, fingerprints, and solver diagnostics name columns and rows by
the element's human-readable name when the user opts in.  **All work
is deferred to the first label render**: a run that never invokes
`write_lp` (or any other labelled-output consumer) pays nothing —
no asciification, no string allocation, no cache memory, no extra
bytes on `SparseCol` / `SparseRow`.

---

## 2. Why

Today's labels embed the element `Uid` (e.g. `307`), which is fast and
collision-free but opaque when an operator is reading an LP file or
chasing a solver warning. Real workflows pass the LP through a
debugger, `gtopt_check_lp`, or a CPLEX `iisfind`; in every case the
operator has to cross-reference UID → element name via the input JSON.

When the input system has descriptive names (`Jadresic220_II→MonteMina220`,
`COAL_NUEVA_VENTANAS_GNL_C`, `Pangue`, `El Toro`), surfacing them in the
label is a small change that meaningfully accelerates the debug loop.

The change is OFF BY DEFAULT — extended labels are larger and would
change LP fingerprints, so opting in is a deliberate trade with
benefits scoped to the runs that need them.

---

## 3. Scope and non-goals

**In scope.**

- A new option `lp_label_style: "compact" | "extended"` selecting how
  `LabelMaker::format_label` renders cols and rows.
- An `asciify` sanitizer producing a strict `[A-Za-z0-9_]` subset
  from arbitrary UTF-8 element names.
- A **lazy** per-`SimulationLP` `AsciiNameCache` populated on the
  first `LabelMaker::lookup` call (which is only made when a consumer
  such as `write_lp` actually emits labels). The cache is empty until
  then — see §6.
- CLI flag, JSON option, igtopt sync, glossary, tests.

**Hard invariant.** Producers do not change. There is no
`element_name` field on `SparseCol` / `SparseRow`. Runs that never
ask for labels pay nothing from this design.

**Out of scope.**

- Shortening class-name prefixes (`generator` → `gen`). Saves 2–6 bytes
  at the cost of grep-friendliness; net negative for debug workflows
  and rejected at design time (see §13 deferred).
- Changing the segment order of the label (`<class>_<var>_<id>_<ctx>`).
  Order is fixed across both styles.
- A "fully qualified" or "ultra-compact" third style. The two-state
  enum is sized for what users actually ask for; extending is cheap
  later if a real need surfaces.

---

## 4. Concept

### 4.1 Today's label format

`LabelMaker::format_label` (`source/label_maker.cpp:52-76`) renders:

```
as_label(lowercase(class_name), variable, uid, context...)
```

producing labels like:

```
line_overloadp_307_1_1_111
              ─┬─
              variable_uid (= line.uid)
```

with `context = (scenario_uid, stage_uid, block_uid) = (1, 1, 111)`.

`as_label` (`include/gtopt/as_label.hpp`) joins arguments with `_`,
lowercases the class name, and stamps numbers via `std::format`.

### 4.2 Extended label format

The new path renders:

```
as_label(lowercase(class_name), variable, asciify(element_name), context...)
```

producing:

```
line_overloadp_jadresic220_ii__montemina220_1_1_111
              ─────────────┬───────────────
              asciify(element_name)
```

The `variable_uid` is **dropped** in extended mode — extended labels
trade UID predictability for human readability. The element name is
unique within a class (gtopt input validation guarantees this), and
the class prefix scopes the name segment, so the resulting label is
collision-free without the UID.

### 4.3 ASCIIfication rule

`asciify(std::string_view in) -> std::string`:

- Each byte `c` is kept verbatim iff `c ∈ [A-Za-z0-9_]` (the strictest
  LP-name subset that both CPLEX LP format and CoinLpIO accept).
- Every other byte (including all multi-byte UTF-8 continuations) is
  replaced by `_`.
- The output is **NOT case-folded** — `Jadresic220_II` stays mixed
  case; only the class-name prefix is lowercased (existing behaviour).
- Runs of `_` are **NOT collapsed**. The `→` in
  `Jadresic220_II→MonteMina220` is three UTF-8 bytes
  (`0xE2 0x86 0x92`), so the output is `Jadresic220_II___MonteMina220`
  with three underscores. Multi-byte separators surviving as
  multi-underscore runs is intentional: it preserves a bijective-ish
  mapping from input to output (fewer collisions on similar names).

Examples:

| Input | Output |
|---|---|
| `MyLine` | `MyLine` |
| `Jadresic220_II→MonteMina220` | `Jadresic220_II___MonteMina220` |
| `Diesel #2` | `Diesel__2` |
| `Pangue@2026` | `Pangue_2026` |
| `nodo_α` (UTF-8 2 bytes) | `nodo___` |
| `""` (empty) | `""` — falls back to UID even in extended mode |

### 4.4 Fallback rule for missing names

Some LP cols / rows are bound to elements without a user-facing name
(synthetic auxiliary columns, virtual aggregators). In extended mode
when `element_name.empty()`, the formatter falls back to the **compact
form for that single label** — i.e. uses `variable_uid`. This is
preferred to emitting an empty segment which would produce
double-underscore artifacts.

---

## 5. Architecture

### 5.1 Single-site format ownership stays single-site

`LabelMaker` is documented as "**the single place in gtopt where LP
column/row name strings are produced**"
(`include/gtopt/label_maker.hpp:8`). This design preserves that
invariant — the only change is that `format_label` now branches on
the style.

### 5.2 No new fields on `SparseCol` / `SparseRow` / their labels

**Hard requirement: no build-time cost when `write_lp` is not called.**
That means no new fields, no per-producer threading, no asciification,
no allocations at LP-build time.

The existing four-tuple on `SparseCol{Label}` and `SparseRow{Label}` —
`class_name`, `variable_name`, `variable_uid`, `context` — is
sufficient to identify the parent element. **`LabelMaker` queries the
asciified-name cache by `(class_name, variable_uid)` at render time
only**, after the user has explicitly invoked `write_lp` (or any other
labelled-output consumer). Producers do nothing new; their call sites
are byte-identical to today.

```cpp
// include/gtopt/sparse_col.hpp — UNCHANGED.
struct SparseCol {
  // ... existing fields
  std::string_view class_name {};
  std::string_view variable_name {};
  Uid              variable_uid {unknown_uid};
  LpContext        context {};
};

// SparseColLabel + SparseRow + SparseRowLabel: all unchanged.
```

**Memory cost at LP-build time: zero.** All renaming work — including
cache construction itself (§6.2) — is deferred to the first label
render. If `write_lp` is never called, no asciification ever runs.

### 5.3 `LabelMaker` carries the style and (optionally) a cache pointer

```cpp
class LabelMaker {
public:
  constexpr LabelMaker(LpNamesLevel level,
                       LpLabelStyle style = LpLabelStyle::compact,
                       const AsciiNameCache* cache = nullptr) noexcept;

  [[nodiscard]] std::string make_col_label(const SparseColLabel&) const;
  [[nodiscard]] std::string make_row_label(const SparseRowLabel&) const;

  // §6.8.5 — write into a caller-owned buffer; the only path called
  // by `write_lp` proper.  Returns a view into the appended bytes.
  std::string_view make_col_label_into(
      std::string& out, const SparseColLabel&) const;
  std::string_view make_row_label_into(
      std::string& out, const SparseRowLabel&) const;

private:
  LpNamesLevel        m_level_ {LpNamesLevel::none};
  LpLabelStyle        m_style_ {LpLabelStyle::compact};      // 1 byte
  const AsciiNameCache* m_cache_ {nullptr};                  // 8 bytes
};
```

`LabelMaker` is constructed at `write_lp` time (and other label
consumers — solver diagnostics, fingerprint debug). The cache pointer
is non-null **only** when `m_style_ == extended`; under `compact` the
field is unused and stays `nullptr`. The cache itself is owned by
`SimulationLP` (§6.2) and is built on demand on the first label
render that needs it.

### 5.4 Format branch

```cpp
// source/label_maker.cpp
[[nodiscard]] auto format_label(LpLabelStyle style,
                                const AsciiNameCache* cache,
                                std::string_view class_name,
                                std::string_view variable,
                                Uid uid,
                                const LpContext& context) -> std::string
{
  if (class_name.empty()) return {};

  // Lookup is lazy: the cache's first probe under `extended` style
  // triggers population (§6.2).  Under `compact`, `cache` is nullptr
  // and we skip the probe entirely.
  const std::string_view element_name =
      (style == LpLabelStyle::extended && cache != nullptr)
          ? cache->lookup(class_name, uid)
          : std::string_view {};

  const bool use_name = !element_name.empty();

  return std::visit(
      [&](const auto& ctx) -> std::string {
        using T = std::decay_t<decltype(ctx)>;
        if constexpr (std::same_as<T, std::monostate>) {
          return use_name
              ? as_label(lowercase(class_name), variable, element_name)
              : as_label(lowercase(class_name), variable, uid);
        } else {
          return use_name
              ? format_with_context_name(class_name, variable, element_name, ctx,
                  std::make_index_sequence<std::tuple_size_v<T>>{})
              : format_with_context     (class_name, variable, uid,          ctx,
                  std::make_index_sequence<std::tuple_size_v<T>>{});
        }
      }, context);
}
```

`cache->lookup` is the lazy entry point. The first call triggers
population (single-shot via `std::call_once` or equivalent); every
subsequent probe is a constant-time read.

---

## 6. Performance — the lazy asciified-name cache

This is the principal design decision in the doc, and it is built
around one hard invariant from the requirements:

> **Zero cost if `write_lp` is never called.**

That rules out any eager population — at `SimulationLP` construction,
at `SystemLP` construction, or in `*LP` element ctors. The cache is
**populated lazily on the first label render that needs it**, which
happens inside `LabelMaker` only when `write_lp` (or another labelled
consumer) is actually invoked.

### 6.1 Why a cache

When `write_lp` IS called, `LabelMaker` would otherwise re-`asciify`
the same element name once per col and once per row — 180K
`asciify` calls per emit on a 100K-col / 80K-row LP. Each call
allocates a fresh `std::string`. Aggregate ~50 ms of redundant work.

The cache materialises the asciified form ONCE per element, on the
first label probe, then serves every subsequent lookup as a
constant-time read into a contiguous arena. The cost amortises across
the entire emit pass.

If the user does not call `write_lp`, the cache is never built and
the entire chapter is dead code — no `asciify`, no allocation, no
memory footprint.

### 6.2 Where the cache lives — and when it gets populated

A **per-`SimulationLP`** cache (shared across all (scene, phase) cells
in cascade / SDDP — see §6.8.4 rationale).

Storage is a single contiguous arena (string bytes) + per-class flat
maps from `Uid` to view into the arena. **The struct is created empty
at `SimulationLP` construction**; population is deferred to the first
`lookup` call.

```cpp
// include/gtopt/ascii_name_cache.hpp  (new)
class AsciiNameCache {
public:
  /// Bind the cache to a simulation.  Stores a reference only; no
  /// population happens here.
  explicit AsciiNameCache(const SimulationLP& sim) noexcept;

  /// Constant-time lookup.  First call triggers population (single-
  /// shot, guarded by `std::call_once`).  Returns an empty view when
  /// (class_name, uid) is not registered or when the element name is
  /// empty.
  [[nodiscard]] std::string_view lookup(
      std::string_view class_name, Uid uid) const;

  /// True once `lookup` has been called at least once and the cache
  /// is populated.  Useful for diagnostics; not on the hot path.
  [[nodiscard]] bool is_populated() const noexcept;

private:
  void populate_() const;  // mutable population guarded by call_once

  const SimulationLP& m_sim_;
  mutable std::once_flag m_once_;
  mutable std::string m_arena_;
  mutable flat_map<std::string_view /*class_snake*/,
                   flat_map<Uid, std::string_view>> m_by_class_;
};

// include/gtopt/simulation_lp.hpp  (new member)
class SimulationLP {
  // ...
  mutable AsciiNameCache m_ascii_cache_ {*this};   // empty until first lookup
};
```

The `AsciiNameCache` object lives on `SimulationLP` but allocates
exactly **zero bytes of heap memory** until `lookup` fires the first
time. Both `m_arena_` and `m_by_class_` default-construct as empty
containers — no allocation. Even the `std::once_flag` is just a small
atomic word.

`populate_()` performs a two-pass walk of `SimulationLP`'s element
collections:

```cpp
void AsciiNameCache::populate_() const {
  // Pass 1: total byte budget — sum of ASCIIfied name lengths.
  std::size_t total_bytes = 0;
  m_sim_.for_each_lp_element(
      [&](LPClassName, Uid, std::string_view name) {
        total_bytes += name.size();  // ASCIIfication never enlarges
      });
  m_arena_.reserve(total_bytes);

  // Pass 2: ASCIIfy each name in place; record the view.
  m_sim_.for_each_lp_element(
      [&](LPClassName cn, Uid uid, std::string_view name) {
        if (name.empty()) return;
        const auto off = m_arena_.size();
        asciify_into(m_arena_, name);                        // §6.8.4
        const auto view = std::string_view{m_arena_.data() + off,
                                           m_arena_.size() - off};
        m_by_class_[cn.snake_case()].emplace(uid, view);
      });
}
```

Pass 1 secures the exact arena capacity, so pass 2's writes never
reallocate. Every recorded view stays valid for the lifetime of the
`SimulationLP`.

The legacy "naïve" form (rejected at design time):

```cpp
// REJECTED — eager build at SimulationLP ctor, plus per-call-site
// element_name field threading.  Pays cost even when write_lp is
// never invoked.  See §6.7 alternatives for the full rationale.
```

### 6.3 No producer-side threading; render-time lookup only

Compared to the previous draft of this doc, **producers do not change
at all**. There is no `.element_name = ...` line at any of the ~120
`SparseCol { … }` / `SparseRow { … }` call sites across `*_lp.cpp`.

The label-rendering path (only fired by `write_lp` and other label
consumers) calls into `LabelMaker::format_label`, which in turn calls
`cache->lookup(class_name, uid)`. The first such call across the
program's lifetime triggers `populate_()`; subsequent calls are O(1).

Sequence under `write_lp`:

```
write_lp()                                 // user opts in to label emit
  ├── construct LabelMaker(level, style, &sim.ascii_cache_)
  └── for each label row:
        make_col_label_into(out, label)
          └── format_label(style, cache, class, var, uid, ctx)
                ├── cache->lookup(class, uid)
                │     └── std::call_once { populate_() }   // FIRST CALL ONLY
                │     └── return view into arena
                └── as_label_into(out, ...)                // §6.8.5
```

When `write_lp` is never invoked the entire branch beneath
`LabelMaker` stays cold and the cache stays empty — `m_arena_.size()`
is `0`, `m_by_class_` is an empty `flat_map`. The only resident cost
is the `AsciiNameCache` member's empty-container footprint (~64 bytes
across two empty containers plus the `once_flag`).

### 6.4 Memory cost of the cache

For a typical CEN PCP case:

- ~3,000 elements total across all classes
- average name length 20 bytes → ~60 KB of string storage
- flat_map overhead ~32 bytes/entry → ~96 KB total

≪ 1 MB. Negligible.

### 6.5 What if `write_lp` is never called?

**Zero cost.** Because population is lazy and gated by
`AsciiNameCache::lookup`, the cache stays at construction-default
state (empty containers, unset `once_flag`) for the lifetime of any
run that doesn't emit labelled output.

This covers the dominant use case: the SDDP / cascade solver loop
that reads and writes Parquet solution / dual streams via
`add_to_output`, never touches `write_lp`. Such runs pay the
construction-default cost of `AsciiNameCache` (~64 bytes per
`SimulationLP`, one-time) and nothing else from this design.

It also covers `LpNamesLevel::none` (the default), which short-circuits
even further: `LabelMaker::make_*_label` early-returns an empty string
on `m_level_ == none` without consulting the cache at all, so
`lookup` is never called and `populate_()` never runs.

Conditions under which `populate_()` DOES run (and the cache pays its
~96 KB resident cost):

- `LpNamesLevel::all` AND `LpLabelStyle::extended` AND at least one
  consumer invokes `LabelMaker::make_*_label` — typically
  `write_lp`, but also `lp_debug` printing and a few solver-diagnostic
  paths in `linear_problem.cpp`.

The `LpLabelStyle::compact` path never consults the cache regardless
of `lp_names_level`; `LabelMaker::m_cache_` is `nullptr` in that
configuration.

### 6.6 Performance summary

| Path | Per-`SimulationLP` cost | When |
|---|---|---|
| `AsciiNameCache` ctor | ~64 bytes resident, no allocation | Always, unconditional |
| LP build (no `write_lp` call) | **0 — no asciification, no allocation** | Whenever labelled output is not requested |
| First label render | O(N_elements × name_length) ≈ 1 ms + ~96 KB arena | Once per run, only when `write_lp` is invoked under extended style |
| Subsequent label probes | 1 nested flat_map find ≈ 100 ns | Hot path inside `write_lp` |
| `LabelMaker::make_*_label_into` reuses caller buffer | 0 heap traffic per label | Hot path inside `write_lp` |

The cache flips the per-label cost from "asciify + allocate" to
"flat_map find into a reused buffer", while paying its full cost
exactly once and only when the user has asked for labelled output.

### 6.7 Alternative caches considered (and rejected)

- **Eager population at `SimulationLP` ctor** (the previous draft of
  this doc) — populated the cache up-front so per-label lookup was
  pure read. **Rejected** under the hard requirement: any user that
  never calls `write_lp` would still pay the cache-build cost. The
  lazy path is identical in steady state once `write_lp` runs, and
  is zero before that.
- **Per-LinearProblem cache** — `LinearProblem` lives below
  `SystemLP` and is rebuilt per (scene, phase) under SDDP. The cache
  would rebuild N_scenes × N_phases times instead of once per
  `SimulationLP`. Rejected.
- **Global thread-local cache** — needs synchronization across scenes
  in cascade / SDDP and complicates lifetime. Element-name
  asciification is deterministic and side-effect-free; no benefit to
  global sharing. Rejected.
- **`element_name` field on `SparseCol` / `SparseRow`** (also previous
  draft) — added a 16-byte view per col/row populated by producers at
  LP-build time. **Rejected**: violates the zero-build-time-cost
  requirement. The lookup is moved into `LabelMaker::format_label`
  instead (§5.4), keeping `SparseCol` byte-identical to today.
- **Per-`*LP` cached view** (also previous draft) — each `*LP` ctor
  did one cache probe and stored the result as a member. **Rejected**:
  this performed asciification work at LP-build time even when
  `write_lp` would never be called. The lazy render-time lookup is
  the correct choice for the stated requirement.
- **Storing asciified names directly on each LP element struct** as a
  `std::string` member — bloats every LP element by 24-32 bytes per
  element even in the no-names case. Rejected.

### 6.8 Performance refinements (concrete optimisations)

§6.1–§6.7 establishes the **lazy-cache contract**; this subsection
specifies the implementation choices that minimise the work done
once the first `lookup` fires. Each refinement is independent and
can land separately.

#### 6.8.1 Cache structure: nested `flat_map`, not `flat_map<pair>`

The cache is keyed by `(class_snake, uid)`. Two representations:

- `flat_map<std::pair<string_view, Uid>, std::string_view>` — pays a
  pair-compare cost per probe (the `string_view` compare alone is
  O(class-name length)).
- `flat_map<string_view, flat_map<Uid, string_view>>` — two-level:
  one class probe, then one `Uid` probe. The class probe dominates
  for the first few cols, but the `flat_map<Uid, …>` reference is
  small enough to keep in an LP-emit-local variable, amortising the
  outer probe across every element in the same class.

The nested form is the design's choice. It also matches how labels
are emitted — `write_lp` typically streams cols then rows, with
runs of same-class entries — so the outer probe can be hoisted
trivially in user code.

#### 6.8.2 String arena: one buffer, not N small strings

`flat_map<Uid, string_view>` values point into a single `std::string`
arena owned by the `AsciiNameCache`. The two-pass populate (§6.2)
reserves the exact byte budget up-front, so no reallocation
invalidates the views. Net win: 3,000 small allocations collapse to
one reservation; the arena is contiguous and L2-hot for the entire
emit pass.

If the arena estimate were ever exceeded, every view would dangle —
the two-pass approach is non-negotiable. Pass 1 sums name lengths;
pass 2 reserves + writes. Both passes finish in sub-millisecond on
typical CEN-scale element counts.

#### 6.8.3 Branchless LUT `asciify`

The naïve byte loop branches once per character. A 256-byte LUT is
branchless and SIMD-friendly. Compiler usually vectorises the
transform via SSE2.

```cpp
// include/gtopt/as_label.hpp — header-inline
namespace detail {
inline constexpr auto k_label_allowed_lut = []() consteval {
  std::array<char, 256> t {};
  for (int c = 0; c < 256; ++c) {
    t[static_cast<std::size_t>(c)] =
        (c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
         c == '_'                ? static_cast<char>(c) : '_';
  }
  return t;
}();
}  // namespace detail

inline void asciify_into(std::string& out, std::string_view in) {
  const auto pre = out.size();
  out.resize(pre + in.size());
  std::ranges::transform(in, out.begin() + pre, [](unsigned char c) {
    return detail::k_label_allowed_lut[c];
  });
}

[[nodiscard]] inline std::string asciify(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  asciify_into(out, in);
  return out;
}
```

The `consteval` LUT is a 256-byte program-lifetime constant; the
runtime path is two memory loads + a store per char, no branches.
Worth doing only because it composes cleanly with `asciify_into` for
arena writes (§6.8.2).

#### 6.8.4 `format_label_into(string& out, ...)` for buffer reuse

`as_label` already has an `as_label_into` partner that writes into a
caller-owned buffer (search `include/gtopt/as_label.hpp:357-420`).
Extend `LabelMaker` symmetrically:

```cpp
// include/gtopt/label_maker.hpp
class LabelMaker {
public:
  // Existing — returns a fresh string per call.
  [[nodiscard]] std::string make_col_label(const SparseColLabel&) const;
  [[nodiscard]] std::string make_row_label(const SparseRowLabel&) const;

  // NEW — write into a reusable buffer the caller owns.  Returns the
  // string_view spanning the appended bytes so the caller can flush it
  // (e.g. fwrite) without copying.  Buffer is cleared on entry.
  std::string_view make_col_label_into(
      std::string& out, const SparseColLabel&) const;
  std::string_view make_row_label_into(
      std::string& out, const SparseRowLabel&) const;
};
```

The LP-file writer (write_lp) emits 100K-300K labels in a tight loop.
With buffer reuse, the writer reserves a single `std::string` once and
all labels reuse its capacity — zero per-label allocation.

**Net win** under `write_lp` on a 200K-row LP: ~3 MB heap traffic
eliminated, ~10-20 ms walltime saved (back-of-envelope; actual win
depends on libc allocator).

#### 6.8.5 Why lazy is correct, not "simple" eager

The first draft of this doc rejected the lazy-cache form on the
grounds that the "did we populate yet?" branch added hot-path cost.
That was wrong on two counts:

1. The branch is checked once at `std::call_once` time per cache
   instance, not per probe. After the first call, subsequent
   `lookup`s are pure reads — `std::call_once` is implemented as a
   single relaxed-atomic load on the fast path.
2. The eager form pays cache-build cost even when no consumer ever
   asks for labels. Real workloads do this routinely (SDDP solving
   end-to-end, writing Parquet, never invoking `write_lp`).

The lazy form is the design's load-bearing choice: it makes the
"zero cost when `write_lp` is not called" requirement structural
rather than a configuration toggle.

### 6.9 Performance summary (combined)

| Path | Cost when applicable |
|---|---|
| `AsciiNameCache` ctor at every `SimulationLP` | ~64 bytes empty containers + `std::once_flag` |
| LP-build hot path (every run, every cell) | **0 — no asciification, no allocation, no cache reference** |
| `LabelMaker::make_*_label` under `LpNamesLevel::none` | Empty-string early return; cache untouched |
| `LabelMaker::make_*_label` under `LpLabelStyle::compact` | Unchanged from today; cache pointer is nullptr |
| First `lookup` under `extended` style | ~1 ms + ~96 KB arena (one-time across the entire run) |
| Subsequent `lookup`s | One nested-flat_map probe ≈ 100 ns |
| Per-label render under `extended` + `make_*_label_into` | One probe + arena-write into caller's reused buffer |
| Cache memory under cascade / SDDP | 1 × 96 KB shared across all cells (§6.2) |

**Bottom line.** A run that never calls `write_lp` pays exactly the
`AsciiNameCache` member-default cost — two empty containers and one
`std::once_flag` — and nothing else from this design. A run that does
call `write_lp` pays asciification + arena population exactly once,
then renders ~200K labels via constant-time probes into a reused
buffer. Estimated 30-50 ms saved on a typical 100K-col `write_lp`
emit relative to the per-label-asciify baseline (≈ 5–10 % of
`write_lp` time on master).

---

## 7. Configuration surface

### 7.1 Enum

```cpp
// include/gtopt/lp_matrix_enums.hpp
enum class LpLabelStyle : std::uint8_t {
  compact  = 0,    // current behaviour — UID in the id segment
  extended = 1,    // asciified element name in the id segment
};
```

### 7.2 JSON

```yaml
options:
  model_options:
    lp_label_style: "extended"  # default "compact"
```

### 7.3 CLI

```
--lp-label-style compact|extended
```

Default `compact`. The flag is meaningful only when `lp_names_level`
is something other than `none`; we WARN-and-continue if the user
combines `--lp-names none --lp-label-style extended` (the style is
ignored because no labels are produced).

### 7.4 Precedence (lowest → highest)

1. Built-in default `compact`.
2. `options.model_options.lp_label_style` from the input JSON.
3. CLI flag `--lp-label-style`.

Matches the existing precedence for `lp_names_level`.

---

## 8. Producer call-site work — none

Producers do not change. The ~120 `SparseCol { … }` / `SparseRow { … }`
construction sites across `source/*_lp.cpp` stay byte-identical to
today. The label-extension work is entirely render-time, scoped to
`LabelMaker` and the lazy `AsciiNameCache` on `SimulationLP`.

This is the load-bearing consequence of the "zero build-time cost"
requirement: any producer-side change would pay per-row work at LP
construction regardless of whether `write_lp` is ever called. By
moving the lookup into `LabelMaker::format_label` (§5.4) and the
population behind `std::call_once` on `AsciiNameCache::lookup`
(§6.2), build-time stays free.

The previous draft of this doc had a Phase 2 dedicated to migrating
those ~120 sites. That phase is **dropped** under the lazy design.

---

## 9. Test plan

| T | Scope | Acceptance |
|---|---|---|
| T0 | `asciify` round-trip per byte class | All `[A-Za-z0-9_]` pass through; all other bytes → `_`; empty in → empty out; UTF-8 multi-byte → run of `_`. |
| T1 | `asciify` worked examples | `Jadresic220_II→MonteMina220` → `Jadresic220_II___MonteMina220`; `Diesel #2` → `Diesel__2`; `Pangue@2026` → `Pangue_2026`. |
| T2 | Cache population skipped in no-names case | Construct `SystemLP` with `LpMatrixOptions{}` all-false; assert `ascii_name(any)` returns empty. |
| T3 | Cache population eager in named case | Naming flags on → `ascii_name(generator, uid_of_gen1)` returns the asciified form, found via O(1)-ish flat_map. |
| T4 | `LabelMaker::make_col_label` compact == today | Build a small system, render with `LpLabelStyle::compact`, compare against a frozen string set. Identical to master fingerprint test. |
| T5 | `LabelMaker::make_col_label` extended emits names | Same system, `LpLabelStyle::extended`, all col labels contain the asciified name segment. |
| T6 | Fallback when element_name is empty | Synthetic aux col (no parent element name) under `extended` style renders the compact form for that single label. |
| T7 | Bit-identical LP under compact (regression) | All `*fingerprint*` tests pass with `lp_label_style=compact`. |
| T8 | Long-name truncation safety | Generator name 300 bytes long, extended style: asserts label length ≤ 250 chars and ends with a 12-char stable hash suffix (`__<hex>`) if truncation triggered. |
| T9 | CLI + JSON precedence | CLI overrides JSON, JSON overrides default; combined with `lp_names_level=none` emits the WARN and skips. |
| T10 | Memory cost ceiling | 5,000-element synthetic system: assert cache size ≤ 200 KB. |
| T11 | Same-name-different-class disambiguation | Two elements both named "X" in different classes: extended labels carry class prefix; no collision. |

---

## 10. Migration & backward compatibility

- `lp_label_style` defaults to `compact` → all existing inputs produce
  bit-identical LP files.
- `SparseCol` / `SparseRow` are unchanged — no field additions, no
  ABI shift, no producer code changes anywhere in `source/*_lp.cpp`.
- `LabelMaker`'s ctor gains an optional `AsciiNameCache*` parameter
  with a `nullptr` default — every existing construction site keeps
  compiling and renders the compact form (cache pointer unused).
- Fingerprint tests stay green throughout — the rendered LP under
  `compact` is unchanged byte-for-byte.
- `AsciiNameCache` is a new mutable member on `SimulationLP` —
  ~64 bytes resident, zero heap allocation at ctor time.  This is
  the only non-zero impact on no-label-runs and is unavoidable
  unless we make the cache `std::unique_ptr<AsciiNameCache>`
  (saves 56 bytes, costs a heap allocation on the first `lookup`).
  Worth the trade-off for runs that don't touch labels — Phase 1
  ships the unique_ptr variant.

---

## 11. Implementation plan

Each phase is one PR. Phase N+1 depends only on Phase N landing.

### Phase 0 — `asciify` + `LpLabelStyle` enum + cache scaffolding

| Item | File | Notes |
|---|---|---|
| `asciify(string_view) -> std::string` + `asciify_into(string&, string_view)` with branchless LUT (§6.8.5) | `include/gtopt/as_label.hpp` (header-inline) | 25 LoC + unit tests. |
| `LpLabelStyle` enum | `include/gtopt/lp_matrix_enums.hpp` | 6 LoC. |
| `LabelMaker` second field + branched render + `make_*_label_into` overloads (§6.8.6) | `include/gtopt/label_maker.hpp`, `source/label_maker.cpp` | 60 LoC. |
| `AsciiNameCache` struct (arena + per-class index vectors per §6.8.1–§6.8.2) on `SimulationLP` (NOT `SystemLP` per §6.8.4) | `include/gtopt/simulation_lp.hpp`, `source/simulation_lp.cpp` | 50 LoC. |
| Two-pass cache population in `SimulationLP` constructor | `source/simulation_lp.cpp` | 30 LoC. |
| `ascii_name(class, element_idx)` accessor returning `string_view` | same | 10 LoC. |
| Tests | `test/source/test_asciify.cpp`, `test_label_maker_styles.cpp` | T0, T1, T2, T3, T4, T5, T10. |
| Risk | LOW | No producer changes; label output unchanged for default `compact`. |

### Phase 1 — `LabelMaker` cache-aware render path

| Item | File | Notes |
|---|---|---|
| `format_label` consults `AsciiNameCache::lookup` at render time under `extended` style | `source/label_maker.cpp` | 30 LoC for the cache-aware branch (§5.4). |
| `LabelMaker` ctor accepts an optional `AsciiNameCache*` | `include/gtopt/label_maker.hpp` | 10 LoC. |
| Wire the cache pointer at every label-consumer call site (`write_lp`, `lp_debug`, fingerprint debug) | `source/linear_problem.cpp`, `source/lp_debug.cpp` (or wherever `LabelMaker` is constructed today) | 3–4 sites × 1–2 LoC each. |
| Tests | `test/source/test_label_maker_extended_with_name.cpp` | T5, T6, T11. |
| Risk | LOW | Cache pointer is nullptr under `compact`; existing behaviour byte-identical. Producers untouched. |

### Phase 2 — Option + CLI + Python sync

| Item | File | Notes |
|---|---|---|
| `lp_label_style` on `ModelOptions` | `include/gtopt/model_options.hpp` | 5 LoC + JSON binding. |
| CLI flag in `MainOptions` + `--lp-label-style` parser | `include/gtopt/main_options.hpp`, `source/main.cpp` | 15 LoC. |
| Wire into `LabelMaker` construction sites | `lp_debug`, `lp_file`, `flatten`, etc. | 3-4 sites. |
| Python igtopt sync | `scripts/igtopt/_options_meta.py` | 2 LoC. |
| Tests | `test_lp_label_style_option.cpp`, integration on `ieee_9b` | T8, T9. |
| Risk | LOW | Default unchanged; opt-in. |

### Phase 3 — Length safety + truncation policy (optional)

| Item | File | Notes |
|---|---|---|
| `clamp_label_length(label, 250) -> string` | `as_label.hpp` | 20 LoC. |
| Stable hash suffix on truncation | same | 10 LoC. |
| `LabelMaker::make_col_label` wraps render in clamp under extended | `label_maker.cpp` | 5 LoC. |
| Tests | T8 | |
| Risk | LOW | Only fires on extremely long names; conservative. |

---

## 12. Risks & mitigations

| Risk | Mitigation |
|---|---|
| LP file size grows under `extended` | Document the trade-off. CEN PCP case estimate: ~+15 % bytes on the LP file. Solvers tolerate this; LP parsing is dwarfed by solve time. |
| Solver name-length caps (CPLEX 255 chars) | Phase 3 truncation + stable hash suffix. |
| Asciified collisions across mistakenly-similar names | gtopt's input validator already enforces unique names per class. The hash suffix under Phase 3 truncation guarantees uniqueness even in adversarial cases. |
| Fingerprint drift under `extended` | Two fingerprint streams: one for `compact` (the existing one), one for `extended` (new in Phase 2). Tests cover both; CI verifies bit-identity within each stream. |
| Cache lifetime bug (string_view dangles) | Single ownership: cache lives on `SimulationLP`, arena reserved up-front so views never relocate after `populate_()`. `LabelMaker::m_cache_` is a non-owning pointer into the SimulationLP's mutable member, lifetime is whatever owns the LabelMaker (typically a write_lp stack frame). Tested by ASan in the existing CI test build. |
| Lazy-cache thread safety | `std::call_once` guards the population pass.  Subsequent `lookup` calls are pure reads of an immutable structure. Concurrent label emission across cells in cascade / SDDP is safe by construction. |
| Lazy-cache failure mode if `populate_()` throws | `std::call_once` leaves the flag unset on exception, so a retry replays the population.  Doc the exception contract: `populate_()` is no-throw in steady state (only `std::bad_alloc` is theoretically possible).  Test T2 exercises the empty-cache path; T3 exercises the populated path. |

---

## 13. Open design points (deliberately deferred)

- **Shortening class names** (`generator` → `gen`). Saves 2–6 bytes
  per label; loses grep-friendliness on operator workflows that do
  `grep "generator_" lp_file`. Net negative. Revisit only on
  large-LP file-size complaint.
- **Hash-suffix-only ultra-compact style** (skip element name AND uid,
  emit a hash). No real demand; would break operator workflows that
  expect either UID or name in the label. Deferred indefinitely.
- **Per-class style overrides** (e.g. extended on `Line` and
  `Generator` but compact on synthetic auxiliary cols). The fallback
  rule §4.4 already handles the only case where we'd actually want
  this (anonymous cols), without per-class tuning. Deferred.
- **Case-folding the element name** (`Jadresic220_II` → `jadresic220_ii`).
  Slight ergonomic win (`grep -i`-free), but breaks bijective input →
  output for case-sensitive name systems. Deferred; users can run the
  LP file through `tr A-Z a-z` if they want.

---

## 14. Glossary

- **Compact label** — the existing format
  `class_var_uid_<scenario>_<stage>_<block>` (e.g.
  `line_overloadp_307_1_1_111`).
- **Extended label** — the proposed format
  `class_var_<asciified_name>_<scenario>_<stage>_<block>` (e.g.
  `line_overloadp_jadresic220_ii___montemina220_1_1_111`).
- **`asciify`** — sanitizer mapping any non-`[A-Za-z0-9_]` byte to
  `_`. Idempotent on already-ASCII names.
- **Asciified-name cache** — per-`SystemLP` flat_map keyed by
  `(class_name_snake, uid)`, storing the ASCIIfied form of each
  element's name. Populated once at build time when labels are
  enabled; queried by producers when constructing `SparseCol` /
  `SparseRow`.
- **`LpLabelStyle`** — new enum `compact | extended` driving
  `LabelMaker`'s render branch.

---

## 15. References

- `include/gtopt/label_maker.hpp` — single-site format ownership.
- `source/label_maker.cpp:52-76` — current `format_label` impl.
- `include/gtopt/sparse_col.hpp` / `sparse_row.hpp` — label-input
  data carriers.
- `include/gtopt/as_label.hpp` — string-concatenation primitive.
- `include/gtopt/lp_matrix_enums.hpp` — `LpNamesLevel` enum (where
  `LpLabelStyle` joins).
- CPLEX LP format spec — col/row name char-set and length caps.
- CoinLpIO validator (referenced by `feedback_unity_anon_namespace`
  and project-memory entries on `a8a0e452` / `PR #429`).
