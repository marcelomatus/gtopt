# Contributing to gtopt

Thank you for your interest in contributing to gtopt! This document provides
guidelines and instructions for contributing.

## Getting Started

1. Fork the repository and clone your fork
2. Follow the build instructions in [BUILDING.md](BUILDING.md)
3. Create a feature branch from `main`

## Development Setup

### Prerequisites

- GCC 14+ (C++26 support required)
- CMake 3.31.6+
- Dependencies listed in [BUILDING.md](BUILDING.md)

### Building and Testing

```bash
# Configure and build tests
CC=gcc-14 CXX=g++-14 cmake -Stest -Bbuild -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
cd build && ctest

# Run a specific test
./build/gtoptTests [test_name]

# Check code formatting
cmake --build build --target check-format

# Auto-format code
cmake --build build --target format
```

### Code Coverage

```bash
CC=gcc-14 CXX=g++-14 cmake -Stest -Bbuild -DENABLE_TEST_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest
```

## Code Style

### C++

- **Standard**: C++26 (C++23 features currently used)
- **Formatting**: Enforced by `.clang-format` (LLVM-based, 80-char column limit)
- **Linting**: Configured via `.clang-tidy` with extensive checks enabled
- **Naming conventions**:
  - Namespaces, variables, functions: `lower_case`
  - Classes/Structs: `CamelCase`
  - Private members: `m_` prefix
  - Macros: `UPPER_CASE`
- **Include guards**: Use `#pragma once`
- **Includes**: Organize in blocks (standard library, external, project)
- **Error handling**: Prefer return values over exceptions
- **Modern C++**: Use concepts, ranges, `std::format`, structured bindings,
  `[[nodiscard]]` where appropriate

### Python

- **Formatting**: Black with 120-char line length
- **Linting**: Flake8 + Pylint (minimum score 8.0)
- **Configuration**: See `.flake8` and `.pylintrc`

### Pre-commit Hooks

The project uses pre-commit hooks for automated checks. Install them with:

```bash
pip install pre-commit
pre-commit install
```

Hooks include: trailing whitespace, YAML validation, Black, Flake8, Pylint,
ShellCheck, yamllint, markdownlint, and cmake-format.

## Pull Request Process

1. Ensure your changes build and all tests pass
2. Run `cmake --build build --target check-format` to verify code style
3. Add tests for new functionality
4. Update documentation if your changes affect user-facing behavior
5. Write clear, descriptive commit messages
6. Open a pull request against `main` with a description of your changes

## Testing Guidelines

- **Framework**: [doctest](https://github.com/doctest/doctest) for C++ tests
- Place test files in `test/source/` (or `test/source/json/` for JSON tests)
- Use `CHECK` and `REQUIRE` assertions
- Name test cases descriptively

## Project Structure

```
gtopt/
├── include/gtopt/     # Public C++ headers
├── source/            # C++ implementation files
├── test/              # C++ test suite (doctest)
├── standalone/        # Standalone binary
├── webservice/        # Node.js/TypeScript web service
├── guiservice/        # Python Flask GUI service
├── scripts/           # Python utility scripts
├── cmake/             # CMake utilities
├── cases/             # Sample test cases
└── documentation/     # Doxygen setup
```

## License

By contributing to gtopt, you agree that your contributions will be released
under the [Unlicense](LICENSE).
