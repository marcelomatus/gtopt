#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
# tools/lint_strong_types.py
#
# Forbid re-introducing the strong-type hygiene issues that the
# `cleanup(format)` / `feat(strong-index)` / `feat(sddp)` PRs eliminated.
#
# Replaces `tools/lint_strong_types.sh`.  Same exit-code contract (0 = clean,
# 1 = violations + summary on stderr).  Same allow-listing approach.  The
# Python rewrite gives us:
#
#   * Multi-line context — Rule 7 detects raw `for (size_t/int X = 0; …)`
#     loops whose BODY uses `X` to reconstruct a strong index, which a
#     single-line grep can't see.
#   * Token-aware comment skipping — strips `// …` and `/* … */` correctly
#     so doc-block examples that mention an anti-pattern verbatim never
#     fire a violation.
#   * Per-rule diagnostics with a fix hint and the canonical replacement
#     pattern, printed as a code block so editors highlight the suggestion.
#   * Easier to extend: adding a new rule is one Python function, not a
#     fragile multi-line bash regex.
#
# Runs on Python ≥ 3.10 (`match` statement, `pathlib.Path.walk`).
# ─────────────────────────────────────────────────────────────────────────────

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(
    subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
)

SEARCH_PATHS = ("source", "include/gtopt")
EXTS = (".cpp", ".hpp")

# Strong-index types whose construction we monitor.  Keep in sync with
# the type aliases in `<gtopt/basic_types.hpp>` and the per-LP-class
# index typedefs.
STRONG_INDEX_RX = (
    r"(?:SceneIndex|PhaseIndex|StageIndex|BlockIndex|ScenarioIndex"
    r"|IterationIndex|ColIndex|RowIndex)"
)
POSITIONAL_INDEX_RX = (
    r"(?:SceneIndex|PhaseIndex|StageIndex|BlockIndex|ScenarioIndex"
    r"|IterationIndex)"
)


# ─── File walking + comment stripping ──────────────────────────────────────


def iter_files() -> list[Path]:
    out: list[Path] = []
    for sp in SEARCH_PATHS:
        root = REPO_ROOT / sp
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if p.is_file() and p.suffix in EXTS:
                out.append(p)
    return sorted(out)


_LINE_COMMENT_RX = re.compile(r"//.*$")
_BLOCK_COMMENT_RX = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(src: str) -> str:
    """Remove block + line comments, preserving line numbers."""

    # Block comments first (replace with same-newline-count spaces).
    def _blk_repl(m: re.Match[str]) -> str:
        return "".join(c if c == "\n" else " " for c in m.group(0))

    src = _BLOCK_COMMENT_RX.sub(_blk_repl, src)
    # Line comments — strip from `//` to end-of-line.
    return "\n".join(_LINE_COMMENT_RX.sub("", line) for line in src.split("\n"))


@dataclass
class Hit:
    path: Path
    line: int
    text: str

    def relpath(self) -> str:
        return str(self.path.relative_to(REPO_ROOT))

    def __str__(self) -> str:
        return f"{self.relpath()}:{self.line}: {self.text.rstrip()}"


@dataclass
class Rule:
    name: str
    description: str
    fix: str
    hits: list[Hit] = field(default_factory=list)


# ─── Per-rule scanners ──────────────────────────────────────────────────────


def scan_redundant_uid_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 1: no `static_cast<Uid>(…)` — UidOf<Tag> is formattable."""
    rx = re.compile(r"static_cast<Uid>\(")
    allow = {
        "source/inertia_provision_lp.cpp",
        "source/reserve_provision_lp.cpp",
    }
    for f in files:
        rel = str(f.relative_to(REPO_ROOT))
        if rel in allow:
            continue
        clean = strip_comments(f.read_text())
        for n, line in enumerate(clean.split("\n"), 1):
            if rx.search(line):
                rule.hits.append(Hit(f, n, line))


def scan_strong_index_size_ctor(rule: Rule, files: list[Path]) -> None:
    """Rule 2: no `StrongIdx{static_cast<Index>(container.size())}`."""
    rx = re.compile(
        rf"{STRONG_INDEX_RX}\s*\{{\s*static_cast<Index>\s*\([^)]*\.\s*size\s*\("
    )
    for f in files:
        clean = strip_comments(f.read_text())
        for n, line in enumerate(clean.split("\n"), 1):
            if rx.search(line):
                rule.hits.append(Hit(f, n, line))


def scan_positional_arith(rule: Rule, files: list[Path]) -> None:
    """Rule 3: no raw `±1` arithmetic on a positional strong index."""
    rx = re.compile(rf"{POSITIONAL_INDEX_RX}\s*[+-]\s*1(?:[^0-9]|$)")
    for f in files:
        clean = strip_comments(f.read_text())
        for n, line in enumerate(clean.split("\n"), 1):
            if rx.search(line):
                rule.hits.append(Hit(f, n, line))


def scan_redundant_index_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 4: no `static_cast<Index>(strong_index)`."""
    # Heuristic: cast around an identifier or `Strong{…}` constructor.
    rx = re.compile(
        r"static_cast<Index>\(("
        r"[a-zA-Z_][a-zA-Z0-9_]*(?:\.[a-zA-Z_][a-zA-Z0-9_]*"
        r"|->[a-zA-Z_][a-zA-Z0-9_]*)*"
        r"|.*Index\{[^}]+\}"
        r")\)"
    )
    # Allow-list: legitimate narrowing-from-size_t or m_-prefixed members.
    skip_substr = (".size()", "size_t", "std::ssize", "static_cast<Index>(m_")
    file_line_allow = {
        ("source/sddp_iteration.cpp", "static_cast<Index>"),
        ("source/sddp_cut_store.cpp", "static_cast<Index>(psi.base_nrows)"),
        ("include/gtopt/iteration.hpp", "static_cast<Index>(cur)"),
        ("include/gtopt/iteration.hpp", "static_cast<Index>(offset)"),
    }
    for f in files:
        rel = str(f.relative_to(REPO_ROOT))
        clean = strip_comments(f.read_text())
        for n, line in enumerate(clean.split("\n"), 1):
            if not rx.search(line):
                continue
            if any(s in line for s in skip_substr):
                continue
            if any(rel == fa and pat in line for fa, pat in file_line_allow):
                continue
            rule.hits.append(Hit(f, n, line))


def scan_misplaced_widening(rule: Rule, files: list[Path]) -> None:
    """Rule 5: `static_cast<size_t>(int_expr ± int_lit)` widens after add."""
    rx = re.compile(r"static_cast<size_t>\([^)]*[+-]\s*[0-9]+\)")
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        for n, line in enumerate(clean.split("\n"), 1):
            if rx.search(line):
                # Allow per-line NOLINT.
                raw_line = (
                    text.split("\n")[n - 1] if n - 1 < len(text.split("\n")) else ""
                )
                if "NOLINT" in raw_line:
                    continue
                rule.hits.append(Hit(f, n, line))


def scan_sim_uid_chain(rule: Rule, files: list[Path]) -> None:
    """Rule 6: no `sim.scenes()[…].uid()` / `sim.phases()[…].uid()`."""
    rx = re.compile(
        r"(?:simulation\(\)|^\s*sim)\.(?:scenes|phases)\(\)\[[^]]+\]\.uid\(\)"
    )
    allow = {
        "include/gtopt/sddp_method.hpp",
        "include/gtopt/simulation_lp.hpp",
    }
    for f in files:
        rel = str(f.relative_to(REPO_ROOT))
        if rel in allow:
            continue
        text = f.read_text()
        clean = strip_comments(text)
        for n, line in enumerate(clean.split("\n"), 1):
            if rx.search(line):
                raw_line = (
                    text.split("\n")[n - 1] if n - 1 < len(text.split("\n")) else ""
                )
                if "NOLINT" in raw_line:
                    continue
                rule.hits.append(Hit(f, n, line))


# Match `for (size_t|int|std::size_t  X = 0; X < …; ++X)` — capture X.
_FOR_LOOP_RX = re.compile(
    r"\bfor\s*\(\s*"
    r"(?:size_t|int|std::size_t|std::ptrdiff_t)\s+"
    r"([a-z][a-zA-Z0-9_]?)\s*=\s*0\s*;"
)


def scan_raw_strong_loop(rule: Rule, files: list[Path]) -> None:
    """Rule 7: raw `for (size_t X = 0; …; ++X)` whose body reconstructs a
    strong index from `X` (e.g. `RowIndex{first + static_cast<int>(X)}`).

    The single-line shape `+ static_cast<int>(X)` is also flagged on its
    own because it implies a loop counter being added to a strong index
    even when the surrounding `for (…)` header is absent (e.g. inside
    range-based loops with `enumerate(…)` that destructure into `X`).
    """
    # Single-line shape — a `+ static_cast<int>(X)` (X is 1–2 chars).
    inline_rx = re.compile(
        r"(?:\+|\{)\s*static_cast<(?:int|Index)>\(\s*[a-z]{1,2}\s*\)"
        r"|static_cast<(?:int|Index)>\([a-zA-Z_][a-zA-Z0-9_]*\)"
        r"\s*\+\s*static_cast<(?:int|Index)>\(\s*[a-z]{1,2}\s*\)"
    )
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        lines = clean.split("\n")
        raw_lines = text.split("\n")

        # First scan for inline anti-patterns (covers enumerate-based loops).
        for n, line in enumerate(lines, 1):
            if not inline_rx.search(line):
                continue
            raw = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
            if "NOLINT" in raw:
                continue
            rule.hits.append(Hit(f, n, line))

        # Then scan for `for (size_t X = …)` headers and check the
        # body for `static_cast<int|Index>(X)`.  Body is approximated by
        # the next ~30 lines; we stop early at the matching closing
        # brace (best-effort token count, not a full parser).
        for n, line in enumerate(lines, 1):
            m = _FOR_LOOP_RX.search(line)
            if not m:
                continue
            counter = m.group(1)
            cast_rx = re.compile(
                rf"static_cast<(?:int|Index)>\(\s*{re.escape(counter)}\s*\)"
            )
            depth = line.count("{") - line.count("}")
            for off in range(1, 31):
                if n + off > len(lines):
                    break
                body = lines[n + off - 1]
                if cast_rx.search(body):
                    raw_body = (
                        raw_lines[n + off - 1] if n + off - 1 < len(raw_lines) else ""
                    )
                    if "NOLINT" in raw_body:
                        continue
                    # Already inline-flagged?  Skip the duplicate.
                    if not inline_rx.search(body):
                        rule.hits.append(Hit(f, n + off, body))
                depth += body.count("{") - body.count("}")
                if depth <= 0:
                    break


# Rule 8: redundant `static_cast<int>(strong_idx)` — strong indices ship
# with `strong::implicitly_convertible_to<int>` so the cast is noise when
# the destination already takes `int`.  Heuristic: the operand must be a
# known strong-index identifier name (loop var from `enumerate<ColIndex>`,
# member-style `idx` / `index` / `_idx`, `col`/`row`/`column`).  Loop-counter
# names that are typically size_t (`i`, `j`, `k`, `n`, `*_size`, `count`)
# are excluded so we don't flag legitimate narrowing casts.

# Identifiers we treat as "definitely strong index" — drop the cast.
_STRONG_NAME_RX = re.compile(
    r"^(?:"
    r"col|row|column|"  # SparseRow/SparseCol fields, enumerate vars
    r"col_idx|row_idx|cidx|ridx|"  # explicit strong-index locals
    r"idx|index|"  # generic strong-index name
    r"[cr]|"  # loop-var pattern from `enumerate<ColIndex>` / `<RowIndex>`
    r".*Index"  # anything ending in "Index" (e.g. col_index, row_index)
    r")$"
)

# Identifiers explicitly NOT strong indices (size_t / int counters).  Even
# if they overlap with the strong list above they are excluded so we never
# false-positive on a real narrowing cast.
_NON_STRONG_NAME_RX = re.compile(
    r"^(?:"
    r"i|j|k|n|"  # generic loop counters
    r"size|count|len|"
    r".*_size|.*_count|.*_len|.*_n|"
    r"num_.*|n_.*|total.*|"
    r"nrows|ncols|"  # backend size accessor results
    r"numrows|numcols"
    r")$"
)


def scan_redundant_int_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 8: redundant `static_cast<int>(strong_index)`.

    Matches `static_cast<int>(IDENT)` where IDENT looks like a strong
    index by name.  Excludes well-known size_t/int counter names so we
    don't flip legitimate narrowing casts.

    Also flags `static_cast<int>(EXPR.get_numrows())` and
    `static_cast<int>(EXPR.get_numcols())` — both return `Index` (int)
    so the cast is a no-op.
    """
    rx_ident = re.compile(r"static_cast<int>\(\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*\)")
    # `static_cast<int>(*.get_num{rows,cols}())` — returns Index already.
    rx_method = re.compile(r"static_cast<int>\(\s*[^)]*\.get_num(?:rows|cols)\(\)\s*\)")
    skip_substr = ("// NOLINT", "/*", "@brief", "* @")
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        raw_lines = text.split("\n")
        for n, line in enumerate(clean.split("\n"), 1):
            raw_line = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
            if any(s in raw_line for s in skip_substr):
                continue
            for m in rx_ident.finditer(line):
                ident = m.group(1)
                if _NON_STRONG_NAME_RX.match(ident):
                    continue
                if not _STRONG_NAME_RX.match(ident):
                    continue
                rule.hits.append(Hit(f, n, line))
            if rx_method.search(line):
                rule.hits.append(Hit(f, n, line))


# Rule 9 — `std::format("...", static_cast<int>(strong_index))` is noise.
# `std::format("{}", X)` works directly for any formattable argument
# (UidOf<Tag>, ColIndex, RowIndex, SceneIndex, …); the cast just hides
# the strong type and adds a token-level conversion.

_FORMAT_CAST_RX = re.compile(
    r"std::format\([^)]*static_cast<(?:int|Index|long|long long|size_t|"
    r"std::int32_t|std::int64_t)>\(\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*\)"
)


def scan_format_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 9: `std::format` argument wrapped in a redundant integer cast.

    `std::format("{}", X)` accepts any formattable type — strong indices
    (ColIndex, RowIndex, SceneIndex, PhaseIndex, …), UidOf<Tag>, and
    plain integers all work without explicit conversion.  Wrapping in
    `static_cast<int>(...)` defeats the strong-type guarantee.
    """
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        raw_lines = text.split("\n")
        for n, line in enumerate(clean.split("\n"), 1):
            for m in _FORMAT_CAST_RX.finditer(line):
                ident = m.group(1)
                if _NON_STRONG_NAME_RX.match(ident):
                    continue
                # Only flag if the operand looks like a strong index, Uid,
                # or get_num*() result.  Bare loop counters (i, j, k, n)
                # still need explicit narrowing.
                if not _STRONG_NAME_RX.match(ident):
                    continue
                raw_line = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
                if "NOLINT" in raw_line:
                    continue
                rule.hits.append(Hit(f, n, line))


# Rule 10 — `spdlog::warn(std::format("...", x))` defeats lazy formatting.
# spdlog routes the format pattern + args through fmt::format ONLY after
# checking the active log level, so an info-level pattern formatted
# behind a `trace` filter pays nothing.  Wrapping the message in
# `std::format` materialises the string unconditionally before spdlog's
# level check has a chance to skip it.

_SPDLOG_FORMAT_RX = re.compile(
    r"spdlog::(?:trace|debug|info|warn|error|critical)\s*\(\s*std::format\s*\("
)


def scan_spdlog_eager_format(rule: Rule, files: list[Path]) -> None:
    """Rule 10: `spdlog::level(std::format(...))` is unconditional formatting."""
    skip_substr = ("// NOLINT", "/*", "@brief", "* @")
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        raw_lines = text.split("\n")
        for n, line in enumerate(clean.split("\n"), 1):
            if not _SPDLOG_FORMAT_RX.search(line):
                continue
            raw_line = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
            if any(s in raw_line for s in skip_substr):
                continue
            rule.hits.append(Hit(f, n, line))


# Rule 11 — `static_cast<int64_t>(*.use_count())` is redundant.
# `std::shared_ptr::use_count()` returns `long`; `long → int64_t` widens
# implicitly on every supported platform (long is 64-bit on Linux, 32-bit
# on Windows but still narrower than int64_t so widening is safe).  The
# explicit cast is portability theatre.

_USE_COUNT_CAST_RX = re.compile(
    r"static_cast<(?:std::int64_t|int64_t|long\s+long)>\(\s*"
    r"[^)]*\.use_count\(\)\s*\)"
)


def scan_redundant_use_count_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 11: redundant `static_cast<int64_t>(*.use_count())`."""
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        raw_lines = text.split("\n")
        for n, line in enumerate(clean.split("\n"), 1):
            if _USE_COUNT_CAST_RX.search(line):
                raw_line = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
                if "NOLINT" in raw_line:
                    continue
                rule.hits.append(Hit(f, n, line))


# Rule 12 — `static_cast<size_t>(<size_t-returning-call>)`.  When the
# operand is already `size_t` (e.g. `container.size()`,
# `std::ranges::size(r)`, `std::ssize` is signed so SKIP), the cast does
# nothing.  We restrict to the ".size()" / "::size()" suffix to keep the
# heuristic conservative.

_SIZE_T_REDUNDANT_RX = re.compile(
    r"static_cast<(?:size_t|std::size_t)>\(\s*"
    r"[^)]*\.size\(\)\s*\)"
)


def scan_redundant_size_t_cast(rule: Rule, files: list[Path]) -> None:
    """Rule 12: redundant `static_cast<size_t>(container.size())`.

    `std::span::size()`, `std::vector::size()`, and friends already
    return `size_type = size_t`.  The cast is noise.
    """
    for f in files:
        text = f.read_text()
        clean = strip_comments(text)
        raw_lines = text.split("\n")
        for n, line in enumerate(clean.split("\n"), 1):
            if _SIZE_T_REDUNDANT_RX.search(line):
                raw_line = raw_lines[n - 1] if n - 1 < len(raw_lines) else ""
                if "NOLINT" in raw_line:
                    continue
                rule.hits.append(Hit(f, n, line))


# Rule 13 — `std::string{string_view_var}` round-trip when the surrounding
# call accepts `std::string_view`.  Hard to detect type-wise without
# semantic info, but a conservative pattern: `std::string{IDENT}` /
# `std::string(IDENT)` immediately followed by feeding the result into a
# function that "looks" like it wants string_view.  Skip — too many
# false positives without symbol resolution.  Documented here for the
# future when we introduce a clang-AST-based linter.


# Rule 14 — `std::format("constant string {}", X)` whose constant prefix
# is repeated across many call sites could be hoisted to a `static
# constexpr std::string_view`.  Too cosmetic, skip.


# ─── Driver ────────────────────────────────────────────────────────────────


RULES: list[tuple[str, str, str, callable]] = [
    (
        "redundant static_cast<Uid>(...)",
        "UidOf<Tag> ships with a `std::formatter` specialisation, so the "
        "cast is never needed when feeding a strong Uid into `std::format` / "
        "spdlog / logs.",
        "drop the cast — std::format, spdlog, etc. handle UidOf<Tag> directly.",
        scan_redundant_uid_cast,
    ),
    (
        "StrongIndex{static_cast<Index>(container.size())}",
        "Use the sized-range factories (`col_index_size`, `row_index_size`) "
        "or the typed accessors (`LinearInterface::numcols_as_index()` / "
        "`numrows_as_index()`) instead.",
        "use col_index_size(...) / row_index_size(...) from sparse_col.hpp / "
        "sparse_row.hpp, or li.numcols_as_index() / li.numrows_as_index() "
        "from linear_interface.hpp.",
        scan_strong_index_size_ctor,
    ),
    (
        "raw ±1 arithmetic on a positional strong index",
        "Positional indices (scene, phase, stage, scenario, block, "
        "iteration) step through `next()` / `previous()` helpers so the "
        "strong type stays visible and the arithmetic is concept-checked.",
        "use next(idx) / previous(idx) from the corresponding header.",
        scan_positional_arith,
    ),
    (
        "redundant static_cast<Index>(strong_index)",
        "StrongIndexType<Tag> and StrongPositionIndexType<Tag> ship with "
        "`strong::implicitly_convertible_to<Index>` AND `strong::formattable`, "
        "so the cast is noise in every context that accepts `Index`.",
        "drop the cast — StrongIndexType / StrongPositionIndexType are "
        "implicitly_convertible_to<Index> AND formattable.",
        scan_redundant_index_cast,
    ),
    (
        "misplaced widening cast static_cast<size_t>(int_expr ± int_lit)",
        "The arithmetic happens in the narrow type first, then widens — so "
        "the cast does nothing to prevent overflow.",
        "cast first, arithmetic second — static_cast<size_t>(i) + 1.",
        scan_misplaced_widening,
    ),
    (
        "sim.scenes()[idx].uid() / sim.phases()[idx].uid() chain",
        "`SimulationLP::uid_of(SceneIndex)` / `uid_of(PhaseIndex)` accessors "
        "exist and centralise the uid-extraction convention.",
        "use sim.uid_of(idx) (SceneIndex or PhaseIndex overload) from "
        "<gtopt/simulation_lp.hpp>.",
        scan_sim_uid_chain,
    ),
    (
        "raw loop counter cast to strong index",
        "A `for (size_t X = 0; …; ++X)` (or single-line cast) using `X` to "
        "reconstruct a strong index defeats the strong-type guarantee.  "
        "Maintain a `StrongIndex` that advances with `++idx` alongside (or "
        "instead of) the size_t/int counter — the strong type stays "
        "visible in the arithmetic.",
        "auto idx = first;  for (const auto& x : range) { …; ++idx; }",
        scan_raw_strong_loop,
    ),
    (
        "redundant static_cast<int>(strong_index)",
        "ColIndex / RowIndex (and other StrongIndexType<Tag> aliases) ship "
        "with `strong::implicitly_convertible_to<int>`, so `static_cast<int>` "
        "is noise when the destination already takes `int` (backend setters, "
        "`vector<int>::push_back`, …).  Also flags `static_cast<int>(*."
        "get_numrows())` / `get_numcols()` since both already return `Index`.",
        "drop the cast — strong indices convert to int implicitly.",
        scan_redundant_int_cast,
    ),
    (
        "std::format(..., static_cast<int>(strong_index))",
        "`std::format` accepts any formattable argument: ColIndex, RowIndex, "
        "SceneIndex, PhaseIndex, UidOf<Tag>, and primitive ints all work "
        "with `{}` placeholders directly.  The cast hides the strong type "
        "and makes the formatted output a bare integer instead of the "
        "strong-typed rendering.",
        'drop the cast — std::format("{}", X) works for formattable X.',
        scan_format_cast,
    ),
    (
        "spdlog::level(std::format(...))",
        "spdlog formats lazily — it routes the format pattern + args "
        "through fmt::format ONLY after the level check passes.  Wrapping "
        "the message in `std::format` materialises the string before the "
        "level check, paying for the allocation even when the line is "
        "filtered out.  Pass the format pattern + args directly.",
        'spdlog::warn("...{}", x);  // not spdlog::warn(std::format("...{}", x))',
        scan_spdlog_eager_format,
    ),
    (
        "redundant static_cast<int64_t>(*.use_count())",
        "`std::shared_ptr::use_count()` returns `long`.  `long → int64_t` "
        "widens implicitly on every supported platform — the cast is "
        "portability theatre.  Use `auto` or drop the cast.",
        "return ptr.use_count();  // returns long; widens to int64_t implicitly.",
        scan_redundant_use_count_cast,
    ),
    (
        "redundant static_cast<size_t>(container.size())",
        "`size()` already returns `size_type = size_t` for every standard "
        "container and view.  The cast is noise.",
        "drop the cast — container.size() is already size_t.",
        scan_redundant_size_t_cast,
    ),
]


def main() -> int:
    files = iter_files()
    failed = False

    for name, desc, fix, scanner in RULES:
        rule = Rule(name=name, description=desc, fix=fix)
        scanner(rule, files)
        if rule.hits:
            failed = True
            print(f"❌ {rule.name}:", file=sys.stderr)
            for h in rule.hits:
                print(f"  {h}", file=sys.stderr)
            print(f"   why: {rule.description}", file=sys.stderr)
            print(f"   fix: {rule.fix}", file=sys.stderr)
            print("", file=sys.stderr)

    if failed:
        print(
            "lint_strong_types.py: strong-type hygiene violations above ↑",
            file=sys.stderr,
        )
        return 1

    print("lint_strong_types.py: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
