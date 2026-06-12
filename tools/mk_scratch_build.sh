#!/usr/bin/env bash
# Allocate a fresh scratch build directory for agent/background builds.
# mktemp is atomic so concurrent agents never collide on the same path.
#
# Builds live under ~/tmp/ (or $TMPDIR when set) — never under /tmp/, since
# WSL2 /tmp/ has historically been tmpfs-backed (RAM-eating) and the
# SDDP WorkPool starved when /tmp/ filled.  See the
# project_tmp_tmpfs_to_disk and feedback_build_in_home_tmp notes.
#
# Usage:
#   BUILD_DIR=$(bash tools/mk_scratch_build.sh)
#   cmake -S all -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=CIFast ...
set -euo pipefail
BASE="${TMPDIR:-$HOME/tmp}"
mkdir -p "$BASE"
exec mktemp -d -p "$BASE" gtopt-build-XXXX
