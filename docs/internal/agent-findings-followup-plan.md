# Agent Findings — Follow-up Plan

> Status: **draft** — created 2026-04-09 to track action items from four
> parallel analysis agents (refactoring, string/allocation, strong-type
> audit, modern-C++ opportunities). The 3 *definite* strong-type bugs
> found by Agent 3 have already been fixed in commit `20a461c7`. This
> document tracks everything else.

## Guiding principles

1. **Behaviour-preserving first.** Each item lands as its own PR/commit
   with passing tests. No drive-by refactors mixed with bug fixes.
2. **Smallest reasonable diff.** Don't restructure a 1500-line file just
   because the agent flagged it as "large" — split only when there is a
   clean seam.
3. **Measure before optimising.** For hot-path items (SHA-256 loop,
   commitment block triple-`.at()`) take a baseline timing run before
   touching the code, and verify a real improvement after.
4. **One topic per PR.** Mixing string_view conversions with
   refactoring makes review impossible.

---

## Wave 1 — High-impact, low-risk (do these first)

These are the items where the cost/benefit ratio is clearly worth it
and the diffs are small enough to review confidently in one sitting.

### 1.1 String / allocation savings (Agent 2)

| # | File | Change | Risk |
|---|------|--------|------|
| W1.1.a | `source/linear_interface.cpp:358,370,1117` | `set_prob_name`, `set_log_file`, `write_lp` accept `std::string_view` | Low — internal API |
| W1.1.b | `source/lp_fingerprint.cpp:77` | `sha256(...)` accept `std::string_view` | Low |
| W1.1.c | `source/output_context.cpp:192` | Eliminate `std::string(zfmt)` temp in codec map lookup (use heterogeneous lookup or `string_view` key) | Low |

**PR plan:** one commit per file. ~30 LOC each. Include a smoke test
that exercises the renamed signature so the change is exercised in CI.

### 1.2 Modern-C++ hotspot wins (Agent 4)

| # | File | Change | Expected gain |
|---|------|--------|---------------|
| W1.2.a | `source/lp_fingerprint.cpp:110-127` | Replace `.at()` with `operator[]` in SHA-256 hot loop (bounds are loop-checked) | ~5% on fingerprint; verify with `gtopt_check_fingerprint` |
| W1.2.b | `source/commitment_lp.cpp:326-332` | Cache the triple `.at()` lookup outside the per-block loop | LP build time on commitment-heavy cases |
| W1.2.c | `source/element_column_resolver.cpp:113-232` | Collapse find+access pattern into a single `try_emplace` / `find` per site (9+ sites) | Build-time, not runtime |

**Prereq:** add a micro-benchmark for the SHA-256 path (one already
exists in `test_lp_fingerprint.cpp` — extend it with a `BENCHMARK`
section if doctest supports it, or use a one-shot timing harness).

### 1.3 Sparse-UID lookup audit (extension of strong-type work)

The 3 fixed bugs were the *clearly wrong* ones. Agent 3 also flagged
several call sites where the same pattern *might* be in play but
needs wider context to confirm. **TODO:** spawn a focused agent to
re-scan for `static_cast<size_t>(*_uid)` and verify each one is OK.

---

## Wave 2 — Bigger payoff, more work

### 2.1 Refactoring (Agent 1)

The agent ranked 15 candidates. Only the top 3 are likely to pay off
within a single sprint; the rest are watch-list items.

| Rank | File | Lines | Why split? | Suggested split |
|------|------|-------|------------|-----------------|
| 1 | `source/sddp_cut_io.cpp` | 1889 | Mixes I/O, parsing, validation, formatting | `sddp_cut_io.cpp` (orchestration) + `sddp_cut_serialize.cpp` (parquet) + `sddp_cut_validate.cpp` |
| 2 | `source/sddp_method.cpp` | 1784 | Forward + backward + iteration loop in one TU | `sddp_method.cpp` (state machine) + already-extracted `sddp_aperture.cpp` + `sddp_forward.cpp` |
| 3 | `source/linear_interface.cpp` | 1506 | Backend lifecycle, caching, sparse-row API, log routing | Pull caching into `linear_interface_cache.cpp`, log into `linear_interface_log.cpp` |

**Acceptance criteria for any split:** zero behaviour change, all
tests pass, no public-header churn. If splitting forces a public-API
change, defer.

**Watch-list (do NOT touch unless they grow further):**

- `include/gtopt/linear_interface.hpp` (1352)
- `include/gtopt/planning_options_lp.hpp` (1153)
- `include/gtopt/main_options.hpp` (971)
- `source/commitment_lp.cpp` (955)
- `include/gtopt/work_pool.hpp` (911)
- `source/constraint_parser.cpp` (826)

**Python (separate concern, lower priority):**

- `scripts/.../template_builder.py` (2346)
- `scripts/.../_tui.py` (1737)
- `scripts/gtopt_compare/main.py` (1529)
- `guiservice/app.py` (1498)

### 2.2 Map-container audit (in progress)

A separate agent is auditing every `std::map`, `std::unordered_map`,
and `gtopt::flat_map` declaration to:

- Find loop-fill patterns missing `map_reserve()`.
- Decide whether `map_reserve` should be extended to handle
  `std::unordered_map` (which has its own `.reserve()`), and whether
  the helper deserves its own header (`include/gtopt/map_reserve.hpp`)
  separated from `fmap.hpp`.

Plan goes here once that agent reports.

---

## Wave 3 — Style / cosmetic (anytime)

- `source/aperture_data_cache.cpp:175-195` — convert raw thread to
  `std::jthread`. Almost zero risk; do it during the next time the
  file is touched for another reason.

---

## Out of scope (for this round)

- Header-file warnings surfaced by `run-clang-tidy` (project policy:
  do not run clang-tidy on headers).
- Python lint/coverage churn — covered by CI.
- The Linear/CI/release pipeline — handled separately.

---

## Tracking

Each Wave-1 item will become a separate task once approved. Wave-2
items need an explicit go-ahead before any code is touched, because
the diffs are large enough that aborting halfway through is expensive.
