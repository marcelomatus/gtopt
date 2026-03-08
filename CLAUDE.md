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

### How the CI installs Clang 21

The canonical Clang 21 install procedure is defined in
`.github/actions/install-clang/action.yml`.  The CI workflow (`ubuntu.yml`)
runs it **after `ccache` is set up and other apt dependencies are installed,
but before cmake is configured**.  The ordering matters: cmake bakes the
compiler-launcher path at configure time.

> **Note**: clang-22 packages are not yet available in the `apt.llvm.org`
> repository.  Use version 21 until clang-22 becomes available.

The action adds the LLVM APT repository with GPG key verification, installs
the versioned packages, and registers unversioned `update-alternatives`
entries so that `clang`, `clang++`, `clang-format`, and `clang-tidy` all
resolve to version 21 without a version suffix.

### Bootstrap from scratch (preferred — APT Arrow, then conda fallback)

> **Quickest option**: run the provided setup script which does everything
> below automatically, including APT-first Arrow with conda fallback:
> ```bash
> bash tools/setup_sandbox.sh          # deps only
> bash tools/setup_sandbox.sh --build  # deps + configure + build + test
> ```

Follow the same step order as `ubuntu.yml`: ccache first, then Arrow/Parquet
(APT first, conda fallback), then Clang 21, then cmake.

```bash
# 1. System packages — install ccache FIRST (CMake bakes its path at configure time)
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ccache \
  coinor-libcbc-dev \
  libboost-container-dev libspdlog-dev \
  liblapack-dev libblas-dev \
  zlib1g-dev ca-certificates lsb-release wget

# 2. Arrow / Parquet — try APT first (mirrors CI); fall back to conda if blocked.
#    APT is preferred: no conda overhead, no PREFIX_PATH needed for cmake.
DISTRO=$(lsb_release --id --short | tr 'A-Z' 'a-z')
CODENAME=$(lsb_release --codename --short)
ARROW_DEB="apache-arrow-apt-source-latest-${CODENAME}.deb"
ARROW_VIA_CONDA=false
if wget -q --timeout=30 \
     "https://packages.apache.org/artifactory/arrow/${DISTRO}/${ARROW_DEB}" \
   && sudo apt-get install -y -q --no-install-recommends "./${ARROW_DEB}" \
   && sudo apt-get update -q \
   && sudo apt-get install -y --no-install-recommends libarrow-dev libparquet-dev
then
  echo "✓ Arrow/Parquet installed via APT"
else
  echo "APT Arrow unavailable – falling back to conda"
  conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp
  ARROW_VIA_CONDA=true
fi

# 3. Clang 21 — via LLVM APT repository (matches .github/actions/install-clang)
#    Must be installed BEFORE cmake configure so the compiler path is baked in.
#    Note: clang-22 is not yet available on apt.llvm.org; use version 21.
for attempt in 1 2 3; do
  wget -qO /tmp/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key \
    && break
  echo "Attempt $attempt/3: wget failed, retrying in 15s..."
  sleep 15
done
sudo gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
  /tmp/llvm-snapshot.gpg.key
echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] \
  https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-21 main" \
  | sudo tee /etc/apt/sources.list.d/llvm-21.list
for attempt in 1 2 3; do
  sudo apt-get update -q \
    -o "Dir::Etc::sourcelist=/etc/apt/sources.list.d/llvm-21.list" \
    -o "Dir::Etc::sourceparts=-" && break
  echo "Attempt $attempt/3: apt-get update failed, retrying in 15s..."
  sleep 15
done
sudo apt-get install -y --no-install-recommends \
  clang-21 clang-tools-21 clang-format-21 clang-tidy-21 \
  llvm-21-dev llvm-21-tools libomp-21-dev \
  libc++-21-dev libc++abi-21-dev \
  libclang-common-21-dev libclang-21-dev libclang-cpp21-dev
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
| `/bin/sh: ccache: not found` during `cmake --build` | `ccache` not installed before CMake configure | `sudo apt-get install -y ccache` **then delete the build dir and reconfigure** |
| `Could not find ArrowConfig.cmake` | Arrow/Parquet not installed | Try APT install first; if blocked use `conda install -y -c conda-forge arrow-cpp parquet-cpp` then add `-DCMAKE_PREFIX_PATH="$(conda info --base)"` |
| `Unable to fetch some archives` from apt | Stale package lists | `sudo apt-get update` before `apt-get install` |
| `COIN solver: none configured` | COIN-OR not installed | `sudo apt-get install -y coinor-libcbc-dev` |
| `Could not find BoostConfig.cmake` | Boost not installed | `conda install -y -c conda-forge boost-cpp` (or `sudo apt-get install -y libboost-container-dev`) |
| `undefined reference to OsiClpSolverInterface` | Linker missing CLP | Delete build dir, reconfigure after reinstalling `coinor-libcbc-dev` |
| Clang not found / wrong version | Clang 21 not installed | Follow the LLVM APT install steps in the "Bootstrap from scratch" section above (see also `.github/actions/install-clang/action.yml`) |

> **Critical rule**: always install `ccache` **before** running `cmake -S all -B build`.
> CMake bakes the launcher path at configure time; installing ccache later does not help.
> Delete the build directory and reconfigure from scratch.

## Build Commands

> **Important**: The primary build target for development and testing is `cmake -S all -B build`.
> The `all/` super-project builds the library, standalone binary, unit tests, and integration
> tests in one go.  The binary is at `build/standalone/gtopt` and tests run via `ctest`.

### Complete bootstrap from scratch (sandboxed / CI agents)

> **Quickest option**: run the setup script — it handles all steps below,
> including APT Arrow with conda fallback and Clang 21 with retry:
> ```bash
> bash tools/setup_sandbox.sh --build
> ```

Run **exactly this sequence** in a fresh Ubuntu 24.04 environment.
Every step is required; skipping any one will cause a build failure.
This mirrors the step order in `.github/workflows/ubuntu.yml`.

```bash
# 1. System packages – install ccache FIRST (CMake bakes the path at configure time)
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ccache \
  coinor-libcbc-dev \
  libboost-container-dev libspdlog-dev \
  liblapack-dev libblas-dev \
  zlib1g-dev ca-certificates lsb-release wget

# 2. Arrow / Parquet — try APT first (mirrors CI), fall back to conda if blocked
DISTRO=$(lsb_release --id --short | tr 'A-Z' 'a-z')
CODENAME=$(lsb_release --codename --short)
ARROW_DEB="apache-arrow-apt-source-latest-${CODENAME}.deb"
ARROW_VIA_CONDA=false
if wget -q --timeout=30 \
     "https://packages.apache.org/artifactory/arrow/${DISTRO}/${ARROW_DEB}" \
   && sudo apt-get install -y -q --no-install-recommends "./${ARROW_DEB}" \
   && sudo apt-get update -q \
   && sudo apt-get install -y --no-install-recommends libarrow-dev libparquet-dev
then
  echo "✓ Arrow/Parquet installed via APT"
else
  echo "APT Arrow unavailable – falling back to conda"
  conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp
  ARROW_VIA_CONDA=true
fi

# 3. Clang 21 – via LLVM APT repository (matches .github/actions/install-clang/action.yml)
#    Must be installed BEFORE cmake configure so the compiler path is baked in correctly.
#    Note: clang-22 is not yet available on apt.llvm.org; use version 21.
for attempt in 1 2 3; do
  wget -qO /tmp/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key \
    && break
  echo "Attempt $attempt/3: wget failed, retrying in 15s..."
  sleep 15
done
sudo gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
  /tmp/llvm-snapshot.gpg.key
echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] \
  https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-21 main" \
  | sudo tee /etc/apt/sources.list.d/llvm-21.list
for attempt in 1 2 3; do
  sudo apt-get update -q \
    -o "Dir::Etc::sourcelist=/etc/apt/sources.list.d/llvm-21.list" \
    -o "Dir::Etc::sourceparts=-" && break
  echo "Attempt $attempt/3: apt-get update failed, retrying in 15s..."
  sleep 15
done
sudo apt-get install -y --no-install-recommends \
  clang-21 clang-tools-21 clang-format-21 clang-tidy-21 \
  llvm-21-dev llvm-21-tools libomp-21-dev \
  libc++-21-dev libc++abi-21-dev \
  libclang-common-21-dev libclang-21-dev libclang-cpp21-dev
# Register unversioned aliases (clang, clang++, clang-format, clang-tidy…)
for versioned in /usr/bin/clang*-21 /usr/bin/llvm*-21; do
  [ -e "$versioned" ] || continue
  base=$(basename "$versioned" "-21")
  sudo update-alternatives --remove-all "$base" 2>/dev/null || true
  sudo update-alternatives --install /usr/bin/"$base" "$base" "$versioned" 100
done

# 4. Pre-install Python scripts dependencies (speeds up scripts-install-deps CTest
#    fixture from ~35 s to ~3–5 s).  Must run BEFORE cmake configure so that
#    cmake's find_program(PYTHON_EXECUTABLE) picks the same Python.
uv pip install --system -q -e "./scripts[dev]" graphviz

# 5. Configure – Clang 21 + ccache; add conda PREFIX_PATH only if Arrow came from conda
#    Use `all/` super-project (builds library + binary + tests in one step)
CMAKE_PREFIX_ARG=""
${ARROW_VIA_CONDA} && CMAKE_PREFIX_ARG="-DCMAKE_PREFIX_PATH=$(conda info --base)"
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  ${CMAKE_PREFIX_ARG}

# 6. Build and test
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

> **Why try APT Arrow first?**  APT is the same source CI uses (`install-apt-deps`
> action) and produces a system installation that cmake finds without any
> `-DCMAKE_PREFIX_PATH`.  The conda fallback is provided for network-restricted
> sandboxes where `packages.apache.org` is unreachable.

> **Why ccache before cmake configure?** CMake bakes the launcher path into the
> build system at configure time.  Installing ccache *after* configure causes
> every subsequent `cmake --build` to fail even though ccache is now present.
> Always delete the build directory and reconfigure if ccache was missing.

> **Why pre-install Python scripts deps before cmake configure?**
> The `scripts-install-deps` CTest fixture calls `uv pip install -e ./scripts[dev]`
> which downloads and installs pandapower and dozens of transitive dependencies
> from scratch (~35 s) unless they are already present.  Pre-installing via
> `uv pip install --system -q -e "./scripts[dev]" graphviz` before configure means
> cmake finds the same Python that already has all packages, so the CTest fixture
> just verifies the install (~3–5 s).  Always run this before cmake configure so
> `find_program(PYTHON_EXECUTABLE)` picks the right interpreter.

### GCC 14 fallback (when Clang 21 is unavailable)

```bash
# Steps 1-2 same as above (system packages + Arrow/Parquet), then:
# If Arrow was installed via APT (no PREFIX_PATH needed):
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# If Arrow came from conda fallback, add: -DCMAKE_PREFIX_PATH="$(conda info --base)"
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Run a single test

```bash
./build/test/gtoptTests -tc="test name pattern"
# or using doctest bracket syntax:
./build/test/gtoptTests "[test name]"
```

### Unit + integration tests (e2e)

```bash
cmake -S all -B build -DGTOPT_BUILD_INTEGRATION_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" if Arrow came from conda
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Standalone binary

```bash
# The all/ build puts the binary at build/standalone/gtopt
cmake -S all -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" if Arrow came from conda
cmake --build build -j$(nproc)
./build/standalone/gtopt --version
```

### Test coverage

```bash
cmake -S all -B build -DENABLE_TEST_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" if Arrow came from conda
cmake --build build -j$(nproc)
```

## Obtaining the gtopt Binary Without Building From Scratch

The **`tools/get_gtopt_binary.py`** script is a standalone tool for Copilot /
Claude agents.  It is **not installed** via `pip install` or `cmake install`.
It handles the full binary-acquisition pipeline:

1. `GTOPT_BIN` environment variable.
2. `gtopt` already on `PATH`.
3. Standard build directories (`build/standalone/gtopt`, etc.).
4. `/tmp/gtopt-ci-bin/gtopt` – previously downloaded CI artifact.
5. Download `gtopt-binary-debug` CI artifact (requires `GITHUB_TOKEN` or `gh` CLI).
6. Build from source via `cmake` (slowest; use `--build` flag).

### Fastest: run the helper script

```bash
# From repo root – prints the binary path
export GTOPT_BIN=$(python tools/get_gtopt_binary.py)
pytest scripts/igtopt/tests/ -m integration -v

# Force a fresh CI download (skips local search)
python tools/get_gtopt_binary.py --force-download

# Fall back to building from source if CI artifact is unavailable
python tools/get_gtopt_binary.py --build
```

### Manual CI artifact download via `gh` CLI

```bash
# 1. Find the latest non-expired artifact ID
ART_ID=$(gh api "repos/marcelomatus/gtopt/actions/artifacts?name=gtopt-binary-debug" \
    --jq '.artifacts | map(select(.expired|not)) | .[0].id')

# 2. Download and unzip to /tmp/gtopt-ci-bin/
mkdir -p /tmp/gtopt-ci-bin
gh api repos/marcelomatus/gtopt/actions/artifacts/${ART_ID}/zip \
    --header "Accept: application/vnd.github+json" > /tmp/gtopt.zip
unzip -o /tmp/gtopt.zip -d /tmp/gtopt-ci-bin
chmod +x /tmp/gtopt-ci-bin/gtopt

# 3. Verify and run integration tests
export GTOPT_BIN=/tmp/gtopt-ci-bin/gtopt
pytest scripts/igtopt/tests/ -m integration -v
```

### Programmatic use in Python

```python
import sys, pathlib
sys.path.insert(0, str(pathlib.Path("tools").resolve()))

from get_gtopt_binary import get_gtopt_binary, download_gtopt_from_ci

# Auto-discover or download
bin_path = get_gtopt_binary()             # raises RuntimeError if not found
bin_path = get_gtopt_binary(allow_build=True)   # also tries cmake build

# Force CI download to a custom directory
bin_path = download_gtopt_from_ci(pathlib.Path("/tmp/my-dir"))
```

The `gtopt_bin` pytest fixture in `scripts/igtopt/tests/conftest.py` checks
`GTOPT_BIN`, `PATH`, and standard build paths, then **skips** the test if not
found — it never downloads or installs anything automatically.

* **ubuntu.yml / ctest**: `GTOPT_BIN` is set by CTest environment properties.
* **scripts.yml**: the "Download gtopt binary" step runs the tool **before**
  tests to set `GTOPT_BIN`.
* **Agent / local**: `export GTOPT_BIN=$(python tools/get_gtopt_binary.py)`.

### Key notes

* The `gtopt-binary-debug` artifact is a **Debug build** from Ubuntu 24.04
  (Clang 21 + APT Arrow/Parquet + COIN-OR).  It runs on any Ubuntu 24.04
  environment with the same shared libraries.
* Artifacts expire **7 days** after the CI run that uploaded them.
* The artifact name and retention are configured in
  `.github/workflows/ubuntu.yml` (`retention-days: 7`).
* `GITHUB_TOKEN` is automatically injected by GitHub Actions runners – no
  extra configuration needed.

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
cmake -S all -B build \
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
- **Trailing commas**: Always add a trailing comma to the **last element** of every
  brace-initializer list (in-class member initializers, aggregate initializers,
  `std::initializer_list` arguments). This prevents `readability-trailing-comma`
  warnings from clang-tidy and makes future diffs cleaner.
  ```cpp
  // ✓ correct – trailing comma on last element
  Size first_block {0,};
  Array<Phase> phase_array {Phase {},};
  system.bus_array = {{.uid = Uid {1}, .name = "b1",},};
  // ✗ wrong – no trailing comma
  Size first_block {0};
  ```
- **Templates**: Use `requires` concepts for type constraints
- **Error handling**: `std::optional` / return values over exceptions
- **`noexcept`**: Add to non-throwing functions (with conditional `noexcept` where appropriate)
- **`[[nodiscard]]`**: Add to functions whose return must not be silently discarded
- **Documentation**: Doxygen-style class/function documentation

### clang-tidy suppressions in tests

Three inline `// NOLINT` patterns are accepted in test code:

```cpp
// 1. After std::move – use-after-move is intentional in tests
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)

// 2. using namespace at file scope in .hpp test helpers
using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// 3. Anonymous namespace in .hpp test helpers
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
// ... helper functions ...
}  // namespace
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

// value_or for variant-inside-optional (RealFieldSched, Active, etc.)
CHECK(std::get<double>(opt.value_or(0.0)) == doctest::Approx(5000.0));
CHECK(std::get<IntBool>(active_opt.value_or(Active{False})) == True);
CHECK(std::get<Real>(gen.capacity.value_or(RealFieldSched{0.0})) == 60.0);

// intermediate variable to avoid dangling ref when binding const& to vector
const auto val = opt.value_or(Active{std::vector<IntBool>{}});
const auto& vec = std::get<std::vector<IntBool>>(val);
```

**`.clang-tidy` is the source of truth** for which checks are enabled. The file
includes a rationale comment for every disabled check. `modernize-type-traits`
is intentionally **enabled** — always use `_v`/`_t` type-trait aliases
(`std::is_same_v<T,U>`, `std::decay_t<T>`) in new code. To add a new disabled
check, always add its rationale as a comment in `.clang-tidy`.

### Python

The **scripts sub-package** (`scripts/`) is self-contained with its own
`pyproject.toml`, `requirements.txt`, and `requirements-dev.txt`.
It is **independent** of the root `pyproject.toml`.

- **Version**: Python ≥ 3.10 (type-checking); CI uses 3.12
- **Formatter**: `ruff format` (line-length 88)
- **Linter**: `pylint` (configured in `scripts/pyproject.toml`)
- **Type checker**: `mypy` (configured in `scripts/pyproject.toml`)
- **Tests**: `pytest` with `pytest-cov`
- **Coverage threshold**: 83% (`fail_under = 83` in `scripts/pyproject.toml`)

> **⚠️ Mandatory pre-commit checklist for Python code**:
> Before committing **any** Python changes to `scripts/` or `guiservice/`,
> always run **all four** of the following — CI will fail if any of them fail:
>
> ```bash
> # --- scripts/ ---
> cd scripts
> ruff format gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt
> ruff check  gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt
> pylint --jobs=0 gtopt_compare cvs2parquet gtopt_diagram igtopt plp2gtopt pp2gtopt ts2gtopt
> mypy gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt \
>   --ignore-missing-imports
>
> # --- guiservice/ ---
> ruff format guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py
> ruff check  guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py
> pylint --jobs=0 --rcfile=.pylintrc guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py
> mypy guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py --ignore-missing-imports
> ```

```bash
# Install (from repo root)
pip install -e "./scripts[dev]"    # editable + dev tools
pip install -r scripts/requirements.txt          # runtime only

# All commands below run from scripts/ directory
cd scripts

# Format (apply in-place)
ruff format gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt

# Lint (ruff)
ruff check gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt

# Lint (pylint — must pass at 10.00/10)
pylint --jobs=0 gtopt_compare cvs2parquet gtopt_diagram igtopt plp2gtopt pp2gtopt ts2gtopt

# Type check
mypy gtopt_compare cvs2parquet gtopt_diagram.py igtopt plp2gtopt pp2gtopt ts2gtopt \
  --ignore-missing-imports

# Run all tests (fast, < 2 s)
python -m pytest -q

# Run with coverage + missing-lines report
python -m pytest \
  --cov=cvs2parquet --cov=igtopt --cov=plp2gtopt --cov=pp2gtopt --cov=ts2gtopt \
  --cov-report=term-missing -q

# Run a single test
python -m pytest -k "test_parse_single_bess" -q

# Integration tests only
python -m pytest -m integration -q
```

Via CMake (from repo root after `cmake -S scripts -B build-scripts`):

```bash
cmake --build build-scripts --target scripts-install       # pip install -e scripts/[dev]
cmake --build build-scripts --target scripts-format        # ruff format (in-place)
cmake --build build-scripts --target scripts-check-format  # ruff format --check
cmake --build build-scripts --target scripts-lint          # pylint
cmake --build build-scripts --target scripts-ruff          # ruff check
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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
2. `using namespace gtopt;  // NOLINT(google-global-names-in-headers)` at file
   scope — the NOLINT is required because test files use `.hpp` extension and
   clang-tidy applies header rules to all `.hpp` files.
3. Floating-point: use `doctest::Approx(value)`, never `==` on doubles.
4. `REQUIRE` for fatal assertions (stop test on failure); `CHECK` for non-fatal.
5. Prefer `CHECK_FALSE(x)` over `CHECK(x == false)`.
6. **Never use `NOLINT(bugprone-unchecked-optional-access)`**. Use
   `opt.value_or(sentinel)` or `(opt && *opt == val)` instead (see above).
7. `REQUIRE(opt.has_value())` before branches that depend on the optional,
   but still use `value_or` / `&&` in the CHECK expressions themselves.
8. Accepted NOLINTs: `// NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)`
   after intentional post-`std::move` checks; `// NOLINT(google-global-names-in-headers)`
   on `using namespace` at file scope in `.hpp` test files;
   `// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)`
   on anonymous `namespace` blocks in `.hpp` test files.
9. Do NOT add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (already in `test/source/main.cpp`).
10. Use `[[maybe_unused]]` for loop variables used only for side-effects.
11. Use C++23/26 features freely: `std::format`, `std::ranges`, designated initializers, etc.
12. **Always add a trailing comma** to the last element of every brace-initializer list
    (member initializers, aggregate initializers, initializer-list arguments) to satisfy
    `readability-trailing-comma`.
13. **Testing LP via JSON**: you can use direct `from_json<Planning>(json_str)` or the
    merge pattern `Planning base; base.merge(from_json<Planning>(json_str))`.
    Both work now: `SimulationLP` automatically falls back to a single default
    `Phase{}`/`Scene{}` when `phase_array`/`scene_array` are empty.
    The merge pattern is still preferred when loading from multiple JSON files.
14. **Testing `gtopt_main()`**: use `MainOptions{.planning_files=..., .use_single_bus=true}`.
    Only set the fields you need — all others default to `std::nullopt`.

## Domain Quick-Reference

> Full details are in `.github/copilot-instructions.md` → "Domain Knowledge" section.
> Scripts sub-package details are in `.github/copilot-instructions.md` → "Python Scripts Sub-Package".
> PLP file formats and maintenance mappings are in `.github/copilot-instructions.md` → "Key facts for plp2gtopt".

### PLP maintenance file formats (plpmanbat.dat / plpmaness.dat)

The parsers match the PLP Fortran READ statements:

- **`plpmanbat.dat`** (`LeeManBat` in `genpdbaterias.f`): 3 fields per data line:
  `IBind EMin EMax` (block index, min energy MWh, max energy MWh).
  Modifies battery energy bounds → maps to Battery `emin`/`emax` schedules in gtopt.

- **`plpmaness.dat`** (`LeeManEss` in `genpdess.f`): 5-6 fields per data line:
  `IBind Emin Emax DCMin DCMax [DCMod]`.
  Energy bounds → Battery `emin`/`emax`; DC power bounds → Generator `pmax` + Demand `lmax`.

- **`plpess.dat`** field order is `Nombre nd nc mloss Emax DCMax [DCMod] [CenCarga]`
  (Fortran reads discharge efficiency `nd` first, charge efficiency `nc` second).

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
| `use_line_losses` | `true` | Model resistive line losses (default `true`) |
| `demand_fail_cost` | 1000 | $/MWh penalty for unserved load |
| `reserve_fail_cost` | 5000 | $/MWh penalty for unserved spinning reserve |
| `scale_objective` | 1000 | Divides objective coefficients for solver numerics |
| `annual_discount_rate` | 0.1 | 10 % per year for CAPEX discounting |
| `input_format` | `"parquet"` | Preferred input format (`"parquet"` default; falls back to CSV) |
| `output_format` | `"parquet"` | Output format (`"parquet"` default; or `"csv"`) |
| `output_compression` | `"gzip"` | Parquet compression codec (default `"gzip"`) |

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

---

## Documentation Style Guide

When updating documentation files in this repository, follow these guidelines:

### General Principles

- All documentation is written in **GitHub-Flavored Markdown** (GFM).
- Use ATX-style headers (`#`, `##`, `###`) with a blank line before and after.
- Keep lines under 80 characters where practical (tables and URLs may exceed).
- Use `code backticks` for file names, command-line flags, JSON fields, C++
  identifiers, and function names.
- Use **bold** for emphasis on key terms; *italics* for introducing new terms
  or referencing titles.

### Mathematical Notation

- Use LaTeX math blocks (`$$...$$`) for display equations in
  `docs/formulation/MATHEMATICAL_FORMULATION.md`.
- Use inline math (`$...$`) for symbols referenced in text.
- Map every mathematical symbol to its corresponding JSON field name in the
  "Mapping" section (§7) and Parameters table (§2).
- When adding new constraints or variables, add them to the Compact
  Formulation summary table (§3), the detailed section (§5), and the
  JSON mapping table (§7).

### Academic References

- References are numbered `[N]` and collected in Section 9 of
  `docs/formulation/MATHEMATICAL_FORMULATION.md`.
- Use HTML anchor tags: `<a id="refN"></a>` before each reference entry.
- Inline citations use `[[N]](#refN)` markdown syntax.
- Always include DOI links when available: `DOI: [10.xxxx/...](https://doi.org/10.xxxx/...)`.
- Group references by category: FESOP/gtopt publications, TEP classics,
  DC OPF, similar tools, solvers, surveys.
- When adding a new formulation feature, cite the relevant academic source.

### Cross-References Between Documents

- Every major documentation file should have a "See also" section at the
  bottom linking to related documents.
- Use relative paths: `[PLANNING_GUIDE.md](PLANNING_GUIDE.md)` from root,
  `[Planning Guide](../../PLANNING_GUIDE.md)` from `docs/formulation/`.
- The mathematical formulation is the authoritative reference for the LP/MIP
  model; other documents should link to it for formulation details.
- The current cross-reference graph:
  - `README.md` → all documents
  - `PLANNING_GUIDE.md` → `INPUT_DATA.md`, `USAGE.md`, `SCRIPTS.md`,
    `BUILDING.md`, `DIAGRAM_TOOL.md`, `MATHEMATICAL_FORMULATION.md`
  - `MATHEMATICAL_FORMULATION.md` → `PLANNING_GUIDE.md`, `INPUT_DATA.md`,
    `USAGE.md`, `CONTRIBUTING.md`, `BUILDING.md`, `SCRIPTS.md`
  - `USAGE.md` → `MATHEMATICAL_FORMULATION.md`, `PLANNING_GUIDE.md`,
    `INPUT_DATA.md`, `SCRIPTS.md`
  - `INPUT_DATA.md` → `MATHEMATICAL_FORMULATION.md`, `PLANNING_GUIDE.md`,
    `USAGE.md`, `SCRIPTS.md`

### Formulation Validation

- The mathematical formulation in `MATHEMATICAL_FORMULATION.md` has been
  validated against the C++ implementation (`source/*_lp.cpp`,
  `include/gtopt/*_lp.hpp`). Key verified components:
  - DC power flow: `f = B(θ_a − θ_b)` with `B = V²/X`, angle scaling by
    `scale_theta` (default 1000)
  - Battery SoC: `e[b] = e[b-1]·(1−μ·Δb) + p_in·η_in·Δb − p_out·Δb/η_out`
  - Bus balance: `Σ(1−λ_g)·p_g − Σ(1+λ_d)·ℓ_d + net_flows = 0`
  - Discount factor: `δ_t = (1+r)^(−τ_t/8760)`
  - Capacity expansion: `C_t = C_{t-1}·(1−ξ) + expcap·m_t + ΔC_t`
- When modifying the C++ LP assembly code, update the corresponding section
  in the formulation document to maintain consistency.

### Documentation File Purposes

| File | Purpose | Audience |
|------|---------|----------|
| `README.md` | Quick start, feature overview | New users |
| `BUILDING.md` | Build instructions, dependencies | Developers |
| `USAGE.md` | CLI reference, output interpretation | Users |
| `INPUT_DATA.md` | JSON/Parquet input format spec | Case builders |
| `PLANNING_GUIDE.md` | Worked examples, concepts | Planners |
| `SCRIPTS.md` | Python tool overview | Script users |
| `CONTRIBUTING.md` | Code style, testing, CI | Contributors |
| `MATHEMATICAL_FORMULATION.md` | LP/MIP formulation, references | Researchers |
| `DIAGRAM_TOOL.md` | Network diagram tool | Visualization |
| `CLAUDE.md` | AI agent guidance | Claude Code |
| `.github/copilot-instructions.md` | AI agent guidance | GitHub Copilot |
