# LP Label Style: Compact vs Extended

**Status:** draft · **Owner:** marcelo · **Last updated:** 2026-05-31
**Tracks:** LP debug ergonomics, PLEXOS / SDDP traceability

---

## 1. One-sentence summary

Add an `lp_label_style` option with values `compact` (current —
`line_overloadp_307_1_1_111`) and `extended`
(`line_overloadp_jadresic220_ii__montemina220_1_1_111`) so LP file
output, fingerprints, and solver diagnostics name columns and rows by
the element's human-readable name when the user opts in, while keeping
the existing UID-only format as the default.

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
- A per-element name slot on `SparseCol` and `SparseRow` (a
  `std::string_view` borrowed from the owning element's `Id::name`).
- A per-`SystemLP` cache that asciifies each element name ONCE at
  collection-build time and stores the result for `LabelMaker` to
  borrow (see §6 — caching is the principal performance concern and
  is treated as a first-class design decision, not an optimization
  afterthought).
- CLI flag, JSON option, igtopt sync, glossary, tests.

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

### 5.2 New data on `SparseCol`, `SparseRow`, and `SparseColLabel`

```cpp
// include/gtopt/sparse_col.hpp
struct SparseCol {
  // ... existing fields
  std::string_view class_name {};
  std::string_view variable_name {};
  Uid              variable_uid {unknown_uid};
  std::string_view element_name {};  // NEW — points into the element's
                                     // ASCIIfied name cache.  Empty
                                     // when the column has no parent
                                     // element (synthetic aux cols).
  LpContext        context {};
};

// include/gtopt/sparse_col.hpp — lightweight label-side struct that
// FlatLinearProblem / LinearInterface actually stores at flatten time.
// LabelMaker::make_col_label reads from this at write-LP time, NOT
// from the full SparseCol (which is consumed and discarded).
struct SparseColLabel {
  std::string_view class_name {};
  std::string_view variable_name {};
  Uid              variable_uid {unknown_uid};
  std::string_view element_name {};  // NEW — same source as SparseCol;
                                     // populated by the flatten step
                                     // when copying SparseCol fields.
  LpContext        context {};
};
```

Identical additions on `SparseRow` + `SparseRowLabel`. Lifetime contract:
`element_name` must point at storage with `SimulationLP`-equivalent
lifetime — the asciified-name cache (§6, hoisted to `SimulationLP` per
§6.8) provides this.

**Memory cost.** `std::string_view` is 16 bytes. For a 100K-col /
80K-row LP that's ~2.9 MB extra resident on the flat-LP label vectors.
This is paid only when labels are enabled (`LpNamesLevel::all`); under
the default `none` the label vectors are empty.

### 5.3 `LabelMaker` carries the style

```cpp
class LabelMaker {
public:
  constexpr LabelMaker(LpNamesLevel level,
                       LpLabelStyle style = LpLabelStyle::compact) noexcept;

  [[nodiscard]] std::string make_col_label(const SparseCol&) const;
  [[nodiscard]] std::string make_row_label(const SparseRow&) const;

private:
  LpNamesLevel m_level_ {LpNamesLevel::none};
  LpLabelStyle m_style_ {LpLabelStyle::compact};  // NEW — 1 byte
};
```

`LabelMaker` stays a 2-byte value type. Constructed at the same call
sites as today (`lp_debug`, `lp_file`, `flatten`).

### 5.4 Format branch

```cpp
// source/label_maker.cpp
[[nodiscard]] auto format_label(LpLabelStyle style,
                                std::string_view class_name,
                                std::string_view variable,
                                Uid uid,
                                std::string_view element_name,
                                const LpContext& context) -> std::string
{
  if (class_name.empty()) return {};

  const bool use_name = (style == LpLabelStyle::extended)
                     && !element_name.empty();

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

Two near-identical render lambdas, branching on `use_name`. Could be
collapsed to one templated helper; left as two for diff clarity.

`element_name` here is **already asciified** by the time it reaches
`LabelMaker` — `format_label` does not call `asciify`. See §6.

---

## 6. Performance — the asciified-name cache

This is the principal design decision in the doc.

### 6.1 Why a cache

`asciify` is O(n) in the element name's byte length. Element names
are short (typical 8–40 bytes), so `asciify` runs in a couple hundred
nanoseconds — but it would be called **once per `SparseCol::context`
visit by `LabelMaker`**, i.e. potentially once per col and once per
row across the LP. For a 100K-col / 80K-row LP that's 180K
`asciify` calls per label-emit pass, each one allocating a new
`std::string`. The aggregate is small (~50 ms) but completely
avoidable: every element has a constant `name`, and the asciified
form depends only on the name, not on the call site.

### 6.2 Where the cache lives

A **per-`SimulationLP`** cache (hoisted from `SystemLP` per §6.8.4 so
all (scene, phase) cells share one copy) with two-level structure:
arena of asciified bytes + per-class views indexed by element index.

```cpp
// include/gtopt/simulation_lp.hpp  (new field)
class SimulationLP {
  // ...
  struct AsciiNameCache {
    std::string arena;                                      // §6.8.2
    flat_map<std::string_view /*class_snake*/,
             std::vector<std::string_view>> by_class;       // §6.8.1
  };
  AsciiNameCache m_ascii_cache_;
};
```

The refined cache, lookup, and population implementations are spelled
out in §6.8.1, §6.8.2, and §6.8.3. The pseudocode below is the
**baseline only**, kept as a reference for the rejected naïve form:

```cpp
// REJECTED — naïve form for comparison only.  See §6.8 for the design.
using AsciiNameCache = flat_map<std::pair<std::string_view, Uid>,
                                std::string>;
```

That form was the obvious first cut; §6.8 explains why each cell of
its cost profile is improvable.

### 6.3 Threading the cached view into `SparseCol::element_name`

The producer call sites become:

```cpp
SparseCol {
  .class_name    = cname,
  .variable_name = StatusName,
  .variable_uid  = cuid,
  .element_name  = sc.ascii_name(Element::class_name, cuid),  // NEW
  .context       = ctx,
}
```

`sc.ascii_name(...)` is a hash-map lookup returning a
`std::string_view` into the cache's owned `std::string`. **Zero per-row
allocation in the LP-build hot path.**

The cache outlives every `SparseCol::element_name` view because the
cache is owned by the `SystemLP` that owns the `LinearProblem`. Same
lifetime contract that already holds for `class_name` (program-lifetime
`LPClassName::snake_case` buffers).

### 6.4 Memory cost of the cache

For a typical CEN PCP case:

- ~3,000 elements total across all classes
- average name length 20 bytes → ~60 KB of string storage
- flat_map overhead ~32 bytes/entry → ~96 KB total

≪ 1 MB. Negligible.

### 6.5 What about under `LpNamesLevel::none`?

The default `lp_names_level: none` builds zero labels, so the cache is
**only populated when** `m_level_ != LpNamesLevel::none` AT THE
`SystemLP` CONSTRUCTION TIME the matrix-options flag indicates names
will be needed. Skipping cache population in the no-names case saves
the 96 KB and the asciify pass entirely.

Guard:

```cpp
if (options.lp_matrix_options.col_with_names ||
    options.lp_matrix_options.row_with_names ||
    options.lp_matrix_options.col_with_name_map ||
    options.lp_matrix_options.row_with_name_map) {
  populate_ascii_name_cache_();
}
```

### 6.6 Performance summary

| Path | Per-LP cost | When |
|---|---|---|
| Cache build | O(N_elements × name_length) ≈ 1 ms | Once, only when names are enabled |
| Per-col `element_name` lookup | 1 flat_map find ≈ 50 ns | Hot path, ONLY when names enabled |
| `LabelMaker::format_label` | unchanged from today | Hot path |
| Default no-names case | **Zero overhead** — cache not built, lookups never made | Hot path |

The cache flips the per-col cost from "asciify + allocate" to "flat_map
find", a ~50× speedup in the labelled-build hot path while leaving the
default no-names build untouched.

### 6.7 Alternative caches considered (and rejected)

- **Per-LinearProblem cache** — `LinearProblem` lives below `SystemLP`
  in the layering and is rebuilt per (scene, phase) under SDDP. The
  cache would rebuild N_scenes × N_phases times instead of once per
  SystemLP. Rejected.
- **Global thread-local cache** — needs synchronization across scenes
  in cascade/SDDP and complicates lifetime. Element-name asciification
  is deterministic and side-effect-free; no benefit to global sharing.
  Rejected.
- **Lazy cache (populate on first lookup)** — saves 1 ms in cases that
  start labels mid-build. Adds a hot-path branch for "did we populate
  yet?" The eager path is simpler and identical in steady state.
  Rejected.
- **Storing asciified names directly on each LP element** as a member
  field — bloats every LP element by 1 `std::string` per element even
  in the no-names case.  **Partially rescued** by §6.8 below: each
  `*LP` element ctor reads the central cache ONCE and stores the
  resulting `std::string_view` (16 bytes, points into the cache). The
  16-byte overhead is paid only in named builds because the cache is
  empty otherwise — the view is empty too, costing the same.

### 6.8 Performance refinements (concrete optimisations)

§6.1–§6.7 establishes the cache contract; this subsection takes the
naïve "per-`SystemLP` `flat_map<pair<sv,Uid>,string>`" baseline and
optimises it along five dimensions. Each refinement is independent
and can land separately.

#### 6.8.1 Cache structure: vector-by-element-index, not flat_map

The naïve `flat_map<pair<sv, Uid>, std::string>` pays two costs per
lookup:

1. `std::pair<string_view, Uid>` comparison / hash on every probe (the
   `string_view` compare alone is O(class-name length) — usually 5-10
   bytes but still real).
2. `std::string` allocation per cache entry (24-32 byte SSO buffer or
   a separate heap allocation).

gtopt's `InputContext` already exposes an `element_index(SingleId)
-> Index` that maps every (class, uid) to a contiguous per-class
index used throughout the codebase (`generator_lp.cpp` etc. cache
`generator_index_` at ctor time). Reuse it:

```cpp
// include/gtopt/simulation_lp.hpp (cache stored here per §6.8.4)
class SimulationLP {
  // ... existing
  struct AsciiNameCache {
    /// One arena holds every asciified name back-to-back, NUL-terminated
    /// is not needed because views carry their own length.  Single
    /// allocation, hot in L2 for the entire label-emit pass.
    std::string arena;

    /// Per-class vector of views into `arena`.  Empty entries (no
    /// element registered at that index) carry empty views.
    flat_map<std::string_view /*class_snake*/,
             std::vector<std::string_view>> by_class;
  };
  AsciiNameCache m_ascii_cache_;
};
```

Lookup becomes:

```cpp
[[nodiscard]] std::string_view SimulationLP::ascii_name(
    LPClassName cn, Index element_idx) const noexcept {
  const auto& tbl = m_ascii_cache_.by_class.at(cn.snake_case());
  return (element_idx < tbl.size()) ? tbl[element_idx]
                                     : std::string_view {};
}
```

`flat_map<sv, vector>` is queried once per `*LP` ctor (~25 classes,
6 cells in cascade → 150 finds total instead of 600K). After that the
per-element lookup is `tbl[idx]` — a single vector indexing operation.

**Net win**: pair-key hash eliminated, single arena allocation
(typically 60-100 KB), per-row cost drops from "probe + dereference"
to a single indexed load.

#### 6.8.2 String arena: one big buffer, not N small strings

The naïve version stores `std::string` values in the cache. With a
typical 20-byte average name, libstdc++'s SSO threshold (15 bytes on
x86-64) misses for most names, triggering a separate heap allocation
per element. For 3,000 elements that's 3,000 small heap allocations
fragmenting the address space at startup.

The arena approach:

```cpp
void SimulationLP::populate_ascii_cache_() {
  m_ascii_cache_.arena.reserve(64 * 1024);  // ≈ 3000 × 20-byte names

  for_each_element([&](LPClassName cn, Index idx, std::string_view name) {
    auto& views = m_ascii_cache_.by_class[cn.snake_case()];
    if (idx >= views.size()) {
      views.resize(idx + 1);  // gtopt indices are dense per class
    }
    const auto off = m_ascii_cache_.arena.size();
    asciify_into(m_ascii_cache_.arena, name);          // appends in place
    views[idx] = std::string_view{m_ascii_cache_.arena.data() + off,
                                  m_ascii_cache_.arena.size() - off};
  });
}
```

One reservation, one growth pattern, every view points into the same
buffer.

**Lifetime invariant**: `m_ascii_cache_.arena` is reserved up-front
and never reallocated after `populate_ascii_cache_` finishes (the
reserved capacity holds the entire population). All views remain valid
for the lifetime of the `SimulationLP`. The `reserve(64 KB)` is an
upper-bound estimate; a final `shrink_to_fit` releases the unused tail
once population completes.

If the estimate is exceeded, the arena reallocates and invalidates
every view. Mitigation: a two-pass population — pass 1 computes the
exact total byte count, pass 2 reserves and writes. Two passes through
~3000 elements remains sub-millisecond.

**Net win**: 3,000 small allocations → 1 reservation. ~60-100 KB
contiguous, cache-friendly under the label-emit pass.

#### 6.8.3 Per-`*LP` cached view eliminates the cache-find hot path

§6.3 has every `SparseCol { .element_name = sc.ascii_name(class, uid) }`
call site do a `flat_map` find. With 100K-col LPs in a cascade build
that's 100K hash probes per cell × 6 cells = 600K probes.

Better: every `*LP` element looks up its name ONCE in its ctor and
stores the view as a member.

```cpp
// include/gtopt/object.hpp — common ancestor of LP elements
class LpObjectBase {
protected:
  std::string_view m_ascii_name_view_ {};
};

// e.g. include/gtopt/line_lp.hpp ctor body
LineLP::LineLP(const Line& line, const InputContext& ic)
    : Base(line, ic, Element::class_name)
    , ...
{
  // Look up the asciified name ONCE.  Empty when naming is disabled
  // (cache wasn't populated) — `format_label` then falls back to UID.
  m_ascii_name_view_ = ic.simulation().ascii_name(
      Element::class_name, element_index_);
}
```

Producer sites then read a member:

```cpp
SparseCol {
  .class_name    = cname,
  .element_name  = ascii_name_view(),  // member accessor — zero indirection
  .variable_name = LineLP::FlowpName,
  .variable_uid  = uid(),
  .context       = ctx,
}
```

**Net win**: 600K hash probes → 25 hash probes (one per LP class
ctor, six cells). The label-emit hot path becomes a 16-byte member
read — free.

#### 6.8.4 Hoist the cache to `SimulationLP`, not `SystemLP`

The doc draft put the cache on `SystemLP`. Under cascade and SDDP,
gtopt builds N_scenes × N_phases `SystemLP` instances (typically 6
in the regression test, hundreds in real cases) — each would
independently rebuild and store the same cache.

The element-name → asciified-name mapping is **scenario-invariant and
stage-invariant**: element names live on the `System` (one per
`Simulation`). The cache belongs on the same lifetime, which is
`SimulationLP`.

**Net win**:

| Cache home | 6-cell cascade | 100-cell real case |
|---|---|---|
| `SystemLP` (per cell) | 6 × 96 KB = 576 KB | 100 × 96 KB = 9.6 MB |
| `SimulationLP` (shared) | 1 × 96 KB = 96 KB | 96 KB |

Reads are lock-free (the cache is immutable after population);
parallel cell builds in cascade/SDDP simply share the view.

#### 6.8.5 Branchless LUT `asciify`

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

#### 6.8.6 `format_label_into(string& out, ...)` for buffer reuse

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

#### 6.8.7 What §6.7 dismissed and shouldn't have

- **Storing the asciified-name view on each LP element** (§6.7 last
  bullet, "Rejected"). The reasoning ("bloats Id by 1 std::string per
  element") was wrong: the *view* is 16 bytes, not a full `std::string`,
  and §6.8.3 demonstrates this is the correct design — it eliminates
  the per-row cache-find hot path. Reinstated as the recommended
  approach.

- **Lazy cache** was rejected for the "did we populate yet?" branch
  cost. That branch is one byte-flag check per `*LP` ctor (25 ctors
  per cell), not per row. Lazy population would let `LpNamesLevel`
  flip ON mid-run (e.g. only for a single problematic phase) without
  rebuilding the simulation. Minor benefit; still recommend eager
  for simplicity, but the rejection rationale was off.

### 6.9 Performance summary (revised)

| Path | Per-LP cost (original §6) | Per-LP cost (refined §6.8) |
|---|---|---|
| Cache build | O(N × name_len) ≈ 1 ms | Same, one arena alloc |
| Per-`*LP` ctor `ascii_name` find | not called per-ctor | 1 flat_map probe |
| Per-`SparseCol` `element_name` lookup | 1 flat_map probe (~50 ns × 600K = 30 ms) | 1 member read (free) |
| `format_label` per col | 1 `std::string` alloc per call | 1 capacity-borrowed write to reused buffer |
| Cache memory in cascade | N_cells × 96 KB | 1 × 96 KB |
| Default no-names case | zero overhead | zero overhead |

The refined design moves the hot-path cost from O(N_cols) hash probes
+ N_cols allocations to O(N_classes) probes + N_emits buffer writes
into a single arena. Wall-clock impact on a typical 100K-col LP-file
emit: estimated 30-50 ms saved (≈ 5–10 % of write_lp time on master).

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

## 8. Producer threading — call-site work

Every `SparseCol { … }` and `SparseRow { … }` that today reads
`.variable_uid = uid()` needs to also set
`.element_name = sc.ascii_name(Element::class_name, uid())`.

### 8.1 Estimated touch-points

Rough grep:

```bash
$ grep -rE "\.variable_uid\s*=\s*(uid\(\)|cuid)" source/*_lp.cpp | wc -l
~120
```

across ~25 producers. Each addition is one line. The work is
mechanical and trivially reviewable — a small `tools/add_element_name.py`
script could AST-transform them, but a careful hand-edit per file is
fine too.

### 8.2 The right shape for the producer-side helper

To avoid threading `LPClassName` + `Uid` at every call site we add a
helper on the producer base class (or on `SystemContext`):

```cpp
// SystemContext or each *LP base
[[nodiscard]] std::string_view element_ascii_name() const {
  return m_system_lp_->ascii_name(this->class_name(), this->uid());
}
```

so the producer site reads:

```cpp
SparseCol {
  .class_name    = cname,
  .element_name  = element_ascii_name(),
  .variable_name = StatusName,
  .variable_uid  = cuid,
  .context       = ctx,
}
```

The helper is a one-time addition per *LP class hierarchy (~6 hierarchies);
the call-site addition is then `.element_name = element_ascii_name()`,
~120 lines total.

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
- The new `element_name` field on `SparseCol` / `SparseRow` defaults
  to empty `std::string_view{}` → producers that haven't been migrated
  yet keep compiling and render the compact form (fallback rule §4.4).
- The migration of the ~120 call sites can land in one PR or be split
  by file with no observable behaviour change in between.
- Fingerprint tests stay green throughout — the rendered LP under
  `compact` is unchanged byte-for-byte.

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

### Phase 1 — Add `element_name` field + cached view on every `*LP` element

| Item | File | Notes |
|---|---|---|
| `element_name` field on `SparseCol` / `SparseRow` / `SparseColLabel` / `SparseRowLabel` | `include/gtopt/sparse_col.hpp`, `sparse_row.hpp` | 16 LoC across the four structs. |
| Flatten step copies `element_name` from `SparseCol` to `SparseColLabel` | `source/linear_problem.cpp:661-672` (existing copy of class_name / variable_name) | 6 LoC. |
| `format_label` branch reads `element_name` | `source/label_maker.cpp` | 20 LoC modifying the branch. |
| `m_ascii_name_view_` member on `LpObjectBase` (or per-LP-class base) + ctor population per §6.8.3 | `include/gtopt/object.hpp` or per-LP-class base | 10 LoC per hierarchy × 6 = 60 LoC. |
| `ascii_name_view()` accessor | same | 5 LoC per hierarchy. |
| Tests | `test/source/test_label_maker_extended_with_name.cpp` | T5, T6, T11. |
| Risk | LOW | Field is optional and defaults to empty; producers not yet migrated keep emitting compact. |

### Phase 2 — Producer call-site migration

| Item | Files | Notes |
|---|---|---|
| Add `.element_name = ascii_name_view()` at ~120 sites — a member read per §6.8.3, NOT a cache probe | `source/*_lp.cpp` | Mechanical. Can be done file-by-file as separate commits or one bulk commit. |
| Fingerprint regression check | existing T7 fingerprint suite | Stays green under `compact`. |
| Risk | LOW | Compile-time guarantee via the new field's presence at every required site (a sanity check macro could enforce); behaviour unchanged under default style. |

### Phase 3 — Option + CLI + Python sync

| Item | File | Notes |
|---|---|---|
| `lp_label_style` on `ModelOptions` | `include/gtopt/model_options.hpp` | 5 LoC + JSON binding. |
| CLI flag in `MainOptions` + `--lp-label-style` parser | `include/gtopt/main_options.hpp`, `source/main.cpp` | 15 LoC. |
| Wire into `LabelMaker` construction sites | `lp_debug`, `lp_file`, `flatten`, etc. | 3-4 sites. |
| Python igtopt sync | `scripts/igtopt/_options_meta.py` | 2 LoC. |
| Tests | `test_lp_label_style_option.cpp`, integration on `ieee_9b` | T8, T9. |
| Risk | LOW | Default unchanged; opt-in. |

### Phase 4 — Length safety + truncation policy (optional)

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
| Producer call-site threading is mechanical but easy to miss a site | Phase 2's empty-default fallback means missed sites silently fall back to compact for those labels — visible-on-inspection but not a runtime failure. Phase 3 ships a `gtopt_check_lp` warning when the option is `extended` but >5 % of labels miss `element_name`. |
| LP file size grows under `extended` | Document the trade-off. CEN PCP case estimate: ~+15 % bytes on the LP file. Solvers tolerate this; LP parsing is dwarfed by solve time. |
| Solver name-length caps (CPLEX 255 chars) | Phase 4 truncation + stable hash suffix. |
| Asciified collisions across mistakenly-similar names | gtopt's input validator already enforces unique names per class. The hash suffix under Phase 4 truncation guarantees uniqueness even in adversarial cases. |
| Fingerprint drift under `extended` | Two fingerprint streams: one for `compact` (the existing one), one for `extended` (new in Phase 3). Tests cover both; CI verifies bit-identity within each stream. |
| Cache lifetime bug (string_view dangles) | Single ownership: cache lives on `SystemLP`, `SparseCol::element_name` views into the cache's owned strings, `LabelMaker` reads `SparseCol`. Same lifetime story as `class_name`. Tested by T11 + ASan in the existing CI test build. |

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
