#!/usr/bin/env bash
# plp_compress_case.sh — Compress or decompress PLP case files.
#
# Usage:
#   scripts/plp_compress_case.sh <directory> [options]
#   scripts/plp_compress_case.sh --decompress <directory>
#
# Compresses all .dat, .csv, .prn, and .png files in <directory> using
# xz -T0 (multi-threaded).  Files larger than N MB (default 10) after
# compression are automatically split into numbered parts
# (foo.dat.1.xz, foo.dat.2.xz, ...).
#
# With --decompress, restores the original uncompressed files from .xz
# (including reassembling split parts).
#
# plp2gtopt reads compressed/split files transparently via the
# compressed_open module.
#
# Examples:
#   scripts/plp_compress_case.sh support/plp_long_term
#   scripts/plp_compress_case.sh scripts/cases/plp_case_2y --split-mb 8
#   scripts/plp_compress_case.sh --decompress support/plp_long_term

set -euo pipefail

SPLIT_MB=10
XZ_LEVEL=6
MODE=compress

usage() {
    echo "Usage: $0 [--decompress] <directory> [--split-mb N]"
    echo ""
    echo "Compress or decompress .dat, .csv, .prn, and .png files."
    echo ""
    echo "Modes:"
    echo "  (default)      Compress files with xz -T0"
    echo "  --decompress   Restore original files from .xz (reassembles splits)"
    echo ""
    echo "Options:"
    echo "  --split-mb N   Max compressed file size in MB (default ${SPLIT_MB})"
    echo "  --help         Show this help"
    exit "${1:-0}"
}

# Parse arguments
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --decompress|-d) MODE=decompress; shift ;;
        --split-mb)      SPLIT_MB="$2"; shift 2 ;;
        --help|-h)       usage 0 ;;
        -*)              echo "Unknown option: $1"; usage 1 ;;
        *)               POSITIONAL+=("$1"); shift ;;
    esac
done

if [[ ${#POSITIONAL[@]} -lt 1 ]]; then
    usage 1
fi

DIR="${POSITIONAL[0]}"

if [[ ! -d "$DIR" ]]; then
    echo "Error: '$DIR' is not a directory" >&2
    exit 1
fi

# ── Decompress mode ─────────────────────────────────────────────────────

decompress_case() {
    # Find all .xz files (single and split parts)
    mapfile -t XZ_FILES < <(find "$DIR" -maxdepth 1 -name '*.xz' -type f | sort)

    if [[ ${#XZ_FILES[@]} -eq 0 ]]; then
        # Check if there are already uncompressed data files
        n_plain=$(find "$DIR" -maxdepth 1 -type f \
            \( -name '*.dat' -o -name '*.csv' -o -name '*.prn' -o -name '*.png' \) \
            ! -name '*.xz' ! -name '*.gz' ! -name '*.bz2' ! -name '*.zst' ! -name '*.lz4' \
            | wc -l)
        if [[ $n_plain -gt 0 ]]; then
            echo "$DIR: already decompressed ($n_plain data file(s))"
        else
            echo "No .xz or data files found in $DIR"
        fi
        exit 0
    fi

    # Identify split files: group by base name (foo.dat.N.xz -> foo.dat)
    declare -A SPLIT_BASES=()
    declare -a SINGLE_FILES=()

    for f in "${XZ_FILES[@]}"; do
        fname=$(basename "$f")
        # Check if this is a split part: name.N.xz where N is a number
        if [[ "$fname" =~ ^(.+)\.([0-9]+)\.xz$ ]]; then
            base="${BASH_REMATCH[1]}"
            SPLIT_BASES["$base"]=1
        else
            SINGLE_FILES+=("$f")
        fi
    done

    n_single=${#SINGLE_FILES[@]}
    n_split=${#SPLIT_BASES[@]}
    echo "Decompressing in $DIR: ${n_single} single file(s), ${n_split} split file(s) (xz -T0)..."
    echo ""

    # Phase 1: decompress single .xz files in parallel
    if [[ $n_single -gt 0 ]]; then
        pids=()
        for f in "${SINGLE_FILES[@]}"; do
            xz -d -T0 -f "$f" &
            pids+=($!)
        done
        for pid in "${pids[@]}"; do
            wait "$pid"
        done

        for f in "${SINGLE_FILES[@]}"; do
            # Output name: strip .xz suffix
            out="${f%.xz}"
            outname=$(basename "$out")
            if [[ -f "$out" ]]; then
                sz=$(stat -c%s "$out")
                printf "  %-40s %6sK\n" "$outname" "$((sz / 1024))"
            fi
        done
    fi

    # Phase 2: reassemble split files in parallel
    for base in $(echo "${!SPLIT_BASES[@]}" | tr ' ' '\n' | sort); do
        outfile="${DIR}/${base}"
        echo "  ${base}: reassembling split parts..."

        # Collect parts in order (sort numerically by part number)
        mapfile -t PARTS < <(find "$DIR" -maxdepth 1 -name "${base}.[0-9]*.xz" -type f \
            | python3 -c "
import sys, re
lines = [l.strip() for l in sys.stdin if l.strip()]
lines.sort(key=lambda p: int(re.search(r'\.(\d+)\.xz$', p).group(1)))
print('\n'.join(lines))
")

        # Decompress all parts in parallel
        pids=()
        for p in "${PARTS[@]}"; do
            xz -d -T0 -f "$p" &
            pids+=($!)
        done
        for pid in "${pids[@]}"; do
            wait "$pid"
        done

        # Concatenate decompressed parts in order
        : > "$outfile"
        for p in "${PARTS[@]}"; do
            raw="${p%.xz}"
            cat "$raw" >> "$outfile"
            rm "$raw"
        done

        sz=$(stat -c%s "$outfile")
        printf "    -> %-38s %6sK  (%d parts)\n" \
            "$base" "$((sz / 1024))" "${#PARTS[@]}"
    done

    echo ""
    echo "Done. Original files restored."
}

# ── Compress mode ────────────────────────────────────────────────────────

compress_case() {
    # Find all compressible files (skip already compressed)
    mapfile -t FILES < <(find "$DIR" -maxdepth 1 -type f \
        \( -name '*.dat' -o -name '*.csv' -o -name '*.prn' -o -name '*.png' \) \
        ! -name '*.xz' ! -name '*.gz' ! -name '*.bz2' ! -name '*.zst' ! -name '*.lz4' \
        | sort)

    if [[ ${#FILES[@]} -eq 0 ]]; then
        # Check if there are already compressed files
        n_xz=$(find "$DIR" -maxdepth 1 -name '*.xz' -type f | wc -l)
        if [[ $n_xz -gt 0 ]]; then
            echo "$DIR: already compressed ($n_xz .xz file(s))"
        else
            echo "No .dat/.csv/.prn/.png files found in $DIR"
        fi
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
}

# ── Dispatch ─────────────────────────────────────────────────────────────

case "$MODE" in
    compress)   compress_case ;;
    decompress) decompress_case ;;
esac
