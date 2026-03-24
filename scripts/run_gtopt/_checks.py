# SPDX-License-Identifier: BSD-3-Clause
"""Pre-flight and post-flight check registry for run_gtopt.

Each check is a named function registered via ``@register_check``.
Checks can be individually enabled/disabled from the CLI or config.

Pre-flight checks run before gtopt:
  - ``json_syntax``   : validate JSON syntax
  - ``input_files``   : verify input files exist and are readable
  - ``output_dir``    : verify output directory is writable
  - ``check_json``    : run gtopt_check_json (--info + validation)

Post-flight checks run after gtopt:
  - ``check_output``  : run gtopt_check_output analysis
  - ``check_lp``      : analyse error LP files on failure
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Check registry
# ---------------------------------------------------------------------------


@dataclass
class CheckResult:
    """Result of a single check."""

    name: str
    passed: bool
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


_CheckFunc = Callable[..., CheckResult]
_PRE_CHECKS: dict[str, _CheckFunc] = {}
_POST_CHECKS: dict[str, _CheckFunc] = {}


def register_pre_check(name: str):
    """Decorator to register a pre-flight check."""

    def decorator(func):
        _PRE_CHECKS[name] = func
        return func

    return decorator


def register_post_check(name: str):
    """Decorator to register a post-flight check."""

    def decorator(func):
        _POST_CHECKS[name] = func
        return func

    return decorator


def available_checks() -> dict[str, list[str]]:
    """Return available check names by phase."""
    return {
        "pre": sorted(_PRE_CHECKS.keys()),
        "post": sorted(_POST_CHECKS.keys()),
    }


# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------

_NON_FILE_FIELDS = frozenset(
    {
        "uid",
        "name",
        "active",
        "bus",
        "bus_a",
        "bus_b",
        "type",
        "generator",
        "flow",
        "junction",
        "waterway",
        "reservoir",
        "turbine",
        "demand",
        "description",
        "version",
        "capacity",
        "conversion_rate",
        "efficiency",
        "embalse",
        "source_scenario",
        "probability_factor",
    }
)


@register_pre_check("json_syntax")
def check_json_syntax(json_path: Path, **_kwargs) -> CheckResult:
    """Validate JSON syntax of a planning file."""
    result = CheckResult(name="json_syntax", passed=True)

    if not json_path.is_file():
        result.passed = False
        result.errors.append(f"JSON file does not exist: {json_path}")
        return result

    try:
        with open(json_path, encoding="utf-8") as f:
            content = f.read()
    except OSError as exc:
        result.passed = False
        result.errors.append(f"cannot read {json_path}: {exc}")
        return result

    if not content.strip():
        result.passed = False
        result.errors.append(f"JSON file is empty: {json_path}")
        return result

    try:
        data = json.loads(content)
    except json.JSONDecodeError as exc:
        result.passed = False
        result.errors.append(
            f"JSON syntax error in {json_path}: "
            f"line {exc.lineno}, column {exc.colno}: {exc.msg}"
        )
        return result

    if not isinstance(data, dict):
        result.passed = False
        result.errors.append(f"JSON root must be an object, got {type(data).__name__}")
        return result

    for key in ("system", "simulation"):
        if key not in data:
            result.warnings.append(f"JSON missing '{key}' section")

    return result


@register_pre_check("input_files")
def check_input_files(json_path: Path, **_kwargs) -> CheckResult:
    """Validate that all input files referenced by the planning JSON exist."""
    result = CheckResult(name="input_files", passed=True)

    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        result.warnings.append(f"cannot load JSON for file check: {exc}")
        return result

    opts = data.get("options", {})
    input_dir = Path(opts.get("input_directory", "input"))
    if not input_dir.is_absolute():
        input_dir = json_path.parent / input_dir

    if not input_dir.is_dir():
        result.warnings.append(f"input_directory does not exist: {input_dir}")
        return result

    if not os.access(input_dir, os.R_OK):
        result.warnings.append(f"input_directory is not readable: {input_dir}")
        return result

    # Collect file references
    system = data.get("system", {})
    class_map = {
        "generator_array": "Generator",
        "generator_profile_array": "GeneratorProfile",
        "demand_array": "Demand",
        "demand_profile_array": "DemandProfile",
        "line_array": "Line",
        "battery_array": "Battery",
        "converter_array": "Converter",
        "flow_array": "Flow",
        "reservoir_array": "Reservoir",
        "turbine_array": "Turbine",
        "waterway_array": "Waterway",
        "reservoir_seepage_array": "ReservoirSeepage",
        "reservoir_discharge_limit_array": "ReservoirDischargeLimit",
        "reservoir_production_factor_array": "ReservoirProductionFactor",
    }

    checked: set[tuple[str, str]] = set()
    input_format = opts.get("input_format", "parquet")

    for array_key, class_name in class_map.items():
        elements = system.get(array_key, [])
        if not isinstance(elements, list):
            continue
        for elem in elements:
            if not isinstance(elem, dict):
                continue
            for field_name, value in elem.items():
                if field_name in _NON_FILE_FIELDS:
                    continue
                if not isinstance(value, str) or not value:
                    continue

                if "@" in value:
                    parts = value.split("@", 1)
                    dir_name, fname = parts[0], parts[1]
                else:
                    dir_name, fname = class_name, value

                key = (dir_name, fname)
                if key in checked:
                    continue
                checked.add(key)

                class_dir = input_dir / dir_name
                candidates = [
                    class_dir / f"{fname}.{ext}"
                    for ext in (
                        "parquet",
                        "parquet.gz",
                        "parquet.zst",
                        "csv",
                        "csv.gz",
                        "csv.zst",
                    )
                ]
                if class_dir.is_dir() and not any(c.is_file() for c in candidates):
                    result.warnings.append(
                        f"missing input file: {dir_name}/{fname}"
                        f".{input_format} (in {class_dir})"
                    )

    if not result.warnings:
        log.info(
            "input file check: %d reference(s) verified in %s",
            len(checked),
            input_dir,
        )
    return result


@register_pre_check("output_dir")
def check_output_directory(json_path: Path, **_kwargs) -> CheckResult:
    """Validate that the output directory is writable."""
    result = CheckResult(name="output_dir", passed=True)

    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return result

    opts = data.get("options", {})
    output_dir = Path(opts.get("output_directory", "output"))
    if not output_dir.is_absolute():
        output_dir = json_path.parent / output_dir

    if output_dir.is_dir():
        if not os.access(output_dir, os.W_OK):
            result.passed = False
            result.errors.append(f"output_directory not writable: {output_dir}")
    else:
        parent = output_dir.parent
        if parent.is_dir():
            if not os.access(parent, os.W_OK):
                result.passed = False
                result.errors.append(
                    f"cannot create output_directory: parent {parent} not writable"
                )
        else:
            result.passed = False
            result.errors.append(
                f"cannot create output_directory: parent {parent} does not exist"
            )

    return result


@register_pre_check("check_json")
def check_json_tool(json_path: Path, **_kwargs) -> CheckResult:
    """Run gtopt_check_json --info and full validation."""
    result = CheckResult(name="check_json", passed=True)

    check_bin = shutil.which("gtopt_check_json")
    if not check_bin:
        log.debug("gtopt_check_json not on PATH, skipping")
        return result

    # --info (display stats)
    cmd_info = [check_bin, "--info", str(json_path)]
    log.info("running: %s", " ".join(cmd_info))
    subprocess.run(cmd_info, check=False)

    # Full validation
    cmd_check = [check_bin, str(json_path)]
    log.info("running: %s", " ".join(cmd_check))
    rc = subprocess.run(cmd_check, check=False).returncode
    if rc != 0:
        result.warnings.append(f"gtopt_check_json found issues (exit code {rc})")

    return result


# ---------------------------------------------------------------------------
# Post-flight checks
# ---------------------------------------------------------------------------


@register_post_check("check_output")
def post_check_output(results_dir: Path, json_path: Path, **_kwargs) -> CheckResult:
    """Run gtopt_check_output analysis on results."""
    result = CheckResult(name="check_output", passed=True)

    if not results_dir.is_dir() or not json_path.is_file():
        return result

    try:
        from gtopt_check_output._checks import run_all_checks  # noqa: PLC0415
        from gtopt_check_output._reader import load_planning  # noqa: PLC0415
        from gtopt_check_output.main import _print_finding  # noqa: PLC0415

        import sys  # noqa: PLC0415

        planning = load_planning(json_path)
        report = run_all_checks(results_dir, planning)
        use_color = sys.stdout.isatty()
        for finding in report.findings:
            if finding.severity != "INFO":
                _print_finding(finding, use_color, quiet=False)
        if not report.ok:
            result.warnings.append("output analysis found issues")
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        log.debug("gtopt_check_output failed: %s", exc)

    return result


@register_post_check("check_lp")
def post_check_lp(results_dir: Path, gtopt_rc: int = 0, **_kwargs) -> CheckResult:
    """Analyse error LP files when gtopt fails."""
    from ._runner import find_error_lp_files, run_check_lp  # noqa: PLC0415

    result = CheckResult(name="check_lp", passed=True)

    if gtopt_rc == 0:
        return result

    log_dirs = [
        results_dir / "logs",
        results_dir.parent / "output" / "logs",
        results_dir.parent / "logs",
    ]
    for log_dir in log_dirs:
        error_lps = find_error_lp_files(log_dir)
        if error_lps:
            run_check_lp(error_lps)
            result.warnings.append(f"{len(error_lps)} error LP file(s) found")
            break

    return result


# ---------------------------------------------------------------------------
# Orchestrators
# ---------------------------------------------------------------------------


def run_preflight_checks(
    json_path: Path,
    strict: bool = False,
    enabled: set[str] | None = None,
) -> bool:
    """Run enabled pre-flight checks.

    Args:
        json_path: Path to the planning JSON file.
        strict: If True, abort on any warning.
        enabled: Set of check names to run.  None = all.

    Returns:
        True if all checks passed.
    """
    ok = True
    for name, func in _PRE_CHECKS.items():
        if enabled is not None and name not in enabled:
            continue
        log.info("=== Pre-check: %s ===", name)
        result = func(json_path)
        for err in result.errors:
            log.error("  CRITICAL: %s", err)
        for warn in result.warnings:
            log.warning("  WARNING: %s", warn)
        if not result.passed:
            ok = False
        elif strict and result.warnings:
            ok = False
        elif not result.errors and not result.warnings:
            log.info("  %s: OK", name)
    return ok


def run_postflight_checks(
    results_dir: Path,
    json_path: Path,
    gtopt_rc: int = 0,
    enabled: set[str] | None = None,
) -> None:
    """Run enabled post-flight checks.

    Args:
        results_dir: Path to the results directory.
        json_path: Path to the planning JSON file.
        gtopt_rc: gtopt exit code (0=success).
        enabled: Set of check names to run.  None = all.
    """
    for name, func in _POST_CHECKS.items():
        if enabled is not None and name not in enabled:
            continue
        func(results_dir=results_dir, json_path=json_path, gtopt_rc=gtopt_rc)
