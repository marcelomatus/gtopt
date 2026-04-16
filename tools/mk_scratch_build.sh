#!/usr/bin/env bash
# Allocate a fresh scratch build directory for agent/background builds.
# mktemp is atomic so concurrent agents never collide on the same path.
#
# Usage:
#   BUILD_DIR=$(bash tools/mk_scratch_build.sh)
#   cmake -S all -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=CIFast ...
exec mktemp -d -p /tmp gtopt-build-XXXX
