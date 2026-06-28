# psse2gtopt test fixtures — provenance

This directory holds the PSS®E `.raw` fixtures used by the
`psse2gtopt` test-suite.

## `synthetic.raw` — hand-crafted (BSD-3-Clause, this repo)

A minimal, fully synthetic PSS®E revision-33 case authored for this
project.  It deliberately exercises every record type and edge case the
parser/writer must handle:

- PQ / PV / slack / **isolated** (IDE=4) buses,
- in-service and **out-of-service** loads, plus a **zero-MW** load,
- in-service / out-of-service generators and a **synchronous condenser**
  (`pmax = 0`),
- a normal branch, an out-of-service branch, and a **zero-reactance**
  branch (reactance-floor test),
- a **CZ=1** and a **CZ=2** two-winding transformer (impedance-base
  conversion test),
- a **three-winding** transformer (star-equivalent expansion test).

All expected values are asserted exactly in `test_raw_parser.py` /
`test_gtopt_writer.py`.

## `ieee14.raw`, `ieee39.raw` — standard public IEEE benchmark systems

These are the classic **IEEE 14-bus** and **New England 39-bus** test
systems, rendered in the PSS®E RAW format (rev 32 and rev 33
respectively).  They are long-standing, publicly distributed power-system
benchmark networks (factual network parameters, originally prepared by
José Conto / ERCOT) that are redistributed across many open-source power
tools.

- Retrieved from the ANDES project case library
  (<https://github.com/cuihantao/andes>, `andes/cases/ieee14/ieee14.raw`
  and `andes/cases/ieee39/ieee39.raw`).
- Used here **read-only**, purely as parser/converter validation
  fixtures — no ANDES source code is vendored or derived.

They provide real-world coverage that the synthetic fixture cannot:
both PSS/E format revisions, real per-unit impedances and MVA ratings,
and a non-trivial mesh topology that gtopt solves to optimality with
zero unserved load.
