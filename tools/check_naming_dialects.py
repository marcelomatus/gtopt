#!/usr/bin/env python3
"""
Check naming-dialects coverage against the C++ JSON contracts.

Scans `include/gtopt/json/*.hpp` for daw::json contract member names
(`json_*<"NAME", ...>`) and cross-references them with
`share/gtopt/naming_dialects.json`.

Three classes of finding, in increasing severity:

  INFO     — per-class coverage summary (canonicals: covered vs uncovered).
  WARNING  — a C++ contract field has no dictionary entry.  Most fields
             don't need aliases (numerical tolerances, debug flags, internal
             tunings); this is a *prompt for review*, not a defect.
  ERROR    — the dictionary itself is broken:
              * `canonical` in `aliases[]` doesn't match any C++ field
                (stale entry after a rename / removal);
              * an alias appears as a canonical name on a different class
                without a class-scope confining it (would cause ambiguous
                rewrites in `canonicalize_json_keys`).

Exit code:
  0 — no errors (warnings allowed).
  1 — one or more ERRORs found.
  2 — one or more WARNINGs found and `--strict` was passed.

Examples
--------
  tools/check_naming_dialects.py
  tools/check_naming_dialects.py --strict        # warnings → exit 2
  tools/check_naming_dialects.py --summary       # only the coverage table
  tools/check_naming_dialects.py --json          # machine-readable output
  tools/check_naming_dialects.py --class line    # filter to one C++ class

Intended use
------------
  * Local: run before editing `share/gtopt/naming_dialects.json` to spot
    drift.
  * CI: add as a ctest entry; treat ERRORS as build-breaking, WARNINGS as
    advisory unless `--strict` is on the contract-stability gate.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable, NamedTuple

# ─── Constants ─────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
JSON_HEADERS_DIR = REPO_ROOT / "include" / "gtopt" / "json"
DICT_FILE = REPO_ROOT / "share" / "gtopt" / "naming_dialects.json"

# Files in include/gtopt/json that are NOT element-class contracts:
# helpers, basic types, parsing policies, etc.  These are excluded from
# canonical-field extraction.
NON_ELEMENT_FILES = frozenset(
    {
        "json_basic_types.hpp",
        "json_field_sched.hpp",
        "json_single_id.hpp",
        "json_parse_policy.hpp",
        "json_enum_option.hpp",
        "json_right_bound_rule.hpp",  # helper struct
        "json_schedule.hpp",  # helper
        "json_variable_scale.hpp",  # nested-only
        "json_lp_validation.hpp",  # nested options
        "json_lp_matrix_options.hpp",  # nested options
    }
)

# Pseudo-classes used in the dictionary to describe fields shared by
# multiple real classes.  Their `canonical` entries are verified by
# checking whether ANY real class has that field.
PSEUDO_CLASSES = frozenset({"_capacity"})

# Matches the json_* member-declaration template, capturing the JSON key
# name in the first quoted argument.  Examples that match:
#   json_number<"uid", Uid>
#   json_string_null<"name", OptName>
#   json_variant_null<"lmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>
#   json_array_null<"bus_array", Array<Bus>, Bus>
#   json_bool_null<"strict_storage_emin", OptBool>
JSON_MEMBER_RE = re.compile(
    r"""
    \b json_                                       # token start
    (?: number | string | bool | variant | array | class | enum )
    (?: _null )?                                   # nullable variant
    \s* < \s*
    " (?P<name> [^"]+ ) "                          # key name (group)
    """,
    re.VERBOSE,
)

# Matches a daw::json contract declaration so we can attribute member names
# to the right C++ struct.  Example:  struct json_data_contract<DemandAttrs>
CONTRACT_RE = re.compile(
    r"struct\s+json_data_contract\s*<\s*(?P<typ>[A-Za-z_][A-Za-z_0-9:<>,\s]*?)\s*>"
)


# ─── Data classes ──────────────────────────────────────────────────────────


class Finding(NamedTuple):
    severity: str  # "ERROR" | "WARNING"
    code: str  # short machine tag
    klass: str  # element/options class name (lowercase, matches dict)
    field: str  # the canonical / alias name involved
    message: str  # human-readable explanation


class ContractField(NamedTuple):
    klass: str  # e.g. "demand", "model_options"
    field: str  # canonical JSON key (e.g. "lmax")
    cpp_type: str  # the C++ struct the contract is bound to


# ─── C++ contract extraction ───────────────────────────────────────────────


def filename_to_class(path: Path) -> str:
    """`json_demand.hpp` → `demand`; `json_lng_terminal.hpp` → `lng_terminal`."""
    stem = path.stem
    if not stem.startswith("json_"):
        return stem
    return stem[len("json_") :]


def extract_contract_fields(path: Path) -> list[ContractField]:
    """Parse one `json_*.hpp` file and yield (class, canonical) entries.

    A file may contain multiple `json_data_contract<X>` blocks (e.g.
    `json_demand.hpp` has `DemandAttrs` and `Demand`).  We attribute each
    `json_*<"NAME", ...>` line to the *most recent* contract header seen
    before it.  The class name returned is always the file-derived
    lowercase-snake-case form (e.g. `demand`), not the C++ struct name.
    """
    text = path.read_text(encoding="utf-8")
    klass = filename_to_class(path)
    out: list[ContractField] = []

    # Split text on contract boundaries.  We can't perfectly bracket-match
    # without a full C++ parser, but we don't need to: the regex for
    # JSON members is narrow enough that an over-inclusive scan still
    # produces the right (class, canonical) pairs as long as we
    # correctly track which contract block we're inside.
    cursor = 0
    current_cpp_type = ""
    for ctr_m in CONTRACT_RE.finditer(text):
        # Process any json_*<"…"> from previous block.
        for m in JSON_MEMBER_RE.finditer(text, cursor, ctr_m.start()):
            out.append(
                ContractField(klass=klass, field=m["name"], cpp_type=current_cpp_type)
            )
        current_cpp_type = ctr_m["typ"].strip()
        cursor = ctr_m.end()

    # Tail: members after the last contract header.
    for m in JSON_MEMBER_RE.finditer(text, cursor):
        out.append(
            ContractField(klass=klass, field=m["name"], cpp_type=current_cpp_type)
        )

    return out


def collect_all_contract_fields() -> list[ContractField]:
    """Walk every `json_*.hpp` and collect canonical fields per element class."""
    fields: list[ContractField] = []
    for path in sorted(JSON_HEADERS_DIR.glob("json_*.hpp")):
        if path.name in NON_ELEMENT_FILES:
            continue
        fields.extend(extract_contract_fields(path))
    return fields


# ─── Dictionary parsing ────────────────────────────────────────────────────


def load_dictionary() -> tuple[list[dict], list[dict]]:
    """Return (global_aliases, class_aliases) from naming_dialects.json."""
    raw = json.loads(DICT_FILE.read_text(encoding="utf-8"))
    return raw.get("aliases", []), raw.get("class_aliases", [])


# ─── Cross-checks ──────────────────────────────────────────────────────────


def check_stale_entries(
    rows: list[dict],
    contract_fields_by_class: dict[str, set[str]],
    *,
    is_class_scoped: bool,
) -> Iterable[Finding]:
    """A `canonical` in the dictionary should exist as a field on either:
      * the row's own class, OR
      * (for shared pseudo-classes like `_capacity`) any real element class.

    Otherwise the row points at nothing — usually a typo or a stale entry
    left over from a rename / removal.
    """
    all_canonical_fields: set[str] = set()
    for fields in contract_fields_by_class.values():
        all_canonical_fields.update(fields)

    for row in rows:
        klass = row.get("class", "")
        canonical = row.get("canonical", "")
        if not canonical:
            continue

        if klass in PSEUDO_CLASSES:
            if canonical not in all_canonical_fields:
                yield Finding(
                    severity="ERROR",
                    code="stale-canonical-shared",
                    klass=klass,
                    field=canonical,
                    message=(
                        f"pseudo-class '{klass}' canonical '{canonical}' does not "
                        "exist as a JSON contract field on any element class"
                    ),
                )
            continue

        own = contract_fields_by_class.get(klass, set())
        if canonical not in own:
            kind = "class-scoped" if is_class_scoped else "global"
            yield Finding(
                severity="ERROR",
                code="stale-canonical",
                klass=klass,
                field=canonical,
                message=(
                    f"{kind} alias row claims canonical '{klass}.{canonical}' "
                    f"but '{canonical}' is not a JSON contract field on "
                    f"'{klass}' (renamed? removed?)"
                ),
            )


def check_alias_canonical_collisions(
    global_aliases: list[dict],
    contract_fields_by_class: dict[str, set[str]],
) -> Iterable[Finding]:
    """A *global* alias name must not also be a canonical on any class —
    otherwise `canonicalize_json_keys` would rewrite a legitimately-
    canonical key (e.g. rewriting `discharge` globally would clobber
    `Flow.discharge`).  Class-scoped aliases are exempt — that's exactly
    what the class scope is for.
    """
    all_canonical_fields: set[tuple[str, str]] = set()
    for klass, fields in contract_fields_by_class.items():
        for f in fields:
            all_canonical_fields.add((klass, f))

    canonical_only = {f for _, f in all_canonical_fields}

    for row in global_aliases:
        alt = row.get("alt", "")
        if not alt:
            continue
        if alt in canonical_only:
            owners = sorted(k for (k, f) in all_canonical_fields if f == alt)
            yield Finding(
                severity="ERROR",
                code="alias-shadows-canonical",
                klass=row.get("class", ""),
                field=alt,
                message=(
                    f"global alias '{alt}' is also a canonical attribute on "
                    f"{owners} — promoting it globally would corrupt those "
                    "elements' parsing.  Move this row to `class_aliases[]` "
                    "with an appropriate `class` scope."
                ),
            )


def check_uncovered_fields(
    contract_fields_by_class: dict[str, set[str]],
    aliased_canonicals: set[tuple[str, str]],
    aliased_shared_canonicals: set[str],
    only_class: str | None,
) -> Iterable[Finding]:
    """A contract field with NO row in `naming_dialects.json` is a
    warning — most fields don't need aliases, but the maintainer should
    explicitly decide.  Skips the universally-present meta-fields
    (`uid`, `name`, `active`, `type`) which never need aliasing.
    """
    META_FIELDS = {"uid", "name", "active", "type", "description"}

    for klass in sorted(contract_fields_by_class):
        if only_class and klass != only_class:
            continue
        for field in sorted(contract_fields_by_class[klass]):
            if field in META_FIELDS:
                continue
            covered = (klass, field) in aliased_canonicals
            shared = field in aliased_shared_canonicals
            if not covered and not shared:
                yield Finding(
                    severity="WARNING",
                    code="uncovered-field",
                    klass=klass,
                    field=field,
                    message=(
                        f"contract field '{klass}.{field}' has no entry in "
                        "naming_dialects.json — consider adding a 'modern' "
                        "alias and per-dialect equivalents, or document why "
                        "no alias is needed"
                    ),
                )


# ─── Reporting ─────────────────────────────────────────────────────────────


def print_coverage_summary(
    contract_fields_by_class: dict[str, set[str]],
    aliased_canonicals: set[tuple[str, str]],
    aliased_shared_canonicals: set[str],
) -> None:
    print("\n── Coverage by class ─────────────────────────────────────────")
    print(f"{'class':<32} {'fields':>7} {'aliased':>8}")
    print("─" * 53)
    total_fields = 0
    total_covered = 0
    for klass in sorted(contract_fields_by_class):
        fields = contract_fields_by_class[klass]
        covered = sum(
            1
            for f in fields
            if (klass, f) in aliased_canonicals or f in aliased_shared_canonicals
        )
        total_fields += len(fields)
        total_covered += covered
        marker = " " if covered == len(fields) else "·"
        print(f"{klass:<32} {len(fields):>7d} {covered:>4d}/{len(fields):<3d} {marker}")
    print("─" * 53)
    pct = (100.0 * total_covered / total_fields) if total_fields else 0.0
    print(
        f"{'TOTAL':<32} {total_fields:>7d} {total_covered:>4d}/{total_fields:<3d}"
        f"  ({pct:.1f}%)"
    )


def print_findings(findings: list[Finding]) -> None:
    by_severity: dict[str, list[Finding]] = defaultdict(list)
    for f in findings:
        by_severity[f.severity].append(f)

    for sev in ("ERROR", "WARNING"):
        bucket = by_severity.get(sev, [])
        if not bucket:
            continue
        marker = "✘" if sev == "ERROR" else "⚠"
        print(f"\n── {sev}S ({len(bucket)}) ──────────────────────────────────")
        for f in bucket:
            print(f"  {marker}  [{f.code}] {f.klass}.{f.field}")
            print(f"      {f.message}")


# ─── CLI ───────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 2 if any WARNING is reported (for CI gates)",
    )
    ap.add_argument(
        "--summary",
        action="store_true",
        help="print only the per-class coverage table and exit",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="machine-readable JSON output instead of human report",
    )
    ap.add_argument(
        "--class",
        dest="only_class",
        default=None,
        help="restrict warnings to a single class (e.g. 'line')",
    )
    args = ap.parse_args()

    # ── Phase 1: gather contract fields ──
    contract_fields = collect_all_contract_fields()
    contract_fields_by_class: dict[str, set[str]] = defaultdict(set)
    for f in contract_fields:
        contract_fields_by_class[f.klass].add(f.field)

    # ── Phase 2: load the dictionary ──
    global_aliases, class_aliases = load_dictionary()

    aliased_canonicals: set[tuple[str, str]] = set()
    aliased_shared_canonicals: set[str] = set()  # pseudo-class canonicals
    for row in global_aliases + class_aliases:
        k = row.get("class", "")
        c = row.get("canonical", "")
        if not c:
            continue
        if k in PSEUDO_CLASSES:
            aliased_shared_canonicals.add(c)
        else:
            aliased_canonicals.add((k, c))

    # ── Phase 3: run checks ──
    findings: list[Finding] = []
    findings.extend(
        check_stale_entries(
            global_aliases, contract_fields_by_class, is_class_scoped=False
        )
    )
    findings.extend(
        check_stale_entries(
            class_aliases, contract_fields_by_class, is_class_scoped=True
        )
    )
    findings.extend(
        check_alias_canonical_collisions(global_aliases, contract_fields_by_class)
    )
    findings.extend(
        check_uncovered_fields(
            contract_fields_by_class,
            aliased_canonicals,
            aliased_shared_canonicals,
            args.only_class,
        )
    )

    # ── Phase 4: report ──
    if args.json:
        out = {
            "totals": {
                "classes": len(contract_fields_by_class),
                "fields": sum(len(s) for s in contract_fields_by_class.values()),
                "global_aliases": len(global_aliases),
                "class_aliases": len(class_aliases),
            },
            "findings": [f._asdict() for f in findings],
        }
        print(json.dumps(out, indent=2))
    else:
        n_classes = len(contract_fields_by_class)
        n_fields = sum(len(s) for s in contract_fields_by_class.values())
        print(f"naming-dialects coverage report")
        print(
            f"  scanned     : {n_classes} element classes, {n_fields} contract fields"
        )
        print(
            f"  dictionary  : {len(global_aliases)} global + {len(class_aliases)} class-scoped aliases"
        )

        if not args.summary:
            print_findings(findings)
        print_coverage_summary(
            contract_fields_by_class, aliased_canonicals, aliased_shared_canonicals
        )

    # ── Phase 5: exit code ──
    n_err = sum(1 for f in findings if f.severity == "ERROR")
    n_warn = sum(1 for f in findings if f.severity == "WARNING")
    if n_err:
        if not args.json:
            print(
                f"\n✘ {n_err} error(s) — naming_dialects.json contains broken entries."
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
