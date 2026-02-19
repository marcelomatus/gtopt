# Test Coverage Monitoring

This project uses fully open-source tools for test coverage monitoring — no paid
services or proprietary platforms required.

## Overview

| Component       | Test Framework | Coverage Tool                  | Report Format   |
|----------------|----------------|--------------------------------|-----------------|
| C++ (core)     | doctest        | GCC `--coverage` + lcov/genhtml | HTML artifact   |
| Python (GUI)   | pytest         | pytest-cov (coverage.py)       | HTML artifact   |
| JS (webservice)| shell scripts  | *(not yet instrumented)*       | —               |

## How It Works

### C++ Unit Test Coverage

The C++ tests use GCC's built-in profiling instrumentation:

1. **Build with coverage**: CMake option `-DENABLE_TEST_COVERAGE=1` adds
   `-fprofile-arcs -ftest-coverage` compiler/linker flags to **both** the
   test binary and the `libgtopt` library.
2. **Run tests**: `ctest` executes the doctest-based test binary, which writes
   `.gcda` profile data files.
3. **Collect with lcov**: The `lcov` tool reads `.gcda`/`.gcno` files and
   produces a `coverage.info` tracefile. The `--rc geninfo_unexecuted_blocks=1`
   flag suppresses false warnings about unexecuted blocks in system headers.
4. **Filter**: External code (system headers, third-party libraries, test files)
   is removed from the tracefile.
5. **Generate HTML**: `genhtml` converts the tracefile into a browsable HTML
   report with per-file/per-function/per-branch line coverage.

The HTML report is uploaded as a GitHub Actions artifact named
**cpp-coverage-report** with 30-day retention.

### Python Test Coverage

Python tests use `pytest-cov`, which wraps the standard `coverage.py` library:

1. **Run with coverage**: `pytest --cov=guiservice --cov-report=html`
2. **Upload**: HTML report uploaded as **python-guiservice-coverage-report** artifact.

### Coverage Summary

Each CI run writes a coverage summary to the GitHub Actions **Job Summary**
(visible on the workflow run page), so you can see the line/function coverage
percentages without downloading the full report.

## Viewing Reports

### From GitHub Actions

1. Navigate to the **Actions** tab in the repository.
2. Select the workflow run for your branch or PR.
3. Scroll to the **Artifacts** section at the bottom.
4. Download the desired coverage report (e.g., `cpp-coverage-report`).
5. Extract the archive and open `index.html` in your browser.

### Locally (C++)

```bash
# Configure with coverage (instruments both tests and libgtopt)
cmake -Stest -Bbuild -DENABLE_TEST_COVERAGE=1 -DCMAKE_BUILD_TYPE=Debug

# Build and test
cmake --build build -j$(nproc)
cd build && ctest && cd ..

# Collect coverage
# --ignore-errors mismatch,mismatch: suppress third-party header mismatch warnings
# --ignore-errors unexecuted,unexecuted: suppress unexecuted block warnings
# --rc unexecuted_blocks=1: zero out counts on unexecuted non-branch lines
lcov --capture --directory build --output-file coverage.info \
     --gcov-tool gcov-14 \
     --ignore-errors mismatch,mismatch \
     --ignore-errors unexecuted,unexecuted \
     --rc unexecuted_blocks=1
lcov --remove coverage.info '/usr/*' '*/cpm_modules/*' '*/test/*' \
     '*/daw/*' '*/strong_type/*' '*/spdlog/*' \
     --output-file coverage.info --rc branch_coverage=1

# Generate HTML report with branch coverage
genhtml coverage.info --output-directory coverage-report \
       --rc branch_coverage=1
# Open coverage-report/index.html
```

### Locally (Python)

```bash
pip install pytest pytest-cov -r guiservice/requirements.txt
python -m pytest guiservice/tests/ --cov=guiservice --cov-report=html:coverage-guiservice
# Open coverage-guiservice/index.html
```

## Tools Used

All tools are free and open-source:

| Tool          | License    | Purpose                              |
|---------------|------------|--------------------------------------|
| [GCC](https://gcc.gnu.org/) | GPL-3.0 | C++ compiler with `--coverage` support |
| [lcov](https://github.com/linux-test-project/lcov) | GPL-2.0 | GCC coverage data collection/filtering |
| [genhtml](https://github.com/linux-test-project/lcov) | GPL-2.0 | HTML coverage report generation |
| [coverage.py](https://github.com/nedbat/coveragepy) | Apache-2.0 | Python code coverage measurement |
| [pytest-cov](https://github.com/pytest-dev/pytest-cov) | MIT | pytest plugin for coverage.py |

## Future Improvements

- Add coverage trend tracking using GitHub Pages or artifact comparison scripts.
- Instrument the Node.js webservice tests (e.g., via c8 or nyc).
- Add coverage thresholds to fail the build if coverage drops below a minimum.
- Add PR comment with coverage diff using a lightweight open-source GitHub Action.
