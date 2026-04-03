# SPDX-License-Identifier: BSD-3-Clause
"""Validation checks for gtopt PAMPL / TAMPL constraint files.

Each check function returns a list of :class:`Finding` objects describing
any issues found.  The :func:`run_all_checks` orchestrator calls every
enabled check and returns the combined list.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from enum import Enum
from typing import Callable


# ── Severity / Finding ──────────────────────────────────────────────────────


class Severity(Enum):
    """Finding severity levels."""

    CRITICAL = "CRITICAL"
    WARNING = "WARNING"
    NOTE = "NOTE"


@dataclass
class Finding:
    """A single validation finding."""

    check_id: str
    severity: Severity
    message: str
    action: str = ""
    line: int | None = None
    file: str = ""


# ── Known gtopt element types and LP variables ──────────────────────────────

ELEMENT_TYPES = {
    "generator",
    "demand",
    "line",
    "battery",
    "converter",
    "bus",
    "reservoir",
    "turbine",
    "junction",
    "waterway",
    "flow",
    "filtration",
    "flow_right",
    "reserve_zone",
    "reserve_provision",
}

LP_VARIABLES = {
    "generation",
    "load",
    "fail",
    "flow",
    "flowp",
    "theta",
    "energy",
    "charge",
    "discharge",
    "volume",
    "spill",
    "capacity",
    "capainst",
    "pmax",
    "lmax",
}

COMPARISON_OPERATORS = {"<=", ">=", "="}

# Regex patterns for PAMPL syntax elements
_RE_CONSTRAINT_HEADER = re.compile(
    r"^\s*(?:inactive\s+)?constraint\s+(\w+)", re.MULTILINE
)
_RE_PARAM_DECL = re.compile(r"^\s*param\s+(\w+)", re.MULTILINE)
_RE_ELEMENT_REF = re.compile(
    r"\b(\w+)\s*\(\s*(?:'([^']*)'|\"([^\"]*)\"|all)\s*\)\s*\.\s*(\w+)"
)
_RE_FOR_CLAUSE = re.compile(r"\bfor\s*\(")
_RE_SUM_EXPR = re.compile(r"\bsum\s*\(")
_RE_TEMPLATE_VAR = re.compile(r"\{\{.*?\}\}")
_RE_TEMPLATE_BLOCK = re.compile(r"\{%.*?%\}")
_RE_MONTHLY_PARAM = re.compile(r"\bparam\s+\w+\s*\[\s*month\s*\]")
_RE_PARAM_VALUE = re.compile(r"\bparam\s+(\w+)\s*(?:\[\s*month\s*\])?\s*=\s*")


# ── Individual check functions ──────────────────────────────────────────────


def check_syntax(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check basic PAMPL syntax: balanced brackets, quotes, comment style."""
    findings: list[Finding] = []
    lines = source.splitlines()

    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    in_template = _has_template_syntax(source)

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line)

        # Strip template variables {{ }} and {% %} once per line
        clean = re.sub(r"\{\{.*?\}\}", "", stripped)
        clean = re.sub(r"\{%.*?%\}", "", clean)

        # Check quote balance on each line (single and double)
        for quote_char in ('"', "'"):
            count = clean.count(quote_char)
            if count % 2 != 0:
                findings.append(
                    Finding(
                        check_id="syntax",
                        severity=Severity.WARNING,
                        message=f"Unbalanced {quote_char} quote on line {lineno}",
                        line=lineno,
                        file=filename,
                        action="Check that all string literals are properly closed.",
                    )
                )

        for ch in stripped:
            if ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth -= 1
            elif ch == "[":
                bracket_depth += 1
            elif ch == "]":
                bracket_depth -= 1
            elif ch == "{" and not in_template:
                brace_depth += 1
            elif ch == "}" and not in_template:
                brace_depth -= 1

            if paren_depth < 0:
                findings.append(
                    Finding(
                        check_id="syntax",
                        severity=Severity.CRITICAL,
                        message=f"Unmatched ')' on line {lineno}",
                        line=lineno,
                        file=filename,
                        action="Add the missing '(' or remove the extra ')'.",
                    )
                )
                paren_depth = 0
            if bracket_depth < 0:
                findings.append(
                    Finding(
                        check_id="syntax",
                        severity=Severity.CRITICAL,
                        message=f"Unmatched ']' on line {lineno}",
                        line=lineno,
                        file=filename,
                        action="Add the missing '[' or remove the extra ']'.",
                    )
                )
                bracket_depth = 0

    if paren_depth != 0:
        findings.append(
            Finding(
                check_id="syntax",
                severity=Severity.CRITICAL,
                message=f"Unbalanced parentheses: {paren_depth} unclosed '('",
                file=filename,
                action="Ensure every '(' has a matching ')'.",
            )
        )
    if bracket_depth != 0:
        findings.append(
            Finding(
                check_id="syntax",
                severity=Severity.CRITICAL,
                message=f"Unbalanced brackets: {bracket_depth} unclosed '['",
                file=filename,
                action="Ensure every '[' has a matching ']'.",
            )
        )

    return findings


def check_semicolons(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check that every constraint and param declaration ends with ';'."""
    findings: list[Finding] = []
    lines = source.splitlines()

    # Track whether we are inside a statement (constraint or param)
    in_statement = False
    statement_start = 0

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line).strip()
        if not stripped:
            continue

        # Detect statement starts (constraint or param)
        if re.match(
            r"^\s*(?:(?:inactive\s+)?constraint\s+\w+|param\s+\w+)",
            stripped,
        ):
            if in_statement:
                findings.append(
                    Finding(
                        check_id="semicolons",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Statement starting at line {statement_start} "
                            f"appears to be missing a terminating ';'"
                        ),
                        line=statement_start,
                        file=filename,
                        action="Add ';' at the end of the statement.",
                    )
                )
            in_statement = True
            statement_start = lineno

        # A bare expression (not starting with keyword) also starts a statement
        if not in_statement and not stripped.startswith("#"):
            # Could be a bare expression like: generator(...) <= 100;
            if _RE_ELEMENT_REF.search(stripped) or _RE_SUM_EXPR.search(stripped):
                in_statement = True
                statement_start = lineno

        if ";" in stripped:
            in_statement = False

    if in_statement:
        findings.append(
            Finding(
                check_id="semicolons",
                severity=Severity.CRITICAL,
                message=(
                    f"Statement starting at line {statement_start} "
                    f"is missing a terminating ';'"
                ),
                line=statement_start,
                file=filename,
                action="Add ';' at the end of the statement.",
            )
        )

    return findings


def check_constraint_names(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check constraint header syntax and naming conventions."""
    findings: list[Finding] = []
    lines = source.splitlines()

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line).strip()

        # Check for 'constraint' keyword without a name
        if re.match(r"^\s*(?:inactive\s+)?constraint\s*:", stripped):
            findings.append(
                Finding(
                    check_id="constraint_names",
                    severity=Severity.CRITICAL,
                    message=f"Missing constraint name after 'constraint' on line {lineno}",
                    line=lineno,
                    file=filename,
                    action="Add a valid identifier after 'constraint'.",
                )
            )

        # Check for 'inactive' without 'constraint'
        match = re.match(r"^\s*inactive\s+(\w+)", stripped)
        if match and match.group(1) != "constraint":
            findings.append(
                Finding(
                    check_id="constraint_names",
                    severity=Severity.CRITICAL,
                    message=(
                        f"'inactive' must be followed by 'constraint' on line "
                        f"{lineno}, got '{match.group(1)}'"
                    ),
                    line=lineno,
                    file=filename,
                    action="Use 'inactive constraint <name>:' syntax.",
                )
            )

        # Check constraint header missing colon
        header_match = re.match(
            r"^\s*(?:inactive\s+)?constraint\s+(\w+)\s*(?:\"[^\"]*\"|'[^']*')?\s*$",
            stripped,
        )
        if header_match:
            findings.append(
                Finding(
                    check_id="constraint_names",
                    severity=Severity.WARNING,
                    message=(
                        f"Constraint '{header_match.group(1)}' on line {lineno} "
                        f"may be missing the ':' separator"
                    ),
                    line=lineno,
                    file=filename,
                    action="Add ':' after the constraint name/description.",
                )
            )

    return findings


def check_param_declarations(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check param declarations for common issues."""
    findings: list[Finding] = []
    lines = source.splitlines()

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line).strip()

        # param without '='
        if re.match(r"^\s*param\s+\w+", stripped) and "=" not in stripped:
            # Could be a multi-line param; check if next lines have '='
            # For simplicity, just warn if the line has a ';' but no '='
            if ";" in stripped:
                findings.append(
                    Finding(
                        check_id="param_declarations",
                        severity=Severity.CRITICAL,
                        message=f"Param on line {lineno} is missing '=' assignment",
                        line=lineno,
                        file=filename,
                        action="Use 'param <name> = <value>;' syntax.",
                    )
                )

        # Monthly param with wrong bracket syntax
        monthly_match = re.match(r"^\s*param\s+\w+\s*\[\s*(\w+)\s*\]", stripped)
        if monthly_match and monthly_match.group(1) != "month":
            findings.append(
                Finding(
                    check_id="param_declarations",
                    severity=Severity.WARNING,
                    message=(
                        f"Param on line {lineno} uses index dimension "
                        f"'{monthly_match.group(1)}'; only 'month' is "
                        f"supported by the PAMPL parser"
                    ),
                    line=lineno,
                    file=filename,
                    action="Use 'param <name>[month] = [...]' for indexed params.",
                )
            )

    return findings


def check_element_references(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check that element references use known gtopt element types and variables."""
    findings: list[Finding] = []

    for match in _RE_ELEMENT_REF.finditer(source):
        elem_type = match.group(1)
        variable = match.group(4)

        if elem_type not in ELEMENT_TYPES:
            # Find line number
            lineno = source[: match.start()].count("\n") + 1
            findings.append(
                Finding(
                    check_id="element_references",
                    severity=Severity.WARNING,
                    message=(
                        f"Unknown element type '{elem_type}' on line {lineno}; "
                        f"known types: {', '.join(sorted(ELEMENT_TYPES))}"
                    ),
                    line=lineno,
                    file=filename,
                    action=f"Use one of: {', '.join(sorted(ELEMENT_TYPES))}.",
                )
            )

        if variable not in LP_VARIABLES:
            lineno = source[: match.start()].count("\n") + 1
            findings.append(
                Finding(
                    check_id="element_references",
                    severity=Severity.NOTE,
                    message=(
                        f"LP variable '{variable}' on line {lineno} is not in "
                        f"the standard set; it may be a custom variable"
                    ),
                    line=lineno,
                    file=filename,
                    action="Verify this variable name matches the LP model.",
                )
            )

    return findings


def check_operator_usage(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check that constraint expressions contain valid comparison operators."""
    findings: list[Finding] = []

    # Process each statement (delimited by ';')
    # Skip param declarations and template blocks
    clean = _strip_template_syntax(source)
    statements = _split_statements(clean)

    for stmt_text, stmt_start_line in statements:
        stripped = stmt_text.strip()
        if not stripped:
            continue

        # Skip param declarations
        if stripped.startswith("param "):
            continue

        # Remove the constraint header if present
        header_match = re.match(
            r"(?:inactive\s+)?constraint\s+\w+\s*(?:\"[^\"]*\"|'[^']*')?\s*:\s*",
            stripped,
        )
        expr = stripped[header_match.end() :] if header_match else stripped

        # Check for comparison operator
        has_operator = False
        for op in COMPARISON_OPERATORS:
            if op in expr:
                has_operator = True
                break

        if not has_operator and expr:
            findings.append(
                Finding(
                    check_id="operator_usage",
                    severity=Severity.WARNING,
                    message=(
                        f"Expression near line {stmt_start_line} lacks a "
                        f"comparison operator (<=, >=, =)"
                    ),
                    line=stmt_start_line,
                    file=filename,
                    action="Add a comparison operator (<=, >=, or =).",
                )
            )

        # Check for accidental use of == instead of =
        if "==" in expr:
            findings.append(
                Finding(
                    check_id="operator_usage",
                    severity=Severity.WARNING,
                    message=(
                        f"'==' found near line {stmt_start_line}; "
                        f"use '=' for equality constraints"
                    ),
                    line=stmt_start_line,
                    file=filename,
                    action="Replace '==' with '='.",
                )
            )

        # Check for accidental use of != or <>
        if "!=" in expr or "<>" in expr:
            findings.append(
                Finding(
                    check_id="operator_usage",
                    severity=Severity.CRITICAL,
                    message=(
                        f"Inequality operator found near line "
                        f"{stmt_start_line}; not supported in "
                        f"LP constraints"
                    ),
                    line=stmt_start_line,
                    file=filename,
                    action="LP constraints only support <=, >=, and =.",
                )
            )

    return findings


def check_domain_clauses(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check for-clause syntax in constraint expressions."""
    findings: list[Finding] = []
    lines = source.splitlines()

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line)

        # Check for 'for(' without proper domain syntax
        for match in re.finditer(r"\bfor\s*\(([^)]*)\)", stripped):
            clause = match.group(1)
            # Should contain 'in' keyword
            if "in" not in clause:
                findings.append(
                    Finding(
                        check_id="domain_clauses",
                        severity=Severity.WARNING,
                        message=(
                            f"for-clause on line {lineno} missing 'in' "
                            f"keyword for domain specification"
                        ),
                        line=lineno,
                        file=filename,
                        action="Use 'for(dim in {values})' or 'for(dim in lo..hi)'.",
                    )
                )

            # Check domain dimensions
            valid_dims = {"stage", "block", "scenario"}
            dim_matches = re.findall(r"(\w+)\s+in\b", clause)
            for dim in dim_matches:
                if dim not in valid_dims:
                    findings.append(
                        Finding(
                            check_id="domain_clauses",
                            severity=Severity.NOTE,
                            message=(
                                f"Domain dimension '{dim}' on line {lineno} "
                                f"is not one of the standard dimensions: "
                                f"{', '.join(sorted(valid_dims))}"
                            ),
                            line=lineno,
                            file=filename,
                            action="Standard dimensions: stage, block, scenario.",
                        )
                    )

    return findings


def check_template_variables(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check Jinja2 template variable/block syntax in .tampl files."""
    findings: list[Finding] = []
    lines = source.splitlines()

    for lineno, line in enumerate(lines, start=1):
        # Check for unclosed {{ or {% blocks
        open_var = line.count("{{")
        close_var = line.count("}}")
        if open_var != close_var:
            findings.append(
                Finding(
                    check_id="template_variables",
                    severity=Severity.CRITICAL,
                    message=f"Unbalanced '{{{{' / '}}}}' template variable on line {lineno}",
                    line=lineno,
                    file=filename,
                    action="Ensure every '{{' has a matching '}}'.",
                )
            )

        open_block = line.count("{%")
        close_block = line.count("%}")
        if open_block != close_block:
            findings.append(
                Finding(
                    check_id="template_variables",
                    severity=Severity.CRITICAL,
                    message=f"Unbalanced '{{% %}}' template block on line {lineno}",
                    line=lineno,
                    file=filename,
                    action="Ensure every '{%' has a matching '%}'.",
                )
            )

    # Check for unmatched {% for %} / {% endfor %}
    for_count = len(re.findall(r"\{%\s*for\b", source))
    endfor_count = len(re.findall(r"\{%\s*endfor\b", source))
    if for_count != endfor_count:
        findings.append(
            Finding(
                check_id="template_variables",
                severity=Severity.CRITICAL,
                message=(
                    f"Unmatched template loops: {for_count} '{{% for %}}' vs "
                    f"{endfor_count} '{{% endfor %}}'"
                ),
                file=filename,
                action="Ensure every '{% for %}' has a matching '{% endfor %}'.",
            )
        )

    # Check for {% if %} / {% endif %}
    if_count = len(re.findall(r"\{%\s*if\b", source))
    endif_count = len(re.findall(r"\{%\s*endif\b", source))
    if if_count != endif_count:
        findings.append(
            Finding(
                check_id="template_variables",
                severity=Severity.CRITICAL,
                message=(
                    f"Unmatched template conditionals: {if_count} '{{% if %}}' vs "
                    f"{endif_count} '{{% endif %}}'"
                ),
                file=filename,
                action="Ensure every '{% if %}' has a matching '{% endif %}'.",
            )
        )

    return findings


def check_duplicate_names(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Check for duplicate constraint names and param names within a file."""
    findings: list[Finding] = []

    constraint_names: dict[str, int] = {}
    param_names: dict[str, int] = {}

    lines = source.splitlines()
    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line).strip()

        # Constraint names
        header_match = re.match(r"^\s*(?:inactive\s+)?constraint\s+(\w+)", stripped)
        if header_match:
            name = header_match.group(1)
            if name in constraint_names:
                findings.append(
                    Finding(
                        check_id="duplicate_names",
                        severity=Severity.WARNING,
                        message=(
                            f"Duplicate constraint name '{name}' on line {lineno} "
                            f"(first defined on line {constraint_names[name]})"
                        ),
                        line=lineno,
                        file=filename,
                        action="Use unique constraint names.",
                    )
                )
            else:
                constraint_names[name] = lineno

        # Param names
        param_match = re.match(r"^\s*param\s+(\w+)", stripped)
        if param_match:
            name = param_match.group(1)
            if name in param_names:
                findings.append(
                    Finding(
                        check_id="duplicate_names",
                        severity=Severity.WARNING,
                        message=(
                            f"Duplicate param name '{name}' on line {lineno} "
                            f"(first defined on line {param_names[name]})"
                        ),
                        line=lineno,
                        file=filename,
                        action="Use unique parameter names.",
                    )
                )
            else:
                param_names[name] = lineno

    return findings


def check_inactive_constraints(
    source: str,
    filename: str = "",
) -> list[Finding]:
    """Report inactive constraints as notes for visibility."""
    findings: list[Finding] = []
    lines = source.splitlines()

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line).strip()

        match = re.match(r"^\s*inactive\s+constraint\s+(\w+)", stripped)
        if match:
            findings.append(
                Finding(
                    check_id="inactive_constraints",
                    severity=Severity.NOTE,
                    message=f"Constraint '{match.group(1)}' on line {lineno} is marked inactive",
                    line=lineno,
                    file=filename,
                    action="Remove 'inactive' keyword when ready to activate.",
                )
            )

    return findings


# ── Summary statistics ──────────────────────────────────────────────────────


@dataclass
class PamplStats:
    """Summary statistics for a PAMPL/TAMPL file."""

    filename: str = ""
    num_constraints: int = 0
    num_params: int = 0
    num_inactive: int = 0
    num_lines: int = 0
    has_templates: bool = False
    element_types_used: list[str] = field(default_factory=list)
    variables_used: list[str] = field(default_factory=list)


def compute_stats(source: str, filename: str = "") -> PamplStats:
    """Compute summary statistics for a PAMPL/TAMPL source."""
    stats = PamplStats(filename=filename)
    stats.num_lines = len(source.splitlines())
    stats.has_templates = _has_template_syntax(source)

    # Count named constraints (with 'constraint' header)
    constraints = _RE_CONSTRAINT_HEADER.findall(source)
    stats.num_constraints = len(constraints)

    # Also count bare expressions (no 'constraint' keyword) as constraints.
    # Named constraints are excluded by the regex match below.
    clean = _strip_template_syntax(source)
    for stmt_text, _ in _split_statements(clean):
        stripped = stmt_text.strip()
        if stripped and not stripped.startswith("param "):
            if not re.match(r"(?:inactive\s+)?constraint\s+", stripped):
                # Bare expression counts as a constraint
                stats.num_constraints += 1

    params = _RE_PARAM_DECL.findall(source)
    stats.num_params = len(params)

    inactive_count = len(re.findall(r"\binactive\s+constraint\b", source))
    stats.num_inactive = inactive_count

    elem_types: set[str] = set()
    variables: set[str] = set()
    for match in _RE_ELEMENT_REF.finditer(source):
        elem_types.add(match.group(1))
        variables.add(match.group(4))

    stats.element_types_used = sorted(elem_types)
    stats.variables_used = sorted(variables)

    return stats


# ── Orchestrator ────────────────────────────────────────────────────────────


CheckFunction = Callable[[str, str], list[Finding]]

_CHECK_FUNCTIONS: dict[str, CheckFunction] = {
    "syntax": check_syntax,
    "semicolons": check_semicolons,
    "constraint_names": check_constraint_names,
    "param_declarations": check_param_declarations,
    "element_references": check_element_references,
    "operator_usage": check_operator_usage,
    "domain_clauses": check_domain_clauses,
    "template_variables": check_template_variables,
    "duplicate_names": check_duplicate_names,
    "inactive_constraints": check_inactive_constraints,
}


def run_all_checks(
    source: str,
    enabled_checks: set[str] | None = None,
    filename: str = "",
) -> list[Finding]:
    """Run all enabled checks and return combined findings.

    Parameters
    ----------
    source
        The PAMPL/TAMPL source text.
    enabled_checks
        Set of check IDs to run.  When None, all checks are run.
    filename
        Source filename for diagnostics.

    Returns
    -------
    list[Finding]
        Combined findings from all checks.
    """
    findings: list[Finding] = []

    for check_id, check_fn in _CHECK_FUNCTIONS.items():
        if enabled_checks is not None and check_id not in enabled_checks:
            continue
        findings.extend(check_fn(source, filename))

    # Sort by severity (CRITICAL first), then by line number
    severity_order = {Severity.CRITICAL: 0, Severity.WARNING: 1, Severity.NOTE: 2}
    findings.sort(key=lambda f: (severity_order.get(f.severity, 9), f.line or 0))

    return findings


# ── Helpers ─────────────────────────────────────────────────────────────────


def _strip_comments(line: str) -> str:
    """Remove # and // comments from a line (respecting quoted strings)."""
    result: list[str] = []
    in_single = False
    in_double = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "'" and not in_double:
            in_single = not in_single
        elif not in_single and not in_double:
            if ch == "#":
                break
            if ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
                break
        result.append(ch)
        i += 1
    return "".join(result)


def _strip_template_syntax(source: str) -> str:
    """Remove Jinja2 {{ }} and {% %} blocks for analysis."""
    result = re.sub(r"\{\{.*?\}\}", '""', source)
    result = re.sub(r"\{%.*?%\}", "", result)
    return result


def _has_template_syntax(source: str) -> bool:
    """Return True if the source contains Jinja2 template syntax."""
    return bool(_RE_TEMPLATE_VAR.search(source) or _RE_TEMPLATE_BLOCK.search(source))


def _split_statements(
    source: str,
) -> list[tuple[str, int]]:
    """Split source into statements (delimited by ';').

    Returns a list of (statement_text, start_line_number) tuples.
    """
    statements: list[tuple[str, int]] = []
    lines = source.splitlines()

    current_stmt: list[str] = []
    start_line = 1

    for lineno, line in enumerate(lines, start=1):
        stripped = _strip_comments(line)

        # Handle semicolons within the line
        parts = stripped.split(";")
        for i, part in enumerate(parts):
            if not current_stmt:
                start_line = lineno
            current_stmt.append(part)

            if i < len(parts) - 1:
                # Semicolon was found
                stmt = "\n".join(current_stmt)
                if stmt.strip():
                    statements.append((stmt, start_line))
                current_stmt = []

    # Handle any remaining text (no trailing semicolon)
    if current_stmt:
        stmt = "\n".join(current_stmt)
        if stmt.strip():
            statements.append((stmt, start_line))

    return statements
