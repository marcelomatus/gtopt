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
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    args = _parse_args(argv)
    case = load_gtopt_case(args.case_file)

    simulation = case.get("simulation", {})
    blocks = simulation.get("block_array", [{"uid": 1, "duration": 1}])

    # Determine block UIDs to process
    if args.block is not None:
        block_uids = _parse_block_spec(args.block)
    else:
        block_uids = [int(blocks[0]["uid"])] if blocks else [1]

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
        out = args.output or args.case_file.with_name(f"{args.case_file.stem}_pp.json")
        pp.to_json(net, str(out))
        print(f"Written: {out}")
    else:
        stem = args.case_file.stem
        for bi in block_uids:
            net = convert(case, scenario=args.scenario, block=bi)
            if args.output:
                out = args.output.with_stem(f"{args.output.stem}_b{bi}")
            else:
                out = args.case_file.with_name(f"{stem}_pp_b{bi}.json")
            pp.to_json(net, str(out))
            print(f"Block {bi}: written {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
