"""CLI entry point for gtopt2pp — convert gtopt JSON to pandapower."""

import argparse
import logging
import sys
from pathlib import Path
from typing import Any

from gtopt_config import add_common_arguments, configure_logging, get_version

from gtopt2pp.convert import (
    convert,
    format_diagnostic,
    load_gtopt_case,
    run_dcopp,
    run_diagnostic,
)

__version__ = get_version()

logger = logging.getLogger(__name__)

try:
    import pandapower as pp

    _HAS_PP = True
except ImportError:  # pragma: no cover
    _HAS_PP = False


def _parse_block_spec(spec: str) -> list[int]:
    """Parse a block specification string into a sorted list of UIDs.

    Accepts:
    - A single integer: ``"1"``
    - A comma-separated list: ``"1,2,4"``
    - A range: ``"1-5"`` (inclusive on both ends)
    - A mix: ``"1,3-5,8"``

    Returns a sorted, deduplicated list of block UIDs.
    """
    uids: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_s, hi_s = part.split("-", maxsplit=1)
            lo, hi = int(lo_s), int(hi_s)
            uids.update(range(lo, hi + 1))
        else:
            uids.add(int(part))
    return sorted(uids)


def _log_source_summary(case: dict[str, Any]) -> None:
    """Log a summary of the source gtopt JSON before conversion."""
    system = case.get("system", {})
    simulation = case.get("simulation", {})

    logger.info("=== Source gtopt case summary ===")
    logger.info("  System name     : %s", system.get("name", "(unnamed)"))
    logger.info("  Buses           : %d", len(system.get("bus_array", [])))
    logger.info("  Generators      : %d", len(system.get("generator_array", [])))
    logger.info(
        "  Generator profs : %d", len(system.get("generator_profile_array", []))
    )
    logger.info("  Demands         : %d", len(system.get("demand_array", [])))
    logger.info("  Demand profs    : %d", len(system.get("demand_profile_array", [])))
    logger.info("  Lines           : %d", len(system.get("line_array", [])))
    logger.info("  Batteries       : %d", len(system.get("battery_array", [])))
    logger.info("  Blocks          : %d", len(simulation.get("block_array", [])))
    logger.info("  Stages          : %d", len(simulation.get("stage_array", [])))
    logger.info("  Scenarios       : %d", len(simulation.get("scenario_array", [])))

    # Note elements that will be skipped during conversion
    skipped: list[str] = []
    for key, label in (
        ("battery_array", "batteries"),
        ("converter_array", "converters"),
        ("junction_array", "junctions"),
        ("waterway_array", "waterways"),
        ("reservoir_array", "reservoirs"),
        ("turbine_array", "turbines"),
        ("reservoir_seepage_array", "seepages"),
        ("reservoir_discharge_limit_array", "discharge limits"),
        ("reservoir_production_factor_array", "production factors"),
        ("flow_array", "flows"),
    ):
        count = len(system.get(key, []))
        if count > 0:
            skipped.append(f"{count} {label}")
    if skipped:
        logger.info("  Skipped (no pp equivalent): %s", ", ".join(skipped))


def run_source_check(case: dict[str, Any]) -> None:
    """Validate the source gtopt JSON using gtopt_check_json.

    Logs a summary of the source case, and if gtopt_check_json is
    available, runs format_info() and run_all_checks().  Skips
    gracefully if gtopt_check_json is not installed.

    Parameters
    ----------
    case
        The parsed gtopt JSON case dictionary.
    """
    _log_source_summary(case)

    try:
        from gtopt_check_json._info import format_info  # noqa: PLC0415
        from gtopt_check_json._checks import (  # noqa: PLC0415
            run_all_checks,
            Severity,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation checks")
        return

    logger.info("=== gtopt_check_json: system info ===")
    for line in format_info(case).splitlines():
        logger.info("  %s", line)

    findings = run_all_checks(case, enabled_checks=None, ai_options=None)

    if not findings:
        logger.info("gtopt_check_json: all checks passed — no issues found.")
        return

    critical_count = 0
    warning_count = 0
    note_count = 0
    for finding in findings:
        if finding.severity == Severity.CRITICAL:
            logger.error("[CRITICAL] (%s) %s", finding.check_id, finding.message)
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            logger.warning("[WARNING] (%s) %s", finding.check_id, finding.message)
            warning_count += 1
        else:
            logger.info("[NOTE] (%s) %s", finding.check_id, finding.message)
            note_count += 1

    logger.info(
        "gtopt_check_json summary: %d critical, %d warnings, %d notes",
        critical_count,
        warning_count,
        note_count,
    )


def _log_conversion_summary(
    case: dict[str, Any],
    net: Any,
    block_uids: list[int],
) -> None:
    """Log a summary of what was actually converted to pandapower."""
    logger.info("=== Conversion summary ===")
    logger.info("  pp buses        : %d", len(net.bus))
    logger.info("  pp ext_grid     : %d", len(net.ext_grid))
    logger.info("  pp generators   : %d", len(net.gen))
    logger.info("  pp loads        : %d", len(net.load))
    logger.info("  pp lines        : %d", len(net.line))
    logger.info("  pp transformers : %d", len(net.trafo))
    logger.info("  Blocks converted: %s", block_uids)


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        prog="gtopt2pp",
        description=(
            "Convert a gtopt JSON case to a pandapower network and "
            "optionally run DC OPF."
        ),
    )
    parser.add_argument(
        "case_file",
        type=Path,
        help="Path to the gtopt JSON case file.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help=(
            "Output path for the pandapower JSON file.  "
            "If not given, writes to <case_stem>_pp.json."
        ),
    )
    parser.add_argument(
        "-s",
        "--scenario",
        type=int,
        default=None,
        help=("Scenario UID as defined in scenario_array (default: first scenario)."),
    )
    parser.add_argument(
        "-b",
        "--block",
        type=str,
        default=None,
        help=(
            "Block UID specification: a single UID, a comma-separated "
            "list (e.g. '1,2,4'), a range (e.g. '1-5'), or a mix "
            "(e.g. '1,3-5,8').  Multiple blocks produce one output file "
            "per block named <stem>_pp_b<UID>.json.  "
            "(default: first block)"
        ),
    )
    parser.add_argument(
        "--solve",
        action="store_true",
        default=False,
        help="Run pandapower DC OPF after conversion.",
    )
    parser.add_argument(
        "--all-blocks",
        action="store_true",
        default=False,
        help=(
            "Convert all blocks (one network per block).  "
            "Output files are named <stem>_pp_b<UID>.json."
        ),
    )
    parser.add_argument(
        "--check",
        dest="run_check",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "validate the source gtopt JSON via gtopt_check_json before "
            "conversion: prints element counts and basic consistency "
            "checks. Use --no-check to disable. (default: enabled)"
        ),
    )
    parser.add_argument(
        "--diagnostic",
        action="store_true",
        default=False,
        help=(
            "Run pandapower diagnostic on the converted network and "
            "print the results.  Useful for detecting topology issues "
            "such as disconnected elements, different voltage levels "
            "connected by lines, or missing load/generation."
        ),
    )
    add_common_arguments(parser)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    args = _parse_args(argv)
    configure_logging(args)
    case = load_gtopt_case(args.case_file)

    # Validate source JSON before conversion
    if args.run_check:
        run_source_check(case)

    simulation = case.get("simulation", {})
    blocks = simulation.get("block_array", [{"uid": 1, "duration": 1}])

    # Determine block UIDs to process
    if args.block is not None:
        block_uids = _parse_block_spec(args.block)
    else:
        block_uids = [int(blocks[0]["uid"])] if blocks else [1]

    def _maybe_diagnostic(net: Any, label: str = "") -> None:
        """Run and print pandapower diagnostic if --diagnostic was given."""
        if not args.diagnostic:
            return
        prefix = f" ({label})" if label else ""
        diag = run_diagnostic(net)
        report = format_diagnostic(diag)
        print(f"\n=== pandapower diagnostic{prefix} ===")
        print(report)

    if args.solve:
        if len(block_uids) > 1:
            print(
                "Warning: --solve uses only the first block; "
                f"ignoring blocks {block_uids[1:]}",
                file=sys.stderr,
            )
        bi = block_uids[0]
        net = run_dcopp(case, scenario=args.scenario, block=bi)
        print(f"DC OPF converged: {net.OPF_converged}")
        print(f"Objective: {net.res_cost:.4f}")

        _log_conversion_summary(case, net, [bi])
        _maybe_diagnostic(net, f"block {bi}")

        out = args.output or args.case_file.with_name(
            f"{args.case_file.stem}_pp_solved.json"
        )
        pp.to_json(net, str(out))
        print(f"Written: {out}")
        return 0

    if args.all_blocks:
        block_uids = [int(b["uid"]) for b in blocks]

    if len(block_uids) == 1:
        bi = block_uids[0]
        net = convert(case, scenario=args.scenario, block=bi)
        _log_conversion_summary(case, net, [bi])
        _maybe_diagnostic(net, f"block {bi}")
        out = args.output or args.case_file.with_name(f"{args.case_file.stem}_pp.json")
        pp.to_json(net, str(out))
        print(f"Written: {out}")
    else:
        stem = args.case_file.stem
        last_net = None
        for bi in block_uids:
            net = convert(case, scenario=args.scenario, block=bi)
            last_net = net
            _maybe_diagnostic(net, f"block {bi}")
            if args.output:
                out = args.output.with_stem(f"{args.output.stem}_b{bi}")
            else:
                out = args.case_file.with_name(f"{stem}_pp_b{bi}.json")
            pp.to_json(net, str(out))
            print(f"Block {bi}: written {out}")
        if last_net is not None:
            _log_conversion_summary(case, last_net, block_uids)
    return 0


if __name__ == "__main__":
    sys.exit(main())
