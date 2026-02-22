# GitHub Copilot Instructions for gtopt

## What is gtopt?

**gtopt** is a high-performance C++ library and solver for **Generation and
Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost
of operation and expansion of electrical power systems (CAPEX + OPEX). It
supports single-bus and multi-bus DC power flow (Kirchhoff's laws), Parquet/CSV/JSON
I/O, and a sparse-matrix LP/MIP formulation via COIN-OR solvers.

The repository also contains:
- `standalone/` – thin `main()` wrapper that builds the `gtopt` binary
- `webservice/` – Next.js REST API and browser UI for submitting/monitoring jobs
- `guiservice/` – Python/Flask GUI for creating and editing cases
- `scripts/` – Python utilities (`plp2gtopt`, `igtopt`, `cvs2parquet`)
- `cases/` – sample optimization cases (`c0`, `c1`, ieee_*)
- `integration_test/` – CMake helpers for end-to-end (e2e) test registration

---

## Environment Setup (Ubuntu / GitHub Actions)

### 1. Install system dependencies

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  coinor-libcbc-dev libboost-container-dev libspdlog-dev \
  lcov ca-certificates lsb-release wget

# Apache Arrow + Parquet (official APT repo)
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get install -y -V \
  "./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

### 2. Install Clang 21 (preferred compiler)

The CI uses a pinned Clang 21 via `.github/actions/install-clang/action.yml`:

```bash
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 21 all
# Register unversioned alternatives (clang, clang++, clang-format, clang-tidy…)
for versioned in /usr/bin/clang*-21 /usr/bin/llvm*-21; do
  [ -e "$versioned" ] || continue
  base=$(basename "$versioned" "-21")
  sudo update-alternatives --remove-all "$base" 2>/dev/null || true
  sudo update-alternatives --install /usr/bin/"$base" "$base" "$versioned" 100
done
```

GCC 14 is also supported as an alternative compiler (`CC=gcc-14 CXX=g++-14`).

### 3. Install Arrow via Conda (alternative)

When the APT route is unavailable (e.g., non-Ubuntu distros or local dev):

```bash
conda install -c conda-forge arrow-cpp parquet-cpp
# Then point CMake to the conda prefix:
cmake -S test -B build \
  -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
  -DCMAKE_BUILD_TYPE=Debug
```

---

## Build Commands

### Unit tests (primary development target)

```bash
# Configure – build from the test/ sub-project, NOT the root CMakeLists.txt
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Build
cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run a single test by name (doctest filter syntax)
./build/gtoptTests "[test name]"
```

### Unit tests + integration tests (e2e)

```bash
cmake -S test -B build \
  -DENABLE_INTEGRATION_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Standalone binary

```bash
cmake -S standalone -B build-standalone -DCMAKE_BUILD_TYPE=Release
cmake --build build-standalone -j$(nproc)
./build-standalone/gtopt --version
```

### Coverage

```bash
cmake -S test -B build -DENABLE_TEST_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest
lcov --capture --directory . --output-file coverage.info \
  --gcov-tool /tmp/llvm-gcov.sh \
  --ignore-errors mismatch,mismatch
```

---

## Formatting and Linting

```bash
# Apply clang-format to all tracked C++ files (same command as CI)
git ls-files -z \
  '*.h' '*.c' '*.hpp' '*.cpp' '*.hxx' '*.cxx' \
  '*.hh' '*.cc' '*.ipp' '*.inc' '*.inl' \
  ':(exclude)cmake/' \
  | xargs -0 clang-format -i

# Or via CMake (requires a configured build directory)
cmake --build build --target format       # apply
cmake --build build --target check-format # check only

# clang-tidy (slow; triggered manually in CI via workflow_dispatch)
cmake -S test -B build \
  -DCMAKE_CXX_CLANG_TIDY="clang-tidy;--warnings-as-errors=*" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

The CI **auto-formats** every push on non-main branches and commits a fixup.
Format violations are warnings only, not CI failures.

---

## Code Style – C++

| Aspect | Rule |
|--------|------|
| Standard | C++26 (`set(CMAKE_CXX_STANDARD 26)`). C++23 features are actively used now; C++26 features as support arrives. |
| Compiler | Clang 21 (primary), GCC 14 (secondary) |
| Indentation | 2 spaces (see `.clang-format`, `IndentWidth: 2`) |
| Column limit | 80 characters |
| Namespace | All library code lives in `namespace gtopt` |
| Naming | Classes/Structs: `PascalCase`; methods/functions: `camelCase`; data members: `snake_case` |
| Header guards | `#pragma once` (not `#ifndef` guards) |
| File headers | Doxygen-style `@file`, `@brief`, `@date`, `@author`, `@copyright` |
| Includes | Grouped and sorted: (1) `<std>` headers, (2) external `<pkg/header.hpp>`, (3) project `<gtopt/...>`. See `.clang-format` `IncludeCategories`. |
| Pointers | Left-aligned: `T*` not `T *` |
| Braces | `BreakBeforeBraces: Custom` – opening brace on new line for functions, classes, namespaces; same line for control flow unless multi-line body |
| Initializer lists | Designated initializers (`SparseCol{.name="x", .cost=1}`) preferred |
| Concepts | Use `requires` constraints for template type safety |
| Error handling | Return values and `std::optional` over exceptions |
| `noexcept` | Add `noexcept` (or conditional `noexcept`) on non-throwing functions |
| `[[nodiscard]]` | Add to functions whose return value must not be silently discarded |
| `[[maybe_unused]]` | Use for intentionally unused variables/parameters |
| Magic numbers | Avoid; define named constants or use designated initializers |
| Casts | C++ casts (`static_cast`, `reinterpret_cast`) only – no C-style casts |
| Auto | Use `auto` for complex types, iterators, lambda returns; be explicit for simple types |

### clang-tidy suppressions in test code

Many clang-tidy checks that fire on test code need inline suppressions:

```cpp
// After std::move – use-after-move is intentional in tests
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)

// Dereferencing an optional after REQUIRE – safe, but tidy complains
CHECK(*opt == "two");  // NOLINT(bugprone-unchecked-optional-access)
```

---

## Writing New Tests

Tests live in `test/source/test_<topic>.cpp`. The framework is
[doctest](https://github.com/doctest/doctest) 2.4.12.

### Minimal test file template

```cpp
// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/<header>.hpp>

using namespace gtopt;

TEST_CASE("<ComponentName> basic behavior")  // NOLINT
{
  SUBCASE("default construction")
  {
    // ...
    CHECK(...);
    REQUIRE(...);
  }

  SUBCASE("edge case – empty input")
  {
    // ...
  }
}
```

### Rules for test files

1. **Include order**: `<doctest/doctest.h>` first, then project headers.
2. **`using namespace gtopt;`** at file scope is fine in test files.
3. One `TEST_CASE` per logical concept; use `SUBCASE` for variants.
4. Floating-point comparisons: use `doctest::Approx(value)` – never `==` on doubles.
5. `REQUIRE` to stop the test case on first failure; `CHECK` for non-fatal checks.
6. Prefer `CHECK_FALSE(x)` over `CHECK(x == false)`.
7. Prefer `REQUIRE(opt.has_value())` before dereferencing an `std::optional`.
8. Add `// NOLINT(...)` for intentional clang-tidy suppressions (see above).
9. Use `[[maybe_unused]]` for loop variables used only for side-effects.
10. Name the test file `test_<snake_case_topic>.cpp` and add it to the glob in
    `test/CMakeLists.txt` automatically via `CONFIGURE_DEPENDS`.
11. Do NOT add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` – that is already in
    `test/source/main.cpp`.
12. Use C++23/26 features freely: `std::format`, `std::ranges`, structured
    bindings, designated initializers, `std::expected`, `std::mdspan`, etc.

### Example – testing a utility function

```cpp
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/utils.hpp>

using namespace gtopt;

TEST_CASE("merge utility")  // NOLINT
{
  SUBCASE("basic merge")
  {
    std::vector<int> a {1, 2};
    std::vector<int> b {3, 4};
    merge(a, std::move(b));
    CHECK(a == std::vector {1, 2, 3, 4});
    CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
  }

  SUBCASE("self-merge is a no-op")
  {
    std::vector<int> a {1, 2};
    merge(a, a);
    CHECK(a == std::vector {1, 2});
  }
}
```

---

## Code Style – Python

The project uses Python for `guiservice/` (Flask), `scripts/`, and tests.

| Aspect | Rule |
|--------|------|
| Version | Python ≥ 3.12 (CI uses 3.12) |
| Formatter | `black` (line-length 88) |
| Import sorter | `isort` (profile `black`) |
| Linter | `ruff` + `pylint` |
| Type checker | `mypy` |
| Test framework | `pytest` |
| Dependencies | `guiservice/requirements.txt`: flask, pandas, pyarrow, requests |

```bash
# Install Python dev dependencies
pip install -e ".[dev]"      # from pyproject.toml

# Format
black .
isort .

# Lint
ruff check .
pylint scripts/ guiservice/

# Type check
mypy scripts/ guiservice/

# Run Python tests
pytest
```

---

## Project Architecture

```
gtopt/
├── include/gtopt/        # Public C++ headers (library API)
├── source/               # Library implementation (.cpp)
├── test/source/          # Unit tests (doctest)
│   └── json/             # JSON-specific tests
├── standalone/source/    # main() for the gtopt binary
├── integration_test/     # CMake helpers for e2e tests
├── cases/                # Sample optimization cases (JSON + Parquet)
├── webservice/           # Next.js web UI + REST API (Node 20)
├── guiservice/           # Flask GUI service (Python)
├── scripts/              # Python conversion utilities
├── cmake/                # Upstream CMake modules (CPM, tools)
├── cmake_local/          # Project-specific CMake modules
└── .github/
    ├── workflows/        # CI: ubuntu.yml, style.yml, autoformat.yml, …
    └── actions/          # Composite actions: install-clang
```

### Key headers to understand first

| Header | Purpose |
|--------|---------|
| `basic_types.hpp` | `Uid`, `Name`, `Real`, `OptReal`, strong types |
| `utils.hpp` | `merge`, `enumerate`, `enumerate_active`, `to_vector`, optional helpers |
| `linear_problem.hpp` | `LinearProblem`, `SparseCol`, `SparseRow`, `FlatLinearProblem` |
| `system.hpp` | Top-level power system model |
| `planning.hpp` | Multi-stage planning problem |
| `simulation_lp.hpp` | LP formulation of a single simulation |

---

## CI Workflows Summary

| Workflow | Trigger | What it does |
|----------|---------|--------------|
| `ubuntu.yml` | push/PR to main | Build (Clang 21), unit + e2e tests, optional coverage |
| `ubuntu.yml` (clang-tidy job) | `workflow_dispatch` with `run_clang_tidy=true` | Full clang-tidy static analysis |
| `style.yml` | every push/PR | clang-format check (non-blocking, warning only) |
| `autoformat.yml` | push to non-main branches | Auto-applies clang-format, commits fixup |
| `webservice.yml` | push/PR to main | Next.js build + integration + e2e tests |
| `guiservice.yml` | push/PR to main | Python GUI service tests |

---

## Useful Tips

- **CPM cache**: CMake dependencies are cached in `~/.cache/cpm` locally and
  `$GITHUB_WORKSPACE/cpm_modules` in CI. The cache is keyed on `CMakeLists.txt`
  hashes, so avoid unnecessary CMake file changes.
- **ccache**: Enabled by default (`USE_CCACHE=YES`). Pass
  `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.
- **In-source builds**: Forbidden. Always use a separate build directory.
- **Solver**: Default is COIN-OR CBC (`coinor-libcbc-dev`). CPLEX, HiGHS, and
  Clp are also supported; set `COIN_SOLVER=CPX|HiGHS|CLP` in CMake.
- **Arrow install path**: Set `CMAKE_PREFIX_PATH` if Arrow is installed outside
  the standard system paths (e.g., via conda or a custom prefix).
- **Test binary name**: `gtoptTests` (not `gtopt`). Run with doctest filter:
  `./build/gtoptTests -tc="test name pattern"`.
- **No `spdlog` format strings in hot paths**: `spdlog` is configured with
  `SPDLOG_USE_STD_FORMAT=1`, so it uses `std::format` syntax.

---

## Domain Knowledge – Power System Optimization

### Problem Context: GTEP

**Generation and Transmission Expansion Planning (GTEP)** finds the minimum-cost
combination of:

- **Operations decisions** – how much each generator produces each hour (OPEX),
  which loads to curtail if capacity is insufficient, and how much power flows
  on each transmission line.
- **Investment decisions** – how many capacity modules to build for generators,
  demands (served by storage/flexible loads), lines, and batteries across
  planning stages (CAPEX).

The objective function minimizes total discounted cost over all scenarios,
stages and blocks:

```
min  Σ_s Σ_t Σ_b  prob_s · discount_t · duration_b ·
       [ Σ_g gcost_g · generation_{g,s,t,b}
       + Σ_d fail_cost · curtailment_{d,s,t,b}
       + Σ_l tcost_l · flow_{l,s,t,b} ]
   + Σ_g annual_capcost_g · invested_modules_g  (expansion terms)
```

Subject to:

| Constraint | Description |
|------------|-------------|
| **Bus balance** | Injection = Load ± line flows at every bus, every block |
| **Kirchhoff (DC OPF)** | `flow_l = (θ_a − θ_b) / reactance_l` for each line |
| **Line capacity** | `−tmax_ba ≤ flow_l ≤ tmax_ab` |
| **Generator output** | `pmin_g ≤ generation_g ≤ pmax_g` (or `capacity_g`) |
| **Demand** | `load_d ≤ lmax_d` (served load ≤ maximum demand) |
| **Reserve** | Up/down reserve requirements per zone per block |
| **State variables** | Battery SoC, reservoir volume: carry over between blocks |
| **Expansion** | `installed_cap = initial_cap + expmod · expunits` (integer or relaxed) |

### Time Structure

```
Scenario (probability)
  └─ Stage  (investment period, discount factor)
       └─ Block  (operating hour, duration in hours)
```

- **Block** – smallest time unit (typically 1 hour). `duration` scales energy
  quantities: `energy_MWh = power_MW × duration_h`.
- **Stage** – groups consecutive blocks into a planning year/period.
  Capacity investments decided at stage level remain in subsequent stages.
- **Scenario** – a possible future (hydrology, demand level, etc.).
  `probability_factor` weights the scenario in the expected-cost objective.
- Single-period operational studies use `count_block=N` with one stage and one
  scenario (all IEEE benchmark cases).

### Modeling Modes

| Option | Value | Meaning |
|--------|-------|---------|
| `use_kirchhoff` | `true` | Full DC power flow (OPF); voltage angle `θ` variables added |
| `use_kirchhoff` | `false` | Transport model: line flows limited only by `tmax_ab/ba` |
| `use_single_bus` | `true` | "Copper plate" – no transmission constraints at all |
| `use_single_bus` | `false` | Multi-bus network model |
| `demand_fail_cost` | e.g. 1000 | Value-of-lost-load penalty for unserved energy ($/MWh) |
| `annual_discount_rate` | e.g. 0.1 | Yearly discount rate for investment costs (10 %) |
| `scale_objective` | e.g. 1000 | Divides all objective coefficients; improves solver numerics |

### Capacity Expansion Fields

A component (generator, demand, line, battery, converter) can be expanded when:

```json
{
  "capacity":      <initial installed capacity>,
  "expcap":        <MW per expansion module>,
  "expmod":        <maximum number of modules to install>,
  "annual_capcost":<annualized investment cost per module, $/year>
}
```

`expmod = null` means no expansion is allowed. Setting `expmod` to a positive
integer enables capacity expansion. The solver decides how many modules to build
(LP relaxation by default; integer when `colint` is set).

### Component Summary

| Component | Role |
|-----------|------|
| `Bus` | Electrical node; reference bus has `θ = 0` |
| `Generator` | Thermal/renewable/hydro unit; `gcost` in $/MWh |
| `GeneratorProfile` | Time-varying capacity factor multiplier (0–1); used for solar/wind |
| `Demand` | Fixed or flexible load; `lmax` can be inline array or Parquet file |
| `DemandProfile` | Time-varying demand scaling |
| `Line` | Transmission branch; `reactance` required for Kirchhoff mode |
| `Battery` | Energy storage: charge/discharge efficiencies, SoC bounds |
| `Converter` | Couples a `Battery` to a `Generator` (discharge) and `Demand` (charge) |
| `ReserveZone` | Up/down spinning-reserve requirement |
| `ReserveProvision` | Specifies which generator contributes to a reserve zone |
| `Junction` | Hydraulic node in a cascaded hydro system |
| `Waterway` | Water channel between junctions |
| `Reservoir` | Storage lake; volume balance across blocks |
| `Turbine` | Hydro turbine: links a waterway to a generator |
| `Flow` | External inflow or evaporation at a junction |
| `Filtration` | Water seepage from a waterway into a reservoir |

---

## IEEE Benchmark Cases

The `cases/` directory contains several standard IEEE test networks used to
validate the solver and provide regression baselines.

### Case `cases/c0/` – Single-bus expansion (simplest)

- **Topology**: 1 bus, 1 generator (g1, 20 MW, $100/MWh), 1 expandable demand.
- **Simulation**: 5 stages × 1 block each (15-hour horizon with varying durations).
- **Expansion**: Demand has `expcap=20 MW`, `expmod=10`, `annual_capcost=8760 $/yr`.
  The solver decides how many 20 MW increments of demand to serve per stage.
- **Files**: `lmax.parquet` provides hourly demand profile; external Parquet I/O.
- **Purpose**: Verifies multi-stage capacity expansion, Parquet input parsing,
  and single-bus (copper-plate) dispatch.

### Case `cases/c1/` – Same expansion, slightly different cost structure

- Mirrors `c0` with different block durations.
- **Purpose**: Regression baseline for the 5-stage expansion case.

### Case `cases/ieee_4b_ori/` – 4-bus original (pure dispatch)

- **Topology**: 4 buses, 2 thermal generators (g1: 300 MW/$20, g2: 200 MW/$35),
  2 demand buses (150 MW @ b3, 100 MW @ b4), 5 lines.
- **Simulation**: 1 stage, 1 block, 1 scenario.
- **Expected result**: G1 (cheaper) dispatches 250 MW, G2 idles. `obj_value=5`
  (= 250 × 20 / `scale_objective=1000`). Status 0.
- **Network**: Illustrates how power routes through parallel paths (b1→b3 vs
  b1→b2→b3) under DC Kirchhoff constraints.
- **Purpose**: Simplest multi-bus OPF; used in `test_ieee4b_ori_planning.cpp`.

### Case `cases/ieee_9b_ori/` – IEEE 9-bus standard base case

- **Topology**: 9 buses, 3 thermal generators (g1: 250 MW/$20, g2: 300 MW/$35,
  g3: 270 MW/$30), 3 demand buses (125+100+90 = 315 MW total), 9 lines.
- **Simulation**: 1 stage, 1 block (single snapshot).
- **Expected dispatch**: Economic merit order dispatches g1 first (cheapest),
  then g3, then g2; line limits force a solution away from pure merit order.
- **Origin**: Anderson & Fouad "Power Systems Control and Stability" – widely
  used for OPF benchmarking.
- **Purpose**: Validates DC OPF solution; used in `test_ieee9b_ori_planning.cpp`.

### Case `cases/ieee_9b/` – IEEE 9-bus with solar profile (24-hour)

- **Topology**: Same 9-bus network, but g3 is a **solar plant** with zero cost
  (`gcost=0`) and a 24-hour generation profile (0 → peaks at noon → 0).
- **Simulation**: 1 stage, 24 hourly blocks, 1 scenario.
- **Hourly demand**: Three loads follow a daily curve with morning and evening
  peaks (matching typical residential profiles).
- **Solar profile**: `generator_profile_array` entry scales g3's `capacity=270`
  by the profile vector (0.0 to 1.0) per block.
- **Key behaviour**: During solar hours (blocks 7–20), g3 displaces the more
  expensive g2; at night, g2 carries more load. Line flows change direction
  during peak solar.
- **Purpose**: Tests generator profiles, 24-block simulation, and the interplay
  of renewable dispatch with network constraints.
- Used in `test_ieee14_planning.cpp`-style tests and e2e integration.

### Case `cases/ieee_14b_ori/` – IEEE 14-bus standard base case

- **Topology**: 14 buses, 5 generators (g1@b1: 260 MW/$20, g2@b2: 130 MW/$35,
  g3@b3: 130 MW/free, g6@b6: 100 MW/$40, g8@b8: 80 MW/$45),
  11 demand buses, 20 transmission lines.
- **Simulation**: 1 stage, 24 blocks, 1 scenario.
- **Origin**: Based on the IEEE 14-bus Power Flow Test Case; standard academic
  OPF benchmark.
- **Purpose**: Larger network stress-test; validates LP assembly and convergence.

### Case `cases/ieee_14b/` – IEEE 14-bus with constrained lines

- Same as `ieee_14b_ori` but with tighter limits on l1_2 and l1_5 to force
  both lines to saturate simultaneously at the evening peak.
- **Purpose**: Tests active line-limit constraints (binding Kirchhoff dual
  variables); validates that shadow prices / Lagrange multipliers (`theta_dual`,
  `balance_dual`) are computed correctly.

### Running Benchmark Cases

```bash
cd cases/ieee_9b_ori
gtopt ieee_9b_ori.json
# Verify:  output/solution.csv → status=0
cat output/Generator/generation_sol.csv   # columns: scenario,stage,block,uid:1,uid:2,uid:3

cd cases/ieee_9b
gtopt ieee_9b.json
# 24-row generation_sol.csv; g3 (solar) non-zero only during blocks 7-20
```

---

## Output File Interpretation

After a successful run, the `output/` directory contains:

| File path | Contents |
|-----------|----------|
| `output/solution.csv` | `obj_value` (total cost / `scale_objective`), `kappa` (iterations), `status` (0=optimal) |
| `output/Generator/generation_sol.csv` | Generator dispatch in MW per (scenario, stage, block) |
| `output/Generator/generation_cost.csv` | Cost contribution per generator per (s, t, b) |
| `output/Demand/load_sol.csv` | Served load in MW |
| `output/Demand/fail_sol.csv` | Unserved load in MW (should be 0 for feasible cases) |
| `output/Demand/fail_cost.csv` | Curtailment cost (fail_sol × demand_fail_cost) |
| `output/Bus/balance_dual.csv` | Dual variable of the bus balance constraint = LMP ($/MWh) |
| `output/Bus/theta_sol.csv` | Voltage angle θ in radians (Kirchhoff mode only) |
| `output/Line/flowp_sol.csv` | Active power flow per line per (s, t, b) in MW |
| `output/Line/theta_dual.csv` | Dual of the Kirchhoff angle difference constraint |
| `output/Demand/capacity_dual.csv` | Shadow price of the demand capacity constraint |
| `output/Demand/capacost_sol.csv` | Installed expansion capacity per stage (expansion cases) |

**Interpretation tips**:
- `status=0` means optimal solution found.
- `balance_dual` gives the **Locational Marginal Price (LMP)** at each bus.
  In a lossless single-bus model all LMPs are equal to the marginal generator
  cost. When a line is congested, LMPs diverge between the two sides.
- `fail_sol > 0` indicates load shedding – raise `demand_fail_cost` or check
  that generation capacity plus expansion covers the demand.
- The objective value in `solution.csv` is divided by `scale_objective` (set in
  `options`). Multiply back to get the actual cost in the unit of `gcost × MWh`.

---

## LP Formulation – Key Variables and Constraints

This is what the C++ code (`simulation_lp.cpp`, `bus_lp.cpp`, `generator_lp.cpp`,
`demand_lp.cpp`, `line_lp.cpp`, …) assembles into the solver's `LinearProblem`:

```
Variables (SparseCol):
  generation_{g,s,t,b}   ∈ [pmin, pmax]    generator output
  load_{d,s,t,b}         ∈ [0, lmax]       served demand
  fail_{d,s,t,b}         ≥ 0               unserved demand
  flowp_{l,s,t,b}        ∈ [-tmax, tmax]   line power flow
  theta_{b,s,t,b}        free              voltage angle (Kirchhoff only)
  capainst_{g,t}         ∈ [0, expmod]     expansion units (capacity variables)

Constraints (SparseRow):
  balance_{bus,s,t,b}:   Σ_g gen - Σ_d (load+fail) - Σ_l flow = 0
  kirchhoff_{l,s,t,b}:   flow - (theta_a - theta_b)/reactance = 0
  capacity_{g,t}:        generation ≤ capacity + capainst·expcap
```

The `PlanningLP` class (`planning_lp.hpp`) orchestrates all component `*LP`
objects; calling `planning_lp.resolve()` assembles and solves the full LP.

---

## Best Practices for New Cases

1. **Start from `ieee_9b_ori`** when adding a new single-snapshot OPF test.
   It has the simplest structure and well-known analytical solutions.

2. **Use `scale_objective: 1000`** for MW-scale problems to keep objective
   coefficients in the range [0.001, 1000], which improves solver conditioning.

3. **Set `demand_fail_cost` = 10× the most expensive generator cost** to avoid
   load shedding while keeping the LP bounded.

4. **Validate with `use_single_bus: true` first** to check that total generation
   capacity covers peak demand before introducing network constraints.

5. **Check `solution.csv` `status=0`** before inspecting any other output.
   Status 1 = infeasible, 2 = unbounded, 5 = LP not solved.

6. **Generator profiles**: inline the 24-element array directly in the JSON for
   tests; use an external Parquet file (`input_directory`) for production cases
   with many time points.

7. **Multi-stage expansion**: set `annual_discount_rate` and `annual_capcost`
   in consistent units. Annual cost × number of years ≈ total capital recovery.

8. **Reserve modeling**: add a `reserve_zone_array` with `urreq`/`drreq` and
   link generators via `reserve_provision_array`. Each provision specifies
   `urmax` (max up-reserve MW) and cost `urcost`.
