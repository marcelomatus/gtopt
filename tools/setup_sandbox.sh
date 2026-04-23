#!/usr/bin/env bash
# setup_sandbox.sh — Bootstrap a fresh Ubuntu 24.04 sandbox for building gtopt
#
# Usage:
#   bash tools/setup_sandbox.sh [OPTIONS]
#
# Options:
#   --no-python      Skip pre-installing Python scripts dependencies
#   --configure      Also run cmake configure after deps are installed
#   --build          Also run cmake --build after configure (implies --configure)
#   --build-type T   CMake build type: CIFast (default) | Debug | Release | RelWithDebInfo
#   --debug          Shortcut for --build-type Debug (full symbols, asserts)
#   --no-save-ccjson Skip saving compile_commands.json to tools/ after build
#   --help           Show this help and exit
#
# Build-type rationale:
#   CIFast is -O0 -g1 with gc-sections — no optimisation (fast compile) and
#   only minimal line-number debug info (backtraces still work on test
#   failures).  This is what CI and agent-driven iteration want: a pass/fail
#   signal plus a readable stack trace.  Reconfigure with --debug when you
#   actually need to step through code in gdb.
#
# What this script does (in the same order as .github/workflows/ubuntu.yml):
#   1. Install ccache + base APT packages (COIN-OR, Boost, spdlog, LAPACK, etc.)
#   2-3. Install Arrow/Parquet (conda) AND Clang 21 compiler IN PARALLEL.
#      If the LLVM APT repository is unavailable, fall back to GCC 14.
#   4. Pre-install Python scripts dev dependencies (speeds up CTest fixture).
#   5. (Optional) cmake configure — uses whichever compiler was installed.
#   6. (Optional) cmake --build + ctest.
#   7. (Optional) After a successful build, copy compile_commands.json to
#      tools/compile_commands.json in the repository.  This allows agents
#      to run clang-tidy on individual files without rebuilding:
#        clang-tidy -p tools/compile_commands.json source/my_file.cpp
#      Use --no-save-ccjson to skip this step.
#
# Environment:
#   REPO_ROOT   Path to the repository root (default: directory containing this
#               script's parent, i.e. the repo root when called as
#               bash tools/setup_sandbox.sh from the repo root).
#
# Notes:
#   • Run from the repository root: bash tools/setup_sandbox.sh
#   • Idempotent: safe to run more than once; already-installed packages are
#     skipped automatically by apt-get / conda.
#   • Clang 21 is the preferred compiler (same as CI).  If the LLVM APT
#     repository is unreachable, GCC 14 is used as a fallback.  Both produce
#     a fully working build; the summary at the end reports which was chosen.
#   • clang-22 packages are not yet available on apt.llvm.org; use version 21.
#   • compile_commands.json is saved to tools/ after every successful build
#     so that clang-tidy can be run on demand without rebuilding.
#     Committed to the repo so agents start with a usable compile DB.
#       clang-tidy -p tools/compile_commands.json source/my_file.cpp


set -euo pipefail

# ── Resolve repo root ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(dirname "$SCRIPT_DIR")}"
cd "$REPO_ROOT"

# ── Defaults ──────────────────────────────────────────────────────────────────
INSTALL_PYTHON=true
DO_CONFIGURE=false
DO_BUILD=false
BUILD_TYPE=CIFast
CLANG_VERSION=21
SAVE_CCJSON=true

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-python)    INSTALL_PYTHON=false ;;
    --configure)    DO_CONFIGURE=true ;;
    --build)        DO_CONFIGURE=true; DO_BUILD=true ;;
    --build-type)   shift; BUILD_TYPE="$1" ;;
    --debug)        BUILD_TYPE=Debug ;;
    --no-save-ccjson) SAVE_CCJSON=false ;;
    --help|-h)
      sed -n '2,/^# Notes:/p' "$0" | sed 's/^# \?//'
      exit 0 ;;
    *)
      echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

log()  { echo "▶ $*"; }
ok()   { echo "✓ $*"; }
warn() { echo "⚠ $*" >&2; }

# ── Detect OS codename (needed for LLVM APT) ──────────────────────────────────
CODENAME=$(lsb_release --codename --short 2>/dev/null || \
           awk -F= '/^VERSION_CODENAME/{gsub(/["[:space:]]/, "", $2); print $2}' \
             /etc/os-release)
if [[ -z "$CODENAME" ]]; then
  echo "ERROR: could not determine OS codename (lsb_release and /etc/os-release failed)." >&2
  exit 1
fi

# ── Step 1: ccache + base APT packages ────────────────────────────────────────
# ccache MUST be installed before cmake configure; cmake bakes the launcher
# path into the build system at configure time.  Installing it later requires
# deleting the build directory and reconfiguring from scratch.
log "Installing ccache and base APT packages..."
sudo apt-get update -q
sudo apt-get install -y --no-install-recommends \
  ccache \
  coinor-libcbc-dev \
  libboost-container-dev \
  libspdlog-dev \
  liblapack-dev libblas-dev \
  libjemalloc-dev \
  zlib1g-dev \
  libzstd-dev zstd \
  liblz4-dev \
  lcov \
  ca-certificates lsb-release wget
ok "ccache and base packages installed"

# ── Steps 2-3: Arrow/Parquet + Compiler (in parallel) ────────────────────────
# These two installations are independent and can run concurrently, saving
# ~1-2 minutes on fresh setups.

# --- Arrow / Parquet via conda-forge ---
install_arrow() {
  # Always use conda-forge.  The APT Arrow packages (packages.apache.org) are
  # NOT used in sandbox/agent environments because the APT libarrow version can
  # conflict with the conda libarrow at link time.
  # Conda gives a self-consistent Arrow+Parquet+Boost set that always matches.

  # Ensure conda itself works.  Some sandbox images ship a broken certifi
  # package (ImportError: cannot import name 'where') or a libmamba solver
  # that cannot load.  Fix both before proceeding.
  if command -v conda &>/dev/null; then
    if ! python3 -c "import certifi; certifi.where()" &>/dev/null; then
      warn "certifi is broken in conda's Python -- reinstalling..."
      pip install certifi --force-reinstall --upgrade --quiet 2>/dev/null || true
    fi
    if ! conda info --base &>/dev/null 2>&1; then
      warn "conda default solver is broken -- switching to classic solver"
      conda config --set solver classic 2>/dev/null || true
    fi
  fi

  # Check that the *real* Arrow library is installed (not just the dummy
  # arrow-cpp transitional package v0.2.post).
  local CONDA_PREFIX_DIR
  CONDA_PREFIX_DIR="$(conda info --base 2>/dev/null || echo "")"
  if [[ -n "$CONDA_PREFIX_DIR" ]] && \
     find "$CONDA_PREFIX_DIR" -name "ArrowConfig.cmake" -print -quit 2>/dev/null | grep -q .; then
    ok "Arrow/Parquet already installed via conda (ArrowConfig.cmake found)"
    return 0
  fi

  if ! command -v conda &>/dev/null; then
    echo "ERROR: conda not found.  Install Miniconda/Anaconda first." >&2
    return 1
  fi
  log "Installing real Arrow/Parquet libraries via conda-forge..."
  conda remove -y --force arrow-cpp parquet-cpp 2>/dev/null || true
  if ! conda install -y -c conda-forge libarrow libparquet boost-cpp 2>/dev/null; then
    warn "conda install failed with default solver -- retrying with classic solver..."
    conda install -y --solver=classic -c conda-forge libarrow libparquet boost-cpp
  fi
  ok "Arrow/Parquet installed via conda-forge"
}

# --- Compiler: Clang 21 preferred, GCC 14 fallback ---
install_compiler() {
  local VER=$CLANG_VERSION

  if (
    set -e

    for attempt in 1 2 3; do
      wget -qO /tmp/llvm-snapshot.gpg.key \
        https://apt.llvm.org/llvm-snapshot.gpg.key && break
      echo "Attempt ${attempt}/3: wget gpg key failed; retrying in 15 s..." >&2
      if [[ $attempt -lt 3 ]]; then sleep 15; else exit 1; fi
    done

    sudo gpg --yes --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
      /tmp/llvm-snapshot.gpg.key

    echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] \
    https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${VER} main" \
      | sudo tee /etc/apt/sources.list.d/llvm-${VER}.list

    for attempt in 1 2 3; do
      sudo apt-get update -q \
        -o "Dir::Etc::sourcelist=/etc/apt/sources.list.d/llvm-${VER}.list" \
        -o "Dir::Etc::sourceparts=-" && break
      echo "Attempt ${attempt}/3: apt-get update failed; retrying in 15 s..." >&2
      if [[ $attempt -lt 3 ]]; then sleep 15; else exit 1; fi
    done

    sudo apt-get install -y --no-install-recommends \
      clang-${VER} clang-tools-${VER} clang-format-${VER} clang-tidy-${VER} \
      lld-${VER} \
      llvm-${VER}-dev llvm-${VER}-tools libomp-${VER}-dev \
      libc++-${VER}-dev libc++abi-${VER}-dev \
      libclang-common-${VER}-dev libclang-${VER}-dev libclang-cpp${VER}-dev
  ); then
    for versioned in /usr/bin/clang*-${VER} /usr/bin/llvm*-${VER} /usr/bin/lld-${VER} /usr/bin/ld.lld-${VER} /usr/bin/wasm-ld-${VER} /usr/bin/lld-link-${VER}; do
      [ -e "$versioned" ] || continue
      base=$(basename "$versioned" "-${VER}")
      sudo update-alternatives --remove-all "$base" 2>/dev/null || true
      sudo update-alternatives --install /usr/bin/"$base" "$base" \
        "$versioned" 100
    done
    ok "Clang ${VER} installed and registered as default 'clang'/'clang++'"
    echo "clang" > /tmp/_sandbox_compiler
  else
    warn "Clang ${VER} installation failed (LLVM APT repo unreachable?)."
    warn "Falling back to GCC 14."
    sudo apt-get install -y --no-install-recommends gcc-14 g++-14
    ok "GCC 14 installed as fallback compiler"
    echo "gcc" > /tmp/_sandbox_compiler
  fi
}

log "Installing Arrow/Parquet and compiler in parallel..."
install_arrow &
ARROW_PID=$!
install_compiler &
COMPILER_PID=$!

ARROW_OK=true
COMPILER_OK=true
wait $ARROW_PID || ARROW_OK=false
wait $COMPILER_PID || COMPILER_OK=false

if ! $ARROW_OK; then
  echo "ERROR: Arrow/Parquet installation failed." >&2
  exit 1
fi
if ! $COMPILER_OK; then
  echo "ERROR: Compiler installation failed." >&2
  exit 1
fi

ARROW_INSTALLED_VIA="conda"
if [[ -f /tmp/_sandbox_compiler ]] && [[ "$(cat /tmp/_sandbox_compiler)" == "clang" ]]; then
  CLANG_INSTALLED=true
  CC=clang
  CXX=clang++
else
  CLANG_INSTALLED=false
  CC=gcc-14
  CXX=g++-14
fi
rm -f /tmp/_sandbox_compiler

# ── Step 4: Python scripts dependencies ──────────────────────────────────────
# Pre-installing BEFORE cmake configure ensures cmake's
# find_program(PYTHON_EXECUTABLE) picks the same Python that already has all
# packages, reducing the scripts-install-deps CTest fixture from ~35 s to ~3 s.
if $INSTALL_PYTHON; then
  if ! command -v uv &>/dev/null; then
    log "uv not found -- installing via pip..."
    pip install --break-system-packages uv -q 2>/dev/null \
      || pip install uv -q
    ok "uv installed"
  fi
  log "Pre-installing Python scripts dev dependencies..."
  uv pip install --system --break-system-packages -q -e "./scripts[dev]" graphviz 2>/dev/null \
    || uv pip install --system -q -e "./scripts[dev]" graphviz 2>/dev/null \
    || pip install --break-system-packages -q -e "./scripts[dev]" graphviz 2>/dev/null \
    || { warn "Python scripts dep install failed -- CTest fixture will install them (slower)."; }
  ok "Python scripts dev dependencies installed"
fi

# ── Steps 5-6: Configure and build (optional) ─────────────────────────────────
if $DO_CONFIGURE; then
  # Determine the cmake prefix path – always conda for Arrow in sandboxes
  CMAKE_PREFIX_ARG="-DCMAKE_PREFIX_PATH=$(conda info --base)"

  # CPM package cache – reuse downloaded CMake packages across repeated runs.
  # Matches the CPM_SOURCE_CACHE env variable used by ubuntu.yml CI.
  CPM_CACHE="${HOME}/.cache/cpm_modules"
  mkdir -p "${CPM_CACHE}"

  log "Configuring cmake (${BUILD_TYPE}, ${CC}/${CXX})..."
  CMAKE_ARGS=(
    -S all -B build
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_C_COMPILER=${CC}"
    "-DCMAKE_CXX_COMPILER=${CXX}"
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    "-DCPM_SOURCE_CACHE=${CPM_CACHE}"
  )
  if [[ -n "${CMAKE_PREFIX_ARG}" ]]; then
    CMAKE_ARGS+=("${CMAKE_PREFIX_ARG}")
  fi
  cmake "${CMAKE_ARGS[@]}"
  ok "cmake configure done"
fi

if $DO_BUILD; then
  log "Building..."
  cmake --build build -j"$(nproc)"
  ok "Build complete"

  log "Running tests..."
  (cd build && ctest --output-on-failure -j"$(nproc)")
  ok "All tests passed"

  # Save compile_commands.json to tools/ so clang-tidy can be run on demand
  # without rebuilding.  Commit it so agent sessions work without a build:
  #   clang-tidy -p tools/compile_commands.json source/my_file.cpp
  if $SAVE_CCJSON && [[ -f build/compile_commands.json ]]; then
    log "Saving compile_commands.json to tools/compile_commands.json..."
    cp build/compile_commands.json tools/compile_commands.json
    ok "compile_commands.json saved -- run clang-tidy without rebuilding:"
    ok "  clang-tidy -p tools/compile_commands.json source/my_file.cpp"
  fi
fi

# ── Summary ────────────────────────────────────────────────────────────────────
if $CLANG_INSTALLED; then
  COMPILER_LINE=" Compiler     : Clang ${CLANG_VERSION} ($(clang --version 2>/dev/null | head -1))"
else
  COMPILER_LINE=" Compiler     : GCC 14 fallback ($(${CXX} --version 2>/dev/null | head -1))"
fi
echo ""
echo "======================================================="
echo " gtopt sandbox setup complete"
echo " Arrow source : ${ARROW_INSTALLED_VIA}"
echo "${COMPILER_LINE}"
echo " ccache       : $(ccache --version 2>/dev/null | head -1)"
if $DO_BUILD; then
  echo " Build        : build/"
  if $SAVE_CCJSON && [[ -f tools/compile_commands.json ]]; then
    echo " compile_commands: tools/compile_commands.json (clang-tidy ready)"
  fi
fi
if ! $CLANG_INSTALLED; then
  echo ""
  echo " NOTE: CI always uses Clang 21.  GCC 14 is a local-only fallback."
  echo "       Re-run this script when the LLVM APT repository is reachable"
  echo "       to switch to Clang 21."
fi
echo "======================================================="
echo ""
echo "Next steps:"
if ! $DO_CONFIGURE; then
  if [[ "$ARROW_INSTALLED_VIA" == "conda" ]]; then
    PREFIX_HINT='  -DCMAKE_PREFIX_PATH="$(conda info --base)"'
  else
    PREFIX_HINT=""
  fi
  echo "  cmake -S all -B build \\"
  echo "    -DCMAKE_BUILD_TYPE=CIFast \\"
  echo "    -DCMAKE_C_COMPILER=${CC} \\"
  echo "    -DCMAKE_CXX_COMPILER=${CXX} \\"
  if [[ -n "$PREFIX_HINT" ]]; then
    echo "    -DCMAKE_C_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCPM_SOURCE_CACHE=\${HOME}/.cache/cpm_modules \\"
    echo "    ${PREFIX_HINT}"
  else
    echo "    -DCMAKE_C_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCPM_SOURCE_CACHE=\${HOME}/.cache/cpm_modules"
  fi
  echo "  cmake --build build -j\$(nproc)"
  echo "  cd build && ctest --output-on-failure"
fi
