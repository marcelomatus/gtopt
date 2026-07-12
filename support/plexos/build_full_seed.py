#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build a COMPLETE MIP warm-start seed from a solved reduced case.

gtopt writes ``Commitment/status_sol.parquet`` SPARSELY — only the cells
whose commitment is ON (``value == 1``) appear.  A seed that lists just
those rows leaves every OFF (generator, block) unspecified, so CPLEX must
guess them; the resulting partial MIP-start is frequently rejected.

This builder enumerates the FULL cross product ``committable-generators ×
blocks`` and fills absent cells with ``u = 0``, producing the dense
``generator_uid, block_uid, u`` seed the full-MIP ``mip_start`` expects.

The commitment-uid → generator-uid mapping is imported verbatim from
``run_warmstart_experiment`` so the reduced and full models agree on
generator identity (reduction never renames generators).

Usage:
    python build_full_seed.py REDUCED_OUT_DIR REDUCED_JSON OUT_SEED.csv
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import pyarrow.dataset as pads

from run_warmstart_experiment import _commitment_to_generator


def build_full_seed(out_dir: Path, mip_json: Path, seed_csv: Path) -> dict:
    """Enumerate every (committable generator, block); absent → u=0.

    Returns a small summary dict (rows, u1, u0, generators, blocks).
    """
    status = out_dir / "Commitment" / "status_sol.parquet"
    if not status.exists():
        raise FileNotFoundError(status)
    df = pads.dataset(status).to_table(columns=["block", "uid", "value"]).to_pandas()

    c2g = _commitment_to_generator(mip_json)
    df["generator_uid"] = df["uid"].map(c2g)
    df = df[df["generator_uid"].notna()].copy()

    # ON set: (generator, block) with any commitment status >= 0.5.
    on = df[df["value"] >= 0.5]
    on_pairs = {(int(g), int(b)) for g, b in zip(on["generator_uid"], on["block"])}

    generators = sorted({int(g) for g in c2g.values()})
    blocks = sorted({int(b) for b in df["block"].unique()})

    u1 = 0
    with seed_csv.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["generator_uid", "block_uid", "u"])
        for g in generators:
            for b in blocks:
                u = 1 if (g, b) in on_pairs else 0
                u1 += u
                w.writerow([g, b, u])

    total = len(generators) * len(blocks)
    return {
        "rows": total,
        "u1": u1,
        "u0": total - u1,
        "generators": len(generators),
        "blocks": len(blocks),
    }


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    out_dir, mip_json, seed_csv = (Path(a) for a in sys.argv[1:4])
    summary = build_full_seed(out_dir, mip_json, seed_csv)
    print(
        f"seed {seed_csv.name}: rows={summary['rows']} "
        f"u1={summary['u1']} u0={summary['u0']} "
        f"({summary['generators']} generators × {summary['blocks']} blocks)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
