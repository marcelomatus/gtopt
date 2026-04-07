# GitHub Copilot Instructions for gtopt

## What is gtopt?

**gtopt** is a high-performance C++ library and solver for **Generation and
Transmission Expansion Planning (GTEP)**. It minimizes the total expected cost
of operation and expansion of electrical power systems (CAPEX + OPEX). It
supports single-bus and multi-bus DC power flow (Kirchhoff's laws), Parquet/CSV/JSON
I/O, and a sparse-matrix LP/MIP formulation via pluggable LP solver backends
(CLP, CBC, CPLEX, HiGHS) loaded dynamically at runtime.

The repository also contains:
- `standalone/` – thin `main()` wrapper that builds the `gtopt` binary
- `webservice/` – Next.js REST API and browser UI for submitting/monitoring jobs
- `guiservice/` – Python/Flask GUI for creating and editing cases
- `scripts/` – Python utilities (`plp2gtopt`, `igtopt`, `cvs2parquet`)
- `cases/` – sample optimization cases (`c0`, `c1`, ieee_*)
- `integration_test/` – CMake helpers for end-to-end (e2e) test registration

---

## Environment Setup (Ubuntu / GitHub Actions)

> ### ⚡ ALWAYS START HERE — use the sandbox script
>
> In **every** sandbox or agent session, run this **first** before any
> `cmake` or `apt-get` commands.  It installs all dependencies correctly
> (conda Arrow, Clang 21, ccache, COIN-OR, Python deps) and optionally
> configures, builds, and tests:
>
> ```bash
> bash tools/setup_sandbox.sh          # install deps only
> bash tools/setup_sandbox.sh --build  # install deps + build + test (PREFERRED)
> ```
>
> **Use `--build` in agent sessions** — it builds the project and saves
> `tools/compile_commands.json` so clang-tidy can be run on any file
> without rebuilding:
> ```bash
> clang-tidy -p tools/compile_commands.json source/my_file.cpp
> ```
>
> The script is idempotent — safe to run again if something was missed.
> **The script first attempts Clang 21** (preferred, matches CI); if the LLVM
> APT repository is unreachable it **falls back automatically to GCC 14**
> without asking.  A summary at the end reports which compiler was chosen.
> Do not install Arrow via APT (`libarrow-dev` from `packages.apache.org`);
> the APT v2300 package has versioned curl symbols that conflict with conda
> Arrow at link time, producing `undefined reference` linker errors.

### How the CI installs Clang 21

The canonical procedure for installing Clang 21 is defined in
`.github/actions/install-clang/action.yml`.  The CI workflow
(`.github/workflows/ubuntu.yml`) runs it in this order:

1. **Set up ccache** (before any compilation, including cmake feature tests)
2. **Install APT dependencies** (`install-apt-deps` action — COIN-OR, Boost, spdlog, Arrow/Parquet, etc.)
3. **Install Clang 21** (`install-clang` action — adds LLVM APT repo, installs packages, registers alternatives)
4. **Configure** (`cmake -S all -B build …`)
5. **Build and test**

> **Note**: clang-22 packages are not yet available on `apt.llvm.org`.
> Use version 21 until clang-22 packages become available.

The `install-clang` action registers unversioned `update-alternatives` entries
so that `clang`, `clang++`, `clang-format`, `clang-tidy`, etc. resolve to
version 21 without a version suffix.

### Complete bootstrap from scratch (sandboxed / CI agents)

> **Quickest option**: run the setup script — it handles all steps below
> including conda Arrow and Clang 21 with retry:
> ```bash
> bash tools/setup_sandbox.sh --build
> ```

Run **exactly this sequence** in a fresh Ubuntu 24.04 environment.
Every step is required; skipping any one causes a build failure.

**Sandbox/agent environments**: use **conda** for Arrow/Parquet and pass
`-DCMAKE_PREFIX_PATH="$(conda info --base)"` to cmake. Do not mix APT and
conda Arrow in the same environment.

**Local development**: use system-installed Arrow (`sudo apt-get install -y
libarrow-dev libparquet-dev`) — no conda or `CMAKE_PREFIX_PATH` needed.

```bash
# 1. System packages — install ccache FIRST (CMake bakes its path at configure time)
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ccache \
  coinor-libcbc-dev \
  libboost-container-dev libspdlog-dev \
  liblapack-dev libblas-dev \
  lcov zlib1g-dev libzstd-dev zstd ca-certificates lsb-release wget

# 2. Arrow / Parquet — via conda-forge (sandbox/agent environments only).
#    Locally, use APT: sudo apt-get install -y libarrow-dev libparquet-dev
#    When using conda Arrow, pass -DCMAKE_PREFIX_PATH="$(conda info --base)" to cmake.
conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp

# 3. Clang 21 — via LLVM APT repository (matches .github/actions/install-clang/action.yml)
#    Must be installed BEFORE cmake configure so the compiler path is baked in correctly.
#    Note: clang-22 is not yet available on apt.llvm.org; use version 21.
CODENAME=$(lsb_release --codename --short)
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

# 5. Configure — Clang 21 + ccache + conda PREFIX_PATH (needed when using conda Arrow)
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"

# 6. Build and test
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

> **When is `CMAKE_PREFIX_PATH` needed?** Only when Arrow/Parquet are installed
> via conda (sandbox/agent environments). With system-installed Arrow (APT
> `libarrow-dev`), cmake finds Arrow automatically — no `CMAKE_PREFIX_PATH`
> needed. Do not mix APT and conda Arrow in the same environment: cmake may
> resolve headers from one and libraries from the other, causing linker errors.

> **Why ccache before cmake configure?** CMake bakes the compiler-launcher path
> into the build system at configure time.  Installing ccache *after* configure
> causes every `cmake --build` invocation to fail even though ccache is now
> present.  Delete the build directory and reconfigure if this happens.

> **Why pre-install Python scripts deps before cmake configure?**
> The `scripts-install-deps` CTest fixture calls `uv pip install -e ./scripts[dev]`
> which downloads and installs pandapower and dozens of transitive dependencies
> from scratch (~35 s) unless they are already present.  Pre-installing via
> `uv pip install --system -q -e "./scripts[dev]" graphviz` before configure means
> cmake's `find_program(PYTHON_EXECUTABLE)` finds the same Python that already has
> all packages, so the CTest fixture only needs to verify the install (~3–5 s).

## Build Commands

### Primary build target (library + binary + tests)

```bash
# Configure – use `all/` super-project (same as CI)
# Add -DCMAKE_PREFIX_PATH only if using conda Arrow (sandbox); not needed locally:
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"

# Build
cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run a single test by name (doctest filter syntax)
./build/test/gtoptTests "[test name]"
```

### Unit tests + integration tests (e2e)

```bash
cmake -S all -B build \
  -DGTOPT_BUILD_INTEGRATION_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" only if using conda Arrow (sandbox)
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Standalone binary

```bash
# The `all/` build places the binary at build/standalone/gtopt
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" only if using conda Arrow (sandbox)
cmake --build build -j$(nproc)
./build/standalone/gtopt --version
```

### Coverage

```bash
cmake -S all -B build \
  -DENABLE_TEST_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" only if using conda Arrow (sandbox)
cmake --build build -j$(nproc)
cd build && ctest
lcov --capture --directory . --output-file coverage.info \
  --gcov-tool /tmp/llvm-gcov.sh \
  --ignore-errors mismatch,mismatch
```

---

## Verified Build Environment

The following combination produces a **100% passing** build on Ubuntu 24.04 (Noble):

| Component | Version | Notes |
|-----------|---------|-------|
| OS | Ubuntu 24.04 (Noble) | GitHub Actions runner |
| Compiler | Clang 21 (preferred) | Installed via LLVM APT repo (`.github/actions/install-clang`); `setup_sandbox.sh` falls back to GCC 14 if LLVM APT is unreachable |
| CMake | 3.31.6 | Pre-installed on runner |
| Arrow / Parquet | 12.0.0 | System APT `libarrow-dev` (local) or conda `arrow-cpp parquet-cpp` (sandbox) |
| Boost.Container | 1.83.0 | `sudo apt-get install -y libboost-container-dev` (or `conda install -c conda-forge boost-cpp` in sandbox) |
| COIN-OR solver | CLP 1.17 (auto) | `coinor-libcbc-dev`; CMake auto-selects CLP; CBC works too |
| spdlog | 1.12.0 | `libspdlog-dev` from Ubuntu repos |
| LAPACK/BLAS | 3.12.0 | `liblapack-dev libblas-dev` (required by COIN-OR) |
| ccache | any | Must be installed **before** `cmake -S all -B build` |
| conda | 26.1.0 | Base prefix at `/usr/share/miniconda`; only needed in sandbox/agent environments |

**Verified configure command (Clang 21; add PREFIX_PATH only with conda Arrow):**

```bash
# Quickest: use the setup script
bash tools/setup_sandbox.sh --build
# Expected: 100% tests passed, 0 tests failed
```

Or manually (after running `tools/setup_sandbox.sh` deps-only):

```bash
uv pip install --system -q -e "./scripts[dev]" graphviz
cmake -S all -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
# Expected: 100% tests passed, 0 tests failed
```

### Common build failures and fixes

| Symptom | Cause | Fix |
|---------|-------|-----|
| `/bin/sh: ccache: not found` during `cmake --build` | `ccache` not installed before CMake configure | `sudo apt-get install -y ccache` **then delete build dir and reconfigure** |
| `Could not find ArrowConfig.cmake` | Arrow/Parquet not installed | **Local**: `sudo apt-get install -y libarrow-dev libparquet-dev`. **Sandbox**: `conda install -y -c conda-forge arrow-cpp parquet-cpp` + `-DCMAKE_PREFIX_PATH="$(conda info --base)"` |
| `undefined reference to curl_*@CURL_OPENSSL_4` | APT Arrow conflicts with conda Arrow | Pick one: either system APT Arrow (no PREFIX_PATH) or conda Arrow (with PREFIX_PATH). Do not mix both |
| `Unable to fetch some archives` from apt | Stale package lists | `sudo apt-get update` before `apt-get install` |
| `COIN solver: none configured` / no solver found | COIN-OR not installed | `sudo apt-get install -y coinor-libcbc-dev`; CLP is auto-selected and sufficient for unit tests |
| `Could not find BoostConfig.cmake` | Boost not installed | `sudo apt-get install -y libboost-container-dev` (or `conda install -y -c conda-forge boost-cpp` in sandbox) |
| `undefined reference to OsiClpSolverInterface` | Linker missing CLP | Delete build dir, reconfigure after reinstalling `coinor-libcbc-dev` |
| `clang: command not found` or wrong version | Clang 21 not installed | Follow the LLVM APT install steps in the "Complete bootstrap" section above (see `.github/actions/install-clang/action.yml`) |
| `scripts-install-deps` CTest fixture slow (~35 s) | Python scripts deps not pre-installed | Run `uv pip install --system -q -e "./scripts[dev]" graphviz` **before** `cmake -S all -B build` |

**Critical rule**: install `ccache` **before** `cmake -S all -B build`.
CMake bakes the launcher path into the build system at configure time; if ccache
is missing when you configure, every subsequent `cmake --build` invocation fails
even after installing ccache later.

**Second critical rule**: pre-install scripts deps (`uv pip install --system -q -e "./scripts[dev]" graphviz`)
**before** `cmake -S all -B build`.  The `scripts-install-deps` CTest fixture runs
during `ctest` and installs packages using whichever Python cmake found at configure
time.  Pre-installing ensures cmake picks the same Python and the fixture only needs
to verify the install (~3–5 s) instead of downloading everything from scratch (~35 s).

---

## Obtaining the gtopt Binary Without Building From Scratch

The **`tools/get_gtopt_binary.py`** script is a standalone Copilot / Claude
agent tool.  It is **not installed** via `pip install ./scripts` or
`cmake install`.  Use it to obtain a `gtopt` binary with minimal effort.

### Strategy (tried in order)

1. `GTOPT_BIN` environment variable → path to an existing binary.
2. `shutil.which("gtopt")` → binary on `PATH`.
3. Standard build-directory paths relative to the repository root:
   `build/standalone/gtopt`, `build/gtopt`, `build-standalone/gtopt`,
   `all/build/gtopt`.
4. `/tmp/gtopt-ci-bin/gtopt` → previously downloaded CI artifact.
5. Download `gtopt-binary-debug` artifact from the latest successful CI run
   (requires `GITHUB_TOKEN` or `gh` CLI).
6. Build from source via `cmake` (`--build` flag; slowest option).

### Fastest: run the helper script

```bash
# From repo root – prints the binary path and exports it
export GTOPT_BIN=$(python tools/get_gtopt_binary.py)
pytest scripts/igtopt/tests/ -m integration -v

# Force a fresh CI download
python tools/get_gtopt_binary.py --force-download

# Also try building from source if CI artifact unavailable
python tools/get_gtopt_binary.py --build
```

### Manual CI artifact download via `gh` CLI

```bash
# 1. Find the latest non-expired artifact ID
ART_ID=$(gh api "repos/marcelomatus/gtopt/actions/artifacts?name=gtopt-binary-debug" \
    --jq '.artifacts | map(select(.expired|not)) | .[0].id')

# 2. Download and unzip
mkdir -p /tmp/gtopt-ci-bin
gh api repos/marcelomatus/gtopt/actions/artifacts/${ART_ID}/zip \
    --header "Accept: application/vnd.github+json" > /tmp/gtopt.zip
unzip -o /tmp/gtopt.zip -d /tmp/gtopt-ci-bin
chmod +x /tmp/gtopt-ci-bin/gtopt
/tmp/gtopt-ci-bin/gtopt --version

# 3. Run integration tests
export GTOPT_BIN=/tmp/gtopt-ci-bin/gtopt
pytest scripts/igtopt/tests/ -m integration -v
```

### Programmatic use in Python

```python
import sys, pathlib
sys.path.insert(0, str(pathlib.Path("tools").resolve()))

from get_gtopt_binary import get_gtopt_binary, download_gtopt_from_ci

# Auto-discover or download (raises RuntimeError if not found)
bin_path = get_gtopt_binary()

# Also try cmake build as last resort
bin_path = get_gtopt_binary(allow_build=True)

# Force CI download to a custom directory
bin_path = download_gtopt_from_ci(pathlib.Path("/tmp/my-dir"))
```

The `gtopt_bin` fixture in `scripts/igtopt/tests/conftest.py` checks `GTOPT_BIN`,
`PATH`, and standard build paths, then **skips** the test if not found — it never
downloads or installs anything.  To run igtopt integration tests:

* **ubuntu.yml / ctest**: `GTOPT_BIN` is set automatically via CTest environment
  properties pointing at `build/standalone/gtopt`.
* **scripts.yml**: the "Download gtopt binary" step runs
  `python tools/get_gtopt_binary.py` **before** the tests to download the artifact
  and export `GTOPT_BIN`.
* **Agent / local**: run `export GTOPT_BIN=$(python tools/get_gtopt_binary.py)`
  once before `pytest scripts/igtopt/tests/ -m integration`.

### Key notes

* The artifact is a **Debug build** from Ubuntu 24.04 (Clang 21 + APT Arrow/Parquet
  + COIN-OR).  It runs on any Ubuntu 24.04 environment with the same shared libraries.
* Artifacts expire **7 days** after the CI run that created them.
* The artifact name is `gtopt-binary-debug`; configured in
  `.github/workflows/ubuntu.yml` under `upload gtopt binary` step.
* `GITHUB_TOKEN` is automatically injected by GitHub Actions runners and is
  sufficient for downloading artifacts via `gh api` or `urllib.request`.
* `tools/get_gtopt_binary.py` lives in the `tools/` directory (not `scripts/`).
  It is an **agent-only tool** and must **not** be installed via `pip install`
  or `cmake install`.

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
cmake -S all -B build \
  -DCMAKE_CXX_CLANG_TIDY="clang-tidy;--warnings-as-errors=*" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# Add -DCMAKE_PREFIX_PATH="$(conda info --base)" only if using conda Arrow (sandbox)
cmake --build build -j$(nproc)
```

> **⚠️ Before committing ANY code**, always format and lint the changed files:
>
> **C++ files** — run from the repo root (requires a configured build directory):
>
> ```bash
> # Step 1 — clang-format (REQUIRED before every C++ commit)
> git diff --name-only --diff-filter=d HEAD \
>   | grep -E '\.(cpp|hpp|h|cc|cxx|hxx)$' \
>   | xargs -r clang-format -i
>
> # Step 2 — clang-tidy (REQUIRED before every C++ commit in agent sessions)
> # Uses tools/compile_commands.json (saved by setup_sandbox.sh --build).
> # If tools/compile_commands.json does not exist, run:
> #   bash tools/setup_sandbox.sh --build
> # NOTE: clang-tidy must ONLY be run on *.cpp files, NEVER on *.hpp files.
> # Running clang-tidy on .hpp files triggers spurious header-specific checks
> # (e.g. cppcoreguidelines-avoid-non-const-global-variables for TEST_CASE
> # macros, hicpp-member-init for aggregate structs) that are false positives
> # in this project's conventions. The .cpp translation units already pull in
> # the headers and are the correct analysis targets.
> git diff --name-only --diff-filter=d HEAD \
>   | grep -E '\.cpp$' \
>   | xargs -r clang-tidy -p tools/compile_commands.json --warnings-as-errors='*'
> ```
>
> **Python files** — run from the repo root before every Python commit:
>
> ```bash
> # Step 1 — ruff format (REQUIRED before every Python commit)
> ruff format scripts/ guiservice/
>
> # Step 2 — ruff check (REQUIRED before every Python commit)
> cd scripts && ruff check gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt
>
> # Step 3 — pylint (REQUIRED; exit code MUST be 0 — no messages of any category)
> # IMPORTANT: pylint reports a score of 10.00/10 even when convention/refactor/
> # warning messages are present.  The EXIT CODE is the authoritative pass/fail
> # signal, not the score line.  Exit code 0 = clean; any other value = failure.
> # Pylint exit codes are bitwise OR of: 1=fatal, 2=error, 4=warning,
> # 8=refactor, 16=convention, 32=usage-error.
> # Even a single "C" (convention) message produces exit code 16 — a CI failure.
> cd scripts && pylint --jobs=0 gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt
>
> # Step 4 — mypy (REQUIRED; no errors allowed)
> cd scripts && mypy gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt --ignore-missing-imports
> ```
>
> **Both steps are mandatory in agent sessions.** Fix all clang-tidy warnings
> before committing C++ code.  If a warning is a false positive, add an inline
> `// NOLINT(check-name)` with a justification comment.  See the
> "clang-tidy suppressions in test code" section below for accepted patterns.

The CI **auto-formats** every push on non-main branches and commits a fixup.
Format violations are warnings only, not CI failures.

---

## Code Style – C++

| Aspect | Rule |
|--------|------|
| Standard | C++26 (`set(CMAKE_CXX_STANDARD 26)`). **Always use modern C++26** features in new code: `std::format`, `std::ranges`, `std::expected`, `std::flat_map`, `std::mdspan`, concepts, designated initializers, structured bindings, `constexpr` containers. Clang 21 is required. |
| Compiler | Clang 21 (required) |
| Indentation | 2 spaces (see `.clang-format`, `IndentWidth: 2`) |
| Column limit | 80 characters |
| Namespace | All library code lives in `namespace gtopt` |
| Naming | Classes/Structs: `PascalCase`; free functions and methods: `snake_case`; data members: `snake_case`; private class members: `m_` prefix + `_` suffix (e.g. `m_simulation_`, `m_options_`) |
| Header guards | `#pragma once` (not `#ifndef` guards) |
| File headers | Doxygen-style `@file`, `@brief`, `@date`, `@author`, `@copyright` |
| Includes | Grouped and sorted: (1) `<std>` headers, (2) external `<pkg/header.hpp>`, (3) project `<gtopt/...>`. See `.clang-format` `IncludeCategories`. |
| Pointers | Left-aligned: `T*` not `T *` |
| Braces | `BreakBeforeBraces: Custom` – opening brace on new line for functions, classes, namespaces; same line for control flow unless multi-line body |
| Initializer lists | Designated initializers (`SparseCol{.cost=1}`) preferred |
| Trailing commas | Always add a trailing comma to the **last element** of every brace-initializer list (member initializers, aggregate initializers, `std::initializer_list` arguments). Prevents `readability-trailing-comma` and makes diffs cleaner: `Array<Phase> phase_array {Phase {},};` |
| Concepts | Use `requires` constraints for template type safety |
| Error handling | Return values and `std::optional` over exceptions |
| `noexcept` | Add `noexcept` (or conditional `noexcept`) on non-throwing functions |
| `[[nodiscard]]` | Add to functions whose return value must not be silently discarded |
| `[[maybe_unused]]` | Use for intentionally unused variables/parameters |
| Magic numbers | Avoid; define named constants or use designated initializers |
| Casts | C++ casts (`static_cast`, `reinterpret_cast`) only – no C-style casts |
| Auto | Use `auto` for complex types, iterators, lambda returns; be explicit for simple types |

### clang-tidy suppressions in test code

Three inline `// NOLINT` patterns are accepted in test code:

```cpp
// 1. After std::move – use-after-move is intentional in tests
CHECK(b.empty());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)

// 2. using namespace at file scope in .hpp test files – clang-tidy applies
//    header rules to all .hpp files, but test helpers use .hpp only for
//    #include-based aggregation (included by *_all.cpp), not as API headers.
using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// 3. Anonymous namespace in .hpp test files – same rationale as above.
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
// ... helper functions ...
}  // namespace
```

**Never use `// NOLINT(bugprone-unchecked-optional-access)`.**
Instead, access optional values with safe checked patterns:

```cpp
// Pattern A – value_or(sentinel) where sentinel ≠ expected value
CHECK(opt.value_or(-1.0) == doctest::Approx(2.5));
CHECK(opt.value_or(0) == 42);
CHECK(opt.value_or("") == "hello");
CHECK(opt.value_or(false) == true);

// Pattern B – boolean short-circuit (&&), useful for expressions
CHECK((opt && *opt == 2.0));
CHECK((opt && (*opt)->member == expected));

// Pattern C – value_or for variant-inside-optional
// (use a sentinel that is the same variant alternative as expected)
CHECK(std::get<double>(opt.value_or(0.0)) == doctest::Approx(5000.0));
CHECK(std::get<IntBool>(active_opt.value_or(Active{False})) == True);
// For RealFieldSched (std::variant<Real,Vector,FileSched>) use RealFieldSched sentinel:
CHECK(std::get<Real>(opt.value_or(RealFieldSched{0.0})) == 60.0);

// Pattern D – intermediate variable when binding const& to a vector inside
// a variant-inside-optional (avoids dangling reference from temporary)
REQUIRE(opt.has_value());
const auto val = opt.value_or(Active{std::vector<IntBool>{}});
const auto& vec = std::get<std::vector<IntBool>>(val);
CHECK(vec.size() == 4);
```

---

## .clang-tidy Configuration Rationale

The project enables all clang-tidy checks (`"*"`) and selectively disables a
curated list. Every disabled check has a documented reason in `.clang-tidy`.
Key principles for the disabled list:

| Group | Reason for disabling |
|-------|---------------------|
| `altera-*`, `llvmlibc-*` | Domain-specific (FPGA / LLVM libc); not relevant |
| `fuchsia-*` | Fuchsia OS style; conflicts with project conventions |
| `llvm-header-guard`, `portability-avoid-pragma-once` | Project uses `#pragma once` |
| `llvm-include-order` | clang-format handles include ordering |
| `google-build-using-namespace` | `using namespace` in `.cpp` TU scope is accepted |
| `modernize-use-trailing-return-type` | Both styles used; not uniformly enforced |
| `modernize-macro-to-enum`, `cppcoreguidelines-macro-*` | COIN-OR and deps use C macros |
| `misc-non-private-member-variables-in-classes`, `misc-multiple-inheritance` | Structs with public members and MI are idiomatic in this codebase |
| `readability-magic-numbers`, `cppcoreguidelines-avoid-magic-numbers` | LP coefficients (0.0, 1.0, 0.5) are self-documenting |
| `readability-identifier-length`, `readability-identifier-naming` | Single-char vars idiomatic in LP/math code; custom naming rules not enforced |
| `readability-redundant-member-init` | Explicit `{}` initialization is intentional |
| `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` | `[]` preferred over `.at()` in hot LP-assembly paths |
| `bugprone-easily-swappable-parameters` | Too noisy for LP/math API functions |
| `clang-analyzer-{security.ArrayBound,cplusplus.NewDeleteLeaks,…}` | False positives with COIN-OR internal code |
| `llvm-prefer-static-over-anonymous-namespace` | `misc-use-anonymous-namespace` (enabled) takes the opposite, preferred stance |
| `misc-include-cleaner` | Too aggressive; third-party transitive includes are common |

**`modernize-type-traits` is intentionally ENABLED** (not in disabled list).
The project targets C++26 and uses modern `_v`/`_t` type-trait aliases
(`std::is_same_v`, `std::decay_t`, etc.). Any code using old `::value` / `::type`
style should be updated.

**To add a new disabled check**, always add its rationale as a comment in the
`Checks:` block in `.clang-tidy`. Never disable a check silently.

---

## Writing New Tests

Tests live in `test/source/test_<topic>.cpp`. The framework is
[doctest](https://github.com/doctest/doctest) 2.4.12.

### Minimal test file template

```cpp
// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/<header>.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
2. **`using namespace gtopt;  // NOLINT(google-global-names-in-headers)`** at file scope.
   The NOLINT is required because test helpers use `.hpp` extension (for `#include`-based
   aggregation by `*_all.cpp`) but clang-tidy applies header rules to all `.hpp` files.
3. One `TEST_CASE` per logical concept; use `SUBCASE` for variants.
4. Floating-point comparisons: use `doctest::Approx(value)` – never `==` on doubles.
5. `REQUIRE` to stop the test case on first failure; `CHECK` for non-fatal checks.
6. Prefer `CHECK_FALSE(x)` over `CHECK(x == false)`.
7. **Never use `NOLINT(bugprone-unchecked-optional-access)`**. Access optional
   values with checked patterns: `opt.value_or(sentinel)` or
   `(opt && *opt == val)`. See the "clang-tidy suppressions" section above.
8. `REQUIRE(opt.has_value())` before any logic that branches on the optional
   — but still use `value_or` / `&&` in the CHECK expressions themselves.
9. Accepted NOLINTs:
   - `// NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)` after intentional
     post-`std::move` checks.
   - `// NOLINT(google-global-names-in-headers)` on `using namespace` in `.hpp` test
     files.
   - `// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)`
     on anonymous `namespace` blocks in `.hpp` test files.
10. Use `[[maybe_unused]]` for loop variables used only for side-effects.
11. Name the test file `test_<snake_case_topic>.cpp` and add it to the glob in
    `test/CMakeLists.txt` automatically via `CONFIGURE_DEPENDS`.
12. Do NOT add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` – that is already in
    `test/source/main.cpp`.
13. **Always use modern C++26** features: `std::format`, `std::ranges`,
    `std::expected`, `std::flat_map`, `std::mdspan`, concepts with `requires`,
    designated initializers, structured bindings, `constexpr` containers,
    `auto` return types, etc.  The project targets C++26 with Clang 21.
14. **Always add a trailing comma** to the last element of every brace-initializer list
    (member initializers, aggregate initializers, initializer-list arguments):
    ```cpp
    // ✓ correct
    Array<Phase> phase_array {Phase {},};
    system.bus_array = {{.uid = Uid {1}, .name = "b1",},};
    ```
15. **Testing the LP solver via JSON**: direct `from_json<Planning>(json_str)` now
    works safely — `SimulationLP` falls back to default `Phase{}`/`Scene{}` when
    `phase_array`/`scene_array` are empty.  The merge pattern is still preferred
    when accumulating multiple JSON files:
    ```cpp
    Planning base;
    base.merge(from_json<Planning>(json_str));
    PlanningLP plp(base, ...);
    ```
16. **Testing `gtopt_main()`**: use `MainOptions` with designated initializers.
    Write temporary JSON to `std::filesystem::temp_directory_path()` and pass
    the stem (without `.json`) as `planning_files`:
    ```cpp
    const auto stem = write_tmp_json("my_test", json_str);
    auto result = gtopt_main(MainOptions{
        .planning_files = {stem.string()},
        .use_single_bus = true,
    });
    REQUIRE(result.has_value());
    CHECK(*result == 0);
    ```

### Example – testing a utility function

```cpp
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/utils.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
| Formatter | `ruff format` (line-length 88) |
| Import sorter | `isort` (profile `black`) |
| Linter | `ruff` + `pylint` |
| Type checker | `mypy` |
| Test framework | `pytest` |
| Dependencies | `guiservice/requirements.txt`: flask, pandas, pyarrow, requests |
| Scripts deps | `scripts/requirements.txt` (runtime), `scripts/requirements-dev.txt` (dev+test) |

> **⚠️ Mandatory pre-commit checklist for Python code**:
> Before committing **any** Python changes, always run the following.
> CI will fail on any violation.
> **This applies to every new `.py` file you create** — run all tools
> before committing, including on new modules and their tests:
>
> **CRITICAL — pylint exit code**: pylint prints a score (`10.00/10`) even when
> convention/refactor/warning messages are present.  The **exit code** is what
> CI checks — it must be **0**.  Any message category (C/R/W/E/F) makes the
> exit code non-zero: 16=convention, 8=refactor, 4=warning, 2=error.  Always
> verify `echo $?` after running pylint; a non-zero value means CI will fail.
>
> ```bash
> # Step 1 — format (REQUIRED, same command the CI autoformat uses)
> ruff format scripts/ guiservice/
>
> # Step 2 — lint, type-check (scripts/)
> # pylint MUST exit 0 — any C/R/W message (even with 10.00/10 score) = CI failure
> cd scripts
> ruff check  gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt
> pylint --jobs=0 gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt
> mypy gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt \
>   --ignore-missing-imports
>
> # Step 3 — lint, type-check (guiservice/, run from repo root)
> ruff check  guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py
> pylint --jobs=0 --rcfile=.pylintrc guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py
> mypy guiservice/app.py guiservice/gtopt_gui.py guiservice/gtopt_guisrv.py --ignore-missing-imports
> ```

```bash
# ---- guiservice ----
pip install ruff pylint mypy types-requests
pip install -r guiservice/requirements.txt

# ---- scripts sub-package (self-contained) ----
pip install -e scripts/      # production install (editable)
pip install -e "scripts/[dev]"  # with dev/test tools
pip install -r scripts/requirements.txt       # runtime deps only
pip install -r scripts/requirements-dev.txt   # dev+test deps

# Format scripts/ (in-place)
cd scripts
ruff format gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt

# Lint scripts/ with ruff
ruff check gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt

# Lint scripts/ with pylint — exit code MUST be 0 (no messages of any category).
# NOTE: pylint prints "10.00/10" even when convention/refactor messages exist.
# The exit code is the authoritative pass/fail signal (0=clean, non-zero=fail).
pylint --jobs=0 gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt

# Type-check scripts/
mypy gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor igtopt plp2gtopt pp2gtopt gtopt_monitor ts2gtopt \
  --ignore-missing-imports

# Run all script tests (from scripts/ directory)
python -m pytest

# Run with coverage
python -m pytest \
  --cov=cvs2parquet --cov=igtopt --cov=plp2gtopt --cov=pp2gtopt --cov=ts2gtopt \
  --cov-report=term-missing
```

---

## Python Scripts Sub-Package (`scripts/`)

The command-line programs (`gtopt_compare`, `cvs2parquet`, `gtopt_diagram`,
`igtopt`, `plp2gtopt`, `pp2gtopt`, `ts2gtopt`) live under `scripts/` as a
**self-contained Python package** with its own `pyproject.toml`,
`requirements.txt`, and `requirements-dev.txt`.  They are **independent of the
repository root** `pyproject.toml`.

### Directory layout

```
scripts/
├── pyproject.toml          ← package declaration + tool config (ruff/pylint/mypy/pytest)
├── requirements.txt        ← runtime: numpy, pandas, pyarrow, openpyxl, pandapower, …
├── requirements-dev.txt    ← dev+test: -r requirements.txt + pytest, pylint, ruff, …
├── CMakeLists.txt          ← CMake targets (see below)
├── gtopt_compare/     ← pandapower ↔ gtopt comparison tool
├── cvs2parquet/            ← CSV → Parquet converter
│   ├── __init__.py
│   ├── cvs2parquet.py      ← main() entry point
│   └── tests/
├── gtopt_diagram.py        ← single-file diagram generator
├── igtopt/                 ← Excel → gtopt JSON converter
│   ├── __init__.py
│   ├── igtopt.py           ← main() entry point
│   └── tests/
├── plp2gtopt/              ← PLP → gtopt JSON converter
│   ├── __init__.py
│   ├── main.py             ← CLI entry point (calls convert_plp_case)
│   ├── plp2gtopt.py        ← convert_plp_case() orchestrator
│   ├── plp_parser.py       ← PLPParser (parses all .dat files)
│   ├── *_parser.py         ← individual file parsers
│   ├── *_writer.py         ← individual JSON/Parquet writers
│   └── tests/
├── pp2gtopt/               ← pandapower → gtopt JSON converter
│   └── tests/
├── ts2gtopt/               ← time-series → gtopt horizon converter
│   └── tests/
└── cases/                  ← sample input cases for integration tests
    ├── plp_dat_ex/         ← full PLP example (all .dat files)
    ├── plp_min_1bus/       ← minimal 1-bus PLP case
    ├── plp_min_bess/       ← minimal BESS case (plpbess.dat + plpess.dat)
    ├── igtopt_c0/          ← Excel workbook for igtopt integration test
    └── json_c0/            ← reference JSON output for igtopt
```

### Install from repo root

```bash
# Production (registers all scripts on PATH)
pip install ./scripts

# Editable (changes to source take effect immediately)
pip install -e ./scripts

# With dev/test dependencies
pip install -e "./scripts[dev]"

# Runtime deps only (no pip install needed)
pip install -r scripts/requirements.txt
```

### CMake targets (via `cmake -S scripts -B build-scripts`)

| Target | Command | CTest? |
|--------|---------|--------|
| `scripts-pip-requirements` | `pip install -r requirements.txt` | — |
| `scripts-install` | `pip install -e scripts/[dev]` | — |
| `scripts-format` | `ruff format` in-place | — |
| `scripts-check-format` | `ruff format --check` | ✓ |
| `scripts-lint` | `pylint` all packages | ✓ |
| `scripts-ruff` | `ruff check` | ✓ |
| `scripts-mypy` | `mypy … --ignore-missing-imports` | ✓ |
| `scripts-test` | `pytest` (unit tests only) | ✓ |
| `scripts-test-integration` | `pytest -m integration` | ✓ |
| `scripts-coverage` | `pytest --cov … --cov-report=html` | — |

System-wide install alongside the `gtopt` binary:

```bash
cmake -S scripts -B build-scripts
cmake --install build-scripts --prefix /usr/local
# → installs all scripts into /usr/local/bin/
```

### Running tests quickly (no CMake needed)

```bash
cd scripts

# All unit tests (fast)
python -m pytest -q

# Single package
python -m pytest cvs2parquet/tests -q
python -m pytest igtopt/tests -q
python -m pytest plp2gtopt/tests -q

# Integration tests only (marked; uses cases/ fixtures)
python -m pytest -m integration -q

# With coverage + missing-line report
python -m pytest \
  --cov=cvs2parquet --cov=igtopt --cov=plp2gtopt --cov=pp2gtopt --cov=ts2gtopt \
  --cov-report=term-missing -q

# Run a single test by name
python -m pytest -k "test_parse_single_bess" -q
```

### Coverage baseline (202/202 tests · 83.4% total)

| Module | Coverage | Priority |
|--------|---------|---------|
| `bus_writer`, `extrac_*` | 100% | — |
| `bess_writer`, `block_writer`, `line_writer` | 94–98% | — |
| `central_parser`, `gtopt_writer`, `junction_writer` | 91–96% | — |
| `mance_writer`, `manem_writer`, `manli_writer` | 66–74% | Medium |
| `base_writer` | 53% | Medium |
| `generator_profile_writer` | 57% | Medium |
| `cvs2parquet/cvs2parquet.py` | 64% | High – CLI `main()` untested |
| `igtopt/igtopt.py` | 69% | High – CLI `main()` untested |
| `plp2gtopt/main.py` | 37% | High – CLI `main()` untested |
| `maness_parser` | 88% | — (improved from 22%) |

`fail_under = 83` is enforced in `scripts/pyproject.toml`; `pytest --cov` will
fail if total coverage drops below that threshold.

### Key facts for plp2gtopt

- `PLPParser` in `plp_parser.py` orchestrates all individual `*_parser.py`
  files; call `parser.parse_all()` in tests.
- BESS and ESS are **mutually exclusive** (`elif` in `bess_writer._all_entries()`):
  when both parsers are provided, only BESS entries are used.
- BAT central UIDs come from `plpcnfce.dat` `number` field, **not** from
  `plpbess.dat`.  In `plp_min_bess/`, `BESS1` central has `number=2`, so
  `bat["uid"] == 2` and the charge demand column is `uid:10002`
  (`BESS_UID_OFFSET + 2`).
- `convert_plp_case(options)` in `plp2gtopt.py` is the top-level function used
  in both the CLI (`main.py`) and integration tests.
- Integration test fixtures live in `scripts/cases/`; use `_make_opts()` helper
  from `test_integration.py` to point them at a `tmp_path`.

#### PLP file formats and Fortran field order

> **When analysing any PLP file format**, always consult the authoritative
> Fortran source in the PLP storage repository:
>
> **https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src**
>
> Each `.dat` file has a corresponding `lee*.f` reader subroutine (e.g.
> `leefilemb.f` reads `plpfilemb.dat`, `leemanem.f` reads `plpmanem.dat`).
> The Fortran `READ` statements are the source of truth for field order, unit
> conventions, and any special handling.  Use `web_fetch` with the raw URL
> `https://raw.githubusercontent.com/marcelomatus/plp_storage/main/CEN65/src/<file>.f`
> to retrieve any `.f` file whenever you need to implement or verify a PLP
> parser.

All parsers match the PLP Fortran READ statements (source of truth at
https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src).

| PLP file | Fortran subroutine | Fields per data line |
|----------|-------------------|---------------------|
| `plpess.dat` | `LeeEss` (genpdess.f) | `CenNombre nd nc mloss Emax DCMax [DCMod] [CenCarga]` |
| `plpcenbat.dat` | `LeeCenBat` (genpdbaterias.f) | `BatInd BatNom`, `NIny`, `NomBatIny FPC` ×N, `BatBar FPD BatEMin BatEMax` |
| `plpmanbat.dat` | `LeeManBat` (genpdbaterias.f) | `IBind EMin EMax` (3 fields per block) |
| `plpmaness.dat` | `LeeManEss` (genpdess.f) | `IBind Emin Emax DCMin DCMax [DCMod]` (5-6 fields per block) |
| `plpfilemb.dat` | `LeeFilEmb` (leefilemb.f) | Per filtration: `'NomEmb'`, `FiltProm`, `FiltNTramo`, then `Ind Vol_Mm3 Slope Const` ×N, then `'NomCen'`; Vol×1000→dam³, Slope/1000→/dam³ |

**Important**: In `plpess.dat` the Fortran READ order is `nd, nc` (discharge
efficiency first, then charge efficiency).  Some PLP data file comments label
the columns as "nc nd" but the Fortran reads nd first.

#### DCMod mapping

| DCMod | CenCarga | gtopt mapping |
|-------|----------|---------------|
| 0 | — | Standalone Battery (no coupling) |
| 1 | solar/wind | Battery with `source_generator` → internal bus topology |
| 2 | hydro (serie/pasada/embalse) | **Reservoir** attached to paired generator's junction (not Battery) |

DCMod=2 entries (regulation tanks, typically nc=nd=1.0) are excluded from
`battery_array` and mapped to hydro Reservoir elements.  The paired hydro
generator must have a junction and turbine; `plp2gtopt` validates this and
raises `ValueError` if the paired central is not a hydro type.

#### Maintenance schedule mapping to gtopt

| PLP file | Fortran modifies | gtopt mapping |
|----------|-----------------|---------------|
| `plpmanbat.dat` | `BatEMin(IBat,IBind)`, `BatEMax(IBat,IBind)` | Battery `emin`/`emax` schedules (absolute energy values) |
| `plpmaness.dat` | `Ess%Emin`, `Ess%Emax` | Battery `emin`/`emax` schedules |
| `plpmaness.dat` | `Ess%DCMin`, `Ess%DCMax` | Generator `pmax` + Demand `lmax` schedules |
| `plpfilemb.dat` | `FiltParam(PFiltPend)`, `FiltParam(PFiltConst)` | `Filtration.segments` (slope/constant LP coefficients updated via `FiltrationLP::update_lp`) |

When maintenance is present, the battery_writer:
- Writes `Battery/emin.parquet` and `Battery/emax.parquet` with per-block
  absolute energy values (applies to both plpmanbat and plpmaness).
- For plpmaness.dat only (which has DCMin/DCMax): also writes
  `Generator/pmax.parquet` and `Demand/lmax.parquet` for the discharge
  generator and charge demand paths.

### Key facts for igtopt

- `_run(args)` in `igtopt/igtopt.py` is the testable core; `main()` is only the
  argparse wrapper.  Pass an `argparse.Namespace` object directly in unit tests:
  ```python
  from igtopt.igtopt import _run as _igtopt_run
  args = argparse.Namespace(filenames=[str(xlsx)], json_file=..., ...)
  rc = _igtopt_run(args)
  ```
- Sheets starting with `.` are silently skipped; sheets with `@` (e.g.
  `Demand@lmax`) are written as time-series Parquet files to `input_directory`.
- Global `json_indent` / `json_separators` are switched by `--pretty`; do **not**
  call `main()` twice in the same process without resetting them.

### Key facts for cvs2parquet

- `csv_to_parquet(csv_path, parquet_path, use_schema, verbose)` is the core
  function; `main()` is only the argparse wrapper.
- Columns named `stage`, `block`, or `scenario` are cast to `int32`; all others
  to `float64`.
- `--schema` uses an explicit PyArrow schema; default uses pandas dtype casting
  (both produce identical output).

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
    └── actions/          # Composite actions:
        ├── install-apt-deps/        # Installs COIN-OR, Boost, spdlog, Arrow/Parquet
        ├── install-clang/           # Installs Clang/LLVM from apt.llvm.org
        ├── install-gcc/             # Registers GCC unversioned alternatives
        └── setup-python-uv/        # Python 3.12 + uv cache setup
```

### Key headers to understand first

| Header | Purpose |
|--------|---------|
| `basic_types.hpp` | `Uid`, `Name`, `Real`, `OptReal`, strong types |
| `utils.hpp` | `merge`, `enumerate`, `enumerate_active`, `to_vector`, optional helpers |
| `field_sched.hpp` | `FieldSched<Type,Vec>` variant (scalar / vector / filename); `FileSched` alias |
| `schedule.hpp` | `Schedule<T,Uid…>` and `OptSchedule<T,Uid…>` – typed data accessors |
| `array_index_traits.hpp` | `csv_read_table`, `parquet_read_table`, `try_read_table`, `build_table_path`, `ArrayIndexBase` |
| `arrow_types.hpp` | `ArrowTable`, `ArrowArray`, `ArrowTraits<T>`, `cast_to_int32_array`, `cast_to_double_array` |
| `input_context.hpp` | `InputContext` – wires `SystemContext` to the Arrow/CSV table cache |
| `output_context.hpp` | `OutputContext` – writes LP solution arrays to Parquet/CSV files |
| `planning_options_lp.hpp` | `PlanningOptionsLP` – typed accessors with defaults for every option field |
| `linear_problem.hpp` | `LinearProblem`, `SparseCol`, `SparseRow`, `FlatLinearProblem` |
| `system.hpp` | Top-level power system model |
| `planning.hpp` | Multi-stage planning problem |
| `planning_lp.hpp` | `PlanningLP` – assembles and solves the full LP |
| `simulation_lp.hpp` | LP formulation of a single simulation |
| `system_context.hpp` | `SystemContext` – central LP context (costs, labels, element access) |
| `gtopt_main.hpp` | `MainOptions` struct + `gtopt_main(const MainOptions&)` entry point |
| `main_options.hpp` | `parse_main_options(vm, files)` → `MainOptions`; `apply_cli_options` |
| `cli_options.hpp` | Lightweight custom CLI parser (`gtopt::cli` namespace); mirrors boost::program_options surface |
| `work_pool.hpp` | Adaptive thread pool with CPU-load–aware scheduling |
| `cpu_monitor.hpp` | Real-time CPU usage monitoring via `/proc/stat` |

---

## CI Workflows Summary

| Workflow | Trigger | What it does |
|----------|---------|--------------|
| `ubuntu.yml` | push/PR to main | Build (Clang 21), unit + e2e tests, optional coverage; uploads **`gtopt-binary-debug`** artifact (7-day retention) |
| `ubuntu.yml` (clang-tidy job) | `workflow_dispatch` with `run_clang_tidy=true` | Full clang-tidy static analysis |
| `style.yml` | every push/PR | clang-format + ruff format checks (non-blocking, warning only) |
| `autoformat.yml` | push to non-main branches | Auto-applies clang-format + ruff format, commits fixup |
| `scripts.yml` | push/PR when `scripts/**` changes | Python lint (ruff+pylint+mypy), unit tests, integration tests, cmake install |
| `guiservice.yml` | push/PR when `guiservice/**` changes | Python lint (ruff+pylint+mypy), unit tests, cmake install |
| `webservice.yml` | push/PR to main | Next.js build + integration + e2e tests |

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
  `./build/test/gtoptTests -tc="test name pattern"`.
- **No `spdlog` format strings in hot paths**: `spdlog` is configured with
  `SPDLOG_USE_STD_FORMAT=1`, so it uses `std::format` syntax.
- **`gtopt_main()` takes a single `MainOptions` struct** (not 15 positional
  args). Use designated initializers — only set the fields you need:
  ```cpp
  auto result = gtopt_main(MainOptions{
      .planning_files   = {"my_case"},
      .output_directory = "/tmp/output",
      .use_single_bus   = true,
      .print_stats      = true,
  });
  ```
- **`parse_main_options(vm, files)`** in `main_options.hpp` builds a
  `MainOptions` from a parsed CLI `variables_map` — use in `main()` wrappers.
  The `variables_map` type lives in `namespace gtopt::cli` (a custom
  lightweight parser, not boost::program_options).
- **`--stats` / `-S` CLI flag**: when passed to the `gtopt` binary it logs
  pre-solve system statistics (bus/gen/line counts, key option values) and
  post-solve results (unscaled objective, LP dimensions, solve time).
- **`Planning::merge()` when loading from multiple JSON files**.
  When loading from a single JSON file, direct `from_json<Planning>()` now
  works correctly because `SimulationLP` falls back to a single default
  `Phase{}`/`Scene{}` when `phase_array`/`scene_array` are empty.
  Use the merge pattern only when accumulating content from multiple JSON files:
  ```cpp
  Planning base;
  base.merge(daw::json::from_json<Planning>(json_str));
  PlanningLP plp(base, ...);   // works whether or not JSON has phase_array/scene_array
  ```
  Direct `from_json<Planning>(json_str)` also works for single-file loads:
  ```cpp
  Planning p = daw::json::from_json<Planning>(json_str);
  PlanningLP plp(p, ...);  // SimulationLP adds default Phase{}/Scene{} if needed
  ```
- **Diagnostics on failure**: `gtopt_main` logs the full `SolverOptions` used
  (algorithm, threads, tolerances) when the solver does not find an optimal
  solution, and includes filename + position for JSON parse errors.
- **LP Fingerprint**: structural integrity verification for LP formulations.
  Captures a sorted, deduplicated set of `(class, variable, context_type)`
  triples and their SHA-256 hash.  Enable with `--set options.lp_fingerprint=true`.
  See `docs/lp-fingerprint.md` for details and `scripts/gtopt_lp_fingerprint/`
  for the external verification tool.

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
| `use_line_losses` | `true` (default) | Model resistive line losses |
| `demand_fail_cost` | e.g. 1000 | Value-of-lost-load penalty for unserved energy ($/MWh) |
| `reserve_fail_cost` | e.g. 5000 | Penalty for unserved spinning-reserve requirement ($/MWh) |
| `scale_objective` | e.g. 1000 | Divides all objective coefficients; improves solver numerics |
| `input_directory` | `"input"` (default) | Root directory for Parquet/CSV input files |
| `input_format` | `"parquet"` (default) | Preferred input format; falls back to the other format |
| `output_directory` | `"output"` (default) | Root directory for solution output files |
| `output_format` | `"parquet"` (default) | Output file format (`"parquet"` or `"csv"`) |
| `output_compression` | `"zstd"` (default) | Parquet/CSV compression codec (`"zstd"`, `"gzip"`, `"lzo"`, `"uncompressed"`) |

### Simulation-Level Fields

These fields belong in the `simulation` section (not `options`):

| Field | Value | Meaning |
|-------|-------|---------|
| `annual_discount_rate` | e.g. 0.1 | Yearly discount rate for investment costs (10 %) |
| `boundary_cuts_file` | path | CSV file with boundary (future-cost) cuts |
| `boundary_cuts_valuation` | `"end_of_horizon"` (default) | Valuation mode: `"end_of_horizon"` or `"present_value"` |

> For backward compatibility, `annual_discount_rate` is also accepted
> in `options`, and `boundary_cuts_file` in `sddp_options` or
> `monolithic_options`.

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

7. **Multi-stage expansion**: set `annual_discount_rate` (in `simulation`)
   and `annual_capcost` in consistent units. Annual cost × number of years
   ≈ total capital recovery.

8. **Reserve modeling**: add a `reserve_zone_array` with `urreq`/`drreq` and
   link generators via `reserve_provision_array`. Each provision specifies
   `urmax` (max up-reserve MW) and cost `urcost`.

---

## Domain Glossary

| Term | Meaning in gtopt |
|------|-----------------|
| **GTEP** | Generation and Transmission Expansion Planning — the class of optimization problem gtopt solves |
| **OPF** | Optimal Power Flow — finding the least-cost dispatch of generators satisfying Kirchhoff's laws |
| **DC OPF** | Linearized (DC) approximation of AC OPF: ignores reactive power and voltage magnitudes, keeps real power and angles |
| **Kirchhoff (KVL)** | Kirchhoff's Voltage Law: `flow = Δθ / reactance` for each branch |
| **LMP** | Locational Marginal Price — the shadow price (dual variable) of the bus balance constraint; represents the marginal cost of delivering 1 MWh to that node |
| **Block** | Smallest time unit (one operating period, typically 1 h). `energy = power × duration`. |
| **Stage** | Investment period grouping consecutive blocks; capacity decisions made at stage level persist to later stages |
| **Scenario** | One possible realization of uncertain inputs (demand, hydrology); weighted by `probability_factor` |
| **CAPEX** | Capital expenditure — annualized investment cost for new capacity |
| **OPEX** | Operating expenditure — variable generation cost per MWh |
| **`expcap`** | Capacity per expansion module (MW/module) |
| **`expmod`** | Maximum modules the solver may build |
| **`annual_capcost`** | Annualized investment cost per module ($/year) |
| **`gcost`** | Variable generation cost ($/MWh) |
| **`demand_fail_cost`** | Value-of-lost-load penalty for unserved energy ($/MWh); should exceed the most expensive generator cost |
| **`scale_objective`** | Divides all objective coefficients to keep them in [0.001, 1000] range for solver numerical stability |
| **`pmin` / `pmax`** | Minimum / maximum generator output (MW) |
| **`tmax_ab` / `tmax_ba`** | Maximum power flow in each direction on a transmission line (MW) |
| **`reactance`** | Line reactance (per-unit Ω); used with Kirchhoff constraints |
| **Copper plate** | `use_single_bus=true` mode; no network constraints; total generation = total demand |
| **flat_map** | `gtopt::flat_map` alias (backed by `boost::container::flat_map`; or `std::flat_map` in C++23 when available) — used instead of `std::map` for 10–27× faster iteration in LP assembly |
| **`FieldSched<T>`** | `std::variant<T, std::vector<T>, FileSched>` — holds a scalar, an inline vector, or a filename pointing to an Arrow/Parquet table |
| **`FileSched`** | `std::string` alias used as the filename arm of `FieldSched`; triggers Arrow I/O at construction time |
| **`PlanningOptionsLP`** | Wrapper around `PlanningOptions` providing typed accessors with compile-time defaults (e.g. `input_format()` → `"parquet"`, `output_format()` → `"parquet"`). Backward-compat alias: `OptionsLP` |

---

## Comparable Tools

Understanding the ecosystem helps when designing features and writing tests.

| Tool | Language | Scope | Key difference from gtopt |
|------|----------|-------|---------------------------|
| [PyPSA](https://github.com/PyPSA/PyPSA) | Python | Multi-carrier energy system (electricity, gas, heat) | Sector-coupling, unit commitment, storage; Python-only; no C++ performance core |
| [pandapower](https://github.com/e2nIEE/pandapower) | Python | Power flow, short-circuit, state estimation | Primarily AC power flow; no capacity expansion planning |
| [GenX](https://github.com/GenXProject/GenX) | Julia | Electricity capacity expansion | Julia; no transmission Kirchhoff (transport model default); large US-scale models |
| [QuESt Planning](https://github.com/sandialabs/quest_planning) | Python | Long-term capacity expansion with storage | Python; broad storage technology support; Sandia NL tool |
| [PLEXOS](https://www.energyexemplar.com) | Commercial | Unit commitment + expansion planning | Commercial; much larger scope; gtopt targets open-source, scriptable workflows |
| [PROMAX / PLP](https://en.wikipedia.org/wiki/PLEXOS) | Commercial | Hydrothermal scheduling (Latin America) | gtopt includes a `plp2gtopt` converter in `scripts/` for importing PLP cases |

**What makes gtopt distinctive**:
- High-performance C++26 LP assembly with `flat_map`-based sparse matrices
- Native Parquet I/O (default) and CSV fallback for large time-series via Apache Arrow
- Hydro cascade modeling (Junction/Waterway/Reservoir/Turbine/Flow/Filtration)
- REST API (Next.js webservice) + browser GUI (Flask guiservice) out of the box
- IEEE test cases embedded as in-memory JSON unit tests (no file I/O required)
- Multi-stage, multi-scenario expansion with per-stage discount factors
- Adaptive thread pool (`work_pool.hpp`) with CPU-load–aware scheduling
- Lightweight custom CLI parser (no boost::program_options dependency)

---

## Documentation Style Guide

When creating or updating documentation files, follow these conventions to
maintain consistency across the project.

### General Principles

- All documentation is **GitHub-Flavored Markdown** (GFM).
- Use ATX-style headers (`#`, `##`, `###`) with a blank line before and after.
- Keep lines under 80 characters where practical (tables and URLs may exceed).
- Use `code backticks` for file names, CLI flags, JSON fields, C++ identifiers.
- Use **bold** for emphasis on key terms; *italics* for new-term introductions.

### Mathematical Formulation Document

The canonical formulation is `docs/formulation/mathematical-formulation.md`.

- Use LaTeX display math (`$$...$$`) for equations and inline math (`$...$`)
  for symbols in text.
- Map every mathematical symbol to its JSON field name in the Parameters table
  (§2) and JSON Mapping table (§7).
- When adding new constraints or variables, update the Compact Formulation
  summary (§3), the detailed component section (§5), and the JSON mapping (§7).
- Academic references use numbered anchors: `<a id="refN"></a>` with inline
  citations as `[[N]](#refN)`. Always include DOI links.
- Group references by category (FESOP/gtopt, TEP, DC OPF, tools, solvers).

### Cross-References Between Documents

Every major doc should have a "See also" section at the bottom linking to
related documents. The current cross-reference structure:

| Document | Links to |
|----------|----------|
| `README.md` | All documents (hub) |
| `docs/planning-guide.md` | INPUT_DATA, USAGE, SCRIPTS, BUILDING, DIAGRAM_TOOL, MATH_FORMULATION |
| `docs/formulation/mathematical-formulation.md` | PLANNING_GUIDE, INPUT_DATA, USAGE, CONTRIBUTING, BUILDING, SCRIPTS |
| `docs/usage.md` | MATH_FORMULATION, PLANNING_GUIDE, INPUT_DATA, SCRIPTS |
| `docs/input-data.md` | MATH_FORMULATION, PLANNING_GUIDE, USAGE, SCRIPTS |

Use relative paths: `[docs/planning-guide.md](docs/planning-guide.md)` from root,
`[Planning Guide](../../docs/planning-guide.md)` from `docs/formulation/`.

### Formulation Validation (verified 2026-03)

The mathematical formulation has been validated against the C++ implementation
(`source/*_lp.cpp`, `include/gtopt/*_lp.hpp`). Key verified components:

| Component | File | Formula |
|-----------|------|---------|
| DC power flow | `line_lp.cpp` | $f = (V^2/X)(\theta_a - \theta_b)$ with `scale_theta` scaling |
| Battery SoC | `storage_lp.hpp` | $e[b] = e[b{-}1](1{-}\mu\Delta) + p_{in}\eta_{in}\Delta - p_{out}\Delta/\eta_{out}$ |
| Bus balance | `generator_lp.cpp`, `demand_lp.cpp` | $(1{-}\lambda_g)p_g - (1{+}\lambda_d)\ell_d + \text{flows} = 0$ |
| Discount factor | `utils.hpp` | $\delta_t = (1+r)^{-\tau_t/8760}$ |
| Capacity expansion | `capacity_object_lp.cpp` | $C_t = C_{t-1}(1{-}\xi) + M \cdot m_t + \Delta C$ |

When modifying the C++ LP assembly code, update the corresponding section
in `docs/formulation/mathematical-formulation.md` to maintain consistency.

### Documentation File Purposes

| File | Purpose | Audience |
|------|---------|----------|
| `README.md` | Quick start, feature overview | New users |
| `BUILDING.md` | Build instructions, dependencies | Developers |
| `docs/usage.md` | CLI reference, output interpretation | Users |
| `docs/input-data.md` | JSON/Parquet input format spec | Case builders |
| `docs/planning-guide.md` | Worked examples, concepts | Planners |
| `docs/scripts-guide.md` | Python tool overview | Script users |
| `CONTRIBUTING.md` | Code style, testing, CI | Contributors |
| `docs/formulation/mathematical-formulation.md` | LP/MIP formulation, references | Researchers |
| `docs/tools/diagram.md` | Network diagram tool | Visualization |
| `CLAUDE.md` | AI agent guidance | Claude Code |
| `.github/copilot-instructions.md` | AI agent guidance | GitHub Copilot |

### Key Academic References

The following references are cited in the mathematical formulation and should
be consulted when validating or extending the formulation:

- **FESOP**: Buitrago Villada et al. (2022), IEEE KPEC — foundational FESOP paper
- **Hydrothermal**: Pereira-Bonvallet, Matus et al. (2016), Energy Procedia
- **TEP models**: Romero & Monticelli (2002), IEE Proc.; Lumbreras & Ramos (2016)
- **DC OPF**: Stott, Jardim & Alsaç (2009), IEEE Trans. Power Systems
- **IEEE test cases**: Anderson & Fouad (2002), *Power Systems Control and Stability*
- **Similar tools**: PyPSA — Brown et al. (2018); GenX — Jenkins & Sepulveda (2017)
- **Solver**: COIN-OR CBC — Forrest & Lougee-Heimer (2005)

Full citations with DOIs are in
`docs/formulation/mathematical-formulation.md` §9.
