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
  coinor-libcbc-dev libboost-container-dev libspdlog-dev \
  lcov ca-certificates lsb-release wget

# Apache Arrow (required for Parquet I/O)
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get install -y -V \
  "./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

### Install Arrow via Conda (alternative)

```bash
conda install -c conda-forge arrow-cpp parquet-cpp
cmake -S test -B build -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" -DCMAKE_BUILD_TYPE=Debug
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
  - Methods/functions: `camelCase`
  - Data members: `snake_case`
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

Use inline `// NOLINT(...)` for known false-positive cases:

```cpp
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
CHECK(*opt == "two");  // NOLINT(bugprone-unchecked-optional-access)
```

### Python

- **Version**: Python ≥ 3.12
- **Formatter**: `black` (line-length 88) + `isort` (profile `black`)
- **Linter**: `ruff` + `pylint`
- **Type checker**: `mypy`
- **Tests**: `pytest`

```bash
pip install -e ".[dev]"   # install dev dependencies from pyproject.toml
black . && isort .
ruff check . && pylint scripts/ guiservice/
mypy scripts/ guiservice/
pytest
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
6. `REQUIRE(opt.has_value())` before dereferencing `std::optional`.
7. Add `// NOLINT(...)` for intentional clang-tidy suppressions.
8. Do NOT add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (already in `test/source/main.cpp`).
9. Use `[[maybe_unused]]` for loop variables used only for side-effects.
10. Use C++23/26 features freely: `std::format`, `std::ranges`, designated initializers, etc.

## Domain Quick-Reference

> Full details are in `.github/copilot-instructions.md` → "Domain Knowledge" section.

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
