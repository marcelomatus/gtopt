"""Generate a text report from a Python pstats file.

Usage::

    python3 generate_pstats_report.py <pstats_file> <output_txt> [n_functions]

Produces a cumulative-time-sorted text profile report with the top N
functions (default 30).  This helper is invoked by the
``profiling-python`` CMake target in ``profiling/CMakeLists.txt``.
"""

import io
import pstats
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <pstats_file> <output_txt> [n_functions]")
        return 1

    pstats_file = sys.argv[1]
    output_txt = sys.argv[2]
    n_functions = int(sys.argv[3]) if len(sys.argv) > 3 else 30

    try:
        buf = io.StringIO()
        stats = pstats.Stats(pstats_file, stream=buf)
        stats.sort_stats("cumulative")
        stats.print_stats(n_functions)
        report = buf.getvalue()
    except Exception as exc:  # noqa: BLE001
        print(f"Error reading pstats file '{pstats_file}': {exc}", file=sys.stderr)
        return 1

    with open(output_txt, "w", encoding="utf-8") as fh:
        fh.write(report)

    print(f"Profile report written to: {output_txt}")
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
