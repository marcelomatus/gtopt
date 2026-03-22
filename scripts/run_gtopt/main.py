# SPDX-License-Identifier: BSD-3-Clause
"""Main entry point for run_gtopt — smart gtopt wrapper."""

from __future__ import annotations

import argparse
import logging
import signal
import sys
from pathlib import Path

from ._binary import find_gtopt_binary, find_plp2gtopt
from ._checks import (
    available_checks,
    run_postflight_checks,
    run_preflight_checks,
)
from ._detect import CaseType, detect_case_type, infer_gtopt_dir
from ._environment import detect_compression_codec, detect_cpu_count
from ._runner import report_solution, run_gtopt, run_plp2gtopt
from ._sanitize import sanitize_json

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
Smart wrapper for the gtopt solver.

Detects PLP and gtopt case directories, runs conversions when needed,
and passes appropriate runtime options to the solver.

Pre-flight checks (enabled by default):
  - JSON syntax validation
  - Input file existence and readability
  - Output directory writability
  - Compression codec availability (auto-fallback)
  - gtopt_check_json --info (system statistics)
  - gtopt_check_json full validation

Examples:
  run_gtopt plp_case_2y            # PLP case: convert + solve
  run_gtopt gtopt_case_2y          # gtopt case: solve directly
  run_gtopt gtopt_case_2y.json     # explicit JSON file: pass to gtopt
  run_gtopt                        # auto-detect from CWD
"""


def _signal_handler(sig, _frame):
    signame = signal.strsignal(sig)
    print(f"\nCaught signal {signame}. Exiting...", file=sys.stderr)
    sys.exit(128 + sig)


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser."""
    parser = argparse.ArgumentParser(
        prog="run_gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "case",
        nargs="?",
        default=None,
        metavar="CASE",
        help=(
            "PLP directory, gtopt directory, or JSON file. "
            "When omitted, auto-detects from the current directory."
        ),
    )
    parser.add_argument(
        "-t",
        "--threads",
        type=int,
        default=None,
        metavar="N",
        help="number of LP solver threads (default: auto-detect CPU count)",
    )
    parser.add_argument(
        "-C",
        "--compression",
        default="zstd",
        metavar="CODEC",
        help="output compression codec (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        metavar="DIR",
        help=(
            "override gtopt output directory for PLP conversion "
            "(default: gtopt_NAME for plp_NAME)"
        ),
    )
    parser.add_argument(
        "--plp-args",
        default=None,
        metavar="ARGS",
        help=(
            "extra arguments to pass to plp2gtopt "
            "(quote the whole string, e.g. '--plp-args \"-y 1 -s 5\"')"
        ),
    )
    parser.add_argument(
        "--check",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "run pre-flight checks before solving: JSON syntax, input "
            "files, output directory, gtopt_check_json (default: enabled)"
        ),
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=False,
        help="abort on any warning during pre-flight checks",
    )
    all_checks = available_checks()
    all_names = sorted(set(all_checks["pre"]) | set(all_checks["post"]))
    parser.add_argument(
        "--enable-check",
        action="append",
        default=None,
        metavar="NAME",
        help=(
            f"enable a specific check (may repeat). Available: {', '.join(all_names)}"
        ),
    )
    parser.add_argument(
        "--disable-check",
        action="append",
        default=None,
        metavar="NAME",
        help="disable a specific check (may repeat)",
    )
    parser.add_argument(
        "--list-checks",
        action="store_true",
        default=False,
        help="list available checks and exit",
    )
    parser.add_argument(
        "--convert-only",
        action="store_true",
        default=False,
        help="convert PLP case but do not run gtopt",
    )
    parser.add_argument(
        "--export-json",
        type=Path,
        default=None,
        metavar="FILE",
        help=(
            "write the sanitized planning JSON to FILE and keep it. "
            "The sanitized JSON includes all runtime fixes "
            "(compression codec, threads, directory overrides)."
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
        help="print commands without executing",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        metavar="LEVEL",
        help="logging verbosity (default: %(default)s)",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _split_extra_args(raw: str | None) -> list[str]:
    """Split a quoted argument string into a list."""
    if not raw:
        return []
    import shlex  # noqa: PLC0415

    return shlex.split(raw)


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and orchestrate the run."""
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    parser = make_parser()

    # Split on '--' to separate run_gtopt args from gtopt passthrough args
    if argv is None:
        argv = sys.argv[1:]

    gtopt_extra: list[str] = []
    if "--" in argv:
        sep = argv.index("--")
        gtopt_extra = argv[sep + 1 :]
        argv = argv[:sep]

    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    # ── List checks ──
    if args.list_checks:
        checks = available_checks()
        print("Pre-flight checks (run before gtopt):")
        for name in checks["pre"]:
            print(f"  {name}")
        print("Post-flight checks (run after gtopt):")
        for name in checks["post"]:
            print(f"  {name}")
        return

    # ── Build enabled check set ──
    enabled_checks: set[str] | None = None
    if args.enable_check:
        enabled_checks = set(args.enable_check)
    if args.disable_check:
        all_names = set(available_checks()["pre"]) | set(available_checks()["post"])
        if enabled_checks is None:
            enabled_checks = set(all_names)
        for name in args.disable_check:
            enabled_checks.discard(name)

    # ── Environment detection ──
    threads = args.threads if args.threads is not None else detect_cpu_count()
    compression = detect_compression_codec(args.compression)
    log.info(
        "environment: %d CPUs, compression=%s",
        threads,
        compression,
    )

    # ── Determine case directory ──
    case_path = Path(args.case) if args.case else Path.cwd()
    case_type = detect_case_type(case_path)

    if args.case is None and case_type == CaseType.PASSTHROUGH:
        print(
            "error: no case directory given and current directory is not "
            "a recognized PLP or gtopt case.\n"
            "Usage: run_gtopt [CASE] [options]\n"
            "Run 'run_gtopt -h' for help.",
            file=sys.stderr,
        )
        sys.exit(2)

    log.info("case: %s (type: %s)", case_path, case_type.value)

    # ── PLP case: convert first ──
    gtopt_dir = case_path
    if case_type == CaseType.PLP:
        gtopt_dir = args.output_dir or infer_gtopt_dir(case_path)
        log.info("PLP case detected, converting to %s", gtopt_dir)

        plp2gtopt_bin = find_plp2gtopt()
        if not plp2gtopt_bin:
            print(
                "error: plp2gtopt not found on PATH. "
                "Install with: pip install -e ./scripts",
                file=sys.stderr,
            )
            sys.exit(2)

        plp_extra = _split_extra_args(args.plp_args)
        if args.dry_run:
            cmd = [plp2gtopt_bin, str(case_path), "-o", str(gtopt_dir)]
            if plp_extra:
                cmd.extend(plp_extra)
            print(f"[dry-run] {' '.join(cmd)}")
        else:
            rc = run_plp2gtopt(plp2gtopt_bin, case_path, gtopt_dir, plp_extra)
            if rc != 0:
                sys.exit(rc)

        if args.convert_only:
            log.info("conversion complete (--convert-only), skipping solve")
            return

    # ── Passthrough: not a directory, forward as-is ──
    if case_type == CaseType.PASSTHROUGH:
        gtopt_bin = find_gtopt_binary()
        if not gtopt_bin:
            print(
                "error: gtopt binary not found.\n"
                "  Set GTOPT_BIN=/path/to/gtopt, or ensure 'gtopt' is on PATH.\n"
                "  Build from source: bash tools/setup_sandbox.sh --build\n"
                "  Or download: python tools/get_gtopt_binary.py",
                file=sys.stderr,
            )
            sys.exit(2)
        if args.dry_run:
            all_args = [str(case_path)] + gtopt_extra
            print(f"[dry-run] {gtopt_bin} {' '.join(all_args)}")
            return
        rc = run_gtopt(gtopt_bin, case_path, threads, compression, gtopt_extra)
        sys.exit(rc)

    # ── Locate the planning JSON ──
    json_file = gtopt_dir / f"{gtopt_dir.name}.json"

    # ── Pre-flight checks ──
    if args.check and json_file.is_file() and not args.dry_run:
        ok = run_preflight_checks(json_file, strict=args.strict, enabled=enabled_checks)
        if not ok:
            log.error("pre-flight checks failed, aborting")
            sys.exit(2)

    # ── Sanitize JSON (compression, threads, directories) ──
    sanitized_path: Path | None = None
    if json_file.is_file() and not args.dry_run:
        sanitized_path = sanitize_json(
            json_file,
            compression=compression,
            threads=threads,
            export_path=args.export_json,
        )
        if sanitized_path is None:
            log.error("failed to process planning JSON")
            sys.exit(2)

    # ── Find gtopt binary ──
    gtopt_bin = find_gtopt_binary()
    if not gtopt_bin:
        print("error: gtopt binary not found", file=sys.stderr)
        sys.exit(2)

    # ── Build the gtopt target ──
    # If we produced a sanitized JSON, pass it directly instead of the dir
    is_sanitized = sanitized_path is not None and sanitized_path != json_file
    gtopt_target: Path = (
        sanitized_path if is_sanitized and sanitized_path else gtopt_dir
    )

    if args.dry_run:
        cmd_parts = [gtopt_bin, str(gtopt_target)]
        if threads:
            cmd_parts.extend(["--lp-threads", str(threads)])
        if compression:
            cmd_parts.extend(["--output-compression", compression])
        if gtopt_extra:
            cmd_parts.extend(gtopt_extra)
        print(f"[dry-run] {' '.join(cmd_parts)}")
        return

    rc = run_gtopt(gtopt_bin, gtopt_target, threads, compression, gtopt_extra)

    # ── Clean up sanitized JSON (unless exported) ──
    if is_sanitized and sanitized_path and not args.export_json:
        try:
            sanitized_path.unlink()
        except OSError:
            pass

    # ── Post-run report ──
    results_dir = gtopt_dir / "results"
    report_solution(results_dir, gtopt_dir)

    # ── Post-flight checks (error LPs, output analysis) ──
    if args.check:
        planning_json = results_dir / "planning.json"
        if not planning_json.is_file():
            planning_json = json_file
        run_postflight_checks(
            results_dir,
            planning_json,
            gtopt_rc=rc,
            enabled=enabled_checks,
        )

    sys.exit(rc)


if __name__ == "__main__":
    main()
