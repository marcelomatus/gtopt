#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# tools/lint_strong_types.sh
#
# Forbid re-introducing the strong-type hygiene issues that the
# `cleanup(format)` / `feat(strong-index)` / `feat(sddp)` PRs eliminated.
#
# Runs a handful of regex patterns over `source/` and `include/gtopt/`.  Each
# pattern has a per-file allow-list for the handful of legitimate uses the
# review identified.
#
# Exits 0 when every pattern has either zero hits or only allow-listed hits;
# exits 1 (with a human-readable summary on stderr) otherwise.
#
# Uses POSIX `grep -E` — no ripgrep dependency, so it runs on any CI runner
# or developer machine without extra install steps.
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

# Resolve repo root so the script is location-independent.
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$REPO_ROOT"

SEARCH_PATHS=(source include/gtopt)

# Portable recursive regex search: scan every file under each path, emit
# `path:line:match` for hits.  `grep -rEn` is universal across GNU and BSD
# greps; we pass `--include='*.cpp' --include='*.hpp'` to keep it C++-only.
#
# Comment filter: grep's output is `path:line:content`.  Drop hits whose
# `content` is either
#   * a C++ single-line comment (`//…`) — filter `content` starting with `//`,
#   * a Doxygen / block comment continuation (`* …`), OR
#   * any occurrence where the match is preceded by `//` on the same line.
# This prevents the patterns below from firing on doc-block examples that
# literally mention the anti-pattern being flagged (e.g. sparse_col.hpp's
# `col_index_size` docstring referencing `ColIndex{static_cast<Index>(r.size())}`).
run_grep() {
  local pattern="$1"
  shift
  grep --include='*.cpp' --include='*.hpp' --recursive --line-number \
    --extended-regexp "$pattern" "$@" 2>/dev/null \
    | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*(//|\*([[:space:]]|$|/))' \
    || true
}

fail=0

# ── Rule 1: no redundant `static_cast<Uid>(...)` ─────────────────────────────
# Allow-list:
#   - source/inertia_provision_lp.cpp  (parses Uid from a string via std::stoi)
#   - source/reserve_provision_lp.cpp  (same)
# `UidOf<Tag>` ships with a `std::formatter` specialisation, so the cast is
# never needed when feeding a strong Uid into `std::format` / spdlog / logs.
uid_hits=$(run_grep 'static_cast<Uid>\(' "${SEARCH_PATHS[@]}" \
           | grep -vE '^source/(inertia|reserve)_provision_lp\.cpp:' \
           || true)
if [[ -n "$uid_hits" ]]; then
  echo "❌ redundant static_cast<Uid>(…) found (UidOf<Tag> is already formattable):" >&2
  echo "$uid_hits" >&2
  echo "   fix: drop the cast — std::format, spdlog, etc. handle UidOf<Tag> directly." >&2
  echo "" >&2
  fail=1
fi

# ── Rule 2: no `StrongIndex{static_cast<Index>(... .size())}` ────────────────
# Use the sized-range factories (`col_index_size`, `row_index_size`) or the
# typed accessors (`LinearInterface::numcols_as_index()` / `numrows_as_index()`)
# instead.  Caught pattern covers every strong-index family.
idx_ctor=$(run_grep \
  '(SceneIndex|PhaseIndex|StageIndex|BlockIndex|ScenarioIndex|IterationIndex|ColIndex|RowIndex)[[:space:]]*\{[[:space:]]*static_cast<Index>[[:space:]]*\([^)]*\.[[:space:]]*size[[:space:]]*\(' \
  "${SEARCH_PATHS[@]}" || true)
if [[ -n "$idx_ctor" ]]; then
  echo "❌ StrongIndex{static_cast<Index>(container.size())} found:" >&2
  echo "$idx_ctor" >&2
  echo "   fix: use col_index_size(...) / row_index_size(...) from sparse_col.hpp / sparse_row.hpp," >&2
  echo "        or li.numcols_as_index() / li.numrows_as_index() from linear_interface.hpp." >&2
  echo "" >&2
  fail=1
fi

# ── Rule 3: no raw `+ 1` / `- 1` arithmetic on positional strong indices ────
# Positional indices (scene, phase, stage, scenario, block, iteration) step
# through `next()` / `previous()` helpers so the strong type stays visible and
# the arithmetic is concept-checked.  `ColIndex` / `RowIndex` stay off the
# list — they keep full arithmetic for row/column bookkeeping.
pos_arith=$(run_grep \
  '(SceneIndex|PhaseIndex|StageIndex|BlockIndex|ScenarioIndex|IterationIndex)[[:space:]]*[+-][[:space:]]*1([^0-9]|$)' \
  "${SEARCH_PATHS[@]}" || true)
if [[ -n "$pos_arith" ]]; then
  echo "❌ raw ±1 arithmetic on a positional strong index:" >&2
  echo "$pos_arith" >&2
  echo "   fix: use next(idx) / previous(idx) from the corresponding header." >&2
  echo "" >&2
  fail=1
fi

# ── Rule 4: no redundant `static_cast<Index>(strong_index)` ─────────────────
# `StrongIndexType<Tag>` and `StrongPositionIndexType<Tag>` ship with
# `strong::implicitly_convertible_to<Index>` AND `strong::formattable`, so
# the cast is noise in every context that accepts `Index` (comparisons,
# arithmetic, `std::format`, subscripts on raw int arrays, etc.).
#
# Heuristic: flag any `static_cast<Index>(expr)` where `expr` looks like
# a strong-index variable or subscript (`idx`, `idx->second`, `scene_index`,
# etc.) rather than a raw `size_t`.  We exclude the sanctioned
# `static_cast<Index>(container.size())` pattern (Rule 2 already covers
# that) and a handful of legitimate narrowing-from-size_t call sites
# enumerated in the allow-list below.
redundant_index_cast=$(run_grep \
  'static_cast<Index>\(([a-zA-Z_][a-zA-Z0-9_]*(\.[a-zA-Z_][a-zA-Z0-9_]*|->[a-zA-Z_][a-zA-Z0-9_]*)*|.*Index\{[^}]+\})\)' \
  "${SEARCH_PATHS[@]}" \
  | grep -vE '\.size\(\)' \
  | grep -vE 'size_t|std::ssize|static_cast<Index>\(m_' \
  | grep -vE '^source/sddp_iteration\.cpp:(700|1373):.*SceneIndex[[:space:]]*\{[[:space:]]*static_cast<Index>\([a-z_]+_sz\)' \
  | grep -vE '^source/sddp_cut_store\.cpp:214:.*static_cast<Index>\(psi\.base_nrows\)' \
  | grep -vE '^include/gtopt/iteration\.hpp:[0-9]+:.*static_cast<Index>\((cur|offset)\)' \
  || true)
if [[ -n "$redundant_index_cast" ]]; then
  echo "❌ redundant static_cast<Index>(strong_index) found:" >&2
  echo "$redundant_index_cast" >&2
  echo "   fix: drop the cast — StrongIndexType / StrongPositionIndexType are" >&2
  echo "        implicitly_convertible_to<Index> AND formattable." >&2
  echo "" >&2
  fail=1
fi

# ── Rule 5: no `static_cast<size_t>(int_expr + int_lit)` ────────────────────
# Misplaced widening cast (clang-tidy `bugprone-misplaced-widening-cast`):
# the arithmetic happens in the narrow type first, then widens — so the
# cast does nothing to prevent overflow.  Cast first, then add:
#
#     static_cast<size_t>(i + 1)       // ❌ add-then-widen, cast is noise
#     static_cast<size_t>(i) + 1       // ✓ widen-then-add
misplaced_widening=$(run_grep \
  'static_cast<size_t>\([^)]*[+-][[:space:]]*[0-9]+\)' \
  "${SEARCH_PATHS[@]}" \
  | grep -vE '//\s*NOLINT' \
  || true)
if [[ -n "$misplaced_widening" ]]; then
  echo "❌ misplaced widening cast static_cast<size_t>(int_expr ± int_lit):" >&2
  echo "$misplaced_widening" >&2
  echo "   fix: cast first, arithmetic second — static_cast<size_t>(i) + 1." >&2
  echo "" >&2
  fail=1
fi

# ── Rule 6: no `sim.scenes()[…].uid()` / `sim.phases()[…].uid()` ────────────
# `SimulationLP::uid_of(SceneIndex)` / `uid_of(PhaseIndex)` accessors
# exist (see include/gtopt/simulation_lp.hpp) and mirror the free-function
# `gtopt::uid_of(IterationIndex)` idiom from <gtopt/iteration.hpp> — one
# name (`uid_of`) for every Index→Uid conversion.  Use them instead of
# the bracket-access + .uid() idiom: shorter, avoids an unbounded
# subscript, and centralises the uid-extraction convention.  Allow-list:
#   - include/gtopt/sddp_method.hpp   (uid_of(SceneIndex) delegate)
#   - include/gtopt/simulation_lp.hpp (accessor implementation)
sim_uid_chain=$(run_grep \
  '(simulation\(\)|^[[:space:]]*sim)\.(scenes|phases)\(\)\[[^]]+\]\.uid\(\)' \
  "${SEARCH_PATHS[@]}" \
  | grep -vE '//\s*NOLINT' \
  | grep -vE '^include/gtopt/(sddp_method|simulation_lp)\.hpp:' \
  || true)
if [[ -n "$sim_uid_chain" ]]; then
  echo "❌ sim.scenes()[idx].uid() / sim.phases()[idx].uid() chain:" >&2
  echo "$sim_uid_chain" >&2
  echo "   fix: use sim.uid_of(idx) (SceneIndex or PhaseIndex overload) from" >&2
  echo "        <gtopt/simulation_lp.hpp>." >&2
  echo "" >&2
  fail=1
fi

if [[ $fail -ne 0 ]]; then
  echo "lint_strong_types.sh: strong-type hygiene violations above ↑" >&2
  exit 1
fi

echo "lint_strong_types.sh: ok"
