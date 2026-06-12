#!/usr/bin/env python3
"""
Check AMPL/PAMPL dispatch consistency between the C++ registry and
the Python ``gtopt_check_pampl`` validator.

The single authoritative source of ``(class, attribute)`` PAMPL bindings
is ``source/ampl_dispatch_registry.cpp`` — every ``register_ampl_param``
and ``register_ampl_iter`` call there is what user constraints can
actually resolve at runtime.  The Python validator
``scripts/gtopt_check_pampl/_checks.py`` carries its own hard-coded
``ELEMENT_TYPES`` and ``LP_VARIABLES`` sets used to flag suspicious
references in ``*.pampl`` files.  When the two drift, the Python
validator silently passes bogus references or flags valid ones.

This checker scrapes both sides and reports:

  ERROR    — Python ``ELEMENT_TYPES`` lists a class the C++ registry
             does NOT register (``register_ampl_iter`` missing).  The
             validator would accept ``class(...).attr`` references that
             gtopt's user-constraint resolver will reject at runtime.

  WARNING  — C++ registers an iterator/class the Python ``ELEMENT_TYPES``
             does not know about.  The validator silently skips
             coverage for that class — adding it to the Python set is
             a no-cost win.

  INFO     — per-class summary: number of params registered in C++, and
             whether the class iterator is registered.

Exit code:
  0 — no errors (warnings allowed).
  1 — one or more ERRORs found.
  2 — one or more WARNINGs found and ``--strict`` was passed.

Examples
--------
  tools/check_ampl_dispatch.py
  tools/check_ampl_dispatch.py --strict       # warnings → exit 2
  tools/check_ampl_dispatch.py --summary      # only the coverage table
  tools/check_ampl_dispatch.py --json         # machine-readable output

Intended use
------------
  * Local: run before editing ``source/ampl_dispatch_registry.cpp`` or
    ``scripts/gtopt_check_pampl/_checks.py`` to spot drift.
  * CI: wired into pytest via
    ``scripts/gtopt_shared/tests/test_ampl_dispatch_consistency.py`` —
    ERRORs break the build.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import NamedTuple


REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY_FILE = REPO_ROOT / "source" / "ampl_dispatch_registry.cpp"
CHECKS_FILE = REPO_ROOT / "scripts" / "gtopt_check_pampl" / "_checks.py"
INCLUDE_DIR = REPO_ROOT / "include" / "gtopt"

# Manual overrides for PascalCase → snake_case where the C++ name does
# not derive trivially from a regex (acronyms, alphanum-only fallbacks).
_PASCAL_TO_SNAKE_OVERRIDES = {
    "LngTerminal": "lng_terminal",
}


def pascal_to_snake(name: str) -> str:
    """Convert a C++ PascalCase class name to its snake_case
    counterpart (matching ``Class::class_name.snake_case()``)."""
    if name in _PASCAL_TO_SNAKE_OVERRIDES:
        return _PASCAL_TO_SNAKE_OVERRIDES[name]
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s).lower()


# ─── Finding shape ───────────────────────────────────────────────────────────


class Finding(NamedTuple):
    severity: str  # "INFO" | "WARNING" | "ERROR"
    code: str
    klass: str
    field: str
    message: str


# ─── C++ side: parse source/ampl_dispatch_registry.cpp ───────────────────────


_RE_CLS_ALIAS = re.compile(
    r"constexpr\s+auto\s+(\w+)_cls\s*=\s*(\w+)::class_name\.snake_case\(\);"
)
_RE_REGISTER_PARAM = re.compile(r"register_ampl_param\(\s*(\w+_cls)\s*,\s*\"([^\"]+)\"")
_RE_REGISTER_ITER_CLASS = re.compile(
    r"register_ampl_iter\(\s*(\w+)::class_name\.snake_case\(\)"
)
# Catches register_ampl_iter(SomeLP::SomeName, ...) used for synthetic
# iterators (e.g. ReservoirSeepageLP::SeepageName) where the registered
# PAMPL name is the value of a string_view constant declared in a header,
# not the snake_case of the C++ class symbol.
_RE_REGISTER_ITER_LITERAL = re.compile(r"register_ampl_iter\(\s*(\w+)::(\w+)\s*,")
# Resolve such symbols by scanning headers for:
#   static constexpr std::string_view <Symbol> {"<value>"};
# or with `=` initialisation.
_RE_HEADER_LITERAL = re.compile(
    r"static\s+constexpr\s+std::string_view\s+(\w+)\s*"
    r"(?:\{|=)\s*\"([^\"]+)\""
)


def build_header_literal_map(include_dir: Path) -> dict[str, str]:
    """Scan headers for ``static constexpr std::string_view`` constants
    and return a flat symbol→value map.

    Used to resolve ``register_ampl_iter(<Class>::<Symbol>, ...)``
    expressions where the registered PAMPL name is the value of a
    symbol rather than the class's snake_case name.
    """
    out: dict[str, str] = {}
    if not include_dir.is_dir():
        return out
    for hdr in include_dir.glob("*.hpp"):
        try:
            text = hdr.read_text()
        except OSError:
            continue
        for sym, val in _RE_HEADER_LITERAL.findall(text):
            out.setdefault(sym, val)  # first-seen wins
    return out


def parse_cpp_registry(
    text: str, literal_map: dict[str, str] | None = None
) -> tuple[dict[str, set[str]], set[str], list[Finding]]:
    """Return ``(params_by_class, iter_classes, info_findings)``.

    Keys/values are snake_case class names.
    ``params_by_class[cls] = {attr, ...}``.  Any
    ``register_ampl_iter(Class::Symbol, ...)`` that cannot be resolved
    via ``literal_map`` is reported as an INFO finding so the operator
    can extend the resolver or add the symbol to the header scan.
    """
    if literal_map is None:
        literal_map = {}

    alias_to_pascal: dict[str, str] = {}
    for alias, pascal in _RE_CLS_ALIAS.findall(text):
        alias_to_pascal[alias] = pascal

    params_by_class: dict[str, set[str]] = defaultdict(set)
    for cls_alias, attr in _RE_REGISTER_PARAM.findall(text):
        # _RE_CLS_ALIAS captures the alias root (e.g. "generator"),
        # _RE_REGISTER_PARAM captures the full token "<root>_cls".
        # Strip the suffix to look up the PascalCase class name.
        root = cls_alias[:-4] if cls_alias.endswith("_cls") else cls_alias
        pascal = alias_to_pascal.get(root)
        if pascal is None:
            continue  # unknown alias — surfaces as a separate finding
        snake = pascal_to_snake(pascal)
        params_by_class[snake].add(attr)

    iter_classes: set[str] = set()
    info_findings: list[Finding] = []
    for pascal in _RE_REGISTER_ITER_CLASS.findall(text):
        iter_classes.add(pascal_to_snake(pascal))
    for cpp_class, symbol in _RE_REGISTER_ITER_LITERAL.findall(text):
        value = literal_map.get(symbol)
        if value is not None:
            iter_classes.add(value)
        else:
            info_findings.append(
                Finding(
                    severity="INFO",
                    code="unresolved-iter-literal",
                    klass=cpp_class,
                    field=symbol,
                    message=(
                        f"register_ampl_iter({cpp_class}::{symbol}, …) "
                        "could not be resolved via the header literal scan; "
                        "Python coverage for this class is not checked. "
                        "Declare the symbol as `static constexpr "
                        'std::string_view {symbol} {{"…"}};` in a header '
                        "under include/gtopt/."
                    ),
                )
            )

    return params_by_class, iter_classes, info_findings


# ─── Python side: parse gtopt_check_pampl/_checks.py ─────────────────────────


_RE_PY_SET = re.compile(
    r"^(ELEMENT_TYPES|LP_VARIABLES)\s*=\s*\{([^}]*)\}",
    re.MULTILINE | re.DOTALL,
)
_RE_PY_STR_LITERAL = re.compile(r"\"([^\"]+)\"")


def parse_python_sets(text: str) -> dict[str, set[str]]:
    """Return ``{"ELEMENT_TYPES": {...}, "LP_VARIABLES": {...}}``.

    Parsed by regex to avoid importing the gtopt_scripts package from
    the standalone tool — keeps it runnable without the scripts venv.
    """
    out: dict[str, set[str]] = {"ELEMENT_TYPES": set(), "LP_VARIABLES": set()}
    for name, body in _RE_PY_SET.findall(text):
        out[name] = set(_RE_PY_STR_LITERAL.findall(body))
    return out


# ─── Checks ──────────────────────────────────────────────────────────────────


def check_python_only_classes(
    py_element_types: set[str], cpp_iter_classes: set[str]
) -> list[Finding]:
    findings: list[Finding] = []
    for cls in sorted(py_element_types - cpp_iter_classes):
        findings.append(
            Finding(
                severity="ERROR",
                code="python-only-class",
                klass=cls,
                field="",
                message=(
                    f"ELEMENT_TYPES contains '{cls}' but C++ "
                    "ampl_dispatch_registry.cpp does NOT call "
                    "register_ampl_iter for it — the PAMPL validator "
                    "would accept references that the runtime resolver "
                    "rejects.  Either remove from ELEMENT_TYPES or add "
                    "a register_ampl_iter call."
                ),
            )
        )
    return findings


def check_cpp_only_classes(
    py_element_types: set[str], cpp_iter_classes: set[str]
) -> list[Finding]:
    findings: list[Finding] = []
    for cls in sorted(cpp_iter_classes - py_element_types):
        findings.append(
            Finding(
                severity="WARNING",
                code="cpp-only-class",
                klass=cls,
                field="",
                message=(
                    f"C++ registers iterator for class '{cls}' but it "
                    "is missing from ELEMENT_TYPES in "
                    "gtopt_check_pampl/_checks.py — validator silently "
                    "skips coverage for this class. Consider adding it."
                ),
            )
        )
    return findings


# ─── Reporting ───────────────────────────────────────────────────────────────


_SEV_MARK = {"ERROR": "✘", "WARNING": "⚠", "INFO": "·"}


def print_findings(findings: list[Finding]) -> None:
    if not findings:
        return
    by_severity: dict[str, list[Finding]] = defaultdict(list)
    for f in findings:
        by_severity[f.severity].append(f)
    for sev in ("ERROR", "WARNING", "INFO"):
        bucket = by_severity.get(sev, [])
        if not bucket:
            continue
        print(f"\n── {sev}s ────────────────────────────────────────────")
        for f in bucket:
            label = f"{f.klass}.{f.field}" if f.field else f.klass
            print(f"  {_SEV_MARK[sev]}  [{f.code}] {label}")
            print(f"      {f.message}")


def print_coverage_summary(
    params_by_class: dict[str, set[str]],
    cpp_iter_classes: set[str],
    py_element_types: set[str],
) -> None:
    all_classes = sorted(cpp_iter_classes | py_element_types)
    print("\n── Coverage by class ─────────────────────────────────────────")
    print(f"  {'class':<32} {'C++ iter':>10} {'C++ params':>12} {'Py ET':>8}")
    print("  " + "─" * 64)
    for cls in all_classes:
        iter_mark = "yes" if cls in cpp_iter_classes else "—"
        n_params = len(params_by_class.get(cls, set()))
        py_mark = "yes" if cls in py_element_types else "—"
        print(f"  {cls:<32} {iter_mark:>10} {n_params:>12} {py_mark:>8}")
    print("  " + "─" * 64)
    print(
        f"  totals: {len(cpp_iter_classes)} C++ iter, "
        f"{sum(len(s) for s in params_by_class.values())} C++ params, "
        f"{len(py_element_types)} Py ELEMENT_TYPES"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check AMPL/PAMPL dispatch consistency between C++ and Python.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 2 when warnings are present",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="print coverage summary only (no per-finding detail)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a machine-readable JSON report",
    )
    args = parser.parse_args()

    if not REGISTRY_FILE.is_file():
        print(f"✘ missing {REGISTRY_FILE.relative_to(REPO_ROOT)}", file=sys.stderr)
        return 1
    if not CHECKS_FILE.is_file():
        print(f"✘ missing {CHECKS_FILE.relative_to(REPO_ROOT)}", file=sys.stderr)
        return 1

    cpp_text = REGISTRY_FILE.read_text()
    py_text = CHECKS_FILE.read_text()
    literal_map = build_header_literal_map(INCLUDE_DIR)

    params_by_class, cpp_iter_classes, info_findings = parse_cpp_registry(
        cpp_text, literal_map
    )
    py_sets = parse_python_sets(py_text)
    py_element_types = py_sets["ELEMENT_TYPES"]

    findings: list[Finding] = []
    findings.extend(info_findings)
    findings.extend(check_python_only_classes(py_element_types, cpp_iter_classes))
    findings.extend(check_cpp_only_classes(py_element_types, cpp_iter_classes))

    if args.json:
        report = {
            "totals": {
                "cpp_iter_classes": len(cpp_iter_classes),
                "cpp_param_total": sum(len(s) for s in params_by_class.values()),
                "py_element_types": len(py_element_types),
                "py_lp_variables": len(py_sets["LP_VARIABLES"]),
            },
            "cpp_iter_classes": sorted(cpp_iter_classes),
            "cpp_params_by_class": {
                k: sorted(v) for k, v in sorted(params_by_class.items())
            },
            "py_element_types": sorted(py_element_types),
            "py_lp_variables": sorted(py_sets["LP_VARIABLES"]),
            "findings": [f._asdict() for f in findings],
        }
        print(json.dumps(report, indent=2))
    else:
        print("AMPL dispatch consistency report")
        print(
            f"  scanned  : {REGISTRY_FILE.relative_to(REPO_ROOT)}, "
            f"{CHECKS_FILE.relative_to(REPO_ROOT)}"
        )
        if not args.summary:
            print_findings(findings)
        print_coverage_summary(params_by_class, cpp_iter_classes, py_element_types)

    n_err = sum(1 for f in findings if f.severity == "ERROR")
    n_warn = sum(1 for f in findings if f.severity == "WARNING")
    if n_err:
        if not args.json:
            print(
                f"\n✘ {n_err} error(s) — Python validator is out of sync "
                "with the C++ AMPL registry."
            )
        return 1
    if args.strict and n_warn:
        if not args.json:
            print(f"\n⚠ {n_warn} warning(s) in --strict mode.")
        return 2
    if not args.json and not args.summary:
        if n_warn:
            print(f"\n⚠ {n_warn} warning(s) — review when convenient.")
        else:
            print("\n✓ all good.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
