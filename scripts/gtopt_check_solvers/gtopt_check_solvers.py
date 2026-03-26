# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_check_solvers — discover and validate gtopt LP solver plugins.

Usage
-----
::

    # List all available LP solver plugins
    gtopt_check_solvers --list

    # Check all available solvers (default)
    gtopt_check_solvers

    # Check a specific solver
    gtopt_check_solvers --solver clp

    # Check multiple solvers
    gtopt_check_solvers --solver clp --solver highs

    # Use an explicit gtopt binary path
    gtopt_check_solvers --gtopt-bin /opt/gtopt/bin/gtopt

    # Verbose output
    gtopt_check_solvers --verbose
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from gtopt_check_json._terminal import (
    console,
    init as _init_terminal,
    print_section,
)

from ._binary import find_gtopt_binary
from ._solver_tests import (
    BUILTIN_TESTS,
    SolverTestReport,
    list_available_solvers,
    run_solver_tests,
)

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

log = logging.getLogger(__name__)

_DESCRIPTION = """\
Discover and validate gtopt LP solver plugins.

Runs a built-in LP test suite against every available solver plugin
(or a selected subset) and reports pass/fail for each test case.

Built-in test cases:
  single_bus_lp   — single-bus copper-plate LP (1 gen, 1 demand)
  kirchhoff_lp    — 4-bus DC OPF with Kirchhoff constraints
  feasibility_lp  — feasibility check (demand exactly at gen capacity)

Examples:
  gtopt_check_solvers                       # check all available solvers
  gtopt_check_solvers --list                # list solvers only
  gtopt_check_solvers --solver clp          # check CLP only
  gtopt_check_solvers --solver clp --solver highs
  gtopt_check_solvers --gtopt-bin /usr/local/bin/gtopt
"""


def _make_parser() -> argparse.ArgumentParser:
    """Build the argument parser."""
    parser = argparse.ArgumentParser(
        prog="gtopt_check_solvers",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--list",
        "-l",
        action="store_true",
        default=False,
        dest="list_solvers",
        help="List all available LP solver plugins and exit.",
    )
    parser.add_argument(
        "--solver",
        "-s",
        action="append",
        default=None,
        dest="solvers",
        metavar="SOLVER",
        help=(
            "Solver to test (can be specified multiple times). "
            "Defaults to all available solvers."
        ),
    )
    parser.add_argument(
        "--test",
        "-t",
        action="append",
        default=None,
        dest="tests",
        metavar="TEST",
        choices=[name for name, _, _ in BUILTIN_TESTS],
        help=(
            "Test case to run (can be specified multiple times). "
            "Defaults to all built-in tests."
        ),
    )
    parser.add_argument(
        "--gtopt-bin",
        default=None,
        metavar="PATH",
        help=(
            "Path to the gtopt binary. "
            "Defaults to auto-detect (GTOPT_BIN env, PATH, build directories)."
        ),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        metavar="SECONDS",
        help="Per-test timeout in seconds (default: %(default)s).",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        default=False,
        help="Disable coloured output.",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        default=False,
        help="Verbose output (show test details).",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _print_solver_list(solvers: list[str], gtopt_bin: str) -> None:
    """Print the list of available solvers."""
    con = console()
    con.print(f"\n  gtopt binary : [dim]{gtopt_bin}[/dim]")
    con.print()
    if not solvers:
        con.print("  [warn]No LP solver plugins found.[/warn]")
        con.print()
        con.print("  [dim]Hints:[/dim]")
        con.print(
            "  [dim]  - Set GTOPT_PLUGIN_DIR to the directory containing "
            "solver plugin libraries[/dim]"
        )
        con.print(
            "  [dim]  - Install COIN-OR (coinor-libcbc-dev) for CLP/CBC support[/dim]"
        )
        con.print("  [dim]  - Install HiGHS for HiGHS support[/dim]")
    else:
        con.print(f"  Available LP solvers ({len(solvers)}):")
        for s in solvers:
            con.print(f"    [ok]•[/ok] {s}")
    con.print()


def _print_test_result(result, verbose: bool) -> None:
    """Print a single test result line."""
    con = console()
    if result.passed:
        obj_str = ""
        if result.obj_value is not None:
            obj_str = f"  obj={result.obj_value:>12.4g}"
        time_str = f"  {result.duration_s:.2f}s"
        con.print(f"    [ok]✓[/ok]  {result.name:<22}{obj_str}{time_str}")
    else:
        con.print(f"    [err]✗[/err]  {result.name:<22}  {result.message}")
        if verbose and result.details:
            for line in result.details.splitlines():
                con.print(f"       [dim]{line}[/dim]")


def _print_solver_report(report: SolverTestReport, verbose: bool) -> None:
    """Print all test results for one solver."""
    con = console()
    status_str = "[ok]PASS[/ok]" if report.passed else "[err]FAIL[/err]"
    con.print(f"  Solver: [header]{report.solver}[/header]  {status_str}")
    for result in report.results:
        _print_test_result(result, verbose)
    con.print()


def _print_summary(
    reports: list[SolverTestReport],
    gtopt_bin: str,
) -> None:
    """Print the overall summary."""
    con = console()
    n_solvers_ok = sum(1 for r in reports if r.passed)
    n_solvers_fail = sum(1 for r in reports if not r.passed)
    n_tests_ok = sum(r.n_passed for r in reports)
    n_tests_fail = sum(r.n_failed for r in reports)
    total_tests = n_tests_ok + n_tests_fail

    con.print("  ─" * 30)
    con.print()
    con.print(
        f"  Solvers : [ok]{n_solvers_ok} passed[/ok], "
        f"[err]{n_solvers_fail} failed[/err]  "
        f"(out of {len(reports)})"
    )
    con.print(
        f"  Tests   : [ok]{n_tests_ok} passed[/ok], "
        f"[err]{n_tests_fail} failed[/err]  "
        f"(out of {total_tests})"
    )
    con.print()


def check_solvers(
    gtopt_bin: str,
    solvers: list[str],
    *,
    test_names: list[str] | None = None,
    timeout: float = 60.0,
    verbose: bool = False,
) -> int:
    """Run the solver test suite and return an exit code.

    Parameters
    ----------
    gtopt_bin
        Path to the gtopt binary.
    solvers
        Solver names to test.
    test_names
        Optional list of test names to run.
    timeout
        Per-test timeout in seconds.
    verbose
        Print extra details on failure.

    Returns
    -------
    int
        0 if all tests pass, 1 if any test fails.
    """
    print_section("gtopt_check_solvers")

    reports: list[SolverTestReport] = []
    for solver in solvers:
        report = run_solver_tests(
            gtopt_bin,
            solver,
            timeout=timeout,
            test_names=test_names,
        )
        _print_solver_report(report, verbose)
        reports.append(report)

    _print_summary(reports, gtopt_bin)

    return 0 if all(r.passed for r in reports) else 1


def main(argv: list[str] | None = None) -> int:
    """CLI entry point for gtopt_check_solvers."""
    parser = _make_parser()
    args = parser.parse_args(argv)

    # Logging
    level = logging.DEBUG if args.verbose else logging.WARNING
    logging.basicConfig(level=level, format="%(levelname)s: %(message)s")

    # Terminal
    _init_terminal(force_color=False if args.no_color else None)

    # Locate gtopt binary
    gtopt_bin_arg: str | None = args.gtopt_bin
    if gtopt_bin_arg:
        gtopt_bin = gtopt_bin_arg
        if not Path(gtopt_bin).is_file():
            console().print(
                f"[err]Error:[/err] gtopt binary not found: {gtopt_bin}",
            )
            return 2
    else:
        found = find_gtopt_binary()
        if not found:
            console().print(
                "[err]Error:[/err] gtopt binary not found. "
                "Set GTOPT_BIN, add gtopt to PATH, or use --gtopt-bin.",
            )
            return 2
        gtopt_bin = found

    # Discover available solvers
    available = list_available_solvers(gtopt_bin)

    if args.list_solvers:
        print_section("gtopt_check_solvers — available solvers")
        _print_solver_list(available, gtopt_bin)
        return 0

    # Resolve which solvers to test
    if args.solvers:
        solvers_to_test: list[str] = []
        for s in args.solvers:
            if s not in available:
                console().print(
                    f"[warn]Warning:[/warn] solver '{s}' not available "
                    f"(available: {', '.join(available) or 'none'})"
                )
            else:
                solvers_to_test.append(s)
    else:
        solvers_to_test = list(available)

    if not solvers_to_test:
        console().print(
            "[warn]Warning:[/warn] No solvers to test. "
            "Run with --list to see available solvers."
        )
        return 1

    return check_solvers(
        gtopt_bin,
        solvers_to_test,
        test_names=args.tests,
        timeout=args.timeout,
        verbose=args.verbose,
    )


if __name__ == "__main__":
    sys.exit(main())
