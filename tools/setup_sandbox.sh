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
#   --build-type T   CMake build type: Debug (default) | Release | RelWithDebInfo
#   --help           Show this help and exit
#
# What this script does (in the same order as .github/workflows/ubuntu.yml):
#   1. Install ccache + base APT packages (COIN-OR, Boost, spdlog, LAPACK, etc.)
#   2. Install Arrow/Parquet via conda (conda-forge).
#   3. Try to install Clang 21 from the LLVM APT repository (preferred).
#      If the LLVM APT repository is unavailable, fall back to GCC 14.
#      Registers unversioned alternatives (clang/clang++/clang-format/… or
#      gcc/g++ depending on what was installed).
#   4. Pre-install Python scripts dev dependencies (speeds up CTest fixture).
#   5. (Optional) cmake configure — uses whichever compiler was installed.
#   6. (Optional) cmake --build + ctest.
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

set -euo pipefail

# ── Resolve repo root ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(dirname "$SCRIPT_DIR")}"
cd "$REPO_ROOT"

# ── Defaults ──────────────────────────────────────────────────────────────────
INSTALL_PYTHON=true
DO_CONFIGURE=false
DO_BUILD=false
BUILD_TYPE=Debug
CLANG_VERSION=21

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-python)    INSTALL_PYTHON=false ;;
    --configure)    DO_CONFIGURE=true ;;
    --build)        DO_CONFIGURE=true; DO_BUILD=true ;;
    --build-type)   shift; BUILD_TYPE="$1" ;;
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
           awk -F= '/^VERSION_CODENAME/{print $2}' /etc/os-release)

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
  zlib1g-dev \
  ca-certificates lsb-release wget
ok "ccache and base packages installed"

# ── Step 2: Arrow / Parquet ───────────────────────────────────────────────────
# Always use conda-forge.  The APT Arrow packages (packages.apache.org) are
# NOT used in sandbox/agent environments because the APT libarrow version can
# conflict with the conda libarrow at link time (versioned curl symbols such as
# curl_global_cleanup@CURL_OPENSSL_4 are undefined in the system libcurl),
# causing undefined-reference errors even when cmake finds the right headers.
# Conda gives a self-consistent Arrow+Parquet+Boost set that always matches.
log "Installing Arrow/Parquet via conda-forge..."

ARROW_INSTALLED_VIA="conda"
if conda list arrow-cpp 2>/dev/null | grep -q "^arrow-cpp"; then
  ok "Arrow/Parquet already installed via conda"
else
  if ! command -v conda &>/dev/null; then
    echo "ERROR: conda not found.  Install Miniconda/Anaconda first." >&2
    exit 1
  fi
  conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp
  ok "Arrow/Parquet installed via conda-forge"
fi

# ── Step 3: Compiler — Clang 21 preferred, GCC 14 fallback ───────────────────
# We try to install Clang 21 from the LLVM APT repository first (preferred,
# matches CI).  If any step fails (e.g. the LLVM APT repo is temporarily
# unreachable), we fall back silently to GCC 14 which is always available on
# Ubuntu 24.04.  Either compiler produces a fully working gtopt build.
VER=$CLANG_VERSION
CLANG_INSTALLED=false
CC=gcc-14
CXX=g++-14

log "Attempting to install Clang ${VER} from LLVM APT repository..."

# Use a sub-shell so a failure inside does not abort the outer script.
if (
  set -e

  # Fetch GPG key (with retries — apt.llvm.org is intermittently slow)
  for attempt in 1 2 3; do
    wget -qO /tmp/llvm-snapshot.gpg.key \
      https://apt.llvm.org/llvm-snapshot.gpg.key && break
    echo "⚠ Attempt ${attempt}/3: wget gpg key failed; retrying in 15 s..." >&2
    [[ $attempt -lt 3 ]] && sleep 15 || exit 1
  done

  sudo gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
    /tmp/llvm-snapshot.gpg.key

  echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] \
  https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${VER} main" \
    | sudo tee /etc/apt/sources.list.d/llvm-${VER}.list

  for attempt in 1 2 3; do
    sudo apt-get update -q \
      -o "Dir::Etc::sourcelist=/etc/apt/sources.list.d/llvm-${VER}.list" \
      -o "Dir::Etc::sourceparts=-" && break
    echo "⚠ Attempt ${attempt}/3: apt-get update failed; retrying in 15 s..." >&2
    [[ $attempt -lt 3 ]] && sleep 15 || exit 1
  done

  sudo apt-get install -y --no-install-recommends \
    clang-${VER} clang-tools-${VER} clang-format-${VER} clang-tidy-${VER} \
    llvm-${VER}-dev llvm-${VER}-tools libomp-${VER}-dev \
    libc++-${VER}-dev libc++abi-${VER}-dev \
    libclang-common-${VER}-dev libclang-${VER}-dev libclang-cpp${VER}-dev
); then
  # Register unversioned alternatives so 'clang', 'clang++', etc. resolve to
  # the installed version without a suffix (matches install-clang/action.yml).
  for versioned in /usr/bin/clang*-${VER} /usr/bin/llvm*-${VER}; do
    [ -e "$versioned" ] || continue
    base=$(basename "$versioned" "-${VER}")
    sudo update-alternatives --remove-all "$base" 2>/dev/null || true
    sudo update-alternatives --install /usr/bin/"$base" "$base" \
      "$versioned" 100
  done
  ok "Clang ${VER} installed and registered as default 'clang'/'clang++'"
  CLANG_INSTALLED=true
  CC=clang
  CXX=clang++
else
  warn "Clang ${VER} installation failed (LLVM APT repo unreachable?)."
  warn "Falling back to GCC 14 — install gcc-14 / g++-14 if not present."
  sudo apt-get install -y --no-install-recommends gcc-14 g++-14
  ok "GCC 14 installed as fallback compiler"
  CC=gcc-14
  CXX=g++-14
fi

# ── Step 4: Python scripts dependencies ───────────────────────────────────────
# Pre-installing these BEFORE cmake configure ensures cmake's
# find_program(PYTHON_EXECUTABLE) picks the same Python that already has all
# packages, reducing the scripts-install-deps CTest fixture from ~35 s to ~3 s.
if $INSTALL_PYTHON; then
  if command -v uv &>/dev/null; then
    log "Pre-installing Python scripts dev dependencies..."
    uv pip install --system -q -e "./scripts[dev]" graphviz
    ok "Python scripts dev dependencies installed"
  else
    warn "uv not found – skipping Python scripts pre-install (CTest fixture" \
         "will install them on first run, taking ~35 s)"
  fi
fi

# ── Steps 5–6: Configure and build (optional) ─────────────────────────────────
if $DO_CONFIGURE; then
  # Determine the cmake prefix path – always conda for Arrow in sandboxes
  CMAKE_PREFIX_ARG="-DCMAKE_PREFIX_PATH=$(conda info --base)"

  log "Configuring cmake (${BUILD_TYPE}, ${CC}/${CXX})..."
  CMAKE_ARGS=(
    -S all -B build
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_C_COMPILER=${CC}"
    "-DCMAKE_CXX_COMPILER=${CXX}"
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
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
fi

# ── Summary ────────────────────────────────────────────────────────────────────
if $CLANG_INSTALLED; then
  COMPILER_LINE=" Compiler     : Clang ${CLANG_VERSION} ($(clang --version 2>/dev/null | head -1))"
else
  COMPILER_LINE=" Compiler     : GCC 14 fallback ($(${CXX} --version 2>/dev/null | head -1))"
fi
echo ""
echo "═══════════════════════════════════════════════════════"
echo " gtopt sandbox setup complete"
echo " Arrow source : ${ARROW_INSTALLED_VIA}"
echo "${COMPILER_LINE}"
echo " ccache       : $(ccache --version 2>/dev/null | head -1)"
if $DO_BUILD; then
  echo " Build        : build/"
fi
if ! $CLANG_INSTALLED; then
  echo ""
  echo " NOTE: CI always uses Clang 21.  GCC 14 is a local-only fallback."
  echo "       Re-run this script when the LLVM APT repository is reachable"
  echo "       to switch to Clang 21."
fi
echo "═══════════════════════════════════════════════════════"
echo ""
echo "Next steps:"
if ! $DO_CONFIGURE; then
  if [[ "$ARROW_INSTALLED_VIA" == "conda" ]]; then
    PREFIX_HINT='  -DCMAKE_PREFIX_PATH="$(conda info --base)"'
  else
    PREFIX_HINT=""
  fi
  echo "  cmake -S all -B build \\"
  echo "    -DCMAKE_BUILD_TYPE=Debug \\"
  echo "    -DCMAKE_C_COMPILER=${CC} \\"
  echo "    -DCMAKE_CXX_COMPILER=${CXX} \\"
  if [[ -n "$PREFIX_HINT" ]]; then
    echo "    -DCMAKE_C_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \\"
    echo "    ${PREFIX_HINT}"
  else
    echo "    -DCMAKE_C_COMPILER_LAUNCHER=ccache \\"
    echo "    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  fi
  echo "  cmake --build build -j\$(nproc)"
  echo "  cd build && ctest --output-on-failure"
fi
