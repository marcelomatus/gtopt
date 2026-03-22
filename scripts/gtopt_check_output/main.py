# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt_check_output."""

from __future__ import annotations

import argparse
import json
import logging
import sys
from pathlib import Path

from ._checks import Finding, run_all_checks
from ._reader import load_planning

log = logging.getLogger(__name__)

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

_DESCRIPTION = """\
Validate and analyze gtopt solver output.

Checks output completeness, load shedding, generation/demand balance,
line congestion ranking, LMP statistics, cost breakdown, and more.

Examples:
  gtopt_check_output gtopt_case_2y             # auto-find results + JSON
  gtopt_check_output -r results/ -j plan.json  # explicit paths
  gtopt_check_output gtopt_case_2y --quiet      # minimal output
"""

_SEVERITY_COLORS = {
    "CRITICAL": "\033[91m",
    "WARNING": "\033[93m",
    "INFO": "\033[0m",
}
_RESET = "\033[0m"


def _print_finding(f: Finding, use_color: bool, quiet: bool) -> None:
    if quiet and f.severity == "INFO":
        return
    if use_color:
        color = _SEVERITY_COLORS.get(f.severity, "")
        tag = f"{color}[{f.severity:8s}]{_RESET}"
    else:
        tag = f"[{f.severity:8s}]"
    print(f"  {tag} {f.message}")


def make_parser() -> argparse.ArgumentParser:
    """Build the argument parser."""
    parser = argparse.ArgumentParser(
        prog="gtopt_check_output",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "case_dir",
        nargs="?",
        type=Path,
        default=None,
        metavar="CASE_DIR",
        help=(
            "gtopt case directory (auto-discovers JSON and results). "
            "When omitted, uses the current directory."
        ),
    )
    parser.add_argument(
        "-r",
        "--results-dir",
        type=Path,
        default=None,
        metavar="DIR",
        help="explicit path to the results directory",
    )
    parser.add_argument(
        "-j",
        "--json-file",
        type=Path,
        default=None,
        metavar="FILE",
        help=(
            "explicit path to the planning file (planning.json). "
            "By default, uses results/planning.json (saved by gtopt) "
            "or case_dir/case_name.json"
        ),
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        default=False,
        help="disable colored output",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        default=False,
        help="only show warnings and critical findings",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="WARNING",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        metavar="LEVEL",
        help="logging verbosity (default: %(default)s)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        metavar="FILE",
        help="path to .gtopt.conf (default: ~/.gtopt.conf)",
    )
    parser.add_argument(
        "--init-config",
        action="store_true",
        default=False,
        help="initialize [gtopt_check_output] section in .gtopt.conf",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _resolve_paths(
    args: argparse.Namespace,
) -> tuple[Path, Path]:
    """Resolve results_dir and json_path from arguments."""
    case_dir = args.case_dir or Path.cwd()

    # Results directory
    if args.results_dir:
        results_dir = args.results_dir
    else:
        results_dir = case_dir / "results"
        if not results_dir.is_dir():
            results_dir = case_dir / "output"

    # JSON file — prefer planning.json saved by gtopt in the results dir,
    # then fall back to the case-level JSON (dir/dir.json).
    if args.json_file:
        json_path = args.json_file
    else:
        # 1. planning.json inside results (written by gtopt after a successful solve)
        json_path = results_dir / "planning.json"
        if not json_path.is_file():
            # 2. case_dir/case_name.json (the original input)
            json_path = case_dir / f"{case_dir.name}.json"
        if not json_path.is_file():
            # 3. any .json in the case dir
            for candidate in case_dir.glob("*.json"):
                json_path = candidate
                break

    return results_dir, json_path


def _init_config(config_path: Path) -> None:
    """Initialize the [gtopt_check_output] section in .gtopt.conf."""
    from gtopt_config import save_section  # noqa: PLC0415

    defaults = {
        "check_load_shedding": "true",
        "check_congestion": "true",
        "check_lmp_stats": "true",
        "check_cost_breakdown": "true",
        "congestion_top_n": "10",
    }
    save_section(config_path, "gtopt_check_output", defaults)
    print(f"Initialized [gtopt_check_output] in {config_path}")


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and run output checks."""
    parser = make_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    # Config initialization
    if args.init_config:
        from gtopt_config import DEFAULT_CONFIG_PATH  # noqa: PLC0415

        config_path = args.config or DEFAULT_CONFIG_PATH
        _init_config(config_path)
        return

    use_color = not args.no_color and sys.stdout.isatty()

    results_dir, json_path = _resolve_paths(args)

    if not results_dir.is_dir():
        print(f"error: results directory not found: {results_dir}", file=sys.stderr)
        sys.exit(2)

    if not json_path.is_file():
        print(f"error: planning JSON not found: {json_path}", file=sys.stderr)
        sys.exit(2)

    # Load planning
    try:
        planning = load_planning(json_path)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot load {json_path}: {exc}", file=sys.stderr)
        sys.exit(2)

    # Run checks
    if not args.quiet:
        print()
        print("=" * 60)
        print("  gtopt_check_output — Output Analysis")
        print("=" * 60)
        print(f"  Results : {results_dir}")
        print(f"  JSON    : {json_path}")
        print()

    report = run_all_checks(results_dir, planning)

    # Group findings by check
    checks_seen: list[str] = []
    for f in report.findings:
        if f.check not in checks_seen:
            checks_seen.append(f.check)

    for check_id in checks_seen:
        group = [f for f in report.findings if f.check == check_id]
        if args.quiet and all(f.severity == "INFO" for f in group):
            continue
        if not args.quiet:
            print(f"  --- {check_id} ---")
        for f in group:
            _print_finding(f, use_color, args.quiet)

    # Summary
    n_critical = sum(1 for f in report.findings if f.severity == "CRITICAL")
    n_warning = sum(1 for f in report.findings if f.severity == "WARNING")
    n_info = sum(1 for f in report.findings if f.severity == "INFO")

    if not args.quiet:
        print()
        summary = (
            f"  Summary: {n_critical} critical, {n_warning} warnings, {n_info} info"
        )
        if use_color:
            if n_critical > 0:
                summary = f"\033[91m{summary}{_RESET}"
            elif n_warning > 0:
                summary = f"\033[93m{summary}{_RESET}"
            else:
                summary = f"\033[92m{summary}{_RESET}"
        print(summary)
        print()

    sys.exit(1 if n_critical > 0 else 0)


if __name__ == "__main__":
    main()
