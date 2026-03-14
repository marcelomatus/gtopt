"""CLI entry point for gtopt2pp — convert gtopt JSON to pandapower."""

import argparse
import sys
from pathlib import Path

from gtopt2pp.convert import convert, load_gtopt_case, run_dcopp

try:
    import pandapower as pp

    _HAS_PP = True
except ImportError:  # pragma: no cover
    _HAS_PP = False


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
        default=0,
        help="0-based scenario index (default: 0).",
    )
    parser.add_argument(
        "-b",
        "--block",
        type=int,
        default=0,
        help="0-based block index (default: 0).",
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
            "Output files are named <stem>_pp_b<N>.json."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    args = _parse_args(argv)
    case = load_gtopt_case(args.case_file)

    if args.solve:
        net = run_dcopp(case, scenario=args.scenario, block=args.block)
        print(f"DC OPF converged: {net.OPF_converged}")
        print(f"Objective: {net.res_cost:.4f}")

        out = args.output or args.case_file.with_name(
            f"{args.case_file.stem}_pp_solved.json"
        )
        pp.to_json(net, str(out))
        print(f"Written: {out}")
        return 0

    if args.all_blocks:
        simulation = case.get("simulation", {})
        blocks = simulation.get("block_array", [{"uid": 1}])
        for bi in range(len(blocks)):
            net = convert(case, scenario=args.scenario, block=bi)
            stem = args.case_file.stem
            out = args.output or args.case_file.with_name(f"{stem}_pp_b{bi}.json")
            pp.to_json(net, str(out))
            print(f"Block {bi}: written {out}")
        return 0

    net = convert(case, scenario=args.scenario, block=args.block)
    out = args.output or args.case_file.with_name(f"{args.case_file.stem}_pp.json")
    pp.to_json(net, str(out))
    print(f"Written: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
