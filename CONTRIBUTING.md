# Contributing to gtopt

Thank you for your interest in contributing to gtopt! This document provides
guidelines and instructions for contributing.

## Getting Started

1. Fork the repository and clone your fork
2. Follow the build instructions in [BUILDING.md](BUILDING.md)
3. Create a feature branch from `main`

## Development Setup

### Prerequisites

- **Clang 21** (preferred) or GCC 14+ for C++26 support
- CMake 3.31.6+
- Dependencies listed in [BUILDING.md](BUILDING.md)

> See [BUILDING.md](BUILDING.md) for full dependency installation instructions,
> including Apache Arrow (APT or conda) and the Clang 21 install script.

### Building and Testing

The **primary build target** is the `test/` sub-project (not the root
`CMakeLists.txt`):

```bash
# Configure and build tests (Clang 21 preferred)
cmake -S test -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run a specific test (doctest filter syntax)
./build/gtoptTests -tc="test name pattern"

# Check code formatting
cmake --build build --target check-format

# Auto-format code
cmake --build build --target format
```

Alternative with GCC 14:

```bash
CC=gcc-14 CXX=g++-14 cmake -S test -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Code Coverage

```bash
cmake -S test -B build \
  -DENABLE_TEST_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest
```

## Code Style

### C++

- **Standard**: C++26 (`CMAKE_CXX_STANDARD 26`). C++23 features are used
  throughout; adopt C++26 features as Clang 21 / GCC 14 support matures.
- **Compiler flags**: `-Wall -Wpedantic -Wextra -Werror` — all warnings are
  errors.
- **Formatting**: Enforced by `.clang-format` (80-char column limit, 2-space
  indent). The CI auto-formats on every non-main push and commits a fixup.
- **Linting**: Configured via `.clang-tidy` with extensive checks enabled.
- **Naming conventions**:
  - Classes/Structs: `PascalCase` (e.g. `LinearProblem`, `SimulationLP`)
  - Free functions and methods: `snake_case` (e.g. `add_col`, `get_optvalue`,
    `resolve`, `annual_discount_factor`)
  - Local variables and public data members: `snake_case`
  - Private class members: `m_` prefix + `_` suffix (e.g. `m_simulation_`, `m_options_`)
    (e.g. `m_simulation_`, `m_options_`)
  - Macros: `UPPER_CASE`
- **Include guards**: Use `#pragma once`
- **File headers**: Doxygen-style (`@file`, `@brief`, `@date`, `@author`,
  `@copyright`)
- **Includes**: Three sorted groups — `<std>`, external `<pkg/header>`,
  project `<gtopt/...>`
- **Error handling**: Prefer `std::optional` and return values over exceptions
- **Modern C++**: Use concepts (`requires`), ranges, `std::format`, structured
  bindings, designated initializers, `[[nodiscard]]`, `noexcept` where
  appropriate

### Python

- **Version**: Python ≥ 3.12 (CI uses 3.12)
- **Formatting**: `black` with **88-char** line length (configured in
  `pyproject.toml`)
- **Import sorting**: `isort` (profile `black`)
- **Linting**: `ruff` + `pylint`; see `.flake8` and `.pylintrc`
- **Type checking**: `mypy`

```bash
pip install -e ".[dev]"   # install dev dependencies
black . && isort .
ruff check . && pylint scripts/ guiservice/
mypy scripts/ guiservice/
pytest
```

### Pre-commit Hooks

The project uses pre-commit hooks for automated checks. Install them with:

```bash
pip install pre-commit
pre-commit install
```

Hooks include: trailing whitespace, YAML validation, Black, Flake8, Pylint,
ShellCheck, yamllint, markdownlint, and cmake-format.

> **CI auto-format**: The `autoformat.yml` workflow runs `clang-format` on
> every push to non-main branches and commits a fixup automatically. Local
> violations are therefore non-blocking, but running `cmake --build build
> --target format` before pushing keeps the history clean.

## Pull Request Process

1. Ensure your changes build and all tests pass
2. Run `cmake --build build --target check-format` to verify C++ formatting
3. Add tests for new functionality in `test/source/test_<topic>.cpp`
4. Update documentation if your changes affect user-facing behavior
5. Write clear, descriptive commit messages
6. Open a pull request against `main` with a description of your changes

## Testing Guidelines

- **Framework**: [doctest](https://github.com/doctest/doctest) 2.4.12 for C++
  tests
- Place test files in `test/source/` (or `test/source/json/` for JSON tests);
  the `CONFIGURE_DEPENDS` glob picks them up automatically
- Use `REQUIRE` for fatal assertions and `CHECK` for non-fatal ones
- Use `doctest::Approx` for all floating-point comparisons
- Name test cases and sub-cases descriptively
- Add `// NOLINT(...)` inline suppressions for intentional clang-tidy
  false-positives (e.g. `use-after-move`, `unchecked-optional-access`)
- Do **not** add `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (already in
  `test/source/main.cpp`)

See `.github/copilot-instructions.md` for the complete test template and rules.

## Project Structure

```
gtopt/
├── include/gtopt/        # Public C++ headers (library API)
├── source/               # C++ implementation files
├── test/source/          # C++ unit tests (doctest)
│   └── json/             # JSON-specific tests
├── standalone/source/    # main() for the gtopt binary
├── integration_test/     # CMake helpers for e2e tests
├── cases/                # Sample optimization cases (JSON + Parquet)
├── webservice/           # Next.js web UI + REST API
├── guiservice/           # Python/Flask GUI service
├── scripts/              # Python conversion utilities
├── cmake/                # Upstream CMake modules (CPM, tools)
├── cmake_local/          # Project-specific CMake modules
└── documentation/        # Doxygen setup
```

## Domain Context

**gtopt** solves **Generation and Transmission Expansion Planning (GTEP)**
problems — minimizing total discounted OPEX + CAPEX for a multi-stage,
multi-scenario power system model. The solver uses DC power flow (Kirchhoff's
voltage law) or a transport model, and is validated against standard IEEE
benchmark networks (4-bus, 9-bus, 14-bus).

See `.github/copilot-instructions.md` §§ "Domain Knowledge" and "IEEE
Benchmark Cases" for full details.

## License

By contributing to gtopt, you agree that your contributions will be released
under the [Unlicense](LICENSE).
