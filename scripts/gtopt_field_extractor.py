#!/usr/bin/env python3
"""
gtopt_field_extractor.py — Extract field metadata from gtopt C++ headers and
generate Markdown / HTML documentation tables.

Usage
-----
    python scripts/gtopt_field_extractor.py [HEADER_DIR] [--output FILE] [--format md|html]

If HEADER_DIR is omitted the script searches ``include/gtopt/`` relative to
the repository root (the directory containing this script's parent).

The script parses ``///< Description [units]`` comments on struct member
declarations and builds a table with the following columns:

    | Field | C++ Type | JSON Type | Units | Required | Description |

Output format can be ``md`` (GitHub-flavoured Markdown, default) or ``html``
(standalone HTML with internal hyperlinks between element sections).

Examples
--------
    # Dump all element tables to stdout as Markdown
    python scripts/gtopt_field_extractor.py

    # Write full HTML reference to a file
    python scripts/gtopt_field_extractor.py --format html --output INPUT_DATA_API.html

    # Extract only Generator and Demand
    python scripts/gtopt_field_extractor.py --elements Generator Demand
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------


@dataclass
class FieldInfo:
    name: str
    cpp_type: str
    json_type: str
    units: str
    required: bool
    description: str


@dataclass
class StructInfo:
    struct_name: str
    file_path: str
    brief: str
    details: str
    fields: list[FieldInfo] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Type mapping: C++ type → JSON type
# ---------------------------------------------------------------------------

_CPP_TO_JSON: dict[str, str] = {
    # Scalars
    "Uid": "integer",
    "Name": "string",
    "String": "string",
    "Real": "number",
    "Int": "integer",
    "Bool": "boolean",
    "Size": "integer",
    # Optionals of scalars
    "OptName": "string",
    "OptReal": "number",
    "OptInt": "integer",
    "OptBool": "boolean",
    "OptActive": "integer|boolean",
    # FieldSched variants (scalar | array | filename)
    "OptTRealFieldSched": "number|array|string",
    "OptTBRealFieldSched": "number|array|string",
    "OptSTRealFieldSched": "number|array|string",
    "OptSTBRealFieldSched": "number|array|string",
    "STBRealFieldSched": "number|array|string",
    "STRealFieldSched": "number|array|string",
    "TRealFieldSched": "number|array|string",
    "TBRealFieldSched": "number|array|string",
    "RealFieldSched": "number|array|string",
    # References
    "SingleId": "integer|string",
}


def _cpp_to_json_type(cpp_type: str) -> str:
    """Return a JSON type label for a C++ type string."""
    t = cpp_type.strip()
    return _CPP_TO_JSON.get(t, t)


def _is_required(cpp_type: str, field_name: str) -> bool:
    """Heuristic: uid, name, bus_a/bus_b, junction, waterway, generator, demand,
    battery, reserve_zones are required; everything else is optional."""
    required_names = {
        "uid",
        "bus_a",
        "bus_b",
        "junction",
        "waterway",
        "generator",
        "demand",
        "battery",
        "reserve_zones",
    }
    if field_name in required_names:
        return True
    if field_name == "name":
        return True
    # Optional<> → not required
    if cpp_type.startswith("Opt"):
        return False
    # Non-optional scalars (Real, Size, …) may have defaults → not required
    return False


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

# Matches a struct field declaration with an inline ///< comment.
# Groups: (type, name, comment)
_FIELD_RE = re.compile(
    r"""
    ^[ \t]*                       # leading whitespace
    ([\w:<>,\s*&]+?)              # C++ type (group 1) – non-greedy
    \s+                           # space between type and name
    ([\w]+)                       # field name (group 2)
    \s*                           # optional space
    \{[^}]*\}                     # default value { … }
    \s*;                          # semicolon
    \s*///< \s*                   # inline doc comment marker
    (.+)$                         # comment text (group 3)
    """,
    re.VERBOSE,
)

# Extracts [units] from a comment, e.g. "Maximum load [MW]" → "MW"
_UNITS_RE = re.compile(r"\[([^\]]+)\]")

# /** @brief … */ or /// @brief …
_BRIEF_RE = re.compile(r"@brief\s+(.+)")

# struct Foo or struct Bar
_STRUCT_RE = re.compile(r"^\s*struct\s+(\w+)\s*$")


def _extract_units(comment: str) -> tuple[str, str]:
    """Return (description_without_units, units_string)."""
    m = _UNITS_RE.search(comment)
    if m:
        units = m.group(1)
        desc = comment[: m.start()].rstrip(" ;,")
        return desc, units
    return comment, ""


def parse_header(path: Path) -> list[StructInfo]:
    """Parse a single C++ header file and return StructInfo objects."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    lines = text.splitlines()

    structs: list[StructInfo] = []
    current_struct: Optional[StructInfo] = None
    brace_depth = 0
    pending_brief = ""
    pending_details: list[str] = []
    in_comment_block = False
    comment_buffer: list[str] = []

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        # Track multi-line /** … */ comment blocks
        if stripped.startswith("/**") or stripped.startswith("/*!"):
            in_comment_block = True
            comment_buffer = [stripped]
            i += 1
            continue

        if in_comment_block:
            # Strip leading " * " or " *" from doxygen comment lines
            clean = re.sub(r"^\s*\*+\s?", "", stripped)
            comment_buffer.append(clean)
            if stripped.endswith("*/"):
                in_comment_block = False
                block_text = " ".join(comment_buffer)
                # Remove trailing */
                block_text = block_text.rstrip("/ *")
                bm = _BRIEF_RE.search(block_text)
                if bm:
                    # Take only up to the next tag
                    brief_text = re.split(r"\s@", bm.group(1))[0].strip()
                    pending_brief = brief_text
                # Capture @details
                detail_match = re.search(
                    r"@details?\s+(.+?)(?=\s@|\Z)", block_text, re.DOTALL
                )
                if detail_match:
                    pending_details = [detail_match.group(1).strip()]
            i += 1
            continue

        # Single-line /// comment (may contribute to pending_brief)
        if stripped.startswith("///"):
            bm = _BRIEF_RE.search(stripped)
            if bm:
                pending_brief = bm.group(1).strip()
            i += 1
            continue

        # Struct opening
        sm = _STRUCT_RE.match(line)
        if sm and current_struct is None:
            struct_name = sm.group(1)
            # Skip forward-declarations (no following '{')
            # Check if next non-empty line has '{'
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            if j < len(lines) and lines[j].strip() == "{":
                current_struct = StructInfo(
                    struct_name=struct_name,
                    file_path=str(path.name),
                    brief=pending_brief,
                    details=" ".join(pending_details),
                )
                structs.append(current_struct)
                pending_brief = ""
                pending_details = []
                brace_depth = 0
            i += 1
            continue

        if current_struct is not None:
            brace_depth += stripped.count("{") - stripped.count("}")

            # Field declaration
            fm = _FIELD_RE.match(line)
            if fm:
                cpp_type = fm.group(1).strip()
                fname = fm.group(2).strip()
                comment = fm.group(3).strip()
                desc, units = _extract_units(comment)
                current_struct.fields.append(
                    FieldInfo(
                        name=fname,
                        cpp_type=cpp_type,
                        json_type=_cpp_to_json_type(cpp_type),
                        units=units,
                        required=_is_required(cpp_type, fname),
                        description=desc,
                    )
                )

            # End of struct
            if brace_depth < 0 or (
                brace_depth == 0 and "}" in stripped and ";" in stripped
            ):
                current_struct = None
                brace_depth = 0

        i += 1

    return structs


# ---------------------------------------------------------------------------
# Elements of interest (exported in the JSON API)
# ---------------------------------------------------------------------------

SYSTEM_ELEMENTS = [
    "Bus",
    "Generator",
    "GeneratorAttrs",
    "Demand",
    "DemandAttrs",
    "Line",
    "Battery",
    "Converter",
    "GeneratorProfile",
    "DemandProfile",
    "ReserveZone",
    "ReserveProvision",
    "Junction",
    "Waterway",
    "Flow",
    "Reservoir",
    "Filtration",
    "Turbine",
]

SIMULATION_ELEMENTS = ["Block", "Stage", "Scenario", "Phase"]

OPTIONS_ELEMENTS = ["Options"]

ALL_ELEMENTS = OPTIONS_ELEMENTS + SIMULATION_ELEMENTS + SYSTEM_ELEMENTS


# ---------------------------------------------------------------------------
# Markdown renderer
# ---------------------------------------------------------------------------


def _md_field_row(f: FieldInfo) -> str:
    req = "**Yes**" if f.required else "No"
    units = f"`{f.units}`" if f.units else "—"
    desc = f.description.replace("|", "\\|")
    return f"| `{f.name}` | `{f.cpp_type}` | {f.json_type} | {units} | {req} | {desc} |"


def render_markdown(structs_by_name: dict[str, StructInfo], elements: list[str]) -> str:
    lines: list[str] = []
    lines.append("# gtopt JSON Field Reference\n")
    lines.append(
        "Auto-generated from C++ header comments. "
        "Fields that accept `number|array|string` can be a scalar, "
        "an inline array, or a filename pointing to an external Parquet/CSV file.\n"
    )

    for name in elements:
        info = structs_by_name.get(name)
        if info is None:
            continue
        anchor = name.lower()
        lines.append(f"## {name} {{#{anchor}}}\n")
        if info.brief:
            lines.append(f"*{info.brief}*\n")
        if info.details:
            lines.append(f"{info.details}\n")
        lines.append("")
        lines.append(
            "| Field | C++ Type | JSON Type | Units | Required | Description |"
        )
        lines.append(
            "|-------|----------|-----------|-------|----------|-------------|"
        )
        for f in info.fields:
            lines.append(_md_field_row(f))
        lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# HTML renderer
# ---------------------------------------------------------------------------

_HTML_HEADER = """\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>gtopt JSON Field Reference</title>
<style>
  body { font-family: Arial, sans-serif; max-width: 1100px; margin: auto; padding: 2rem; }
  h1 { color: #2c3e50; }
  h2 { color: #2980b9; border-bottom: 1px solid #ddd; padding-bottom: 4px; }
  table { border-collapse: collapse; width: 100%; margin-bottom: 2rem; }
  th { background: #2980b9; color: white; padding: 6px 10px; text-align: left; }
  td { border: 1px solid #ddd; padding: 5px 10px; vertical-align: top; }
  tr:nth-child(even) { background: #f7f9fb; }
  code { background: #f0f0f0; border-radius: 3px; padding: 1px 4px; font-size: 0.9em; }
  .req { color: #c0392b; font-weight: bold; }
  .toc a { display: block; margin: 2px 0; }
  nav { background: #f0f4f8; padding: 1rem; border-radius: 6px; margin-bottom: 2rem; }
</style>
</head>
<body>
<h1>gtopt JSON Field Reference</h1>
<p>Auto-generated from C++ header comments.
Fields with JSON type <code>number|array|string</code> accept a scalar constant,
an inline array, or a filename referencing an external Parquet/CSV file.</p>
<nav>
<strong>Contents</strong><br>
"""


def render_html(structs_by_name: dict[str, StructInfo], elements: list[str]) -> str:
    parts: list[str] = [_HTML_HEADER]

    for name in elements:
        if name in structs_by_name:
            anchor = name.lower()
            parts.append(f'<a href="#{anchor}">{name}</a>\n')

    parts.append("</nav>\n")

    for name in elements:
        info = structs_by_name.get(name)
        if info is None:
            continue
        anchor = name.lower()
        parts.append(f'<h2 id="{anchor}">{name}</h2>\n')
        if info.brief:
            parts.append(f"<p><em>{info.brief}</em></p>\n")
        if info.details:
            parts.append(f"<p>{info.details}</p>\n")
        parts.append(
            "<table>\n<tr>"
            "<th>Field</th><th>C++ Type</th><th>JSON Type</th>"
            "<th>Units</th><th>Required</th><th>Description</th>"
            "</tr>\n"
        )
        for f in info.fields:
            req_str = '<span class="req">Yes</span>' if f.required else "No"
            units_str = f"<code>{f.units}</code>" if f.units else "—"
            parts.append(
                f"<tr>"
                f"<td><code>{f.name}</code></td>"
                f"<td><code>{f.cpp_type}</code></td>"
                f"<td>{f.json_type}</td>"
                f"<td>{units_str}</td>"
                f"<td>{req_str}</td>"
                f"<td>{f.description}</td>"
                f"</tr>\n"
            )
        parts.append("</table>\n")

    parts.append("</body>\n</html>\n")
    return "".join(parts)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _find_repo_root(start: Path) -> Path:
    """Walk up until we find a directory containing 'include/gtopt'."""
    current = start.resolve()
    for parent in [current, *current.parents]:
        if (parent / "include" / "gtopt").is_dir():
            return parent
    return current


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract gtopt field metadata from C++ headers and generate docs."
    )
    parser.add_argument(
        "header_dir",
        nargs="?",
        help="Path to include/gtopt (auto-detected if omitted)",
    )
    parser.add_argument(
        "--output",
        "-o",
        default="-",
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "--format",
        "-f",
        choices=["md", "html"],
        default="md",
        help="Output format: md (Markdown) or html (default: md)",
    )
    parser.add_argument(
        "--elements",
        "-e",
        nargs="+",
        metavar="ELEMENT",
        help="Restrict output to these element names (e.g. Generator Demand)",
    )
    args = parser.parse_args(argv)

    # Resolve header directory
    if args.header_dir:
        header_dir = Path(args.header_dir)
    else:
        repo_root = _find_repo_root(Path(__file__).parent)
        header_dir = repo_root / "include" / "gtopt"

    if not header_dir.is_dir():
        print(f"Error: header directory '{header_dir}' not found.", file=sys.stderr)
        return 1

    # Parse all .hpp files
    structs_by_name: dict[str, StructInfo] = {}
    for hpp in sorted(header_dir.glob("*.hpp")):
        for s in parse_header(hpp):
            structs_by_name[s.struct_name] = s

    elements = args.elements if args.elements else ALL_ELEMENTS

    # Render
    if args.format == "html":
        output = render_html(structs_by_name, elements)
    else:
        output = render_markdown(structs_by_name, elements)

    if args.output == "-":
        print(output)
    else:
        Path(args.output).write_text(output, encoding="utf-8")
        print(f"Written to {args.output}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
