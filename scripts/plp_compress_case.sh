#!/usr/bin/env bash
# plp_compress_case.sh — Compress PLP case files before committing to git.
#
# Usage:
#   scripts/plp_compress_case.sh <directory> [--split-mb N]
#
# Compresses all .dat, .csv, .prn, and .png files in <directory> using
# xz -T0 (multi-threaded).  Files larger than N MB (default 10) after
# compression are automatically split into numbered parts
# (foo.dat.1.xz, foo.dat.2.xz, ...).
#
# plp2gtopt reads these compressed/split files transparently via the
# compressed_open module.
#
# Examples:
#   scripts/plp_compress_case.sh support/plp_long_term
#   scripts/plp_compress_case.sh scripts/cases/plp_case_2y --split-mb 8

set -euo pipefail

SPLIT_MB=10
XZ_LEVEL=6

usage() {
    echo "Usage: $0 <directory> [--split-mb N]"
    echo ""
    echo "Compress .dat, .csv, .prn, and .png files in <directory> with xz."
    echo "Files exceeding N MB compressed (default ${SPLIT_MB}) are split."
    echo ""
    echo "Options:"
    echo "  --split-mb N   Max compressed file size in MB (default ${SPLIT_MB})"
    echo "  --help         Show this help"
    exit "${1:-0}"
}

# Parse arguments
if [[ $# -lt 1 ]]; then
    usage 1
fi

DIR="$1"
shift

while [[ $# -gt 0 ]]; do
    case "$1" in
        --split-mb) SPLIT_MB="$2"; shift 2 ;;
        --help)     usage 0 ;;
        *)          echo "Unknown option: $1"; usage 1 ;;
    esac
done

if [[ ! -d "$DIR" ]]; then
    echo "Error: '$DIR' is not a directory" >&2
    exit 1
fi

# Find all compressible files (skip already compressed)
mapfile -t FILES < <(find "$DIR" -maxdepth 1 -type f \
    \( -name '*.dat' -o -name '*.csv' -o -name '*.prn' -o -name '*.png' \) \
    ! -name '*.xz' ! -name '*.gz' ! -name '*.bz2' ! -name '*.zst' ! -name '*.lz4' \
    | sort)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No .dat/.csv/.prn/.png files found in $DIR"
    exit 0
fi

echo "Compressing ${#FILES[@]} file(s) in $DIR (xz -${XZ_LEVEL} -T0, split > ${SPLIT_MB}MB)..."
echo ""

SPLIT_BYTES=$((SPLIT_MB * 1024 * 1024))

# Phase 1: compress all files in parallel
pids=()
for f in "${FILES[@]}"; do
    xz -"${XZ_LEVEL}" -T0 -k -f "$f" &
    pids+=($!)
done
for pid in "${pids[@]}"; do
    wait "$pid"
done

# Phase 2: check sizes, split oversized files
for f in "${FILES[@]}"; do
    fname=$(basename "$f")
    fsize=$(stat -c%s "$f")
    xzfile="${f}.xz"
    xzsize=$(stat -c%s "$xzfile")

    if [[ $xzsize -le $SPLIT_BYTES ]]; then
        printf "  %-40s %6sK -> %6sK  (xz)\n" \
            "$fname" "$((fsize / 1024))" "$((xzsize / 1024))"
    else
        rm "$xzfile"
        echo "  $fname: $((xzsize / 1024))K compressed > $((SPLIT_BYTES / 1024))K limit, splitting..."

        python3 -c "
import math, subprocess
from pathlib import Path

f = Path('$f')
data = f.read_bytes()
lines = data.split(b'\n')

# Estimate parts needed (with 20% margin)
est_ratio = len(data) / $xzsize
target_raw = int($SPLIT_BYTES * est_ratio * 0.8)
n_parts = max(2, math.ceil(len(data) / target_raw))
lines_per_part = math.ceil(len(lines) / n_parts)

raw_paths = []
for i in range(n_parts):
    start = i * lines_per_part
    end = min((i + 1) * lines_per_part, len(lines))
    chunk = b'\n'.join(lines[start:end])
    if i < n_parts - 1:
        chunk += b'\n'
    raw = f.parent / f'{f.name}.{i+1}'
    raw.write_bytes(chunk)
    raw_paths.append(str(raw))

# Compress all parts in parallel using xz -T0
procs = [subprocess.Popen(['xz', '-${XZ_LEVEL}', '-T0', '-f', r]) for r in raw_paths]
for p in procs:
    p.wait()

for i in range(n_parts):
    xz = Path(f'{f}.{i+1}.xz')
    sz = xz.stat().st_size
    print(f'    {xz.name}: {sz//1024}K')
"
    fi

    rm "$f"
done

echo ""
echo "Done. Files are ready to commit."
echo "plp2gtopt reads .xz and split files transparently."
