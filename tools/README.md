# tools/

This directory contains **agent and developer tools** that are NOT installed
as part of the `gtopt` user-facing distribution.

These scripts must **not** be added to `scripts/pyproject.toml` (as
`py-modules` or `packages.find.include`) and must **not** be added to any
`cmake install` target.

## Tools

### `setup_sandbox.sh`

Bootstrap a fresh Ubuntu 24.04 sandbox for building gtopt.  Installs all
required build dependencies in the same order as `.github/workflows/ubuntu.yml`:

1. ccache + base APT packages (COIN-OR, Boost, spdlog, LAPACK)
2. Arrow/Parquet — tries the Apache Arrow APT repository first; falls back to
   conda-forge if `packages.apache.org` is unreachable
3. Clang 21 from the LLVM APT repository (with retry logic)
4. Python scripts dev dependencies (pre-installed before cmake configure)
5. (Optional) cmake configure + build + test

**Quick start (from repo root):**

```bash
# Install all dependencies only (no build)
bash tools/setup_sandbox.sh

# Install deps + configure + build + test
bash tools/setup_sandbox.sh --build

# Skip Clang 21 install (use GCC 14 instead)
bash tools/setup_sandbox.sh --no-clang --build

# Release build
bash tools/setup_sandbox.sh --build --build-type Release

# Help
bash tools/setup_sandbox.sh --help
```

**Options:**

| Option | Description |
|--------|-------------|
| `--no-clang` | Skip Clang 21; use GCC 14 instead |
| `--no-python` | Skip Python scripts pre-install |
| `--configure` | Run cmake configure after deps |
| `--build` | Run cmake --build + ctest (implies `--configure`) |
| `--build-type T` | CMake build type: `Debug` (default) / `Release` / `RelWithDebInfo` |
| `--help` | Show usage and exit |

---

### `clang-tidy`

Transparent wrapper that forwards every invocation to `run-clang-tidy`
(the parallel driver), injecting `-j $(nproc)` when the caller did not
specify a jobs flag.  Shadows the system `clang-tidy` when placed first
on `PATH`, so developers and LLM agents who type `clang-tidy <file>`
out of habit automatically get the same parallel execution that CI and
the pre-commit hook use.

**Quick start (from repo root):**

```bash
# One-off symlink (auto-updates on git pull):
ln -s "$(pwd)/tools/clang-tidy" ~/.local/bin/clang-tidy

# Or prepend tools/ to PATH:
export PATH="$(pwd)/tools:$PATH"

# Verify the wrapper wins over /usr/bin/clang-tidy:
command -v clang-tidy     # must print the wrapper path
```

**Behaviour:**

| Caller invocation | Forwarded invocation |
|---|---|
| `clang-tidy foo.cpp` | `run-clang-tidy -j $(nproc) foo.cpp` |
| `clang-tidy -j 4 foo.cpp` | `run-clang-tidy -j 4 foo.cpp` (preserved) |
| `clang-tidy --jobs=8 foo.cpp` | `run-clang-tidy --jobs=8 foo.cpp` (preserved) |

**Bypass:** call the real binary by absolute path, e.g. `/usr/bin/clang-tidy`.

---

### `get_gtopt_binary.py`

Standalone helper for Copilot / Claude agents that need the compiled `gtopt`
binary without waiting for a full 5–10 minute build from source.

**Quick start (from repo root):**

```bash
# Print the path to a working gtopt binary (downloads CI artifact if needed)
export GTOPT_BIN=$(python tools/get_gtopt_binary.py)

# Also build from source as a last resort
python tools/get_gtopt_binary.py --build --verbose

# Force a fresh download even if a binary is already present
python tools/get_gtopt_binary.py --force-download
```

**Strategy (in order):**

1. `GTOPT_BIN` environment variable.
2. `gtopt` on `PATH`.
3. Standard build directories (`build/standalone/gtopt`, etc.).
4. `/tmp/gtopt-ci-bin/gtopt` – previously downloaded CI artifact.
5. Download `gtopt-binary-debug` CI artifact via `gh` CLI or `GITHUB_TOKEN`.
6. Build from source with `cmake` (requires build dependencies).

See the full module docstring for detailed usage and CI artifact download
step-by-step instructions.
