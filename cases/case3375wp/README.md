# `case3375wp` — Polish 3375-Bus Grid (DC OPF, large-scale)

Large-scale e2e fixture: the Polish 400 / 220 / 110 kV transmission
network during winter 2007-08 evening peak, including equivalents of
the German, Czech and Slovak networks.  Used to exercise gtopt on a
~3375-bus DC OPF (the largest network in the e2e suite) and to verify
solve time, memory, and feasibility on a real-world-sized problem.

## Source

* **Original**: Roman Korab (Silesian University of Technology), 2007-08.
* **Vendored via**: [MATPOWER `case3375wp.m`](https://github.com/MATPOWER/matpower/blob/master/data/case3375wp.m)
  (3-clause BSD per the MATPOWER LICENSE).
* **Converter**: a Python one-shot that reads the MATPOWER `.m` via
  [`matpowercaseframes`](https://pypi.org/project/matpowercaseframes/)
  and writes the gtopt JSON directly.  See "Regenerating the JSON"
  below.

## Why this case stands in for `cn_3266b`

The original goal was the Chinese provincial 3266-bus dataset from
IEEE DataPort (DOI 10.21227/vma9-wk20).  That dataset is behind a
paid IEEE DataPort subscription, so it can't be vendored or fetched
in CI.  `case3375wp` is the closest *publicly-available* substitute
of comparable scale (3375 vs 3266 buses, BSD-licensed) and is what
the e2e test under this name actually drives.  `cases/cn_3266b/`
remains as a stub README describing the manual fetch path if you
have access to the original data.

## System data (as ingested by `pp2gtopt`-equivalent direct converter)

| Quantity | Value |
|----------|---:|
| Buses | 3 374 |
| Generators | 590 (zero-cap gens skipped) |
| Loads | 2 414 |
| Branches | 4 161 (lines + trafos collapsed to gtopt `Line`) |
| Base voltage | 400 kV / 220 kV / 110 kV |
| Base MVA | 100 |
| Total `pmax` capacity | 71 095 MW |
| Total `Pd` demand | 52 099.20 MW |
| Cost model | linear (MATPOWER `gencost` model 2, `C2 = 0`, `C1 > 0`) |

The case is feasibility-comfortable (capacity ≫ demand) and uses
linear costs only — so unlike `ieee_24b_rts` (which exercises the
demand-fail path) or `ieee_118b` (which has quadratic costs that
gtopt's LP drops), `case3375wp` exercises the *clean* large-scale
DC OPF path.

## Why no exact `obj_value` golden

For a 3 374-bus DC OPF with heterogeneous linear costs and a
non-trivial KVL constraint graph, the optimum is **not analytically
derivable in closed form** — it depends on the binding line-limit
constraints, which are determined by the LP itself.  Per the project
policy "golden values must come from literature / analytical, not
snapshotted from a gtopt run", we therefore **do not pin an exact
`obj_value`**.

The e2e test instead exercises:

1. `e2e_case3375wp_solve` — the gtopt binary must build the LP and
   solve to optimality (`status = 0`) within the configured
   timeout.  This is the primary regression check: a regression in
   LP build / cost wiring / Kirchhoff routing on a real-world-sized
   network would fire here as a solver failure, a non-zero status,
   or a timeout.
2. `e2e_case3375wp_validate_solution` — `tools/validate_solution.py`
   confirms `status = 0` and that `obj_value` parses as a finite
   number.

No `e2e_case3375wp_compare_<csv>` entries are registered (empty
`output/` dir ⇒ nothing to compare against).  The check is
intentionally a *feasibility + sanity* check on the large-scale
solve path, not a numerical golden comparison.

Pandapower DC OPF cross-tool reference *should* match (same LP
formulation under linear costs), but pandapower's `from_mpc`
converter has a known shape bug on this fixture
(`pandapower/converter/pypower/from_ppc.py:303`) that prevents it
from being run as the reference.  If pandapower fixes that bug, the
case can be re-baselined with `add_pandapower_comparison(case3375wp)`.

## Regenerating the JSON

```python
from matpowercaseframes import CaseFrames
import json
from pathlib import Path

cf = CaseFrames("/path/to/MATPOWER/data/case3375wp.m")
# … (the per-element loops mirror the cases registered in
#    cases/case3375wp/case3375wp.json — see the conversion
#    walkthrough in the git history)

with Path("cases/case3375wp/case3375wp.json").open("w") as fh:
    json.dump(case, fh, indent=2)
```

The committed JSON is a snapshot of this conversion against
MATPOWER `case3375wp.m` at the commit captured in the project's
fixture-provenance log.  Re-running on a different `.m` revision
will produce a different JSON and require a sanity re-check.

## CTest coverage

```cmake
add_e2e_case(case3375wp case3375wp.json)
```

Two CTest entries per the empty-`output/` shape: `_solve` and
`_validate_solution`.  No `_compare_solution`.
