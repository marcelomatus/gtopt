# CLAUDE.md

> **See also**: [`.github/copilot-instructions.md`](.github/copilot-instructions.md) for
> architecture, CI workflows, **GTEP domain knowledge**, and IEEE benchmark cases.

## What is this repository?

**gtopt** is a C++ library and solver for **Generation and Transmission Expansion Planning (GTEP)**.
It builds a sparse LP/MIP formulation of a multi-stage power system and solves it via
pluggable LP solver backends (CLP, CBC, CPLEX, HiGHS) loaded at runtime as shared
libraries. The repo also contains a Next.js web service, a Python/Flask GUI service,
and Python utility scripts.

## Environment Setup

### Sandbox / agent sessions

```bash
bash tools/setup_sandbox.sh          # install deps only
bash tools/setup_sandbox.sh --build  # install deps + build + test (PREFERRED)
bash tools/setup_sandbox.sh --build --debug  # full symbols when actually debugging
```

The script is idempotent, tries Clang 21 first (falls back to GCC 14), uses
conda for Arrow/Parquet, and saves `tools/compile_commands.json` for clang-tidy.

> **Default build type: `CIFast`** — `-O0 -g1` with `-fno-standalone-debug`
> (Clang) and `--gc-sections`. No optimisation, minimal line-only debug info
> (backtraces on test failures still point at a source line). This is the
> right default for both CI and agent-driven iteration: the only signals
> we need are "did it compile" and "did the tests pass". Reconfigure with
> `-DCMAKE_BUILD_TYPE=Debug` (or `setup_sandbox.sh --debug`) when you
> actually need to step through code in gdb.

### Local development (no conda needed)

```bash
sudo apt-get update && sudo apt-get install -y --no-install-recommends \
  ccache coinor-libcbc-dev libarrow-dev libparquet-dev \
  libboost-container-dev libspdlog-dev liblapack-dev libblas-dev \
  zlib1g-dev libzstd-dev zstd liblz4-dev lcov

cmake -S all -B build -G Ninja -DCMAKE_BUILD_TYPE=CIFast \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Use `-DCMAKE_BUILD_TYPE=Debug` instead when you need full symbols for gdb.

> **Ninja recommended**: `-G Ninja` enables file-level dependency tracking,
> allowing test sources to compile in parallel with library sources. Install
> via `pip install ninja` or `apt install ninja-build`.

> **Critical**: install `ccache` **before** `cmake configure` — CMake bakes the
> launcher path at configure time. If ccache was missing, delete build dir and
> reconfigure.

> **`CMAKE_PREFIX_PATH`**: only needed with conda Arrow (sandbox). With system
> Arrow (APT), cmake finds it automatically. Never mix APT and conda Arrow.

### Common build failures

| Symptom | Fix |
|---------|-----|
| `ccache: not found` | Install ccache, delete build dir, reconfigure |
| `Could not find ArrowConfig.cmake` | Install Arrow (APT or conda) |
| `No solver plugins found` | Install COIN-OR (`coinor-libcbc-dev`) and/or HiGHS; set `GTOPT_PLUGIN_DIR` if plugins are in a non-standard location |
| Clang not found | Follow LLVM APT steps in `setup_sandbox.sh` or `.github/actions/install-clang/action.yml` |

## Build Commands

The primary build target is `cmake -S all -B build` — builds library, standalone
binary (`build/standalone/gtopt`), and tests (run via `ctest`).

```bash
# Run a single test
./build/test/gtoptTests -tc="test name pattern"

# Unit + integration tests
cmake -S all -B build -G Ninja -DGTOPT_BUILD_INTEGRATION_TESTS=ON -DCMAKE_BUILD_TYPE=CIFast \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j$(nproc) && cd build && ctest --output-on-failure
```

## Obtaining the gtopt Binary

Use `tools/get_gtopt_binary.py` — checks `GTOPT_BIN` env, `PATH`, build dirs,
cached CI artifact, then downloads from CI (requires `GITHUB_TOKEN` or `gh`).

```bash
export GTOPT_BIN=$(python tools/get_gtopt_binary.py)
python tools/get_gtopt_binary.py --build  # fallback: build from source
```

## Pre-Commit Checks (REQUIRED)

### C++ files

```bash
# Step 1 — clang-format
git diff --name-only --diff-filter=d HEAD \
  | grep -E '\.(cpp|hpp|h|cc|cxx|hxx)$' \
  | xargs -r clang-format -i

# Step 2 — clang-tidy (ONLY on *.cpp files, NEVER on *.hpp)
git diff --name-only --diff-filter=d HEAD \
  | grep -E '\.cpp$' \
  | xargs -r clang-tidy -p tools/compile_commands.json --warnings-as-errors='*'
```

### Python files

```bash
# Format
ruff format scripts/ guiservice/

# Lint + type-check (from scripts/ directory)
cd scripts
ruff check cvs2parquet gtopt2pp gtopt_check_fingerprint gtopt_check_json gtopt_check_lp gtopt_check_output gtopt_check_pampl gtopt_check_solvers gtopt_compare gtopt_compress_lp gtopt_config gtopt_diagram gtopt_expand gtopt_field_extractor igtopt plp2gtopt plp_compress_case pp2gtopt run_gtopt gtopt_monitor ts2gtopt
pylint --jobs=0 cvs2parquet gtopt2pp gtopt_check_fingerprint gtopt_check_json gtopt_check_lp gtopt_check_output gtopt_check_pampl gtopt_check_solvers gtopt_compare gtopt_compress_lp gtopt_config gtopt_diagram gtopt_expand gtopt_field_extractor igtopt plp2gtopt plp_compress_case pp2gtopt run_gtopt gtopt_monitor ts2gtopt
mypy cvs2parquet gtopt2pp gtopt_check_fingerprint gtopt_check_json gtopt_check_lp gtopt_check_output gtopt_check_pampl gtopt_check_solvers gtopt_compare gtopt_compress_lp gtopt_config gtopt_diagram gtopt_expand gtopt_field_extractor igtopt plp2gtopt plp_compress_case pp2gtopt run_gtopt gtopt_monitor ts2gtopt --ignore-missing-imports
```

> **CRITICAL — pylint exit code**: pylint prints `10.00/10` even with warnings.
> CI checks the **exit code** (must be 0). Any C/R/W/E/F message = non-zero = CI failure.

### Python tests

```bash
cd scripts && python -m pytest -q           # all tests
python -m pytest -k "test_name" -q          # single test
python -m pytest -m integration -q          # integration only
```

## Code Style Guidelines

### C++

- **Standard**: C++26. Use modern features: `std::format`, `std::ranges`,
  `std::expected`, `std::flat_map`, concepts, designated initializers, etc.
- **Compiler**: Clang 21, flags: `-Wall -Wpedantic -Wextra -Werror`
- **Format**: 2-space indent, 80-char column limit (`.clang-format`)
- **Namespace**: `namespace gtopt`
- **Naming**: `PascalCase` (types), `snake_case` (functions, variables),
  `m_name_` (private members)
- **Headers**: `#pragma once`, Doxygen-style file headers
- **Includes**: `<std>`, then `<pkg/header>`, then `<gtopt/...>`
- **Trailing commas**: Always on last element of brace-initializer lists
  ```cpp
  Size first_block {0,};
  system.bus_array = {{.uid = Uid {1}, .name = "b1",},};
  ```
- **Error handling**: `std::optional` / return values over exceptions
- **`noexcept`**, **`[[nodiscard]]`**: add where appropriate

### clang-tidy suppressions in tests

Accepted NOLINTs:
```cpp
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
using namespace gtopt;  // NOLINT(google-global-names-in-headers)
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
```

**Never use `NOLINT(bugprone-unchecked-optional-access)`**. Use instead:
```cpp
CHECK(opt.value_or(-1.0) == doctest::Approx(2.5));
CHECK((opt && *opt == 2.0));
CHECK(std::get<double>(opt.value_or(0.0)) == doctest::Approx(5000.0));
```

### Python

- Python ≥ 3.10, CI uses 3.12
- `ruff format` (line-length 88), `pylint`, `mypy`
- `scripts/` is self-contained with its own `pyproject.toml`
- Coverage threshold: 83%

## Writing New Tests

Tests in `test/source/test_<topic>.cpp` — auto-discovered via `CONFIGURE_DEPENDS`.

```cpp
// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/<header>.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("<ComponentName> basic behavior")  // NOLINT
{
  SUBCASE("subcase name")
  {
    CHECK(...);
  }
}
```

**Key rules:**
1. `<doctest/doctest.h>` first, then project headers.
2. `using namespace gtopt; // NOLINT(...)` at file scope.
3. Floating-point: `doctest::Approx(value)`, never `==` on doubles.
4. `REQUIRE` for fatal, `CHECK` for non-fatal assertions.
5. Never `NOLINT(bugprone-unchecked-optional-access)` — use `value_or` / `&&`.
6. No `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (already in `main.cpp`).
7. Always trailing commas in brace-initializer lists.
8. Use C++26 features.
9. **Testing LP via JSON**: `from_json<Planning>(json_str)` or merge pattern.
10. **Testing `gtopt_main()`**: `MainOptions{.planning_files=..., .use_single_bus=true}`.

## Domain Quick-Reference

> Full details in `.github/copilot-instructions.md` → "Domain Knowledge" section.

### PLP Fortran source code

Authoritative source: `https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src`.
Each `.dat` file has a `lee*.f` reader (e.g. `leefilemb.f` → `plpfilemb.dat`).

### PLP maintenance files

- **`plpmanbat.dat`**: 3 fields: `IBind EMin EMax` → Battery `emin`/`emax`
- **`plpmaness.dat`**: 5-6 fields: `IBind Emin Emax DCMin DCMax [DCMod]`
- **`plpess.dat`**: `Nombre nd nc mloss Emax DCMax [DCMod] [CenCarga]`
  (discharge eff `nd` first, charge eff `nc` second)
  - DCMod=0: standalone battery; DCMod=1: `source_generator` (internal bus)
  - DCMod=2: regulation tank → mapped to hydro Reservoir (not Battery)
- **`plpfilemb.dat`**: filtration model with segments and unit conversions

### What gtopt optimizes

Minimize total discounted cost (OPEX + CAPEX) over scenarios, stages, blocks.

- **OPEX**: dispatch cost, demand curtailment penalty, line transfer cost
- **CAPEX**: annualized investment for expansion modules
- **Time**: `Scenario` → `Stage` → `Block` (duration in hours)

### Compression codecs

- **File I/O** (Parquet output): **zstd** (default `output_compression`)
- **In-memory** (LP snapshots in `low_memory` / SDDP): **lz4** (fast
  compress/decompress, preferred for transient data)
- `liblz4-dev` is a required build dependency; `libzstd-dev` likewise

### Key options

| Option | Default | Effect |
|--------|---------|--------|
| `use_kirchhoff` | `true` | DC OPF with voltage angles |
| `use_single_bus` | `false` | Multi-bus network |
| `demand_fail_cost` | 1000 | $/MWh unserved load penalty |
| `scale_objective` | 1000 | Divides obj coefficients for numerics |
| `input_format` / `output_format` | `"parquet"` | I/O format |
| `method` | `"monolithic"` | Planning method: `monolithic`, `sddp`, `cascade` |

### LP solver backends

LP solver backends are loaded as dynamic plugins (`libgtopt_solver_*.so`)
at runtime. The default is auto-detected by priority: cplex > highs > cbc > clp.

| CLI flag | Effect |
|----------|--------|
| `--solver highs` | Use a specific LP solver backend |
| `--solvers` | List available LP solver plugins |

Plugin search path: `$GTOPT_PLUGIN_DIR`, `<exe>/../lib/gtopt/plugins/`,
`<exe>/plugins/`, `/usr/local/lib/gtopt/plugins/`.

### IEEE benchmark cases

| Directory | Buses | Key feature |
|-----------|-------|-------------|
| `ieee_4b_ori` | 4 | Simplest OPF |
| `ieee_9b_ori` | 9 | Classic 9-bus OPF |
| `ieee_9b` | 9 | Solar profile, 24-hour dispatch |
| `ieee_14b_ori` | 14 | Standard 14-bus benchmark |
| `ieee_14b` | 14 | Constrained lines, binding KVL duals |
| `c0` | 1 | Multi-stage capacity expansion |

## Documentation Style Guide

- GitHub-Flavored Markdown, ATX headers, 80-char lines
- LaTeX math in `docs/formulation/mathematical-formulation.md`
- References: numbered `[N]`, HTML anchors, DOI links
- Cross-references use relative paths between doc files
- When modifying LP assembly code, update the formulation document
