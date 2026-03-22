# SPDX-License-Identifier: BSD-3-Clause
"""Subprocess invocation for plp2gtopt and gtopt, and post-run reporting."""

from __future__ import annotations

import logging
import subprocess
import sys
from pathlib import Path

log = logging.getLogger(__name__)


def run_plp2gtopt(
    plp2gtopt_bin: str,
    input_dir: Path,
    output_dir: Path,
    extra_args: list[str] | None = None,
) -> int:
    """Run plp2gtopt to convert a PLP case to gtopt format.

    Returns the process exit code.
    """
    cmd = [plp2gtopt_bin, str(input_dir), "-o", str(output_dir)]
    if extra_args:
        cmd.extend(extra_args)

    log.info("running: %s", " ".join(cmd))
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        log.error("plp2gtopt failed with exit code %d", result.returncode)
    return result.returncode


def run_gtopt(
    gtopt_bin: str,
    case_dir: Path,
    threads: int | None = None,
    compression: str | None = None,
    extra_args: list[str] | None = None,
) -> int:
    """Run the gtopt binary on a case directory.

    Returns the process exit code.
    """
    cmd = [gtopt_bin, str(case_dir)]
    if threads is not None and threads > 0:
        cmd.extend(["--lp-threads", str(threads)])
    if compression:
        cmd.extend(["--output-compression", compression])
    if extra_args:
        cmd.extend(extra_args)

    log.info("running: %s", " ".join(cmd))
    result = subprocess.run(cmd, check=False)
    return result.returncode


# ---------------------------------------------------------------------------
# Error LP diagnostics
# ---------------------------------------------------------------------------


def find_error_lp_files(log_dir: Path) -> list[Path]:
    """Find error LP files in the log directory.

    Returns all files matching ``error*.lp*`` (includes compressed
    variants like .lp.gz, .lp.zst), sorted by modification time
    (oldest first).
    """
    if not log_dir.is_dir():
        return []
    candidates = list(log_dir.glob("error*.lp*"))
    candidates.sort(key=lambda f: f.stat().st_mtime)
    return candidates


def run_check_lp(lp_files: list[Path]) -> None:
    """Run gtopt_check_lp on the first (oldest) error LP file.

    Analyses the first error LP to give the user immediate feedback,
    then prints instructions for checking the remaining files manually.
    """
    import shutil  # noqa: PLC0415

    if not lp_files:
        return

    log_dir = lp_files[0].parent
    total = len(lp_files)

    print()
    print("=" * 60)
    print("  run_gtopt — Error LP Diagnostics")
    print("=" * 60)
    print(f"  Found {total} error LP file(s) in {log_dir}")
    print()

    check_bin = shutil.which("gtopt_check_lp")
    if not check_bin:
        print("  gtopt_check_lp not installed — cannot analyse automatically.")
        print("  Install: pip install -e ./scripts")
        print()
    else:
        # Analyse the first (oldest) error LP file
        first_lp = lp_files[0]
        print(f"  Analysing first error LP: {first_lp.name}")
        print()
        cmd = [check_bin, str(first_lp), "--quiet", "--no-ai"]
        log.info("running: %s", " ".join(cmd))
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        output = result.stdout + result.stderr
        for line in output.splitlines():
            print(f"  {line}")
        print()

    # User instructions
    print("  To analyse error LP files manually:")
    print(f"    gtopt_check_lp {lp_files[0]}")
    if total > 1:
        print(f"    gtopt_check_lp {lp_files[1]}   # (and {total - 1} more)")
    print()
    print("  For deeper analysis (IIS, multiple solvers):")
    print(f"    gtopt_check_lp {lp_files[0]} --solver all")
    print()
    print(f"  All error LP files are in: {log_dir}")
    print("=" * 60)
    print()


# ---------------------------------------------------------------------------
# Post-run reporting
# ---------------------------------------------------------------------------


def check_solution(results_dir: Path) -> dict:
    """Parse solution.csv from the results directory.

    Returns a dict with keys: ``status``, ``objective`` (or empty if
    the file is not found).
    """
    sol_path = results_dir / "solution.csv"
    if not sol_path.is_file():
        sol_parquet = results_dir / "solution.parquet"
        if sol_parquet.is_file():
            try:
                import pyarrow.parquet as pq  # noqa: PLC0415

                table = pq.read_table(sol_parquet)
                df = table.to_pandas()
                if "status" in df.columns:
                    status = int(df["status"].iloc[0])
                    obj = (
                        float(df["objective"].iloc[0])
                        if "objective" in df.columns
                        else None
                    )
                    return {"status": status, "objective": obj}
            except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                pass
        return {}

    try:
        with open(sol_path, encoding="utf-8") as f:
            lines = f.readlines()
        if len(lines) >= 2:
            header = [h.strip() for h in lines[0].split(",")]
            values = [v.strip() for v in lines[1].split(",")]
            record = dict(zip(header, values))
            result: dict = {}
            if "status" in record:
                result["status"] = int(record["status"])
            if "objective" in record:
                result["objective"] = float(record["objective"])
            return result
    except (OSError, ValueError) as exc:
        log.warning("failed to parse %s: %s", sol_path, exc)
    return {}


def _count_output_files(results_dir: Path) -> dict[str, int]:
    """Count output files by type in the results directory."""
    counts: dict[str, int] = {}
    if not results_dir.is_dir():
        return counts
    for f in results_dir.rglob("*"):
        if f.is_file():
            ext = f.suffix.lower()
            if ext in (".parquet", ".csv", ".gz", ".zst"):
                counts[ext] = counts.get(ext, 0) + 1
    return counts


def _count_output_classes(results_dir: Path) -> list[str]:
    """List subdirectory names (element classes) in results."""
    if not results_dir.is_dir():
        return []
    return sorted(
        d.name
        for d in results_dir.iterdir()
        if d.is_dir() and not d.name.startswith(".")
    )


def _format_size(nbytes: int) -> str:
    """Human-readable file size."""
    size = float(nbytes)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} {unit}"
        size /= 1024
    return f"{size:.1f} TB"


def _total_dir_size(path: Path) -> int:
    """Sum of all file sizes under *path*."""
    total = 0
    if path.is_dir():
        for f in path.rglob("*"):
            if f.is_file():
                total += f.stat().st_size
    return total


def report_solution(results_dir: Path, gtopt_dir: Path | None = None) -> None:
    """Print a comprehensive post-run report."""
    print()
    print("=" * 60)
    print("  run_gtopt — Results Summary")
    print("=" * 60)

    # ── Solution status ──
    sol = check_solution(results_dir)
    if sol:
        status: int | None = sol.get("status")
        obj: float | None = sol.get("objective")
        status_map = {0: "OPTIMAL", 1: "NON-OPTIMAL / INFEASIBLE"}
        status_str = status_map.get(
            status if status is not None else -1, f"UNKNOWN ({status})"
        )

        if status == 0:
            print(f"  Status     : {status_str}")
            if obj is not None:
                print(f"  Objective  : {obj:.6g}")
        else:
            print(f"  Status     : {status_str}", file=sys.stderr)
    else:
        print("  Status     : no solution file found")

    # ── Output file inventory ──
    file_counts = _count_output_files(results_dir)
    classes = _count_output_classes(results_dir)
    total_size = _total_dir_size(results_dir)

    if file_counts:
        total_files = sum(file_counts.values())
        print(f"  Files      : {total_files} ({_format_size(total_size)})")
        for ext, cnt in sorted(file_counts.items()):
            print(f"               {ext}: {cnt}")
    if classes:
        print(f"  Classes    : {', '.join(classes)}")

    # ── Output location ──
    print(f"  Results in : {results_dir}")
    if gtopt_dir and gtopt_dir != results_dir.parent:
        print(f"  Case dir   : {gtopt_dir}")

    # ── Key output files ──
    key_files = [
        ("solution", "solution"),
        ("Bus/balance_dual", "LMPs (bus marginal prices)"),
        ("Demand/fail_sol", "load shedding"),
        ("Generator/generation_sol", "generator dispatch"),
    ]
    for stem, label in key_files:
        for ext in (".parquet", ".csv", ".csv.gz", ".csv.zst"):
            fpath = results_dir / (stem + ext)
            if fpath.is_file():
                print(f"  {label:25s}: {fpath}")
                break

    print("=" * 60)
    print()
