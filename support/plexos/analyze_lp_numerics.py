#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Scan gtopt CPLEX-LP files for numerical pathologies.

Streams each ``.lp`` (these are hundreds of MB) section-by-section and
classifies every numeric token:

* **coefficient** — a number in the objective / Subject-To matrix
  (token NOT immediately following a relational operator).
* **rhs** — the number after ``<= >= = < >`` in a constraint.
* **bound** — any number in the Bounds section.

Reports, per file and aggregated:

* coefficient dynamic range (max|c| / min|c|≠0) — the kappa driver;
* magnitude histogram by decade;
* sentinel hits: ±1e30 / ±1e20 / ±1e15 "infinity", ±100000 / ±10000
  PLEXOS no-limit caps, and tiny (<1e-7) nonzero coefficients;
* the largest-magnitude coefficient rows (for eyeballing).
"""

from __future__ import annotations

import argparse
import json
import math
import re
from collections import Counter
from pathlib import Path

NUM_RE = re.compile(r"^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$")
RELOPS = {"<=", ">=", "=", "<", ">"}
SECTION_RE = re.compile(
    r"^\s*(minimize|maximize|subject to|st|s\.t\.|bounds|generals|general|"
    r"binaries|binary|integers|semi-continuous|sos|end)\b",
    re.I,
)
INF = 1e29
SENTINELS = {
    "inf_1e30": lambda a: a >= INF,
    "big_1e20": lambda a: 1e19 <= a < INF,
    "big_1e15": lambda a: 1e14 <= a < 1e19,
    "cap_100000": lambda a: abs(a - 100000.0) < 1e-6,
    "cap_10000": lambda a: abs(a - 10000.0) < 1e-6,
    "tiny_lt_1e-7": lambda a: 0.0 < a < 1e-7,
}


def _decade(a: float) -> int:
    if a == 0.0:
        return -999
    return int(math.floor(math.log10(a)))


def analyze(path: Path) -> dict:
    section = "head"
    # stats per kind
    kinds = ("coef", "rhs", "bound")
    n: dict[str, int] = {k: 0 for k in kinds}
    mn: dict[str, float] = {k: math.inf for k in kinds}
    mx: dict[str, float] = {k: 0.0 for k in kinds}
    deca: dict[str, Counter] = {k: Counter() for k in kinds}
    sent: dict[str, Counter] = {k: Counter() for k in kinds}
    max_coef_rows: list[tuple[float, str]] = []  # (abs, rowname)
    cur_row = "?"

    def record(kind: str, a: float) -> None:
        n[kind] += 1
        if a != 0.0:
            mn[kind] = min(mn[kind], a)
            mx[kind] = max(mx[kind], a)
            deca[kind][_decade(a)] += 1
        for label, fn in SENTINELS.items():
            if fn(a):
                sent[kind][label] += 1

    with path.open("r", encoding="latin-1", errors="ignore") as fh:
        for line in fh:
            s = line.strip()
            if not s or s.startswith("\\"):
                continue
            msec = SECTION_RE.match(s)
            if msec:
                key = msec.group(1).lower()
                if key in ("minimize", "maximize"):
                    section = "obj"
                elif key in ("subject to", "st", "s.t."):
                    section = "matrix"
                elif key == "bounds":
                    section = "bounds"
                elif key == "end":
                    section = "end"
                else:
                    section = "other"
                continue
            if section == "end":
                break
            # capture row label at statement start ("name:")
            head = s.split(None, 1)[0]
            if head.endswith(":"):
                cur_row = head[:-1]
            toks = s.replace("+", " + ").replace(":", " : ").split()
            prev_relop = False
            row_max = 0.0
            for t in toks:
                if t in RELOPS:
                    prev_relop = True
                    continue
                if not NUM_RE.match(t):
                    prev_relop = False
                    continue
                a = abs(float(t))
                if section == "bounds":
                    record("bound", a)
                elif section in ("obj", "matrix"):
                    if prev_relop:
                        record("rhs", a)
                    else:
                        record("coef", a)
                        if section == "matrix" and a > row_max:
                            row_max = a
                prev_relop = False
            if section == "matrix" and row_max > 0.0:
                if len(max_coef_rows) < 25:
                    max_coef_rows.append((row_max, cur_row))
                    max_coef_rows.sort(reverse=True)
                elif row_max > max_coef_rows[-1][0]:
                    max_coef_rows[-1] = (row_max, cur_row)
                    max_coef_rows.sort(reverse=True)

    out: dict = {"file": path.name}
    for k in kinds:
        out[k] = {
            "n": n[k],
            "min_abs_nonzero": None if mn[k] is math.inf else mn[k],
            "max_abs": mx[k],
            "decades": dict(sorted(deca[k].items())),
            "sentinels": dict(sent[k]),
        }
    cmin = out["coef"]["min_abs_nonzero"]
    cmax = out["coef"]["max_abs"]
    out["coef_dynamic_range"] = (cmax / cmin) if (cmin and cmin > 0) else None
    out["top_coef_rows"] = [
        {"abs": round(a, 4), "row": r} for a, r in max_coef_rows[:15]
    ]
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("lp_files", nargs="+")
    ap.add_argument("--out", default=str(Path.home() / "tmp" / "lp_numerics.json"))
    args = ap.parse_args()
    report = []
    for f in args.lp_files:
        p = Path(f)
        if not p.exists():
            print(f"skip (missing): {f}", flush=True)
            continue
        print(f"scanning {p.name} ...", flush=True)
        r = analyze(p)
        report.append(r)
        Path(args.out).write_text(json.dumps(report, indent=1))
        dr = r["coef_dynamic_range"]
        print(
            f"  coef n={r['coef']['n']} range=[{r['coef']['min_abs_nonzero']}, "
            f"{r['coef']['max_abs']}] dyn={dr:.2e}"
            if dr
            else f"  coef n={r['coef']['n']}",
            flush=True,
        )
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
