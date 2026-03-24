#!/usr/bin/env python3
"""Lint LaTeX math expressions in Markdown files.

Checks for common issues that break GitHub's math rendering:

1. ``\\left\\{`` / ``\\right\\}`` delimiters — GitHub's Markdown preprocessor
   consumes the backslash before ``{``/``}``, so ``\\left\\{`` becomes
   ``\\left{`` which is an invalid delimiter.  Use ``\\lbrace`` / ``\\rbrace``.

2. ``\\bigl\\{`` / ``\\bigr\\}`` (and Bigl/Bigr variants) — same issue.

3. Lines starting with ``+`` or ``-`` inside ``$$`` display-math blocks —
   GitHub may parse these as Markdown list items, breaking the formula.

4. Inline math ``$...$`` spanning multiple lines — not supported by GitHub.

5. Unbalanced ``\\left`` / ``\\right`` pairs inside a single ``$$`` block.

Exit code 0 when no issues are found, 1 otherwise.

Usage::

    python tools/check_latex_math.py [FILE_OR_DIR ...]

When called without arguments, checks all ``*.md`` files under the
repository root (excluding ``node_modules/``, ``build/``, and hidden dirs).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

# Brace-delimiter patterns that break on GitHub
_BAD_DELIMITERS: list[tuple[re.Pattern[str], str]] = [
    (
        re.compile(r"\\left\s*\\{"),
        r"Use \lbrace instead of \left\{",
    ),
    (
        re.compile(r"\\right\s*\\}"),
        r"Use \rbrace instead of \right\}",
    ),
    (
        re.compile(r"\\[Bb]igl\s*\\{"),
        r"Use \lbrace instead of \bigl\{",
    ),
    (
        re.compile(r"\\[Bb]igr\s*\\}"),
        r"Use \rbrace instead of \bigr\}",
    ),
]

# Directories to skip
_SKIP_DIRS = {"node_modules", "build", ".git", "__pycache__", "cpm_modules"}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _collect_md_files(paths: list[str]) -> list[Path]:
    """Expand arguments into a sorted list of Markdown files."""
    result: list[Path] = []
    for p in paths:
        pp = Path(p)
        if pp.is_file() and pp.suffix == ".md":
            result.append(pp)
        elif pp.is_dir():
            for md in sorted(pp.rglob("*.md")):
                if any(part in _SKIP_DIRS or part.startswith(".") for part in md.parts):
                    continue
                result.append(md)
    return sorted(set(result))


def _find_display_blocks(
    lines: list[str],
) -> list[tuple[int, int, list[str], bool]]:
    """Return display-math blocks as (start, end, content_lines, content_on_opener).

    *content_on_opener* is ``True`` when the opening ``$$`` line also carries
    formula content (e.g. ``$$ -x + y``).  When ``True``, the first element
    of *content_lines* belongs to the opener line and is safe from
    Markdown list-item parsing.
    """
    blocks: list[tuple[int, int, list[str], bool]] = []
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()

        # Single-line display: $$ ... $$ (content between)
        if stripped.startswith("$$") and stripped.endswith("$$") and len(stripped) > 4:
            content = stripped[2:-2].strip()
            blocks.append((i, i, [content], True))
            i += 1
            continue

        # Opening $$ (possibly with content on same line)
        if stripped.startswith("$$"):
            start = i
            content_lines: list[str] = []
            # Content on the opening line after $$
            after_open = stripped[2:].strip()
            has_content_on_opener = bool(after_open)
            if after_open:
                content_lines.append(after_open)
            i += 1
            while i < len(lines):
                s2 = lines[i].strip()
                if s2.endswith("$$"):
                    before_close = s2[:-2].strip() if s2 != "$$" else ""
                    if before_close:
                        content_lines.append(before_close)
                    blocks.append(
                        (start, i, content_lines, has_content_on_opener)
                    )
                    i += 1
                    break
                content_lines.append(s2)
                i += 1
            continue

        i += 1
    return blocks


def _is_escaped(text: str, pos: int) -> bool:
    """Return True if the character at *pos* is preceded by an odd number of backslashes."""
    n = 0
    while pos - 1 - n >= 0 and text[pos - 1 - n] == "\\":
        n += 1
    return n % 2 == 1


def _find_inline_math(line: str) -> list[tuple[int, int]]:
    """Return (start, end) character positions of ``$...$`` spans in *line*.

    Skips ``$$`` (display math) and escaped ``\\$``.
    """
    spans: list[tuple[int, int]] = []
    pos = 0
    while pos < len(line):
        # Skip escaped dollars (odd number of preceding backslashes)
        if line[pos] == "$" and _is_escaped(line, pos):
            pos += 1
            continue
        # Skip display-math markers
        if line[pos : pos + 2] == "$$":
            pos += 2
            continue
        if line[pos] == "$":
            # Look for closing $
            end = pos + 1
            while end < len(line):
                if line[end] == "$" and not _is_escaped(line, end):
                    if end + 1 < len(line) and line[end + 1] == "$":
                        end += 2
                        continue
                    spans.append((pos, end + 1))
                    pos = end + 1
                    break
                end += 1
            else:
                # No closing $ found on this line
                pos += 1
        else:
            pos += 1
    return spans


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------


def _build_line_masks(lines: list[str]) -> tuple[list[bool], list[bool]]:
    """Return per-line boolean masks: (in_code_fence, in_display_math).

    A code fence opens with a line starting with 3+ backticks (possibly
    followed by an info string like ``bash``).  It closes with a line that
    is **only** backticks (no info string).
    """
    in_fence = [False] * len(lines)
    in_display = [False] * len(lines)

    fence_open = False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("```"):
            if not fence_open:
                # Opening fence — may have info string
                fence_open = True
                in_fence[i] = True
                continue
            # Potential closing fence — only backticks, no info string
            if re.fullmatch(r"`{3,}\s*", stripped):
                in_fence[i] = True
                fence_open = False
                continue
        in_fence[i] = fence_open

    disp_open = False
    for i, line in enumerate(lines):
        if in_fence[i]:
            continue
        stripped = line.strip()
        if stripped.startswith("$$"):
            if not disp_open:
                disp_open = True
                in_display[i] = True
                if stripped.endswith("$$") and len(stripped) > 4:
                    # Single-line display math
                    disp_open = False
            else:
                # Closing $$
                in_display[i] = True
                disp_open = False
            continue
        if stripped.endswith("$$") and disp_open:
            in_display[i] = True
            disp_open = False
            continue
        in_display[i] = disp_open

    return in_fence, in_display


def check_file(filepath: Path) -> list[str]:
    """Return a list of diagnostic messages for *filepath*."""
    diagnostics: list[str] = []
    text = filepath.read_text(encoding="utf-8")
    lines = text.split("\n")
    in_fence, in_display = _build_line_masks(lines)

    # 1 & 2: Bad delimiters (only in math context, not code fences)
    for i, line in enumerate(lines):
        if in_fence[i]:
            continue
        for pat, msg in _BAD_DELIMITERS:
            for m in pat.finditer(line):
                diagnostics.append(
                    f"{filepath}:{i + 1}:{m.start() + 1}: {msg}  [{m.group()}]"
                )

    # 3: Lines starting with +/- inside display-math blocks
    for start_idx, _end_idx, content_lines, on_opener in _find_display_blocks(lines):
        if in_fence[start_idx]:
            continue
        for offset, cline in enumerate(content_lines):
            # If the opening $$ line carries content, the first content line
            # is on the same line as $$ and is safe from list-item parsing.
            if offset == 0 and on_opener:
                continue
            stripped = cline.strip()
            if stripped and stripped[0] in "+-":
                # When content starts on the $$ line (on_opener=True), the
                # offset in content_lines already includes the opener line,
                # so start_idx + offset gives the correct 0-based line index.
                # When $$ is on its own line, content starts one line after
                # the opener, so we add 1.
                line_num = start_idx + offset + (0 if on_opener else 1)
                diagnostics.append(
                    f"{filepath}:{line_num + 1}: "
                    f"Line starts with '{stripped[0]}' inside display math "
                    f"(may render as list item)  [{stripped[:60]}]"
                )

    # 4: Inline math spanning multiple lines — detect unclosed $ at EOL
    # Uses the _find_inline_math parser to find matched $...$ spans, then
    # checks for leftover unmatched $ that could indicate broken inline math.
    # Currency symbols ($20, $/MWh) in plain text are ignored.
    for i, line in enumerate(lines):
        if in_fence[i] or in_display[i]:
            continue
        stripped = line.strip()
        # Remove escaped \$ (odd number of backslashes before $) and $$
        cleaned = re.sub(r"(?<!\\)(\\\\)*\\\$", lambda m: " " * len(m.group()), line)
        cleaned = re.sub(r"\$\$", "  ", cleaned)
        # Remove backtick-quoted spans
        cleaned = re.sub(r"`[^`]*`", lambda m: " " * len(m.group()), cleaned)
        # Find matched inline math spans and mask them out
        spans = _find_inline_math(cleaned)
        for start, end in reversed(spans):
            cleaned = cleaned[:start] + " " * (end - start) + cleaned[end:]
        # Any remaining $ is unmatched — but skip currency patterns
        remaining = re.sub(r"\$(?=\d)", "", cleaned)  # $20, $100 etc.
        remaining = re.sub(r"\$(?=/)", "", remaining)  # $/MWh
        remaining = re.sub(r"\(\$\)", "", remaining)  # ($) currency
        if "$" in remaining:
            diagnostics.append(
                f"{filepath}:{i + 1}: "
                f"Odd number of '$' on line (possible unclosed inline math)  "
                f"[{stripped[:60]}]"
            )

    # 5: Unbalanced \left / \right in display blocks
    for start_idx, _end_idx, content_lines, _on_opener in _find_display_blocks(lines):
        if in_fence[start_idx]:
            continue
        block_text = " ".join(content_lines)
        lefts = len(re.findall(r"\\left\b", block_text))
        rights = len(re.findall(r"\\right\b", block_text))
        if lefts != rights:
            diagnostics.append(
                f"{filepath}:{start_idx + 1}: "
                f"Unbalanced \\left ({lefts}) / \\right ({rights}) in display block"
            )

    return diagnostics


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Lint LaTeX math in Markdown files for GitHub rendering issues."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        default=["."],
        help="Files or directories to check (default: current directory)",
    )
    args = parser.parse_args(argv)

    files = _collect_md_files(args.paths)
    if not files:
        print("No Markdown files found.", file=sys.stderr)
        return 0

    all_diags: list[str] = []
    for f in files:
        all_diags.extend(check_file(f))

    for d in all_diags:
        print(d)

    if all_diags:
        print(f"\n{len(all_diags)} issue(s) found.", file=sys.stderr)
        return 1

    print(f"Checked {len(files)} file(s) — no issues found.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
