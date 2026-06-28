# psse2gtopt — PSS®E RAW to gtopt Converter

Converts a **PSS®E** (Siemens *Power System Simulator for
Engineering*) `.raw` power-flow file into a single-snapshot
**multi-bus DC OPF** gtopt JSON planning, mirroring the role
[`plp2gtopt`](plp2gtopt.md) and [`sddp2gtopt`](sddp2gtopt.md) play for
the PLP and PSR SDDP dialects.

---

## Overview

PSS®E is the de-facto transmission-planning and power-flow tool used by
utilities and system operators worldwide.  A PSS/E study exports its
network as a `.raw` file: a fixed-order, comma-separated text file
describing buses, loads, generators, branches and transformers.

`psse2gtopt` reads the **classic RAW format (revisions 32 and 33)** and
emits a gtopt planning that `gtopt --lp-only` ingests directly:

- every in-service bus → a gtopt `Bus` (referenced as `b<number>`);
- branches and transformers → `Line` entries carrying the per-unit
  reactance (three-winding transformers expand to a synthetic star bus
  plus three lines);
- generators → `Generator` entries (PSS/E has no costs, so a single
  configurable `--gcost` is applied);
- loads → `Demand` entries with the active power `PL` as inline `lmax`.

`use_kirchhoff = true` makes this a proper DC OPF; gtopt auto-pins one
reference bus per electrical island, so no slack handling is required.

> ⚠️  **Format scope**: this targets the classic **rev 32/33** layout.
> The newer **v34+ / RAWX / v35** format (a leading `@!IC,...` header
> comment, a system-wide data block of `GENERAL` / `GAUSS` / `NEWTON` /
> `RATING` lines, and reshaped generator/branch records) is **detected
> and rejected** with an actionable error rather than parsed into a
> garbage network.  Re-export the case as PSS/E v33 (or v32) RAW.

### Who needs this tool?

- Engineers with a PSS/E network model who want to run a DC OPF /
  curtailment study in gtopt.
- Teams cross-validating a PSS/E grid against gtopt's network LP.
- Anyone delivered a planning study as PSS/E `.raw` snapshots (e.g. the
  Guatemalan *Programación de Largo Plazo* AMM database) rather than as
  an optimisation case.

---

## Installation

Installs as part of the gtopt scripts package:

```bash
pip install ./scripts        # registers psse2gtopt on PATH
psse2gtopt --version
psse2gtopt --help
```

No dependencies beyond what the other converters already require.

---

## Quick start

```bash
# Show what's in a case (no conversion)
psse2gtopt --info case.raw

# A directory of snapshots: list resolves to the first .raw, or pick one
psse2gtopt --info ./PSS\(R\)Ev33 --raw PAESEPMED

# Schema sanity check (exit 0 if OK)
psse2gtopt --validate case.raw

# Convert (default: writes ./gtopt_<stem>/<stem>.json)
psse2gtopt case.raw

# Explicit output dir + uniform generation cost
psse2gtopt -i case.raw -o gtopt_case --gcost 25

# End-to-end smoke test
psse2gtopt -i case.raw -o gtopt_case
gtopt --lp-only -s gtopt_case/gtopt_case.json
```

---

## CLI options

| Flag                          | Default  | Description                                                   |
|-------------------------------|----------|---------------------------------------------------------------|
| `INPUT` (positional)          | —        | `.raw` file or a directory of snapshots (alternative to `-i`) |
| `-i, --input PATH`            | —        | Same as positional, takes precedence                          |
| `--raw SUBSTR`                | —        | When the input is a directory, substring selecting one `.raw` |
| `-o, --output-dir DIR`        | inferred | Output dir (`gtopt_<raw-stem>` next to the input)             |
| `--name NAME`                 | raw stem | Planning / system name                                        |
| `--gcost $/MWh`               | `10`     | Base generation cost (uniform, or rank-0 cost with `--ldm`)   |
| `--gcost-step $/MWh`          | `1`      | Per-merit-rank cost increment when `--ldm` is given           |
| `--rating-set A\|B\|C`        | `A`      | Line/transformer rating: A=normal, B/C=emergency (fallback→A) |
| `--nomenclatura PATH`         | —        | Nomenclatura `.xls/.xlsx` for human-readable bus names        |
| `--ldm PATH`                  | —        | LDM (Lista de Mérito) `.xlsx` → rank-based merit-order gcost   |
| `--demand-fail-cost $/MWh`    | `1000`   | Unserved-energy penalty                                       |
| `--scale-objective N`         | `1000`   | Objective scaling divisor for numerics                        |
| `--single-bus`                | off      | Copper-plate single-bus planning (no lines)                   |
| `--info`                      | off      | Print case summary and exit                                   |
| `--validate`                  | off      | Run schema sanity checks and exit (0 = ok)                    |
| `-l, --log-level LEVEL`       | `INFO`   | `DEBUG` / `INFO` / `WARNING` / `ERROR` / `CRITICAL`           |
| `-V, --version`               | —        | Print version and exit                                        |

---

## PSS/E RAW input file format

A classic RAW file is a fixed sequence of sections, each terminated by a
line whose first token is the single character `0` (usually
`0 / END OF … DATA, BEGIN … DATA`).  The converter reads them
positionally rather than trusting the (optional, sometimes mojibake)
comments:

```
case-id line, title 1, title 2
BUS … 0 /
LOAD … 0 /
FIXED SHUNT … 0 /
GENERATOR … 0 /
BRANCH … 0 /
TRANSFORMER … 0 /     (4 lines / 2-winding, 5 lines / 3-winding)
AREA … (and the remaining sections, which are skipped)
```

Files are read as Latin-1 (PSS/E writes Windows-1252 / Latin-1 text).

### Record fields used (0-based indices, validated against
[`grg-pssedata`](https://github.com/lanl-ansi/grg-pssedata),
[MATPOWER](https://matpower.org) and
[PowerFlowData.jl](https://github.com/nickrobinson251/PowerFlowData.jl))

| Record       | Fields read                                                          |
|--------------|---------------------------------------------------------------------|
| Case-id      | `SBASE` (1), `REV` (2), `BASFRQ` (5)                                 |
| BUS          | `I` (0), `NAME` (1), `BASKV` (2), `IDE` (3), `AREA` (4), `ZONE` (5)  |
| LOAD         | `I` (0), `ID` (1), `STATUS` (2), `PL` (5), `QL` (6)                  |
| GENERATOR    | `I` (0), `ID` (1), `STAT` (14), `PT`→pmax (16), `PB`→pmin (17)       |
| BRANCH       | `I` (0), `J` (1), `CKT` (2), `X` (4), `RATEA` (6), `ST` (13)         |
| TRANSFORMER L1 | `I` (0), `J` (1), `K` (2), `CKT` (3), `CZ` (5), `NAME` (10), `STAT` (11) |
| TRANSFORMER L2 (2-wdg) | `X1-2` (1), `SBASE1-2` (2)                                  |
| TRANSFORMER L2 (3-wdg) | `X1-2` (1), `X2-3` (4), `X3-1` (7) + SBASE columns          |
| Winding line | `RATA` (3)                                                           |

> **rev 32 vs 33**: rev-33 bus records add `NVHI/NVLO/EVHI/EVLO` and the
> transformer L1 adds `VECGRP`; both are *trailing* fields the converter
> doesn't read, so a single reader handles rev 32 and rev 33.

### IDE (bus type) codes

`1` = PQ / load, `2` = PV / generator, `3` = swing / slack,
`4` = isolated (**dropped**, along with any load / generator / line that
references it).

---

## Mapping PSS/E elements → gtopt

### Case-id → `options` + `simulation`

A `.raw` is a single power-flow snapshot, so the planning has **one
stage, one block (1 h), one scenario**.  `options.model_options` is set
to `use_single_bus = false`, `use_kirchhoff = true`,
`demand_fail_cost`, `scale_objective`.

### Buses → `bus_array`

Each non-isolated bus becomes `{uid: <number>, name: "b<number>"}`.
Lines, generators and demands reference buses by this name.

### Generators → `generator_array`

Each in-service generator with `PT > 0` becomes a `Generator` with
`pmax = PT`, `pmin = clamp(PB, 0, PT)`, `capacity = PT` and a **uniform
`gcost`** (PSS/E carries no cost data).  Out-of-service machines and
synchronous condensers (`PT = 0`) are dropped.

### Loads → `demand_array`

Each in-service load with `PL > 0` becomes a `Demand` with
`lmax = [[PL]]` (MW).  Out-of-service and zero loads are dropped.

### Branches + transformers → `line_array`

| Source                | gtopt line                                                                 |
|-----------------------|----------------------------------------------------------------------------|
| Branch                | `reactance = X` (p.u.), `tmax_ab = tmax_ba = RATEA` (MW, 0 → large cap)     |
| 2-winding transformer | `reactance = X1-2` resolved to system base via `CZ`, `tmax = RATA1`         |
| 3-winding transformer | synthetic **star bus** + 3 lines with `Z1 = ½(X12+X31−X23)` etc.            |

The per-unit reactance is passed straight through (no `voltage` on the
line), so flows come out in MW and the flow-split among parallel paths
is exact.  Zero / near-zero reactances are floored to ±1e-5 to keep the
DC flow equation `f = (θ_a − θ_b)/X` well-posed.

**`CZ` impedance code**: `1` → already on the system base (used as-is);
`2` → on the winding MVA base, scaled by `SBASE_system / SBASE_winding`;
`3` → load-loss form (rare; logged and used as-is).

---

## Auxiliary AMM data (`--nomenclatura`, `--ldm`)

A PSS/E `.raw` has cryptic bus names and no economic data.  Two
spreadsheets shipped with the Guatemalan AMM package fill part of the
gap; both are optional:

- **`--nomenclatura Nomenclatura.xls`** — a `code → description` table.
  The converter expands each bus-name prefix (`AGU-230` →
  `Aguacapa-230`), keeping the voltage/unit suffix and disambiguating
  collisions by bus number.  Names are ASCII-folded so they stay valid
  LP labels / JSON references.

- **`--ldm "LDM <month> <year>.xlsx"`** — the *Lista de Mérito*
  (merit-order dispatch list).  The unit *order* is the only economic
  signal in the package, so a matched generator gets
  `--gcost + rank·--gcost-step`, making the DC OPF dispatch follow the
  real merit order; unmatched generators are placed after the whole
  list.

> **Match transparency**: the LDM lists ~120 plant codes while the RAW
> transmission model has fewer generator buses (small plants aggregate),
> so the match is partial — the converter **logs how many generators
> matched** (e.g. `matched 56 of 115`).  Unmatched units keep the
> fallback cost; inspect the log before trusting the merit ranking.

## Limitations

- **No costs / merit order** — PSS/E RAW has no economic data, so the
  uniform `gcost` makes the OPF a feasibility / curtailment study, not an
  economic dispatch.  Edit the generated `generator_array` to add real
  costs if needed.
- **DC only** — reactive power, voltage magnitudes, tap ratios and phase
  shifters are ignored (standard DC-OPF assumptions).
- **Single snapshot** — one operating point per `.raw`.  Convert each
  snapshot (MIN / MED / MAX, season, …) separately.
- **rev 34+ / RAWX / v35** is rejected (see the format-scope note above).

---

## Cross-references

- [scripts-guide.md](../scripts-guide.md#psse2gtopt) — short reference.
- [plp2gtopt.md](plp2gtopt.md) / [sddp2gtopt.md](sddp2gtopt.md) — sister
  converters.
- [`grg-pssedata`](https://github.com/lanl-ansi/grg-pssedata) — the
  reference PSS/E v33 field definitions used to validate the parser.
- [MATPOWER `psse_convert_xfmr`](https://matpower.org) — the
  CZ-conversion and 3-winding star-equivalent maths.
