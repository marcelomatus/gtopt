#!/usr/bin/env python3
"""Build a deterministic N-week SDDP case from the one-week NCP JSON.

Each 168-hour week collapses to ONE weekly block (weekly average, matching PSR's
NUMERO DE BLOQUES DEMANDA 1), replicated across N weekly stages so gtopt's SDDP
couples the state-variable reservoirs across weeks and computes its OWN
future-cost cuts.  Deterministic: the single caudcp forecast week is repeated
(PSR's 25 stochastic series are not shipped).

Every block schedule (any depth bottoming out in a 168-value block list:
`[block]`, `[stage][block]`, or `[scene][stage][block]`) is normalized to the
canonical full form `[scene=1][stage=N][block=1]` with the weekly-mean value.

Usage: build_multiweek.py IN_JSON OUT_JSON [N_WEEKS]
"""

import json
import sys

src, out = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 12
BLOCKS = 168
WEEK_HOURS = 168.0
stats = {"count": 0}


def _nums(v):
    return isinstance(v, list) and v and all(isinstance(x, (int, float)) for x in v)


def collapse(v):
    """Collapse a block schedule to N weekly stages, PRESERVING the field's
    original nesting depth (block 168->1 weekly mean; stage dim 1->N)."""
    # 3-deep [scene=1][stage=1][block=168]
    if (
        isinstance(v, list)
        and len(v) == 1
        and isinstance(v[0], list)
        and len(v[0]) == 1
        and _nums(v[0][0])
        and len(v[0][0]) == BLOCKS
    ):
        w = sum(v[0][0]) / BLOCKS
        return [[[w] for _ in range(N)]]  # [1][N][1]
    # 2-deep [stage=1][block=168]
    if isinstance(v, list) and len(v) == 1 and _nums(v[0]) and len(v[0]) == BLOCKS:
        w = sum(v[0]) / BLOCKS
        return [[w] for _ in range(N)]  # [N][1]
    return None


def walk(obj):
    if isinstance(obj, dict):
        out_d = {}
        for k, v in obj.items():
            c = collapse(v)
            if c is not None:
                out_d[k] = c
                stats["count"] += 1
            else:
                out_d[k] = walk(v)
        return out_d
    if isinstance(obj, list):
        return [walk(x) for x in obj]
    return obj


d = json.load(open(src))
d["system"] = walk(d["system"])

d["simulation"]["block_array"] = [
    {"uid": t + 1, "duration": WEEK_HOURS} for t in range(N)
]
d["simulation"]["stage_array"] = [
    {"uid": t + 1, "first_block": t, "count_block": 1, "active": 1} for t in range(N)
]
d["simulation"]["phase_array"] = [
    {"uid": t + 1, "first_stage": t, "count_stage": 1, "active": 1} for t in range(N)
]
d["simulation"]["scenario_array"] = [{"uid": 1, "probability_factor": 1}]

o = d.setdefault("options", {})
o["method"] = "sddp"
o.pop("monolithic_options", None)

json.dump(d, open(out, "w"))
print(
    f"wrote {out}: {N} weekly stages, {stats['count']} block schedules "
    f"normalized to [1][{N}][1] weekly-mean, method=sddp"
)
