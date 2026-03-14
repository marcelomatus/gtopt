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


def _parse_block_spec(spec: str) -> list[int]:
    """Parse a block specification string into a sorted list of indices.

    Accepts:
    - A single integer: ``"0"``
    - A comma-separated list: ``"0,1,3"``
    - A range: ``"0-5"`` (inclusive on both ends)
    - A mix: ``"0,2-4,7"``

    Returns a sorted, deduplicated list of 0-based block indices.
    """
    indices: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_s, hi_s = part.split("-", maxsplit=1)
            lo, hi = int(lo_s), int(hi_s)
            indices.update(range(lo, hi + 1))
        else:
            indices.add(int(part))
    return sorted(indices)


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
        type=str,
        default="0",
        help=(
            "Block specification: a single 0-based index, a comma-separated "
            "list (e.g. '0,1,3'), a range (e.g. '0-5'), or a mix "
            "(e.g. '0,2-4,7').  Multiple blocks produce one output file "
            "per block named <stem>_pp_b<N>.json.  (default: 0)"
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
            "Output files are named <stem>_pp_b<N>.json."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    args = _parse_args(argv)
    case = load_gtopt_case(args.case_file)

    block_indices = _parse_block_spec(args.block)

    if args.solve:
        bi = block_indices[0]
        net = run_dcopp(case, scenario=args.scenario, block=bi)
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
        block_indices = list(range(len(blocks)))

    if len(block_indices) == 1:
        bi = block_indices[0]
        net = convert(case, scenario=args.scenario, block=bi)
        out = args.output or args.case_file.with_name(
            f"{args.case_file.stem}_pp.json"
        )
        pp.to_json(net, str(out))
        print(f"Written: {out}")
    else:
        stem = args.case_file.stem
        for bi in block_indices:
            net = convert(case, scenario=args.scenario, block=bi)
            out = args.output or args.case_file.with_name(
                f"{stem}_pp_b{bi}.json"
            )
            pp.to_json(net, str(out))
            print(f"Block {bi}: written {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
