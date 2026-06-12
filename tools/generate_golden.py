#!/usr/bin/env python3
"""Generate golden reference CSV files from pandapower DC OPF results.

This tool starts from a pandapower network (built-in or loaded from
MATPOWER .m file), adds realistic generation costs, runs DC OPF, and
writes both the gtopt system JSON (via pp2gtopt) and the golden CSV
output files.

Usage:
    # Generate for IEEE 118-bus (built-in pandapower case)
    python tools/generate_golden.py --case 118

    # Generate from a MATPOWER .m file
    python tools/generate_golden.py path/to/case.m --name ieee_118b

    # Regenerate golden files for an existing gtopt case (via gtopt2pp)
    python tools/generate_golden.py --gtopt-case cases/ieee_30b/ieee30b.json
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandapower as pp
import pandapower.networks as pn

# Reference linear costs ($/MWh) from pglib-opf for IEEE 118-bus
PGLIB_118_COSTS = {
    10: 24.983420, 12: 124.581564, 25: 28.948321, 26: 22.220980,
    31: 25.993982, 46: 24.202306, 49: 16.673942, 54: 27.277343,
    59: 24.861868, 61: 16.056042, 65: 34.781778, 66: 32.668781,
    69: 25.758442, 80: 24.600772, 87: 34.072633, 89: 24.605102,
    100: 12.612170, 103: 28.649471, 111: 35.043401,
}


def add_default_costs(net, cost_map=None):
    """Add linear generation costs to a pandapower network."""
    if cost_map is None:
        cost_map = {}
    for i in net.gen.index:
        bus = int(net.gen.at[i, 'bus'])
        cp1 = cost_map.get(bus, 30.0)
        net.poly_cost.loc[len(net.poly_cost)] = {
            "element": i, "et": "gen",
            "cp0_eur": 0.0, "cp1_eur_per_mw": cp1,
            "cp2_eur_per_mw2": 0.0,
            "cq0_eur": 0.0, "cq1_eur_per_mvar": 0.0, "cq2_eur_per_mvar2": 0.0,
        }


def write_golden_csv(output_dir, rel_path, entries):
    """Write one golden CSV file from (uid_str, value) pairs."""
    path = output_dir / rel_path
    path.parent.mkdir(parents=True, exist_ok=True)
    keys = [f'"uid:{u}"' for u, v in entries]
    vals = [f"{v:.15g}" for u, v in entries]
    with open(path, "w") as f:
        f.write(f'"scenario","stage","block",{",".join(keys)}\n')
        f.write(f'1,1,1,{",".join(vals)}\n')


def generate_from_network(net, name, output_dir):
    """Run DC OPF and write golden files from a pandapower network."""
    pp.rundcopp(net)
    add_default_costs(net, PGLIB_118_COSTS)

    nbus, ngen = len(net.bus), len(net.gen)
    nload = len(net.load)

    write_golden_csv(output_dir, "Bus/balance_dual.csv",
                     [(str(i+1), net.res_bus.lam_p.iloc[i]) for i in range(nbus)])
    write_golden_csv(output_dir, "Bus/theta_sol.csv",
                     [(str(i+1), net.res_bus.va_degree.iloc[i]) for i in range(nbus)])
    write_golden_csv(output_dir, "Bus/theta_cost.csv",
                     [(str(i+1), 0.0) for i in range(nbus)])
    write_golden_csv(output_dir, "Generator/generation_sol.csv",
                     [(str(i+1), net.res_gen.p_mw.iloc[i]) for i in range(ngen)])
    write_golden_csv(output_dir, "Generator/generation_cost.csv",
                     [(str(i+1), 0.0) for i in range(ngen)])
    write_golden_csv(output_dir, "Line/flowp_sol.csv",
                     [(str(i+1), net.res_line.p_from_mw.iloc[i]) for i in range(len(net.res_line))])
    write_golden_csv(output_dir, "Demand/load_sol.csv",
                     [(str(i+1), net.res_load.p_mw.iloc[i]) for i in range(nload)])
    write_golden_csv(output_dir, "Demand/load_cost.csv",
                     [(str(i+1), 0.0) for i in range(nload)])
    write_golden_csv(output_dir, "Demand/fail_sol.csv",
                     [(str(i+1), 0.0) for i in range(nload)])

    obj = float(net.get('res_cost', 0))
    with open(output_dir / "solution.csv", "w") as f:
        f.write('"scene","phase","status","obj_value"\n')
        f.write(f'0,0,0,{obj:.15g}\n')
    with open(output_dir / "monolithic_status.json", "w") as f:
        json.dump({"status": "optimal", "obj_value": obj}, f, indent=2)

    print(f"  Objective: {obj:.2f}")
    print(f"  Golden files in {output_dir}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate golden reference CSVs from pandapower")
    parser.add_argument("--case", type=int, choices=[14, 30, 57, 118],
                        help="Built-in pandapower case number")
    parser.add_argument("--matpower", type=str,
                        help="Path to MATPOWER .m file")
    parser.add_argument("--name", type=str, default="ieee_case",
                        help="Case name (for directory naming)")
    parser.add_argument("--gtopt-case", type=str,
                        help="Path to existing gtopt JSON (uses gtopt2pp)")
    parser.add_argument("--output-dir", "-o", type=str,
                        help="Output directory (default: cases/<name>/output)")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent

    if args.gtopt_case:
        # Via gtopt2pp conversion (less accurate)
        sys.path.insert(0, str(repo_root / "scripts" / "gtopt2pp"))
        from convert import load_gtopt_case, convert as to_pandapower
        gtopt_case = load_gtopt_case(args.gtopt_case)
        net = to_pandapower(gtopt_case, scenario=1, block=1)
        output_dir = Path(args.output_dir or Path(args.gtopt_case).parent / "output")
        pp.rundcopp(net)
        generate_from_network(net, Path(args.gtopt_case).stem, output_dir)
        return

    # Build from a pandapower network
    if args.case:
        case_name = f"ieee_{args.case}b"
        net = getattr(pn, f"case{args.case}")()
        add_default_costs(net, PGLIB_118_COSTS if args.case == 118 else None)
    elif args.matpower:
        from pandapower.converter import from_mpc
        net = from_mpc(args.matpower)
        case_name = args.name
        add_default_costs(net)
    else:
        parser.print_help()
        sys.exit(1)

    # Generate gtopt JSON via pp2gtopt
    sys.path.insert(0, str(repo_root / "scripts" / "pp2gtopt"))
    from convert import convert as to_gtopt_json
    case_dir = repo_root / "cases" / case_name
    case_dir.mkdir(parents=True, exist_ok=True)

    gtopt_case = to_gtopt_json(output_path=str(case_dir / f"{case_name}.json"),
                               net=net, name=case_name)

    output_dir = Path(args.output_dir or case_dir / "output")
    pp.rundcopp(net)
    generate_from_network(net, case_name, output_dir)


if __name__ == "__main__":
    main()

