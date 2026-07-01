#!/usr/bin/env python3
"""Compare gtopt day-1 (blocks 1-24) system-avg bus LMP to PSR cmgbuscp."""

import csv
import statistics
import sys

CMG = "support/luis/ncp_pdd_22jun/psr_ref/PDD_DIARIO/cmgbuscp.csv"


def load_psr_day1():
    """cmgbuscp: 3 header lines + col header + 24 hourly rows (830 bus cols)."""
    with open(CMG) as f:
        lines = f.read().splitlines()
    rows = lines[4:]  # skip 3 meta + 1 column-header line
    out = {}
    for ln in rows:
        c = [x.strip() for x in ln.split(",") if x.strip() != ""]
        if len(c) < 4:
            continue
        # The daily curve's 24 hours are in the Stag column (Blck is always 1).
        hour = int(c[0])
        vals = [float(x) for x in c[3:]]
        out[hour] = statistics.fmean(vals)
    return out


def load_gtopt_day1(outdir):
    """Bus/balance_dual long CSV; system-avg per block for blocks 1-24."""
    acc = {}
    with open(f"{outdir}/Bus/balance_dual_s0_p0.csv") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            blk = int(row[2])
            if 1 <= blk <= 24:
                acc.setdefault(blk, []).append(float(row[4]))
    return {b: statistics.fmean(v) for b, v in acc.items()}


def corr(a, b):
    n = len(a)
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = sum((x - ma) ** 2 for x in a) ** 0.5
    db = sum((y - mb) ** 2 for y in b) ** 0.5
    return cov / (da * db) if da and db else float("nan")


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out_baseline"
    psr = load_psr_day1()
    gto = load_gtopt_day1(outdir)
    blocks = sorted(set(psr) & set(gto))
    pv = [psr[b] for b in blocks]
    gv = [gto[b] for b in blocks]
    print(
        f"=== {outdir} : day-1 (blocks {blocks[0]}-{blocks[-1]}, n={len(blocks)}) ==="
    )
    print(f"  PSR  cmgbuscp mean = {statistics.fmean(pv):7.2f}")
    print(f"  gtopt LMP      mean = {statistics.fmean(gv):7.2f}")
    print(f"  correlation         = {corr(pv, gv):+.3f}")
    print(f"  {'blk':>3} {'PSR':>8} {'gtopt':>8} {'diff':>8}")
    for b in blocks:
        print(f"  {b:>3} {psr[b]:>8.1f} {gto[b]:>8.1f} {gto[b] - psr[b]:>8.1f}")


if __name__ == "__main__":
    main()
