"""CLI entry point for the Sienna → gtopt converter.

Usage::

    python -m sienna_to_gtopt cascading_hydro    -o cascading.json
    python -m sienna_to_gtopt monitored_line     -o mline.json
    python -m sienna_to_gtopt hvdc               -o hvdc.json
    python -m sienna_to_gtopt pumped_storage     -o phes.json
    python -m sienna_to_gtopt battery_degeneracy --subcase b -o batt_b.json
    python -m sienna_to_gtopt interruptible_load -o il.json
    python -m sienna_to_gtopt fuel_cost_ts       -o fuel_ts.json
    python -m sienna_to_gtopt wecc_240           -o wecc240.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from sienna_to_gtopt import VARIANTS
from sienna_to_gtopt._battery_degeneracy import VALID_SUBCASES
from sienna_to_gtopt._bundle import extract_bundle
from sienna_to_gtopt._reader import load_case


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="sienna_to_gtopt",
        description=(
            "Port a NREL-Sienna PowerSimulations.jl test case to a gtopt planning JSON."
        ),
    )
    parser.add_argument(
        "variant",
        choices=sorted(VARIANTS.keys()),
        help="Which variant builder to invoke.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output JSON path (default: <variant>.json in cwd).",
    )
    parser.add_argument(
        "--bundle",
        default="5bus",
        help="Which packaged bundle to extract (default: 5bus).",
    )
    parser.add_argument(
        "--subcase",
        choices=list(VALID_SUBCASES),
        default="b",
        help=(
            "battery_degeneracy subcase selector "
            "(b/c/d/e/f) — only consumed by the battery_degeneracy variant."
        ),
    )
    args = parser.parse_args(argv)

    case_dir = extract_bundle(args.bundle)
    case = load_case(case_dir)
    builder = VARIANTS[args.variant]
    if args.variant == "battery_degeneracy":
        payload = builder(case, subcase=args.subcase)
        default_name = f"{args.variant}_{args.subcase}.json"
    else:
        payload = builder(case)
        default_name = f"{args.variant}.json"

    output = args.output or Path(default_name)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w") as fh:
        json.dump(payload, fh, indent=2)

    sys_block = payload.get("system", {})
    print(
        f"Sienna {args.variant}: "
        f"buses={len(sys_block.get('bus_array', []))} "
        f"gens={len(sys_block.get('generator_array', []))} "
        f"lines={len(sys_block.get('line_array', []))} "
        f"junctions={len(sys_block.get('junction_array', []))} "
        f"waterways={len(sys_block.get('waterway_array', []))} "
        f"reservoirs={len(sys_block.get('reservoir_array', []))} "
        f"turbines={len(sys_block.get('turbine_array', []))} "
        f"batteries={len(sys_block.get('battery_array', []))} "
        f"fuels={len(sys_block.get('fuel_array', []))} "
        f"=> {output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
