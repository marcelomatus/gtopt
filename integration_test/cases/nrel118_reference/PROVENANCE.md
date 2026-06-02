# NREL-118 Reference Data — Provenance

External-golden reference for `test/source/test_emission_nrel118_port.cpp`,
which ports the Peña / Martinez-Anido / Hodge 2018 *Extended IEEE 118-Bus
Test System With High Renewable Penetration* into gtopt's emission framework.

This directory captures the numerical regression targets we could recover
from public sources for the CO₂ / NOₓ / SO₂ reduction headline. It also
records every source we tried and what each did or didn't yield, so the
next agent doesn't have to redo the hunt.

## TL;DR

* The paper's **abstract-level headline number** (29-34 % CO₂ reduction at
  33 % wind+solar penetration vs no-renewables baseline) is corroborated
  by a snippet returned from an indexed copy of the paper. Captured in
  `headline_results.json`.
* The paper's NOₓ / SO₂ ranges (16-22 % / 14-24 %) come from the
  abstract as quoted by the converter's own README — we did **not**
  independently re-extract them from the source PDF, since the PDFs we
  located are paywalled (IEEE Xplore) or unreachable from this sandbox
  (`docs.nrel.gov`). Documented as a known caveat below.
* The published **input dataset** (NREL-Sienna/PowerSystemsTestData,
  `118-Bus/`) ships **no** Solution.csv / Results.csv / baseline.csv —
  it is inputs-only (CSV generator / bus / line + time-series Parquet).
* The published dataset **does** ship per-fuel prices (in the
  `data_118bus.jl` Sienna loader) that the gtopt converter currently
  ignores (`Fuel.price = 0`); they are captured here in
  `pena_2018_table_fuel_prices.json` as a future-work hook.
* Regression-test window in `test_emission_nrel118_port.cpp` tightened
  from **[15 %, 50 %]** to **[20 %, 45 %]** — see "Test-assertion update"
  below.

## Sources checked

| # | Source                                                                 | Reachable | Useful content                                       |
|---|------------------------------------------------------------------------|-----------|------------------------------------------------------|
| 1 | IEEE Xplore — `ieeexplore.ieee.org/document/7904729`                   | HTTP 418  | None — paywall + bot block                            |
| 2 | OSTI bibliographic page — `www.osti.gov/biblio/1416258`                | ✅        | Abstract (dataset description, no result figures)    |
| 3 | NREL Research Hub — `research-hub.nrel.gov/en/publications/.../-4/`    | ✅        | Same bibliographic abstract, no supplementary files  |
| 4 | NREL preprint PDF — `docs.nrel.gov/docs/fy17osti/66675.pdf` (guessed)  | DNS/refused | Site unreachable from sandbox                       |
| 5 | ResearchGate — `researchgate.net/publication/316259083`                 | HTTP 403  | Blocked                                              |
| 6 | SciSpace — `scispace.com/papers/.../3lrjnme14i`                        | HTTP 403  | Blocked (cite count 229 visible in Google snippet)   |
| 7 | SemanticScholar — `semanticscholar.org/paper/.../5f07f1...`            | ✅ empty  | Page loaded but content was empty                    |
| 8 | Hodge publications list — `colorado.edu/faculty/hodge/research/...`    | ✅        | Citation only, no PDF, no result snippet              |
| 9 | NREL-Sienna/PowerSystemsTestData/118-Bus/                              | ✅        | Generator inputs + per-fuel prices; **no solutions** |
|10 | Google snippet from a SciSpace listing                                  | ✅        | The 29-34 % CO₂ number itself (cited below)          |
|11 | Carnegie Mellon Pena page — `cmu.academia.edu/IvonnePeña`              | HTTP 403  | Blocked                                              |
|12 | NREL Eastern wind-integration study PR — `www.nrel.gov/news/.../37730` | ✅        | Different study (Eastern, not 118-bus); ~25-45 % CO₂ |

> **Net**: 12 sources tried; 6 reachable; only 2 (sources 9 and 10) added
> any numerical content over what is already documented in the converter
> README at `scripts/nrel118_to_gtopt/README.md`. **No** raw per-scenario
> solution file was located in any public NREL repo or supplementary archive.

### Source-10 evidence snippet (the only direct quote of the result number)

A Google-indexed snippet from a SciSpace listing of the paper returned
verbatim:

> *"up to 33% wind and solar energy penetration in the United States'
> portion of the Western grid avoids 29%–34% carbon dioxide (CO2) emissions"*

This is the same number the converter README already cites, and is the
strongest single piece of evidence we have without paywalled access to
the paper PDF.

## What lives in this directory

* `PROVENANCE.md` — this file.
* `headline_results.json` — the regression-target numbers, source-tagged,
  with units. **The C++ test asserts against this.**
* `pena_2018_table_fuel_prices.json` — per-fuel-family input prices in
  $/MMBTU as defined in the NREL-Sienna Julia loader script
  `118-Bus/data_118bus.jl`. NOT a paper-table, but it IS a piece of
  official input data the converter currently does not consume.
* `pena_2018_table_generator_inventory.json` — generator counts by
  technology, extracted from `118-Bus/Generators.csv` (the 339-row
  master inventory).

Files that are **NOT** here, and why:

* `published_solution_<scenario>.csv` — **does not exist publicly.**
  The NREL-Sienna repo ships only inputs. The 2018 paper does not link to
  a downloadable per-scenario solution. The closest public reproduction
  would be to drive Sienna/PowerSimulations.jl on the dataset and dump
  the dispatch — that is exactly what the sister agent is wiring up for
  the 5-bus cases.

## Test-assertion update

`test/source/test_emission_nrel118_port.cpp` previously asserted the
CO₂ reduction at 33 % renewable share fell in `[15 %, 50 %]`. With the
paper's now-cited 29-34 % annual figure, we tighten to `[20 %, 45 %]`:

* **Lower bound 20 %** — paper's lower band is 29 %; we keep 9 pp of
  slack to absorb single-block-vs-8784-h variance.
* **Upper bound 45 %** — paper's upper band is 34 %; 11 pp of slack on
  the upside (a one-block LP can over-displace coal more cleanly than
  the full annual UC).

The window is still wider than the published 29-34 % because the C++
test is a single-block deterministic LP fixture (1100 MW demand at one
hour, no UC start-up / min-up / ramp dynamics, no hydro dispatch),
whereas the paper's number is an annual production-cost simulation over
8784 hours with full UC.

If the gtopt-side converter ever runs the *full* annual horizon under
UC (issue #510 multi-fuel HR + start-up modelling), this window should
shrink to `[27 %, 36 %]` to be a true cross-tool regression. As of
2026-06, the C++ fixture is too coarse a slice to support that tighter
window without flaking.

## Still missing / future work

1. **Raw paper PDF** — every paywall hit blocked us. Get an
   institutional download of `10.1109/TPWRS.2017.2695963` and quote
   verbatim from Tables I-IV; we can then split `headline_results.json`
   into per-penetration-scenario rows (5, 10, 15, 20, 25, 30, 33 %
   wind+solar) rather than the single 33 % point.
2. **Per-fuel emission rates in the input dataset** — the dataset's own
   Generators.csv has **no** CO₂/NOₓ/SO₂ columns despite the abstract
   advertising "GHG emissions rates". The converter today derives them
   from IPCC AR6 (combustion factor × heat rate); a faithful port should
   replace the IPCC default with whatever per-fuel rate the original
   PCM model used.
3. **Per-quarter / per-month reductions** — paper likely has these in
   Fig. 4-6; not recovered.
4. **NOₓ / SO₂ reproduction** — converter currently emits only CO₂;
   the abstract's 16-22 % NOₓ / 14-24 % SO₂ numbers are recorded in
   `headline_results.json` for future regression once the gtopt fuel
   model gains per-pollutant rates beyond CO₂.

## Citation chain

```
[1] I. Peña, C. B. Martinez-Anido, B.-M. Hodge,
    "An Extended IEEE 118-Bus Test System With High Renewable Penetration,"
    IEEE Trans. Power Syst., vol. 33, no. 1, pp. 281–289, Jan. 2018.
    DOI: 10.1109/TPWRS.2017.2695963
    NREL/JA-5D00-66675 — OSTI ID 1416258.

[2] NREL-Sienna/PowerSystemsTestData, github.com/NREL-Sienna/PowerSystemsTestData
    Subdirectory: 118-Bus/   (Generators.csv, Lines.csv, Buses.csv,
                              data_118bus.jl, Hydro/, Wind/, Solar/, Load/)

[3] IPCC AR6 WG3 Annex III, Table A.III.2 — combustion CO₂ factors used
    by `scripts/nrel118_to_gtopt/_converter.py` (coal 0.0946 tCO₂/GJ,
    natural gas 0.0561 tCO₂/GJ, oil 0.0741 tCO₂/GJ, biomass 0.0
    biogenic-zero).
```
