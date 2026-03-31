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
import logging
import sys
from pathlib import Path
from typing import Any

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

from gtopt_check_json import _colors as col
from gtopt_check_json._checks import (
    Finding,
    Severity,
    analyse_bus_islands,
    run_all_checks,
)
from gtopt_check_json._config import (
    CHECK_DEFAULTS,
    _AI_DEFAULT_PROVIDER,
    _AI_PROVIDERS,
    default_config_path,
    is_check_enabled,
    load_config,
    run_interactive_setup,
)
from gtopt_check_json._info import print_info
from gtopt_check_json._terminal import (
    init as _init_terminal,
    print_connectivity_table,
    print_section,
    print_status,
    print_summary,
)

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
        provider: str = "claude"
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
    ai_options: AiOptions | None = None,
) -> int:
    """Core validation function.

    Parameters
    ----------
    json_paths
        Paths to the gtopt JSON case file(s).
    info_only
        If True, print statistics and exit.
    config_path
        Path to config file.  Defaults to ``~/.gtopt.conf``.
    ai_options
        Pre-built AI options (CLI overrides already merged).
        When *None*, AI settings are read from the config file.

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
        print_info(planning, base_dir=_case_dir)
        return 0

    # Determine enabled checks
    enabled: set[str] = set()
    for check_id in CHECK_DEFAULTS:
        if is_check_enabled(cfg, check_id):
            enabled.add(check_id)

    # AI options — use caller-provided options, or fall back to config
    if ai_options is None:
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
    elif not ai_options.enabled:
        ai_options = None

    # Run checks
    print_section("gtopt_check_json")
    print(f"  Case: {', '.join(json_paths)}")
    print(f"  Checks enabled: {sorted(enabled)}")
    print()

    findings = run_all_checks(
        planning,
        enabled_checks=enabled,
        ai_options=ai_options,
        base_dir=_case_dir,
    )

    # Connectivity table (replaces verbose bus_connectivity findings)
    islands = analyse_bus_islands(planning)
    if islands:
        print_connectivity_table(islands)

    # Collect non-connectivity findings into a table
    _table_checks = {"bus_connectivity"}
    has_critical = False
    rows: list[tuple[str, str, str]] = []
    for finding in findings:
        if finding.severity == Severity.CRITICAL:
            has_critical = True
        if finding.check_id in _table_checks:
            continue
        rows.append(
            (
                finding.severity.name,
                finding.message,
                finding.action,
            )
        )

    if rows:
        from gtopt_check_json._terminal import print_table  # noqa: PLC0415

        print_table(
            headers=["Severity", "Description", "Action / Notes"],
            rows=rows,
            aligns=["left", "left", "left"],
            title="Validation Findings",
            styles=["warn", "", "dim"],
        )

    if not findings:
        print_status("All checks passed — no issues found.", ok=True)

    critical_count = sum(1 for f in findings if f.severity == Severity.CRITICAL)
    warning_count = sum(1 for f in findings if f.severity == Severity.WARNING)
    note_count = sum(1 for f in findings if f.severity == Severity.NOTE)
    print_summary(critical_count, warning_count, note_count)

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
        help=("Path to config file (default: ~/.gtopt.conf)."),
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
    parser.add_argument(
        "--show-simulation",
        action="store_true",
        default=False,
        help="Print detailed simulation structure (scenarios, stages, phases, apertures).",
    )
    parser.add_argument(
        "--show-config",
        action="store_true",
        default=False,
        help="Print the active configuration and exit.",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        default=False,
        help="Minimal output: only show warnings and critical findings.",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="WARNING",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        metavar="LEVEL",
        help="Logging verbosity (default: %(default)s).",
    )
    # ── AI diagnostics ──────────────────────────────────────────────────────
    ai_group = parser.add_argument_group(
        "AI diagnostics",
        "Send the analysis report to an AI provider for an expert diagnosis.\n"
        "Supported providers: " + ", ".join(_AI_PROVIDERS) + ".\n"
        "Requires the corresponding API key in the environment.",
    )
    ai_group.add_argument(
        "--ai",
        dest="ai_enabled",
        action="store_true",
        default=None,
        help="Enable AI diagnostics (default: read from config).",
    )
    ai_group.add_argument(
        "--no-ai",
        dest="ai_enabled",
        action="store_false",
        help="Disable AI diagnostics.",
    )
    ai_group.add_argument(
        "--ai-provider",
        default=None,
        choices=list(_AI_PROVIDERS),
        metavar="PROVIDER",
        help=(
            f"AI provider to use: {', '.join(_AI_PROVIDERS)}  "
            f"(default: {_AI_DEFAULT_PROVIDER})."
        ),
    )
    ai_group.add_argument(
        "--ai-model",
        default=None,
        metavar="MODEL",
        help="Model name override.  Defaults to the provider-specific default.",
    )
    ai_group.add_argument(
        "--ai-prompt",
        default=None,
        metavar="PROMPT",
        help=(
            "Custom prompt template for the AI query.  Must contain a "
            "'{report}' placeholder."
        ),
    )
    ai_group.add_argument(
        "--ai-key",
        default=None,
        metavar="KEY",
        help=(
            "API key override.  When omitted, the key is read from the "
            "environment variable for the selected provider."
        ),
    )

    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point for gtopt_check_json."""
    args = _parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(levelname)s: %(message)s",
    )

    # Colour control
    no_color = args.no_color
    if no_color:
        col.USE_COLOR = False

    _init_terminal(force_color=False if no_color else None)

    config_path = args.config or default_config_path()

    if args.init_config:
        run_interactive_setup(config_path, use_color=col.USE_COLOR)
        return 0

    if args.show_config:
        cfg = load_config(config_path)
        print(f"\nActive configuration ({config_path}):")
        for key, val in sorted(cfg.items()):
            print(f"  {key} = {val!r}")
        return 0

    if not args.json_files:
        print(
            "Error: no JSON files specified. "
            "Usage: gtopt_check_json [--info] <file.json> ...",
            file=sys.stderr,
        )
        return 2

    # --show-simulation: load JSON and print simulation structure
    if args.show_simulation:
        from plp2gtopt.plp2gtopt import show_simulation_summary  # noqa: PLC0415

        for json_file in args.json_files:
            with open(json_file, encoding="utf-8") as fh:
                planning = json.load(fh)
            show_simulation_summary(planning)
        return 0

    # ── Merge CLI AI overrides with config ────────────────────────────────
    cfg = load_config(config_path)
    cfg_ai_enabled = cfg.get("ai_enabled", "false").lower() in ("true", "1", "yes")
    effective_ai_enabled = (
        args.ai_enabled if args.ai_enabled is not None else cfg_ai_enabled
    )
    effective_ai_provider = (
        args.ai_provider
        if args.ai_provider is not None
        else cfg.get("ai_provider", _AI_DEFAULT_PROVIDER)
    )
    effective_ai_model = (
        args.ai_model if args.ai_model is not None else cfg.get("ai_model", "")
    )
    effective_ai_prompt = (
        args.ai_prompt if args.ai_prompt is not None else cfg.get("ai_prompt", "")
    )
    effective_ai_key = args.ai_key if args.ai_key is not None else ""

    ai_options = AiOptions(
        enabled=effective_ai_enabled,
        provider=effective_ai_provider,
        model=effective_ai_model,
        prompt=effective_ai_prompt,
        key=effective_ai_key,
    )

    return check_json(
        args.json_files,
        info_only=args.info,
        config_path=config_path,
        ai_options=ai_options,
    )


if __name__ == "__main__":
    sys.exit(main())
