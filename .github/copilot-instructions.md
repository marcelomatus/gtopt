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
