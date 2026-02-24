# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **See also**: [`.github/copilot-instructions.md`](.github/copilot-instructions.md) for a more
> comprehensive reference covering environment setup, architecture, CI workflows, test-writing
> guidelines, **GTEP domain knowledge**, and IEEE benchmark case descriptions.

## What is this repository?

**gtopt** is a C++ library and solver for **Generation and Transmission Expansion Planning (GTEP)**.
It builds a sparse LP/MIP formulation of a multi-stage power system and solves it via COIN-OR
solvers (CBC by default). The repo also contains a Next.js web service, a Python/Flask GUI service,
and Python utility scripts.

## Environment Setup

### Install system dependencies (Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ccache coinor-libcbc-dev libboost-container-dev libspdlog-dev \
  zlib1g-dev lcov ca-certificates lsb-release wget

# Apache Arrow (required for Parquet I/O)
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get install -y -V \
  "./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

### Install Arrow via Conda (verified alternative)

When the APT repo is unavailable (network-restricted environments, non-Ubuntu
distros), use conda. Verified on Ubuntu 24.04 with conda 26.1.0 → Arrow 12.0.0:

```bash
conda install -y -c conda-forge arrow-cpp parquet-cpp

# Use conda info --base, NOT $CONDA_PREFIX (only set inside activated env)
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"
```

### Install Clang 21 (preferred compiler, same as CI)

```bash
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 21 all
# Register unversioned aliases (clang, clang++, clang-format, clang-tidy…)
for versioned in /usr/bin/clang*-21 /usr/bin/llvm*-21; do
  [ -e "$versioned" ] || continue
  base=$(basename "$versioned" "-21")
  sudo update-alternatives --remove-all "$base" 2>/dev/null || true
  sudo update-alternatives --install /usr/bin/"$base" "$base" "$versioned" 100
done
```

GCC 14 is the alternative compiler (`CC=gcc-14 CXX=g++-14`).

### Common build failures and fixes

| Symptom | Cause | Fix |
|---------|-------|-----|
| `/bin/sh: ccache: not found` during `cmake --build` | `ccache` not installed before CMake configure | `sudo apt-get install -y ccache` **before** running `cmake -S test -B build` |
| `Could not find ArrowConfig.cmake` | Arrow/Parquet not installed | `conda install -y -c conda-forge arrow-cpp parquet-cpp` then add `-DCMAKE_PREFIX_PATH="$(conda info --base)"` |
| `Unable to fetch some archives` from apt | Stale package lists | Always run `sudo apt-get update` before `apt-get install` |
| `COIN solver: none configured` | COIN-OR not installed | `sudo apt-get install -y coinor-libcbc-dev` |
| `Could not find BoostConfig.cmake` | Boost not installed | `sudo apt-get install -y libboost-container-dev` |

> **Critical**: install `ccache` **before** `cmake -S test -B build`.
> CMake bakes the launcher path into the build system at configure time; if
> ccache is absent when you configure, every subsequent build invocation fails
> even after installing ccache later. Delete the build directory and reconfigure.

## Build Commands

> **Important**: The primary build target for development and testing is `cmake -S test -B build`,
> NOT the root `CMakeLists.txt`.  The root CMakeLists only builds the library; the test
> sub-project includes the library via CPM and also builds the test binary.

### Unit tests (development)

```bash
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Run a single test

```bash
./build/gtoptTests -tc="test name pattern"
# or using doctest bracket syntax:
./build/gtoptTests "[test name]"
```

### Unit + integration tests (e2e)

```bash
cmake -S test -B build -DENABLE_INTEGRATION_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Standalone binary

```bash
cmake -S standalone -B build-standalone -DCMAKE_BUILD_TYPE=Release
cmake --build build-standalone -j$(nproc)
./build-standalone/gtopt --version
```

### Test coverage

```bash
cmake -S test -B build -DENABLE_TEST_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

## Formatting and Linting

```bash
# Apply clang-format (same as CI autoformat job)
git ls-files -z '*.h' '*.c' '*.hpp' '*.cpp' '*.hxx' '*.cxx' \
  '*.hh' '*.cc' '*.ipp' '*.inc' '*.inl' ':(exclude)cmake/' \
  | xargs -0 clang-format -i

# Via CMake (requires a configured build dir)
cmake --build build --target format        # apply
cmake --build build --target check-format  # check only

# clang-tidy (slow – same as CI static analysis job)
cmake -S test -B build \
  -DCMAKE_CXX_CLANG_TIDY="clang-tidy;--warnings-as-errors=*" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

## Code Style Guidelines

### C++

- **Standard**: C++26 (`CMAKE_CXX_STANDARD 26`). C++23 features are used throughout;
  C++26 features added as compiler support matures (Clang 21 / GCC 14).
- **Compiler flags**: `-Wall -Wpedantic -Wextra -Werror` on all platforms.
- **Indentation**: 2 spaces (`.clang-format`, `IndentWidth: 2`)
- **Column limit**: 80 characters
- **Namespace**: All library code in `namespace gtopt`
- **Naming**:
  - Classes/Structs: `PascalCase`
  - Free functions and methods: `snake_case` (e.g. `add_col`, `get_optvalue`, `resolve`)
  - Data members and local variables: `snake_case`
  - Private class members: `m_` prefix + `_` suffix (e.g. `m_simulation_`, `m_options_`)
- **Header guards**: `#pragma once`
- **File headers**: Doxygen-style (`@file`, `@brief`, `@date`, `@author`, `@copyright`)
- **Includes**: Three groups — `<std>`, external `<pkg/header>`, project `<gtopt/...>`
- **Pointers**: Left-aligned: `T*` not `T *`
- **Initializers**: Designated initializers (`SparseCol{.name="x", .cost=1}`) preferred
- **Templates**: Use `requires` concepts for type constraints
- **Error handling**: `std::optional` / return values over exceptions
- **`noexcept`**: Add to non-throwing functions (with conditional `noexcept` where appropriate)
- **`[[nodiscard]]`**: Add to functions whose return must not be silently discarded
- **Documentation**: Doxygen-style class/function documentation

### clang-tidy suppressions in tests

Only `bugprone-use-after-move` still needs an inline `// NOLINT`:

```cpp
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
```

**Never use `NOLINT(bugprone-unchecked-optional-access)`.**
Use safe checked patterns for optional access instead:

```cpp
// value_or with a sentinel that differs from the expected value
CHECK(opt.value_or(-1.0) == doctest::Approx(2.5));
CHECK(opt.value_or("") == "hello");
CHECK(opt.value_or(false) == true);

// boolean short-circuit for expressions
CHECK((opt && *opt == 2.0));
CHECK((opt && (*opt)->member == expected));

// value_or for variant-inside-optional
CHECK(std::get<double>(opt.value_or(0.0)) == doctest::Approx(5000.0));
CHECK(std::get<IntBool>(active_opt.value_or(Active{False})) == True);

// intermediate variable to avoid dangling ref when binding const& to vector
const auto val = opt.value_or(Active{std::vector<IntBool>{}});
const auto& vec = std::get<std::vector<IntBool>>(val);
```

### Python

The **scripts sub-package** (`scripts/`) is self-contained with its own
`pyproject.toml`, `requirements.txt`, and `requirements-dev.txt`.
It is **independent** of the root `pyproject.toml`.

- **Version**: Python ≥ 3.10 (type-checking); CI uses 3.12
- **Formatter**: `black` (line-length 88)
- **Linter**: `pylint` (configured in `scripts/pyproject.toml`)
- **Type checker**: `mypy` (configured in `scripts/pyproject.toml`)
- **Tests**: `pytest` with `pytest-cov`
- **Coverage threshold**: 83% (`fail_under = 83` in `scripts/pyproject.toml`)

```bash
# Install (from repo root)
pip install -e "./scripts[dev]"    # editable + dev tools
pip install -r scripts/requirements.txt          # runtime only

# All commands below run from scripts/ directory
cd scripts

# Format
python -m black cvs2parquet igtopt plp2gtopt
python -m black --check cvs2parquet igtopt plp2gtopt   # CI check

# Lint
python -m pylint cvs2parquet igtopt plp2gtopt

# Type check
python -m mypy cvs2parquet igtopt plp2gtopt --ignore-missing-imports

# Run all tests (fast, < 2 s)
python -m pytest -q

# Run with coverage + missing-lines report
python -m pytest \
  --cov=cvs2parquet --cov=igtopt --cov=plp2gtopt \
  --cov-report=term-missing -q

# Run a single test
python -m pytest -k "test_parse_single_bess" -q

# Integration tests only
python -m pytest -m integration -q
```

Via CMake (from repo root after `cmake -S scripts -B build-scripts`):

```bash
cmake --build build-scripts --target scripts-install       # pip install -e scripts/[dev]
cmake --build build-scripts --target scripts-format        # black (in-place)
cmake --build build-scripts --target scripts-check-format  # black --check
cmake --build build-scripts --target scripts-lint          # pylint
cmake --build build-scripts --target scripts-mypy          # mypy
cmake --build build-scripts --target scripts-test          # unit tests
cmake --build build-scripts --target scripts-test-integration
cmake --build build-scripts --target scripts-coverage      # HTML report
```


## Writing New Tests

Tests live in `test/source/test_<topic>.cpp`. Add new files there — the `CONFIGURE_DEPENDS`
glob in `test/CMakeLists.txt` picks them up automatically.

**Minimal template:**

```cpp
// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/<header>.hpp>

using namespace gtopt;

TEST_CASE("<ComponentName> basic behavior")  // NOLINT
{
  SUBCASE("default construction")
  {
    CHECK(...);
    REQUIRE(...);
  }

  SUBCASE("edge case – empty input")
  {
    // ...
  }
}
```

**Key rules:**
1. `<doctest/doctest.h>` first, then project headers.
2. `using namespace gtopt;` at file scope is fine in test files.
3. Floating-point: use `doctest::Approx(value)`, never `==` on doubles.
4. `REQUIRE` for fatal assertions (stop test on failure); `CHECK` for non-fatal.
5. Prefer `CHECK_FALSE(x)` over `CHECK(x == false)`.
6. **Never use `NOLINT(bugprone-unchecked-optional-access)`**. Use
   `opt.value_or(sentinel)` or `(opt && *opt == val)` instead (see above).
7. `REQUIRE(opt.has_value())` before branches that depend on the optional,
   but still use `value_or` / `&&` in the CHECK expressions themselves.
8. Only accepted NOLINT: `// NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)`
   after intentional post-`std::move` checks.
9. Do NOT add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (already in `test/source/main.cpp`).
10. Use `[[maybe_unused]]` for loop variables used only for side-effects.
11. Use C++23/26 features freely: `std::format`, `std::ranges`, designated initializers, etc.
12. **Testing LP via JSON**: always use `Planning base; base.merge(from_json<Planning>(json_str))`.
    Direct `from_json` overwrites `scene_array` with `{}`, so `resolve()` never runs.
    See `.github/copilot-instructions.md` → "Useful Tips" for full explanation.
13. **Testing `gtopt_main()`**: use `MainOptions{.planning_files=..., .use_single_bus=true}`.
    Only set the fields you need — all others default to `std::nullopt`.

## Domain Quick-Reference

> Full details are in `.github/copilot-instructions.md` → "Domain Knowledge" section.
> Scripts sub-package details are in `.github/copilot-instructions.md` → "Python Scripts Sub-Package".

### What gtopt optimizes

**Objective**: minimize total discounted cost (OPEX + CAPEX) over all scenarios,
planning stages, and time blocks.

- **OPEX**: generator dispatch cost (`gcost × power × duration`), demand curtailment
  penalty (`demand_fail_cost`), line transfer cost.
- **CAPEX**: annualized investment cost for expansion modules (generators, demands,
  lines, batteries): `annual_capcost × modules_built`.

### Time structure

`Scenario` → `Stage` (investment period) → `Block` (operating hour, `duration` in h).

### Key options

| Option | Typical value | Effect |
|--------|--------------|--------|
| `use_kirchhoff` | `true` | DC OPF with voltage angles and line reactances |
| `use_single_bus` | `false` | Multi-bus network (set `true` to disable network) |
| `demand_fail_cost` | 1000 | $/MWh penalty for unserved load |
| `scale_objective` | 1000 | Divides objective coefficients for solver numerics |
| `annual_discount_rate` | 0.1 | 10 % per year for CAPEX discounting |

### IEEE benchmark cases in `cases/`

| Directory | Buses | Generators | Blocks | Key feature |
|-----------|-------|------------|--------|-------------|
| `ieee_4b_ori` | 4 | 2 thermal | 1 | Simplest OPF; g1 ($20) serves all load |
| `ieee_9b_ori` | 9 | 3 thermal | 1 | Classic Anderson–Fouad 9-bus OPF |
| `ieee_9b` | 9 | 2 thermal + 1 solar | 24 | Solar profile; 24-hour dispatch |
| `ieee_14b_ori` | 14 | 5 generators | 24 | Standard IEEE 14-bus OPF benchmark |
| `ieee_14b` | 14 | 5 generators | 24 | Constrained lines; binding KVL duals |
| `c0` | 1 | 1 thermal | 5 stages | Multi-stage capacity expansion (Parquet I/O) |

### Validating a solved case

```bash
# Status 0 = optimal
cat output/solution.csv

# Locational Marginal Prices (dual of bus balance constraint)
cat output/Bus/balance_dual.csv

# Check no load shedding
cat output/Demand/fail_sol.csv   # should be all zeros
```
