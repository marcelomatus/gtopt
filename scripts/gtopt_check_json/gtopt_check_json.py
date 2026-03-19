# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt_check_json — validate gtopt JSON cases.

Usage
-----
::

    # Print system/simulation statistics (like gtopt --stats)
    gtopt_check_json --info gtopt_case.json

    # Run all validation checks
    gtopt_check_json gtopt_case.json

    # Interactive config setup
    gtopt_check_json --init-config
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from gtopt_check_json import _colors as col
from gtopt_check_json._checks import Finding, Severity, run_all_checks
from gtopt_check_json._config import (
    CHECK_DEFAULTS,
    _AI_DEFAULT_PROVIDER,
    default_config_path,
    is_check_enabled,
    load_config,
    run_interactive_setup,
)
from gtopt_check_json._info import format_info

# Use AiOptions from gtopt_check_lp when available; fall back to a minimal
# implementation so that gtopt_check_json works even if gtopt_check_lp is not
# installed.
try:
    from gtopt_check_lp._ai import AiOptions  # noqa: PLC0415
except ImportError:  # pragma: no cover
    from dataclasses import dataclass  # noqa: PLC0415

    @dataclass
    class AiOptions:  # type: ignore[no-redef]
        """Minimal AiOptions shim when gtopt_check_lp is not installed."""

        enabled: bool = True
        provider: str = "github"
        model: str = ""
        prompt: str = ""
        key: str = ""
        timeout: int = 60


def _load_planning(
    json_paths: list[str],
) -> tuple[dict[str, Any], str]:
    """Load and merge one or more gtopt JSON files into a planning dict.

    Returns (planning_dict, case_directory).
    """
    planning: dict[str, Any] = {}
    case_dir = ""

    for jp in json_paths:
        p = Path(jp)
        if not p.suffix:
            p = p.with_suffix(".json")
        if not p.exists():
            print(
                f"Error: file not found: {p}",
                file=sys.stderr,
            )
            sys.exit(1)
        with open(p, encoding="utf-8") as fh:
            data = json.load(fh)

        if not case_dir:
            case_dir = str(p.parent.resolve())

        # Merge: top-level keys are combined
        for key, val in data.items():
            if key not in planning:
                planning[key] = val
            elif isinstance(val, dict) and isinstance(planning[key], dict):
                planning[key].update(val)
            else:
                planning[key] = val

    return planning, case_dir


def _format_finding(finding: Finding) -> str:
    """Format a Finding for terminal output."""
    sev = finding.severity
    if sev == Severity.CRITICAL:
        tag = col.c(col.RED, "[CRITICAL]")
    elif sev == Severity.WARNING:
        tag = col.c(col.YELLOW, "[WARNING]")
    else:
        tag = col.c(col.CYAN, "[NOTE]")
    return f"  {tag} ({finding.check_id}) {finding.message}"


def check_json(
    json_paths: list[str],
    info_only: bool = False,
    config_path: Path | None = None,
) -> int:
    """Core validation function.

    Parameters
    ----------
    json_paths
        Paths to the gtopt JSON case file(s).
    info_only
        If True, print statistics and exit.
    config_path
        Path to config file.  Defaults to ``~/.gtopt_check_json.conf``.

    Returns
    -------
    int
        Exit code: 0 = ok/warnings only, 1 = critical issues found.
    """
    if config_path is None:
        config_path = default_config_path()
    cfg = load_config(config_path)

    planning, _case_dir = _load_planning(json_paths)

    if info_only:
        print(format_info(planning, base_dir=_case_dir))
        return 0

    # Determine enabled checks
    enabled: set[str] = set()
    for check_id in CHECK_DEFAULTS:
        if is_check_enabled(cfg, check_id):
            enabled.add(check_id)

    # AI options
    ai_options = None
    if (
        cfg.get("ai_enabled", "false").lower() in ("true", "1", "yes")
        and "ai_system_analysis" in enabled
    ):
        ai_options = AiOptions(
            provider=cfg.get("ai_provider", _AI_DEFAULT_PROVIDER),
            model=cfg.get("ai_model", ""),
            key="",
            timeout=60,
        )

    # Run checks
    print(col.header("gtopt_check_json"))
    print(f"  Case: {', '.join(json_paths)}")
    print(f"  Checks enabled: {sorted(enabled)}")
    print()

    findings = run_all_checks(
        planning,
        enabled_checks=enabled,
        ai_options=ai_options,
    )

    # Report
    has_critical = False
    for finding in findings:
        print(_format_finding(finding))
        if finding.severity == Severity.CRITICAL:
            has_critical = True

    if not findings:
        print(f"  {col.c(col.GREEN, '✓')} All checks passed — no issues found.")

    # Summary
    critical_count = sum(1 for f in findings if f.severity == Severity.CRITICAL)
    warning_count = sum(1 for f in findings if f.severity == Severity.WARNING)
    note_count = sum(1 for f in findings if f.severity == Severity.NOTE)
    print()
    print(
        f"  Summary: "
        f"{col.c(col.RED, str(critical_count))} critical, "
        f"{col.c(col.YELLOW, str(warning_count))} warnings, "
        f"{col.c(col.CYAN, str(note_count))} notes"
    )

    return 1 if has_critical else 0


def _parse_args(
    argv: list[str] | None = None,
) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        prog="gtopt_check_json",
        description=("Validate gtopt JSON planning files and report potential issues."),
    )
    parser.add_argument(
        "json_files",
        nargs="*",
        help="Path(s) to the gtopt JSON case file(s).",
    )
    parser.add_argument(
        "--info",
        action="store_true",
        default=False,
        help=("Print system/simulation statistics (like gtopt --stats) and exit."),
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help=("Path to config file (default: ~/.gtopt_check_json.conf)."),
    )
    parser.add_argument(
        "--init-config",
        action="store_true",
        default=False,
        help="Run interactive configuration setup.",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        default=False,
        help="Disable coloured output.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point for gtopt_check_json."""
    args = _parse_args(argv)

    # Colour control
    if args.no_color:
        col.USE_COLOR = False

    config_path = args.config or default_config_path()

    if args.init_config:
        run_interactive_setup(config_path, use_color=col.USE_COLOR)
        return 0

    if not args.json_files:
        print(
            "Error: no JSON files specified. "
            "Usage: gtopt_check_json [--info] <file.json> ...",
            file=sys.stderr,
        )
        return 2

    return check_json(
        args.json_files,
        info_only=args.info,
        config_path=config_path,
    )


if __name__ == "__main__":
    sys.exit(main())
