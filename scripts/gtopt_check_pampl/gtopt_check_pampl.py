# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt_check_pampl — validate gtopt PAMPL/TAMPL files.

Usage
-----
::

    # Check a single .pampl file
    gtopt_check_pampl constraints.pampl

    # Check multiple files (including .tampl templates)
    gtopt_check_pampl constraints.pampl template.tampl

    # Print file statistics
    gtopt_check_pampl --info constraints.pampl

    # Interactive config setup
    gtopt_check_pampl --init-config

    # Show active configuration
    gtopt_check_pampl --show-config
"""

import argparse
import logging
import sys
from pathlib import Path

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

from gtopt_check_pampl._checks import (
    Finding,
    PamplStats,
    Severity,
    compute_stats,
    run_all_checks,
)
from gtopt_check_pampl._config import (
    CHECK_DEFAULTS,
    _AI_DEFAULT_PROVIDER,
    _AI_PROVIDERS,
    default_config_path,
    is_check_enabled,
    load_config,
    run_interactive_setup,
)

# ── ANSI colour helpers ─────────────────────────────────────────────────────

BOLD = "\033[1m"
RED = "\033[31m"
YELLOW = "\033[33m"
GREEN = "\033[32m"
CYAN = "\033[36m"
DIM = "\033[2m"
RESET = "\033[0m"

USE_COLOR = True


def _c(code: str, text: str) -> str:
    return f"{code}{text}{RESET}" if USE_COLOR else text


# ── Info / stats printing ───────────────────────────────────────────────────


def print_stats(stats: PamplStats) -> None:
    """Print file statistics in a human-readable format."""
    print()
    print(_c(BOLD, f"  File: {stats.filename}"))
    print(f"  Lines: {stats.num_lines}")
    print(f"  Constraints: {stats.num_constraints}")
    print(f"  Parameters: {stats.num_params}")
    print(f"  Inactive: {stats.num_inactive}")
    print(f"  Template syntax: {'yes' if stats.has_templates else 'no'}")
    if stats.element_types_used:
        print(f"  Element types: {', '.join(stats.element_types_used)}")
    if stats.variables_used:
        print(f"  LP variables: {', '.join(stats.variables_used)}")
    print()


# ── Core validation ─────────────────────────────────────────────────────────


def check_pampl(
    file_paths: list[str],
    info_only: bool = False,
    config_path: Path | None = None,
) -> int:
    """Core validation function.

    Parameters
    ----------
    file_paths
        Paths to the .pampl / .tampl file(s).
    info_only
        If True, print statistics and exit.
    config_path
        Path to config file.  Defaults to ``~/.gtopt.conf``.

    Returns
    -------
    int
        Exit code: 0 = ok/warnings only, 1 = critical issues found.
    """
    if config_path is None:
        config_path = default_config_path()
    cfg = load_config(config_path)

    # Determine enabled checks
    enabled: set[str] = set()
    for check_id in CHECK_DEFAULTS:
        if is_check_enabled(cfg, check_id):
            enabled.add(check_id)

    all_findings: list[Finding] = []
    has_critical = False

    for fp in file_paths:
        p = Path(fp)
        if not p.exists():
            print(
                f"Error: file not found: {p}",
                file=sys.stderr,
            )
            return 2

        try:
            source = p.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"Error: cannot read file: {exc}", file=sys.stderr)
            return 2

        filename = str(p)

        if info_only:
            stats = compute_stats(source, filename)
            print_stats(stats)
            continue

        # Only enable template_variables for .tampl files
        file_enabled = set(enabled)
        if not filename.endswith(".tampl"):
            file_enabled.discard("template_variables")

        findings = run_all_checks(source, file_enabled, filename)
        all_findings.extend(findings)

    if info_only:
        return 0

    # Print findings
    print()
    print(_c(BOLD, "─── gtopt_check_pampl ───"))
    print(f"  Files: {', '.join(file_paths)}")
    print(f"  Checks enabled: {sorted(enabled)}")
    print()

    for finding in all_findings:
        if finding.severity == Severity.CRITICAL:
            tag = _c(RED, "[CRITICAL]")
            has_critical = True
        elif finding.severity == Severity.WARNING:
            tag = _c(YELLOW, "[WARNING]")
        else:
            tag = _c(CYAN, "[NOTE]")

        loc = ""
        if finding.file:
            loc = f"{finding.file}"
            if finding.line:
                loc += f":{finding.line}"
            loc += " — "

        print(f"  {tag} {loc}{finding.message}")
        if finding.action:
            print(f"    {_c(DIM, '→ ' + finding.action)}")

    if not all_findings:
        print(_c(GREEN, "  ✓ All checks passed — no issues found."))

    # Summary
    critical_count = sum(1 for f in all_findings if f.severity == Severity.CRITICAL)
    warning_count = sum(1 for f in all_findings if f.severity == Severity.WARNING)
    note_count = sum(1 for f in all_findings if f.severity == Severity.NOTE)

    print()
    parts = []
    if critical_count:
        parts.append(_c(RED, f"{critical_count} critical"))
    if warning_count:
        parts.append(_c(YELLOW, f"{warning_count} warning(s)"))
    if note_count:
        parts.append(_c(CYAN, f"{note_count} note(s)"))
    if parts:
        print(f"  Summary: {', '.join(parts)}")
    else:
        print(_c(GREEN, "  Summary: clean"))
    print()

    return 1 if has_critical else 0


# ── Argument parsing ────────────────────────────────────────────────────────


def _parse_args(
    argv: list[str] | None = None,
) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        prog="gtopt_check_pampl",
        description=(
            "Validate gtopt PAMPL (.pampl) and TAMPL (.tampl) user constraint files "
            "and report potential issues."
        ),
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="Path(s) to the .pampl / .tampl file(s).",
    )
    parser.add_argument(
        "--info",
        action="store_true",
        default=False,
        help="Print file statistics and exit.",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Path to config file (default: ~/.gtopt.conf).",
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
        "Send the analysis report to an AI provider for expert diagnosis.\n"
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

    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser.parse_args(argv)


# ── Main ────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    """CLI entry point for gtopt_check_pampl."""
    args = _parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(levelname)s: %(message)s",
    )

    # Colour control
    global USE_COLOR  # noqa: PLW0603
    if args.no_color:
        USE_COLOR = False

    config_path = args.config or default_config_path()

    if args.init_config:
        run_interactive_setup(config_path, use_color=USE_COLOR)
        return 0

    if args.show_config:
        cfg = load_config(config_path)
        print(f"\nActive configuration ({config_path}):")
        for key, val in sorted(cfg.items()):
            print(f"  {key} = {val!r}")
        return 0

    if not args.files:
        print(
            "Error: no files specified. "
            "Usage: gtopt_check_pampl [--info] <file.pampl> ...",
            file=sys.stderr,
        )
        return 2

    return check_pampl(
        args.files,
        info_only=args.info,
        config_path=config_path,
    )


if __name__ == "__main__":
    sys.exit(main())
