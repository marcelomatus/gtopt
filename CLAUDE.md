# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands
- Configure: `cmake -S . -B build`
- Build: `cmake --build build`
- Run tests: `cd build && ctest` or `cmake --build build --target test`
- Run single test: `./build/test/gtoptTests [test name]`
- Enable test coverage: Add `-DENABLE_TEST_COVERAGE=ON` to configure command

## Code Style Guidelines
- **C++ Standard**: C++23
- **Indentation**: 2 spaces
- **Namespaces**: Code in `gtopt` namespace
- **Includes**: Organize in blocks (standard library, external, project)
- **Templates**: Use concepts for type constraints when possible
- **Naming**: Classes/Structs: PascalCase, methods/functions: camelCase, members: snake_case
- **Error Handling**: Use return values over exceptions where possible
- **Testing**: Use doctest framework with CHECK/REQUIRE assertions
- **Documentation**: Doxygen-style file headers and class/function documentation

Run linter/formatters with `cmake --build build --target format`