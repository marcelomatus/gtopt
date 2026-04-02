# Documentation Update Checklist

Use this checklist alongside `update_docs.prompt` to track periodic
documentation review progress.

## Pre-Flight

- [ ] Read the latest `git log --oneline -20` to identify recent changes
- [ ] Check if any C++ headers were modified (new fields, renamed structs)
- [ ] Check if any Python scripts were modified (new CLI flags, renamed modules)
- [ ] Check if any new cases were added to `cases/`

## C++ ↔ Documentation Sync

- [ ] `include/gtopt/generator.hpp` ↔ `docs/input-data.md` §3.2
- [ ] `include/gtopt/demand.hpp` ↔ `docs/input-data.md` §3.3
- [ ] `include/gtopt/line.hpp` ↔ `docs/input-data.md` §3.4
- [ ] `include/gtopt/bus.hpp` ↔ `docs/input-data.md` §3.1
- [ ] `include/gtopt/battery.hpp` ↔ `docs/input-data.md` §3.5
- [ ] `include/gtopt/converter.hpp` ↔ `docs/input-data.md` §3.6
- [ ] `include/gtopt/junction.hpp` ↔ `docs/input-data.md` §3.7
- [ ] `include/gtopt/waterway.hpp` ↔ `docs/input-data.md` §3.8
- [ ] `include/gtopt/reservoir.hpp` ↔ `docs/input-data.md` §3.9
- [ ] `include/gtopt/turbine.hpp` ↔ `docs/input-data.md` §3.10
- [ ] `include/gtopt/flow.hpp` ↔ `docs/input-data.md` §3.11
- [ ] `include/gtopt/filtration.hpp` ↔ `docs/input-data.md` §3.12
- [ ] `include/gtopt/reserve_zone.hpp` ↔ `docs/input-data.md` §3.16
- [ ] `include/gtopt/reserve_provision.hpp` ↔ `docs/input-data.md` §3.17
- [ ] `include/gtopt/generator_profile.hpp` ↔ `docs/input-data.md` §3.14
- [ ] `include/gtopt/demand_profile.hpp` ↔ `docs/input-data.md` §3.15
- [ ] `include/gtopt/planning_options.hpp` ↔ `docs/input-data.md` §1
- [ ] `include/gtopt/sddp_options.hpp` ↔ `docs/input-data.md` §1.4
- [ ] `include/gtopt/simulation.hpp` ↔ `docs/input-data.md` §2
- [ ] `include/gtopt/system.hpp` ↔ `docs/input-data.md` §3

## Python ↔ Documentation Sync

- [ ] `scripts/igtopt/template_builder.py` ↔ `docs/scripts/igtopt.md`
- [ ] `scripts/pyproject.toml` entry points ↔ `docs/scripts-guide.md`
- [ ] SDDP_OPTION_KEYS ↔ `include/gtopt/sddp_options.hpp`

## Cross-Reference Integrity

- [ ] All internal links in docs/*.md resolve correctly
- [ ] docs/formulation refs ↔ docs/white_paper/references.bib consistency
- [ ] Case descriptions match actual JSON file contents

## White Paper

- [ ] references.bib has entries for all refs in mathematical-formulation.md
- [ ] Benchmark table in validation.tex matches cases/*/output/solution.csv
- [ ] Author affiliations are current
