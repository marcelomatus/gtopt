# Building gtopt from Source

This guide provides detailed instructions for building gtopt from source, including dependency installation, build configuration, and verification steps.

## Table of Contents

- [Dependencies](#dependencies)
- [Installing Dependencies](#installing-dependencies)
  - [Ubuntu/Debian](#ubuntudebian)
  - [macOS](#macos)
  - [Other Linux Distributions](#other-linux-distributions)
- [Building the Standalone Binary](#building-the-standalone-binary)
- [Installing System-wide](#installing-system-wide)
- [Building and Running Tests](#building-and-running-tests)
- [Formatting and Linting](#formatting-and-linting)
- [Troubleshooting](#troubleshooting)

## Dependencies

| Dependency | Minimum Version | Purpose | Ubuntu Package |
|-----------|----------------|---------|----------------|
| GCC | 14+ | C++26 compiler | `gcc-14 g++-14` |
| CMake | 3.31+ | Build system | `cmake` (or install from [cmake.org](https://cmake.org/download/)) |
| Boost | 1.70+ | Container library | `libboost-container-dev` |
| Apache Arrow | 10.0+ | Parquet I/O | `libarrow-dev libparquet-dev` |
| COIN-OR CBC | 2.10+ | LP/MIP solver | `coinor-libcbc-dev` |

**Alternative Solvers**: While CBC is the default open-source solver, gtopt also supports HiGHS, Clp, CPLEX, and Gurobi. To use an alternative solver, you'll need to install it separately and configure CMake accordingly.

## Installing Dependencies

### Ubuntu/Debian

#### GCC 14

GCC 14 is required for C++26 support:

```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
```

To verify the installation:

```bash
gcc-14 --version
g++-14 --version
```

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

Apache Arrow provides high-performance Parquet I/O:

```bash
sudo apt-get install -y ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

Verify:

```bash
pkg-config --modversion arrow
pkg-config --modversion parquet
```

#### CBC Solver

CBC (COIN-OR Branch-and-Cut) is a free, open-source MILP solver:

```bash
sudo apt-get install -y coinor-libcbc-dev
```

Verify:

```bash
pkg-config --modversion cbc
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

# Use a specific solver (if you have alternatives installed)
cmake -S standalone -B build -DGTOPT_SOLVER=HiGHS
```

## Installing System-wide

To install gtopt to the system path (default: `/usr/local/bin/gtopt`):

```bash
sudo cmake --install build
```

Verify the installation:

```bash
gtopt --version
which gtopt
```

To install to a custom prefix:

```bash
cmake --install build --prefix /opt/gtopt
# Binary will be at /opt/gtopt/bin/gtopt
# Add to PATH: export PATH=/opt/gtopt/bin:$PATH
```

To uninstall:

```bash
sudo cmake --build build --target uninstall
# Or manually: sudo rm /usr/local/bin/gtopt
```

## Building and Running Tests

### Unit Tests

Build and run the unit test suite:

```bash
CC=gcc-14 CXX=g++-14 cmake -S test -B build/test
cmake --build build/test -j$(nproc)
ctest --test-dir build/test
```

Run tests with verbose output:

```bash
ctest --test-dir build/test --verbose
```

Run a specific test:

```bash
./build/test/gtoptTests [test name]
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

**Solution**: Ensure Apache Arrow dev packages are installed:

```bash
# Ubuntu/Debian
sudo apt-get install -y libarrow-dev libparquet-dev

# Verify installation
pkg-config --modversion arrow
```

If installed in a non-standard location, set `PKG_CONFIG_PATH`:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### "Could not find CBC"

**Error**: `Could NOT find CBC`

**Solution**: Install the CBC solver development package:

```bash
sudo apt-get install -y coinor-libcbc-dev
pkg-config --modversion cbc
```

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

**Solution**: Use `sudo` or install to a user-writable location:

```bash
# Option 1: Use sudo
sudo cmake --install build

# Option 2: Install to user directory
cmake --install build --prefix $HOME/.local
export PATH=$HOME/.local/bin:$PATH
```

## Next Steps

After successfully building and installing gtopt:

- Read the [Usage Guide](USAGE.md) for detailed usage instructions
- Try the [sample case](cases/c0/) to verify your installation
- Explore the [web service](webservice/INSTALL.md) for browser-based optimization
- Check out the [GUI service](guiservice/INSTALL.md) for visual case creation
