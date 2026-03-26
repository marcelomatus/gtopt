# Building gtopt from Source

This guide provides detailed instructions for building gtopt from source, including dependency installation, build configuration, and verification steps.

## Table of Contents

- [Dependencies](#dependencies)
- [Installing Dependencies](#installing-dependencies)
  - [Ubuntu/Debian](#ubuntudebian)
  - [macOS](#macos)
  - [Other Linux Distributions](#other-linux-distributions)
- [Building Everything (Unified)](#building-everything-unified)
- [Building the Standalone Binary](#building-the-standalone-binary)
- [Installing](#installing)
- [Building and Running Tests](#building-and-running-tests)
- [Formatting and Linting](#formatting-and-linting)
- [Troubleshooting](#troubleshooting)

## Dependencies

| Dependency | Minimum Version | Purpose | Ubuntu Package |
|-----------|----------------|---------|----------------|
| GCC | 14+ | C++26 compiler (CI fallback) | `gcc-14 g++-14` |
| Clang | 21+ | C++26 compiler (CI primary) | see install script below |
| CMake | 3.31+ | Build system | `cmake` (or [cmake.org](https://cmake.org/download/)) |
| Boost | 1.70+ | Container library | `libboost-container-dev` |
| Apache Arrow | 10.0+ | Parquet I/O | `libarrow-dev libparquet-dev` or conda |
| COIN-OR CBC/CLP | 2.10+ | LP/MIP solver | `coinor-libcbc-dev` |
| HiGHS | 1.5+ | LP/MIP solver (optional) | build from source (see below) |
| spdlog | 1.12+ | Logging | `libspdlog-dev` |

**LP Solver Backends**: gtopt loads LP solver backends as dynamic plugins at
runtime. The default is auto-detected by priority: CPLEX > HiGHS > CBC > CLP.
Installing `coinor-libcbc-dev` provides CLP/CBC. HiGHS must be built from
source on Ubuntu 24.04 (it is available via apt starting with Ubuntu 25.04).
Use `--lp-solvers` to list available backends, or `--lp-solver
highs` to select a specific one. Set `GTOPT_PLUGIN_DIR` to point to a custom
plugin directory.

## Installing Dependencies

### Ubuntu/Debian

#### GCC 14

GCC 14 is required for C++26 support and is the verified CI fallback compiler:

```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
```

Verify:

```bash
gcc-14 --version   # Ubuntu 14.2.0-4ubuntu2~24.04 or newer
g++-14 --version
```

#### Clang 21 (preferred CI compiler)

Clang 21 is the primary compiler used in CI. Install via the official LLVM
script:

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

Verify:

```bash
clang++ --version   # clang version 21.x.x

#### CMake 3.31+

Ubuntu 22.04 and earlier ship with older CMake versions. If `cmake --version` shows < 3.31, install from the official site:

```bash
# Download and install the latest CMake
wget https://github.com/Kitware/CMake/releases/download/v3.31.2/cmake-3.31.2-linux-x86_64.sh
chmod +x cmake-3.31.2-linux-x86_64.sh
sudo ./cmake-3.31.2-linux-x86_64.sh --prefix=/usr/local --skip-license
```

Or use the CMake PPA for Ubuntu:

```bash
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
# Note: Official CMake builds may still be needed for 3.31+
```

#### Boost

```bash
sudo apt-get install -y libboost-container-dev
```

Verify:

```bash
dpkg -l | grep libboost-container
```

#### Apache Arrow / Parquet

Apache Arrow provides high-performance Parquet I/O. **Two installation routes
are available** — use whichever fits your environment.

##### Route A: APT (official Apache Arrow repo — recommended when network allows)

```bash
sudo apt-get install -y ca-certificates lsb-release wget
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb"
sudo apt-get install -y -V "./apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb"
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

Verify:

```bash
pkg-config --modversion arrow
pkg-config --modversion parquet
```

##### Route B: Conda (verified fallback — no extra APT repo needed)

If the APT route fails (e.g., network-restricted CI runners or non-Ubuntu
distros), install via conda. This was verified on Ubuntu 24.04 with
`conda 26.1.0` and produces Arrow 12.0.0:

```bash
# Install into the active/base conda environment
conda install -y -c conda-forge arrow-cpp parquet-cpp

# Find the conda prefix (works even outside an activated environment)
CONDA_BASE=$(conda info --base)   # e.g. /usr/share/miniconda
echo "Arrow cmake: $(find ${CONDA_BASE}/lib/cmake/Arrow -name ArrowConfig.cmake)"
```

When building, pass the conda prefix to CMake so it can locate
`ArrowConfig.cmake`:

```bash
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"
```

> **Important**: Use `conda info --base` (not `$CONDA_PREFIX`) because
> `$CONDA_PREFIX` is only set inside an activated conda environment.

#### CBC Solver

CBC (COIN-OR Branch-and-Cut) is a free, open-source MILP solver:

```bash
sudo apt-get install -y coinor-libcbc-dev
```

Verify:

```bash
pkg-config --modversion cbc
```

#### HiGHS Solver (optional)

HiGHS is a high-performance open-source LP/MIP solver. It is **not
packaged** in Ubuntu 24.04 (Noble); apt packages (`libhighs-dev`,
`libhighs1`) are available starting with Ubuntu 25.04 (Plucky).

On Ubuntu 25.04+:

```bash
sudo apt-get install -y libhighs-dev
```

On Ubuntu 24.04, build from source:

```bash
git clone --depth 1 --branch v1.10.0 https://github.com/ERGO-Code/HiGHS.git /tmp/HiGHS
cmake -S /tmp/HiGHS -B /tmp/HiGHS/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /tmp/HiGHS/build -j$(nproc)
sudo cmake --install /tmp/HiGHS/build
sudo ldconfig
rm -rf /tmp/HiGHS
```

Verify:

```bash
ls /usr/local/include/highs/Highs.h
```

### macOS

Install dependencies using Homebrew:

```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install GCC 14
brew install gcc@14

# Install CMake
brew install cmake

# Install Boost
brew install boost

# Install Apache Arrow
brew install apache-arrow

# Install CBC solver
brew install coin-or-tools/coinor/cbc
```

When building, you may need to specify the compiler explicitly:

```bash
export CC=/opt/homebrew/bin/gcc-14
export CXX=/opt/homebrew/bin/g++-14
```

### Other Linux Distributions

#### Fedora/RHEL

```bash
sudo dnf install gcc-c++ cmake boost-devel
# For Arrow and CBC, you may need to build from source or use EPEL
```

#### Arch Linux

```bash
sudo pacman -S gcc cmake boost arrow coin-or-cbc
```

## Building Everything (Unified)

The `all/` sub-project is the single entry point that configures and installs
every gtopt component in one pass: the solver binary, Python conversion scripts,
web service, and GUI service.  It is the recommended starting point for a
complete installation.

```bash
# Configure all components
cmake -S all -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"   # if Arrow was installed via conda

# Build (C++ binary and web-service assets)
cmake --build build-all -j$(nproc)

# Install everything (defaults to ~/.local for non-root users)
cmake --install build-all
```

> **ccache**: Install `ccache` before running cmake to enable compilation
> caching. The `all/` build auto-detects ccache and enables it by default
> (`USE_CCACHE=YES`). Pass `-DUSE_CCACHE=NO` to disable. **Important**: ccache
> must be installed *before* `cmake -S all -B build-all`; CMake bakes the
> launcher path at configure time. If ccache was missing at configure time,
> delete the build directory and reconfigure.

### Component options

All components are enabled by default except documentation (which requires a
network connection).  Disable individual components by passing `-D<OPTION>=OFF`:

| CMake option | Default | Requires |
|---|---|---|
| `GTOPT_BUILD_STANDALONE` | `ON` | C++26 compiler, COIN-OR, Arrow/Parquet |
| `GTOPT_BUILD_TESTS` | `ON` | C++26 compiler, doctest (fetched via CPM) |
| `GTOPT_BUILD_SCRIPTS` | `ON` | Python ≥ 3.8, pip |
| `GTOPT_BUILD_WEBSERVICE` | `ON` | Node.js / npm (auto-skipped with warning if absent) |
| `GTOPT_BUILD_GUISERVICE` | `ON` | Python ≥ 3.10 (auto-skipped with warning if absent) |
| `GTOPT_BUILD_DOCS` | `OFF` | Doxygen + internet (m.css theme) |

Example – build only the solver binary and Python scripts:

```bash
cmake -S all -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DGTOPT_BUILD_WEBSERVICE=OFF \
  -DGTOPT_BUILD_GUISERVICE=OFF \
  -DGTOPT_BUILD_TESTS=OFF
cmake --build build-all -j$(nproc)
cmake --install build-all
```

For single-component builds, use the individual sub-project directories
(`standalone/`, `scripts/`, `webservice/`, `guiservice/`, `test/`).

## Building the Standalone Binary

Once all dependencies are installed:

```bash
# Clone the repository
git clone https://github.com/marcelomatus/gtopt.git
cd gtopt

# Configure the build
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release

# Build (using all available CPU cores)
cmake --build build -j$(nproc)

# Test the binary
./build/gtopt --version
./build/gtopt --help
```

### Build Types

- **Release**: Optimized for performance (recommended for production)
  ```bash
  cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
  ```

- **Debug**: Includes debug symbols and assertions (for development)
  ```bash
  cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Debug
  ```

- **RelWithDebInfo**: Optimized with debug symbols (for profiling)
  ```bash
  cmake -S standalone -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
  ```

### Build Options

Additional CMake options:

```bash
# Enable test coverage reporting
cmake -S standalone -B build -DENABLE_TEST_COVERAGE=ON

# Specify a custom install prefix
cmake -S standalone -B build -DCMAKE_INSTALL_PREFIX=/opt/gtopt

# Build with HiGHS support (requires HiGHS headers/lib installed)
cmake -S standalone -B build -DHIGHS_ROOT_DIR=$HOME/.local
```

LP solver backends are selected at runtime via `--lp-solver`, not at build
time. All detected solver plugins (`libgtopt_solver_*.so`) are built
automatically when their dependencies are found.

## Installing

### Non-root user install (default)

When configuring as a non-root user, `CMAKE_INSTALL_PREFIX` automatically
defaults to `~/.local` (the XDG user prefix).  No `sudo` is needed:

```bash
cmake --install build
# Binary installed to: ~/.local/bin/gtopt
```

Ensure `~/.local/bin` is in your `PATH` (most modern Linux distributions
include it automatically; add the following to `~/.bashrc` or `~/.profile`
if it is missing):

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Verify the installation:

```bash
gtopt --version
which gtopt
```

### System-wide install (root)

To install to the system path (`/usr/local/bin/gtopt`), run as root or with
`sudo`:

```bash
sudo cmake --install build
```

### Custom prefix

Override the install location with `--prefix` at install time:

```bash
cmake --install build --prefix /opt/gtopt
# Binary will be at /opt/gtopt/bin/gtopt
# Add to PATH: export PATH=/opt/gtopt/bin:$PATH
```

Or set the prefix at configure time:

```bash
cmake -S standalone -B build -DCMAKE_INSTALL_PREFIX=/opt/gtopt
cmake --install build
```

To uninstall:

```bash
# Using the uninstall CMake target (recommended — works for all install types):
cmake --build build --target uninstall

# Or manually for a standalone binary-only install:
# System-wide install
sudo rm /usr/local/bin/gtopt

# User install
rm ~/.local/bin/gtopt
```

The `uninstall` target reads `build/install_manifest.txt` that was written by
the last `cmake --install` run and removes every file it contains.  It also
reads `build/scripts/scripts_install_manifest.txt` (written by the scripts
`install(CODE ...)` step) to remove pip-installed entry-point scripts such as
`gtopt_compare`, `cvs2parquet`, etc., which are not tracked in the main
cmake manifest.  Run `cmake --install` first if the manifests do not exist yet.

To verify that end-to-end tests work *without* installing the scripts (the
recommended development workflow):

```bash
# 1. Build
cmake -S all -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# 2. Install (records install_manifest.txt and scripts_install_manifest.txt)
cmake --install build

# 3. Uninstall — removes the gtopt binary and the pip-installed scripts
cmake --build build --target uninstall

# 4. Run e2e tests — must still pass using the source-tree gtopt_compare
cd build && ctest --output-on-failure -R gtopt_compare
```

The e2e pandapower comparison tests automatically fall back to running
`gtopt_compare` directly from the `scripts/` source tree (using
`PYTHONPATH=scripts/ python -m gtopt_compare`) when the installed
entry-point is not present.

## Building and Running Tests

### Unit Tests

Build and run the unit test suite (the **primary development target** is the
`test/` sub-project, not the root `CMakeLists.txt`):

```bash
# Configure — Arrow via APT (no CMAKE_PREFIX_PATH needed):
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14

# — or — Arrow via conda (pass conda base as prefix):
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_PREFIX_PATH="$(conda info --base)"

# Build
cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure
# Expected: 100% tests passed, 0 tests failed out of 584+
```

Run tests with verbose output:

```bash
cd build && ctest --output-on-failure --verbose
```

Run a specific test by name (doctest filter syntax):

```bash
./build/gtoptTests -tc="test name pattern"
# or bracket syntax:
./build/gtoptTests "[test name]"
```

### End-to-End Integration Tests

The standalone build includes end-to-end tests that run gtopt against the sample case in `cases/c0/` and validate output against reference results:

```bash
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build
```

### Test Coverage

To generate test coverage reports:

```bash
cmake -S test -B build/test -DENABLE_TEST_COVERAGE=ON
cmake --build build/test -j$(nproc)
ctest --test-dir build/test
# Coverage report will be generated in build/test/coverage/
```

## Formatting and Linting

The project uses clang-format and clang-tidy for code formatting and linting.

Format all source files:

```bash
cmake --build build --target format
```

Or manually:

```bash
cmake --build build --target fix-format
```

Check formatting without applying changes:

```bash
cmake --build build --target check-format
```

## Troubleshooting

### "CMake version too old"

**Error**: `CMake 3.31 or higher is required. You are running version X.XX`

**Solution**: Install a newer CMake from [cmake.org](https://cmake.org/download/).

### "Could not find GCC 14"

**Error**: `No CMAKE_CXX_COMPILER could be found` or version mismatch

**Solution**: Install GCC 14 and explicitly set the compiler:

```bash
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build
```

### "Could not find Arrow" or "Could not find Parquet"

**Error**: `Could NOT find Arrow` or `Could NOT find Parquet`

**Solution A** — APT packages missing:

```bash
# Ubuntu/Debian
sudo apt-get install -y libarrow-dev libparquet-dev

# Verify
pkg-config --modversion arrow
pkg-config --modversion parquet
```

**Solution B** — Arrow installed via conda (prefix not on default path):

```bash
# Tell CMake where to find ArrowConfig.cmake
cmake -S test -B build \
  -DCMAKE_PREFIX_PATH="$(conda info --base)" \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14
```

**Solution C** — Arrow installed to a custom prefix:

```bash
export PKG_CONFIG_PATH=/path/to/arrow/lib/pkgconfig:$PKG_CONFIG_PATH
cmake -S test -B build -DCMAKE_PREFIX_PATH=/path/to/arrow
```

### "Could not find CBC"

**Error**: `Could NOT find CBC`

**Solution**: Install the CBC solver development package:

```bash
sudo apt-get install -y coinor-libcbc-dev
pkg-config --modversion cbc
```

### COIN-OR ABI Mismatch (undefined symbol errors)

**Error**: `Failed to load plugin libgtopt_solver_osi.so: undefined symbol: _ZTv0_n24_N21OsiClpSolverInterface12initialSolveEv` (or similar undefined symbol errors involving COIN-OR classes)

**Cause**: COIN-OR libraries (Osi, CoinUtils, Clp, OsiClp, Cbc, OsiCbc, Cgl)
are loaded from different installations with incompatible ABIs. For example,
CLP from a custom build at `/opt/coinor/lib` mixed with CBC from system
packages at `/lib/x86_64-linux-gnu/`. Use `ldd` to diagnose:

```bash
ldd /path/to/lib/gtopt/plugins/libgtopt_solver_osi.so | grep -E 'Osi|Clp|Cbc|Cgl|Coin'
```

If some libraries resolve to `/opt/coinor/lib` and others to system paths,
that's the conflict.

**Solution A** — Use all system packages (simplest):

```bash
sudo apt-get install -y coinor-libcbc-dev
```

Ensure `COIN_ROOT_DIR` is not set (or points to `/usr`) so CMake finds
everything from the system.

**Solution B** — Build all COIN-OR from source to a custom prefix:

```bash
cd /tmp
wget https://raw.githubusercontent.com/coin-or/coinbrew/master/coinbrew
chmod u+x coinbrew
./coinbrew fetch Cbc@master
./coinbrew build Cbc --prefix=/opt/coinor --tests=none
sudo ./coinbrew install Cbc --prefix=/opt/coinor
```

Then configure gtopt with:

```bash
cmake -S all -B build -DCOIN_ROOT_DIR=/opt/coinor ...
```

> **Note**: CMake will now detect and warn about COIN-OR ABI mismatches at
> configure time. If CBC/OsiCbc are found from a different directory than the
> core Osi/CoinUtils, CBC support is automatically disabled with a warning to
> prevent runtime crashes.

### Build Fails with Compilation Errors

**Error**: Various C++ compilation errors

**Solution**: Ensure you're using GCC 14 or later:

```bash
g++-14 --version
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build
```

Clear the build cache if switching compilers:

```bash
rm -rf build
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build
```

### Tests Fail

**Error**: Tests fail during `ctest`

**Solution**: 
1. Check that the binary runs successfully: `./build/gtopt --version`
2. Verify dependencies are correctly installed
3. Try running tests with verbose output: `ctest --test-dir build/test --verbose`
4. Check the test log files in `build/test/Testing/Temporary/`

### Installation Permission Denied

**Error**: `Permission denied` during `cmake --install`

**Cause**: The install prefix is set to a system directory (e.g.
`/usr/local`) that requires root privileges.

**Solution**: This should not happen for non-root users when configuring
with a fresh build directory, because `CMAKE_INSTALL_PREFIX` is
automatically defaulted to `~/.local`.  If it does occur:

```bash
# Option 1: install to user prefix (no sudo needed)
cmake --install build --prefix $HOME/.local
export PATH=$HOME/.local/bin:$PATH

# Option 2: system-wide install with sudo
sudo cmake --install build
```

## Next Steps

After successfully building and installing gtopt:

- Read the [Usage Guide](docs/usage.md) for detailed usage instructions
- Try the [sample case](cases/c0/) to verify your installation
- Explore the [web service](webservice/INSTALL.md) for browser-based optimization
- Check out the [GUI service](guiservice/INSTALL.md) for visual case creation
