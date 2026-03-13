# SPDX-License-Identifier: BSD-3-Clause
"""Static LP file analyser and formatted report generator for gtopt_check_lp."""

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import _colors as col
from ._compress import read_lp_text

_LARGE_COEFF_THRESHOLD = 1e10
_SMALL_COEFF_THRESHOLD = 1e-10

# Matches a variable name token (used to collect names from each section).
_VAR_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_:.]*")


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass
class VariableBounds:
    """Bounds for one variable parsed from the Bounds section."""

    name: str
    lb: float = 0.0
    ub: float = float("inf")


@dataclass
class LPStats:
    """Statistics and potential issues discovered in an LP file."""

    n_vars: int = 0
    n_constraints: int = 0
    n_integers: int = 0
    n_binary: int = 0
    has_objective: bool = False
    is_minimization: bool = True

    # Numerical range
    max_abs_coeff: float = 0.0
    min_abs_nonzero_coeff: float = float("inf")

    # Issues found
    infeasible_bounds: list[VariableBounds] = field(default_factory=list)
    empty_constraints: list[str] = field(default_factory=list)
    fixed_variables: list[str] = field(default_factory=list)
    duplicate_constraint_names: list[str] = field(default_factory=list)
    large_coeff_constraints: list[str] = field(default_factory=list)
    small_coeff_constraints: list[str] = field(default_factory=list)
    unbounded_variables: list[str] = field(default_factory=list)

    def has_issues(self) -> bool:
        """Return True when at least one type of potential issue was detected."""
        return bool(
            self.infeasible_bounds
            or self.empty_constraints
            or self.duplicate_constraint_names
            or self.large_coeff_constraints
        )


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

_SECTIONS_START: dict[str, str] = {
    "minimize": "objective",
    "minimum": "objective",
    "min": "objective",
    "maximize": "objective",
    "maximum": "objective",
    "max": "objective",
    "subject to": "constraints",
    "such that": "constraints",
    "st": "constraints",
    "s.t.": "constraints",
    "bounds": "bounds",
    "bound": "bounds",
    "general": "general",
    "generals": "general",
    "integer": "general",
    "integers": "general",
    "binary": "binary",
    "binaries": "binary",
    "end": "end",
}


def _strip_comments(line: str) -> str:
    """Remove LP-format inline comments (text after ``\\``)."""
    idx = line.find("\\")
    return line[:idx].strip() if idx >= 0 else line.strip()


def _parse_coefficients(expr: str) -> list[float]:
    """Extract all numeric coefficients from an LP constraint expression."""
    return [float(m) for m in re.findall(r"[+-]?\s*\d+\.?\d*(?:[eE][+-]?\d+)?", expr)]


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def analyze_lp_file(lp_path: Path) -> LPStats:  # noqa: PLR0912, PLR0915
    """
    Parse *lp_path* (CPLEX LP format, plain or gzip-compressed) and return
    an :class:`LPStats` object describing problem statistics and any detected
    issues.

    Recognised compressed extensions: ``.lp.gz``, ``.lp.gzip``.

    Raises :class:`FileNotFoundError` when *lp_path* does not exist or cannot
    be read.
    """
    stats = LPStats()
    bounds: dict[str, VariableBounds] = {}
    constraint_names: list[str] = []
    all_var_names: set[str] = set()

    try:
        text = read_lp_text(lp_path)
    except OSError as exc:
        raise FileNotFoundError(f"Cannot read LP file: {exc}") from exc

    section = "preamble"
    current_con_name = ""
    current_con_expr = ""

    def _flush_constraint(name: str, expr: str) -> None:
        if not name:
            return
        coeffs = _parse_coefficients(expr.split(":")[1] if ":" in expr else expr)
        if not coeffs:
            stats.empty_constraints.append(name)
        else:
            abs_coeffs = [abs(c) for c in coeffs if c != 0.0]
            if abs_coeffs:
                local_max = max(abs_coeffs)
                local_min = min(abs_coeffs)
                stats.max_abs_coeff = max(stats.max_abs_coeff, local_max)
                stats.min_abs_nonzero_coeff = min(
                    stats.min_abs_nonzero_coeff, local_min
                )
                if local_max >= _LARGE_COEFF_THRESHOLD:
                    stats.large_coeff_constraints.append(name)
                if 0.0 < local_min <= _SMALL_COEFF_THRESHOLD:
                    stats.small_coeff_constraints.append(name)

    for raw_line in text.splitlines():
        line = _strip_comments(raw_line)
        if not line:
            continue

        lower = line.lower()

        matched_section = None
        for key, sec in _SECTIONS_START.items():
            if (
                lower == key
                or lower.startswith(key + " ")
                or lower.startswith(key + ":")
            ):
                matched_section = sec
                break

        if matched_section is not None:
            if section == "constraints" and current_con_name:
                _flush_constraint(current_con_name, current_con_expr)
                current_con_name = ""
                current_con_expr = ""
            section = matched_section
            if section == "objective":
                stats.has_objective = True
                stats.is_minimization = "min" in lower
            continue

        if section == "end":
            break

        if section == "objective":
            all_var_names.update(
                v for v in _VAR_RE.findall(line) if not re.match(r"^[eE]$", v)
            )

        elif section == "constraints":
            con_match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_:.()\[\]]*)\s*:", line)
            if con_match:
                if current_con_name:
                    _flush_constraint(current_con_name, current_con_expr)
                current_con_name = con_match.group(1)
                current_con_expr = line
                constraint_names.append(current_con_name)
                stats.n_constraints += 1
                all_var_names.update(
                    v for v in _VAR_RE.findall(line) if not re.match(r"^[eE]$", v)
                )
            elif current_con_name:
                current_con_expr += " " + line
                all_var_names.update(
                    v for v in _VAR_RE.findall(line) if not re.match(r"^[eE]$", v)
                )

        elif section == "bounds":
            line_clean = line.replace(" ", "")
            free_m = re.match(
                r"^([A-Za-z_][A-Za-z0-9_:.]*)>=?-[Ii]nf$", line_clean
            ) or re.match(r"^([A-Za-z_][A-Za-z0-9_:.]*)[Ff]ree$", line_clean)
            if free_m:
                v = free_m.group(1)
                bounds[v] = VariableBounds(name=v, lb=float("-inf"), ub=float("inf"))
                all_var_names.add(v)
                continue

            m = re.match(
                r"^([+-]?[\d.eE+\-]+|[+-]?[Ii]nf)\s*<=?\s*"
                r"([A-Za-z_][A-Za-z0-9_:.]*)\s*<=?\s*"
                r"([+-]?[\d.eE+\-]+|[+-]?[Ii]nf)$",
                line_clean,
                re.IGNORECASE,
            )
            if m:
                v = m.group(2)
                lb_s, ub_s = m.group(1), m.group(3)
                lb = float("-inf") if "inf" in lb_s.lower() else float(lb_s)
                ub = float("inf") if "inf" in ub_s.lower() else float(ub_s)
                bounds[v] = VariableBounds(name=v, lb=lb, ub=ub)
                all_var_names.add(v)
                continue

            m2 = re.match(
                r"^([A-Za-z_][A-Za-z0-9_:.]*)\s*>=?\s*"
                r"([+-]?[\d.eE+\-]+|[+-]?[Ii]nf)$",
                line_clean,
                re.IGNORECASE,
            )
            if m2:
                v = m2.group(1)
                lb_s = m2.group(2)
                lb = float("-inf") if "inf" in lb_s.lower() else float(lb_s)
                existing = bounds.get(v, VariableBounds(name=v))
                bounds[v] = VariableBounds(name=v, lb=lb, ub=existing.ub)
                all_var_names.add(v)
                continue

            m3 = re.match(
                r"^([A-Za-z_][A-Za-z0-9_:.]*)\s*<=?\s*"
                r"([+-]?[\d.eE+\-]+|[+-]?[Ii]nf)$",
                line_clean,
                re.IGNORECASE,
            )
            if m3:
                v = m3.group(1)
                ub_s = m3.group(2)
                ub = float("inf") if "inf" in ub_s.lower() else float(ub_s)
                existing = bounds.get(v, VariableBounds(name=v))
                bounds[v] = VariableBounds(name=v, lb=existing.lb, ub=ub)
                all_var_names.add(v)
                continue

            m4 = re.match(
                r"^([+-]?[\d.eE+\-]+|[+-]?[Ii]nf)\s*>=?\s*"
                r"([A-Za-z_][A-Za-z0-9_:.]*)$",
                line_clean,
                re.IGNORECASE,
            )
            if m4:
                v = m4.group(2)
                ub_s = m4.group(1)
                ub = float("inf") if "inf" in ub_s.lower() else float(ub_s)
                existing = bounds.get(v, VariableBounds(name=v))
                bounds[v] = VariableBounds(name=v, lb=existing.lb, ub=ub)
                all_var_names.add(v)
                continue

            all_var_names.update(
                v for v in _VAR_RE.findall(line) if not re.match(r"^[eE]$", v)
            )

        elif section == "general":
            stats.n_integers += len(_VAR_RE.findall(line))
            all_var_names.update(_VAR_RE.findall(line))

        elif section == "binary":
            stats.n_binary += len(_VAR_RE.findall(line))
            all_var_names.update(_VAR_RE.findall(line))

    if section == "constraints" and current_con_name:
        _flush_constraint(current_con_name, current_con_expr)

    stats.n_vars = len(all_var_names)

    for vb in bounds.values():
        if vb.lb > vb.ub:
            stats.infeasible_bounds.append(vb)
        elif vb.lb == vb.ub:
            stats.fixed_variables.append(vb.name)
        if vb.lb == float("-inf") and vb.ub == float("inf"):
            stats.unbounded_variables.append(vb.name)

    seen: set[str] = set()
    for name in constraint_names:
        if name in seen:
            stats.duplicate_constraint_names.append(name)
        seen.add(name)

    if stats.min_abs_nonzero_coeff == float("inf"):
        stats.min_abs_nonzero_coeff = 0.0

    return stats


def format_static_report(lp_path: Path, stats: LPStats) -> str:
    """Return a formatted, ANSI-coloured static-analysis report string."""

    def _c(code: str, text: str) -> str:
        return col.c(code, text)

    lines: list[str] = []
    lines.append(col.header(f"Static Analysis: {lp_path.name}"))

    lines.append(f"\n{_c(col._BOLD, 'Problem statistics')}")  # noqa: SLF001
    sense = "Minimization" if stats.is_minimization else "Maximization"
    lines.append(f"  Objective:      {sense}")
    lines.append(f"  Variables:      {stats.n_vars}")
    lines.append(f"  Constraints:    {stats.n_constraints}")
    if stats.n_integers:
        lines.append(f"  Integer vars:   {stats.n_integers}")
    if stats.n_binary:
        lines.append(f"  Binary vars:    {stats.n_binary}")
    if stats.max_abs_coeff > 0:
        lines.append(
            f"  Max |coeff|:    {stats.max_abs_coeff:.3e}"
            + (
                _c(col._RED, "  ← very large")  # noqa: SLF001
                if stats.max_abs_coeff >= _LARGE_COEFF_THRESHOLD
                else ""
            )
        )
    if 0 < stats.min_abs_nonzero_coeff < float("inf"):
        lines.append(
            f"  Min |coeff| ≠0: {stats.min_abs_nonzero_coeff:.3e}"
            + (
                _c(col._YELLOW, "  ← very small")  # noqa: SLF001
                if stats.min_abs_nonzero_coeff <= _SMALL_COEFF_THRESHOLD
                else ""
            )
        )
    coeff_ratio = (
        stats.max_abs_coeff / stats.min_abs_nonzero_coeff
        if stats.min_abs_nonzero_coeff > 0 and stats.max_abs_coeff > 0
        else 0.0
    )
    if coeff_ratio > 1e6:
        lines.append(
            f"  Coeff ratio:    {coeff_ratio:.3e}"
            + _c(col._YELLOW, "  ← poor numerical conditioning")  # noqa: SLF001
        )

    if not stats.has_issues():
        lines.append(
            f"\n{_c(col._GREEN, '✓ No obvious static infeasibilities detected.')}"  # noqa: SLF001
        )
    else:
        lines.append(
            f"\n{_c(col._BOLD, _c(col._RED, '⚠ Potential infeasibility causes:'))}"  # noqa: SLF001
        )

        if stats.infeasible_bounds:
            n_bounds = len(stats.infeasible_bounds)
            label = _c(
                col._RED,  # noqa: SLF001
                f"Conflicting variable bounds ({n_bounds}):",
            )
            lines.append(f"\n  {label} (lb > ub — variable domain is empty)")
            for vb in stats.infeasible_bounds[:20]:
                lines.append(f"    • {vb.name}: lb={vb.lb:g}  >  ub={vb.ub:g}")
            if len(stats.infeasible_bounds) > 20:
                lines.append(f"    … and {len(stats.infeasible_bounds) - 20} more")

        if stats.empty_constraints:
            n_empty = len(stats.empty_constraints)
            empty_label = _c(
                col._YELLOW,  # noqa: SLF001
                f"Empty constraints ({n_empty}):",
            )
            lines.append(f"\n  {empty_label} (no non-zero coefficients)")
            for name in stats.empty_constraints[:10]:
                lines.append(f"    • {name}")
            if len(stats.empty_constraints) > 10:
                lines.append(f"    … and {len(stats.empty_constraints) - 10} more")

        if stats.duplicate_constraint_names:
            dup_label = _c(
                col._YELLOW,  # noqa: SLF001
                f"Duplicate constraint names "
                f"({len(stats.duplicate_constraint_names)}):",
            )
            lines.append(f"\n  {dup_label}")
            for name in stats.duplicate_constraint_names[:10]:
                lines.append(f"    • {name}")

        if stats.large_coeff_constraints:
            large_label = _c(
                col._YELLOW,  # noqa: SLF001
                f"Constraints with very large coefficients"
                f" (≥ {_LARGE_COEFF_THRESHOLD:.0e}):",
            )
            lines.append(
                f"\n  {large_label} ({len(stats.large_coeff_constraints)} constraints)"
            )
            for name in stats.large_coeff_constraints[:10]:
                lines.append(f"    • {name}")
            if len(stats.large_coeff_constraints) > 10:
                lines.append(
                    f"    … and {len(stats.large_coeff_constraints) - 10} more"
                )

    if stats.fixed_variables:
        n_fixed = len(stats.fixed_variables)
        fixed_label = _c(
            col._CYAN,  # noqa: SLF001
            f"Fixed variables ({n_fixed}):",
        )
        lines.append(f"\n  {fixed_label} (lb == ub)")
        for name in stats.fixed_variables[:10]:
            lines.append(f"    • {name}")
        if len(stats.fixed_variables) > 10:
            lines.append(f"    … and {len(stats.fixed_variables) - 10} more")

    return "\n".join(lines)
