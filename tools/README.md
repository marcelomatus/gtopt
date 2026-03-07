# tools/

This directory contains **agent and developer tools** that are NOT installed
as part of the `gtopt` user-facing distribution.

These scripts must **not** be added to `scripts/pyproject.toml` (as
`py-modules` or `packages.find.include`) and must **not** be added to any
`cmake install` target.

## Tools

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
