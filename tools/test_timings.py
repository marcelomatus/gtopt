#!/usr/bin/env python3
"""Per-test timing harness for the gtopt ctest suite.

Runs ctest once with ``--output-junit`` so every individual test case is
timed, then emits a durable, sorted report (and CSV) instead of forcing a
re-run to recover timings.  Doubles as a regression guard: ``--budget`` and
``--slowest`` make the run fail when the suite (or any single test) blows
past a threshold.

Examples
--------
    # Measure the current Release build, keep the timings on disk
    python tools/test_timings.py -B build_release -j 20

    # Guard: fail if wall-clock estimate > 300 s or any test > 45 s
    python tools/test_timings.py -B build --budget 300 --slowest 45

    # Re-parse the last run without re-running ctest
    python tools/test_timings.py --parse-only build_release/test_timings.xml
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


@dataclass
class TestTiming:
    """One ctest / doctest case with its wall time and status."""

    name: str
    seconds: float
    status: str  # "pass" | "fail" | "skip"


def run_ctest(
    build_dir: Path,
    jobs: int,
    junit_path: Path,
    label_regex: str | None,
    exclude_regex: str | None,
) -> int:
    """Run ctest with JUnit output; return the ctest exit code."""
    # ctest chdir's into the build dir internally, so a relative
    # --output-junit path lands in the wrong place; force absolute.
    cmd = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "-j",
        str(jobs),
        "--output-junit",
        str(junit_path.resolve()),
    ]
    if label_regex:
        cmd += ["-L", label_regex]
    if exclude_regex:
        cmd += ["-E", exclude_regex]
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    proc = subprocess.run(cmd, check=False)
    return proc.returncode


def parse_junit(junit_path: Path) -> list[TestTiming]:
    """Parse a ctest JUnit XML file into TestTiming records."""
    tree = ET.parse(junit_path)
    root = tree.getroot()
    timings: list[TestTiming] = []
    for case in root.iter("testcase"):
        name = case.get("name", "<unknown>")
        seconds = float(case.get("time", "0") or "0")
        status = "pass"
        if case.find("failure") is not None or case.find("error") is not None:
            status = "fail"
        elif case.find("skipped") is not None:
            status = "skip"
        timings.append(TestTiming(name, seconds, status))
    return timings


def bucketize(timings: list[TestTiming]) -> dict[str, int]:
    """Count tests in coarse duration buckets."""
    buckets = {">10s": 0, "1-10s": 0, "0.1-1s": 0, "<0.1s": 0}
    for t in timings:
        if t.seconds > 10:
            buckets[">10s"] += 1
        elif t.seconds > 1:
            buckets["1-10s"] += 1
        elif t.seconds > 0.1:
            buckets["0.1-1s"] += 1
        else:
            buckets["<0.1s"] += 1
    return buckets


def report(timings: list[TestTiming], top: int, jobs: int) -> None:
    """Print a sorted slowest-first report and summary stats."""
    ranked = sorted(timings, key=lambda t: t.seconds, reverse=True)
    total = sum(t.seconds for t in timings)
    failed = [t for t in timings if t.status == "fail"]

    print(f"\n=== slowest {top} of {len(timings)} tests ===")
    for t in ranked[:top]:
        flag = "" if t.status == "pass" else f"  [{t.status.upper()}]"
        print(f"{t.seconds:9.2f}s  {t.name}{flag}")

    print("\n=== buckets ===")
    for name, count in bucketize(timings).items():
        print(f"{name:>8}: {count}")

    over_1s = [t for t in timings if t.seconds > 1]
    sum_over_1s = sum(t.seconds for t in over_1s)
    print("\n=== summary ===")
    print(f"tests                : {len(timings)}")
    print(f"serial CPU time      : {total:.1f}s ({total / 60:.1f} min)")
    if over_1s:
        print(
            f">1s tests            : {len(over_1s)} "
            f"({sum_over_1s:.1f}s = {100 * sum_over_1s / total:.0f}% of total)"
        )
    print(
        f"ideal wall @ -j{jobs:<3} : {total / jobs:.1f}s "
        f"(perfect balance; real wall is bounded below by the "
        f"longest test = {ranked[0].seconds:.1f}s)"
    )
    if failed:
        print(f"FAILED               : {len(failed)}")


def write_csv(timings: list[TestTiming], csv_path: Path) -> None:
    """Persist all timings to CSV, slowest first."""
    ranked = sorted(timings, key=lambda t: t.seconds, reverse=True)
    with csv_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["seconds", "status", "name"])
        for t in ranked:
            writer.writerow([f"{t.seconds:.4f}", t.status, t.name])
    print(f"\nwrote {csv_path}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "-B",
        "--build-dir",
        default="build_release",
        help="ctest build directory (default: build_release)",
    )
    parser.add_argument(
        "-j", "--jobs", type=int, default=20, help="parallel ctest jobs (default: 20)"
    )
    parser.add_argument(
        "--top",
        type=int,
        default=30,
        help="how many slowest tests to print (default: 30)",
    )
    parser.add_argument(
        "-L", "--label", default=None, help="only run tests with this label regex"
    )
    parser.add_argument(
        "-E", "--exclude", default=None, help="exclude tests matching this label regex"
    )
    parser.add_argument(
        "--parse-only",
        metavar="XML",
        default=None,
        help="skip the run, just parse an existing JUnit XML",
    )
    parser.add_argument(
        "--budget",
        type=float,
        default=None,
        help="fail if serial CPU time / jobs exceeds this many seconds",
    )
    parser.add_argument(
        "--slowest",
        type=float,
        default=None,
        help="fail if any single test exceeds this many seconds",
    )
    args = parser.parse_args()

    if args.parse_only:
        junit_path = Path(args.parse_only)
        ctest_rc = 0
    else:
        build_dir = Path(args.build_dir)
        junit_path = build_dir / "test_timings.xml"
        ctest_rc = run_ctest(build_dir, args.jobs, junit_path, args.label, args.exclude)

    if not junit_path.exists():
        print(f"error: {junit_path} not found", file=sys.stderr)
        return 2

    timings = parse_junit(junit_path)
    report(timings, args.top, args.jobs)
    write_csv(timings, junit_path.with_suffix(".csv"))

    rc = 0 if ctest_rc == 0 else 1
    total = sum(t.seconds for t in timings)
    if args.budget is not None and total / args.jobs > args.budget:
        print(
            f"\nBUDGET EXCEEDED: ideal wall {total / args.jobs:.1f}s "
            f"> {args.budget:.1f}s",
            file=sys.stderr,
        )
        rc = 1
    if args.slowest is not None:
        offenders = [t for t in timings if t.seconds > args.slowest]
        if offenders:
            print(
                f"\nSLOWEST EXCEEDED: {len(offenders)} test(s) over "
                f"{args.slowest:.1f}s",
                file=sys.stderr,
            )
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
