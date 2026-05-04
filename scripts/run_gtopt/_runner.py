# SPDX-License-Identifier: BSD-3-Clause
"""Subprocess invocation for plp2gtopt and gtopt, and post-run reporting."""

from __future__ import annotations

import logging
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

log = logging.getLogger(__name__)


def _extract_runpath(so_path: Path) -> list[str]:
    """Extract RUNPATH/RPATH entries from a shared library via readelf."""
    try:
        result = subprocess.run(
            ["readelf", "-d", str(so_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        for line in result.stdout.splitlines():
            if "RUNPATH" in line or "RPATH" in line:
                # Format: ... Library runpath: [/opt/coinor/lib:/usr/local/lib]
                start = line.find("[")
                end = line.find("]")
                if 0 <= start < end:
                    paths = line[start + 1 : end].split(":")
                    return [p for p in paths if p and p != "$ORIGIN"]
    except FileNotFoundError:
        pass
    return []


def _build_ld_library_path(gtopt_bin: str) -> str:
    """Build LD_LIBRARY_PATH from the gtopt binary and its plugins.

    Adds directories where solver plugins and their shared-library
    dependencies are expected to live, mirroring the search order used
    by ``SolverRegistry::discover_default_paths`` in the C++ code.

    Also extracts RUNPATH entries from discovered plugin .so files so
    that solver-specific library directories (e.g. /opt/coinor/lib,
    /opt/cplex/lib) are included without manual LD_LIBRARY_PATH setup.
    """
    exe_dir = Path(gtopt_bin).resolve().parent
    candidates = [
        exe_dir / ".." / "lib" / "gtopt" / "plugins",
        exe_dir / ".." / "lib",
        exe_dir / "plugins",
        exe_dir / ".." / "plugins",
        exe_dir,
        Path("/usr/local/lib/gtopt/plugins"),
        Path("/usr/local/lib"),
    ]
    lib_dirs: list[str] = [str(p.resolve()) for p in candidates if p.is_dir()]

    # Extract RUNPATH from plugin .so files to pick up solver lib dirs.
    for cand in candidates:
        if cand.is_dir():
            for so_file in cand.glob("libgtopt_solver_*.so"):
                for rp in _extract_runpath(so_file):
                    rp_resolved = str(Path(rp).resolve())
                    if rp_resolved not in lib_dirs:
                        lib_dirs.append(rp_resolved)

    existing = os.environ.get("LD_LIBRARY_PATH", "")
    if existing:
        lib_dirs.append(existing)
    return ":".join(lib_dirs)


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


def _build_gtopt_cmd(
    gtopt_bin: str,
    case_dir: Path,
    threads: int | None = None,
    compression: str | None = None,
    extra_args: list[str] | None = None,
) -> list[str]:
    """Build the gtopt command line.

    Threads and compression are already baked into the sanitized JSON
    by :func:`sanitize_json`, so they are not passed as CLI flags here.
    """
    cmd = [gtopt_bin, str(case_dir)]
    if extra_args:
        cmd.extend(extra_args)
    return cmd


def _resolve_log_dir(case_dir: Path) -> Path | None:
    """Return the directory where gtopt will write its log files.

    Mirrors the resolution done by ``setup_file_logging`` on the C++
    side: ``<case>/output/logs`` (or ``<case_parent>/...`` if a JSON
    file path was passed instead of a directory).  The directory is
    created so the tail thread can poll it for new files.
    """
    base = case_dir.parent if case_dir.is_file() else case_dir
    log_dir = base / "output" / "logs"
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        return log_dir
    except OSError:
        return None


def _snapshot_gtopt_logs(log_dir: Path) -> set[Path]:
    """Return the set of existing ``gtopt_*.log`` files in @p log_dir.

    Used as a baseline so the tail thread knows which file is the *new*
    one created by the run we're about to launch.
    """
    try:
        return set(log_dir.glob("gtopt_*.log"))
    except OSError:
        return set()


def _wait_for_new_log(
    log_dir: Path,
    baseline: set[Path],
    stop_event: threading.Event,
    poll_interval: float = 0.1,
) -> Path | None:
    """Wait until a new ``gtopt_*.log`` appears in @p log_dir; return it.

    Compares the current directory listing against @p baseline; the
    first novel file is the one the C++ side just opened via
    ``next_numbered_log_path()``.  Returns ``None`` if @p stop_event
    fires before any new file shows up.
    """
    while not stop_event.is_set():
        try:
            current = set(log_dir.glob("gtopt_*.log"))
        except OSError:
            current = set()
        new = current - baseline
        if new:
            # If multiple appeared (very unlikely — only one writer per
            # gtopt run), pick the most recently modified.
            return max(new, key=lambda p: p.stat().st_mtime)
        time.sleep(poll_interval)
    return None


def _tail_log_to(
    log_path: Path,
    consume,
    stop_event: threading.Event,
    poll_interval: float = 0.1,
) -> None:
    """Tail @p log_path line-by-line, calling @p consume(line) for each.

    Returns when @p stop_event is set AND the file has been fully
    drained.  Silently no-ops if the file vanishes or can't be opened.
    """
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            while True:
                line = f.readline()
                if line:
                    consume(line)
                    continue
                if stop_event.is_set():
                    # Drain anything written between our last read and
                    # now, then exit.
                    for tail_line in f:
                        consume(tail_line)
                    return
                time.sleep(poll_interval)
    except OSError:
        return


def _watch_and_tail(
    log_dir: Path,
    baseline: set[Path],
    consume,
    stop_event: threading.Event,
) -> None:
    """Wait for a new gtopt_N.log to appear, then tail it.

    Combines ``_wait_for_new_log`` and ``_tail_log_to`` for use as the
    target of a background tail thread.
    """
    log_path = _wait_for_new_log(log_dir, baseline, stop_event)
    if log_path is None:
        return
    _tail_log_to(log_path, consume, stop_event)


def _run_batch(cmd: list[str], env: dict[str, str], case_dir: Path) -> int:
    """Run gtopt and stream its log file to stdout.

    The C++ binary detects non-TTY stdout (this Popen pipes nothing) and
    writes its spdlog output to ``<case>/output/logs/gtopt_N.log`` via
    ``setup_file_logging`` (with the same ``N`` as the matching
    ``trace_N.log``).  We snapshot the directory before launch, wait for
    the new ``gtopt_N.log`` to appear, then tail it line-by-line and
    forward every line to stdout so an operator running ``run_gtopt``
    from a CI / shell pipe still sees progress in real time.
    """
    log.info("running: %s", " ".join(cmd))
    log_dir = _resolve_log_dir(case_dir)

    if log_dir is None:
        # Couldn't even create the log directory — fall back to letting
        # the subprocess inherit our stdout (gtopt's TTY check will then
        # produce normal stdout output).
        result = subprocess.run(cmd, check=False, env=env)
        return result.returncode

    baseline = _snapshot_gtopt_logs(log_dir)
    stop_event = threading.Event()

    def _emit(line: str) -> None:
        sys.stdout.write(line)
        sys.stdout.flush()

    tail_thread = threading.Thread(
        target=_watch_and_tail,
        args=(log_dir, baseline, _emit, stop_event),
        daemon=True,
    )
    tail_thread.start()

    try:
        # Let stderr pass through unchanged; nothing is captured on
        # stdout because gtopt itself routes spdlog to the log file.
        result = subprocess.run(  # noqa: S603
            cmd, check=False, env=env, stdout=subprocess.DEVNULL
        )
        returncode = result.returncode
    finally:
        stop_event.set()
        tail_thread.join(timeout=2.0)

    return returncode


def _run_interactive(cmd: list[str], env: dict[str, str], case_dir: Path) -> int:
    """Run gtopt with a live Rich terminal dashboard.

    The dashboard runs in a background thread while the solver
    subprocess runs in the foreground.  The solver writes its log
    directly to ``<case>/output/logs/gtopt_N.log`` (via the C++
    file-only sink that activates when stdout is not a TTY — Popen
    pipes nothing here), and a tail thread discovers the new
    ``gtopt_N.log`` and forwards every line into the dashboard's log
    panel.

    Interactive commands (single-key):
      s  Graceful stop — create stop-request file (saves cuts)
      i  Toggle system stats overlay
      h  Toggle help overlay
      q  Quit — terminate the solver process immediately
    """
    import contextlib  # noqa: PLC0415
    import signal  # noqa: PLC0415

    from ._tui import SolverDisplay  # noqa: PLC0415

    case_path = Path(case_dir)
    case_name = case_path.stem if case_path.is_file() else case_path.name
    log_dir = _resolve_log_dir(case_path)

    # Extract --solver from command line for display
    solver_hint = ""
    for i, arg in enumerate(cmd):
        if arg == "--solver" and i + 1 < len(cmd):
            solver_hint = cmd[i + 1]
            break

    display = SolverDisplay(
        case_name=case_name,
        case_dir=case_path,
        solver_hint=solver_hint,
    )
    display.start()

    log.info("running (interactive): %s", " ".join(cmd))

    stop_event = threading.Event()
    tail_thread: threading.Thread | None = None
    if log_dir is not None:
        baseline = _snapshot_gtopt_logs(log_dir)
        tail_thread = threading.Thread(
            target=_watch_and_tail,
            args=(log_dir, baseline, display.add_log_line, stop_event),
            daemon=True,
        )
        tail_thread.start()

    with contextlib.ExitStack() as stack:
        proc = stack.enter_context(
            subprocess.Popen(  # noqa: S603
                cmd,
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
            )
        )
        try:
            # Poll the TUI quit flag while the subprocess runs.  We
            # don't read its stdout — the log is delivered via the tail
            # thread reading the file gtopt writes.
            while True:
                if proc.poll() is not None:
                    break
                if display.quit_requested.is_set():
                    log.info("quit requested via TUI — terminating solver")
                    proc.send_signal(signal.SIGTERM)
                    break
                time.sleep(0.1)
            proc.wait()
        finally:
            stop_event.set()
            if tail_thread is not None:
                tail_thread.join(timeout=2.0)
            display.stop()
            display.print_final(proc.returncode)

        return proc.returncode


def run_gtopt(
    gtopt_bin: str,
    case_dir: Path,
    threads: int | None = None,
    compression: str | None = None,
    extra_args: list[str] | None = None,
) -> int:
    """Run the gtopt binary on a case directory.

    When stdout is an interactive terminal, a Rich live dashboard is
    shown in a background thread.  Otherwise the solver runs as a
    plain subprocess with output flowing to stdout.

    Returns the process exit code.
    """
    from ._tui import is_interactive  # noqa: PLC0415

    cmd = _build_gtopt_cmd(gtopt_bin, case_dir, threads, compression, extra_args)

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = _build_ld_library_path(gtopt_bin)
    log.info("LD_LIBRARY_PATH=%s", env["LD_LIBRARY_PATH"])

    if is_interactive():
        return _run_interactive(cmd, env, case_dir)
    return _run_batch(cmd, env, case_dir)


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


def _read_result_table(results_dir: Path, stem: str):
    """Try to read a result file as a pandas DataFrame.

    Handles three layouts transparently:

    * Legacy single file: ``{stem}.{ext}``.
    * Hive-partitioned parquet directory: ``{stem}.parquet/`` containing
      ``scene=<N>/phase=<M>/part.parquet`` — read as one frame via
      ``pd.read_parquet``.
    * CSV shards: ``{stem}_s*_p*.{csv,csv.zst,csv.gz}`` concatenated in
      sorted order.
    """
    try:
        import pandas as pd  # noqa: PLC0415

        pq_path = results_dir / (stem + ".parquet")
        if pq_path.is_dir() or pq_path.is_file():
            return pd.read_parquet(pq_path)

        parent = results_dir / Path(stem).parent
        name = Path(stem).name
        for ext in (".csv", ".csv.zst", ".csv.gz"):
            shards = sorted(parent.glob(f"{name}_s*_p*{ext}"))
            if shards:
                frames = [pd.read_csv(f) for f in shards]
                return pd.concat(frames, ignore_index=True) if frames else None

            fpath = results_dir / (stem + ext)
            if fpath.is_file():
                return pd.read_csv(fpath)
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        pass
    return None


def _compute_energy_indicators(
    results_dir: Path, planning_json: Path | None
) -> dict[str, float]:
    """Compute quick energy indicators from solver output.

    Returns a dict with keys like ``generated_twh``, ``served_twh``,
    ``shed_twh``, ``scale_objective``, ``discounted_energy_twh``.
    Missing indicators are omitted.
    """
    indicators: dict[str, float] = {}
    try:
        import json as _json  # noqa: PLC0415
        import numpy as np  # noqa: PLC0415

        import math  # noqa: PLC0415

        _HOURS_PER_YEAR = 8766.0  # 365.25 × 24, matching C++ hours_per_year

        # Load simulation structure from planning JSON
        durations: dict[int, float] = {}
        block_to_stage: dict[int, int] = {}
        # Effective discount factor per stage: combines the JSON
        # discount_factor with the annual discount rate applied at the
        # stage's start time (matching the C++ StageLp constructor).
        stage_effective_discount: dict[int, float] = {}
        scale_obj = 1000.0
        if planning_json and planning_json.is_file():
            with open(planning_json, encoding="utf-8") as f:
                data = _json.load(f)
            blocks = data.get("simulation", {}).get("block_array", [])
            for b in blocks:
                durations[b["uid"]] = b.get("duration", 1.0)

            stages = data.get("simulation", {}).get("stage_array", [])
            block_uids = [b["uid"] for b in blocks]

            # Annual discount rate from options (same lookup as C++)
            opts = data.get("options", {})
            annual_rate = opts.get("annual_discount_rate", 0.0) or 0.0
            model_rate = opts.get("model_options", {}).get("annual_discount_rate", 0.0)
            if annual_rate == 0.0 and model_rate:
                annual_rate = model_rate

            # Build block → stage mapping and effective discount factors.
            # timeinit = cumulative hours from the start of the horizon
            # to the beginning of each stage (sum of block durations
            # for all blocks before this stage's first block).
            cumulative_hours = [0.0] * (len(block_uids) + 1)
            for i, b in enumerate(blocks):
                cumulative_hours[i + 1] = cumulative_hours[i] + b.get("duration", 1.0)

            for st in stages:
                first = st.get("first_block", 0)
                count = st.get("count_block", -1)
                json_df = st.get("discount_factor", 1.0) or 1.0
                end = first + count if count > 0 else len(block_uids)

                # C++ formula: exp(-ln(1+r) × timeinit / hours_per_year)
                timeinit = (
                    cumulative_hours[first] if first < len(cumulative_hours) else 0.0
                )
                if annual_rate > 0:
                    annual_df = math.exp(
                        -math.log1p(annual_rate) * timeinit / _HOURS_PER_YEAR
                    )
                else:
                    annual_df = 1.0

                stage_effective_discount[st["uid"]] = annual_df * json_df

                for bi in range(first, min(end, len(block_uids))):
                    block_to_stage[block_uids[bi]] = st["uid"]

            scale_obj = opts.get("scale_objective", 1000.0) or 1000.0
        indicators["scale_objective"] = scale_obj

        def _energy_twh(df) -> float:
            """Sum uid:* columns weighted by block duration → TWh."""
            uid_cols = [c for c in df.columns if c.startswith("uid:")]
            if not uid_cols or "block" not in df.columns:
                return 0.0
            if "scenario" in df.columns:
                df = df.groupby("block", as_index=False)[uid_cols].mean()
            total = 0.0
            for _, row in df.iterrows():
                dur = durations.get(int(row["block"]), 1.0)
                total += float(np.sum(row[uid_cols].to_numpy(dtype=np.float64))) * dur
            return total / 1e6  # MWh → TWh

        def _discounted_energy_twh(df) -> float:
            """Sum uid:* columns × duration × effective_discount → TWh.

            The effective discount factor per stage combines the JSON
            ``discount_factor`` with ``exp(-ln(1+r)×t/8766)`` from the
            ``annual_discount_rate``, matching the C++ LP coefficients.
            """
            uid_cols = [c for c in df.columns if c.startswith("uid:")]
            if not uid_cols or "block" not in df.columns:
                return 0.0
            if "scenario" in df.columns:
                df = df.groupby("block", as_index=False)[uid_cols].mean()
            total = 0.0
            for _, row in df.iterrows():
                blk = int(row["block"])
                dur = durations.get(blk, 1.0)
                stage_uid = block_to_stage.get(blk)
                eff_df = (
                    stage_effective_discount.get(stage_uid, 1.0)
                    if stage_uid is not None
                    else 1.0
                )
                mw = float(np.sum(row[uid_cols].to_numpy(dtype=np.float64)))
                total += mw * dur * eff_df
            return total / 1e6  # MWh → TWh

        # Generated energy
        gen_df = _read_result_table(results_dir, "Generator/generation_sol")
        if gen_df is not None:
            indicators["generated_twh"] = _energy_twh(gen_df)
            indicators["discounted_energy_twh"] = _discounted_energy_twh(gen_df)

        # Served demand
        load_df = _read_result_table(results_dir, "Demand/load_sol")
        if load_df is not None:
            indicators["served_twh"] = _energy_twh(load_df)

        # Load shedding
        fail_df = _read_result_table(results_dir, "Demand/fail_sol")
        if fail_df is not None:
            shed = _energy_twh(fail_df)
            if shed > 1e-6:
                indicators["shed_twh"] = shed

    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        log.debug("energy indicator computation failed", exc_info=True)

    return indicators


def report_solution(
    results_dir: Path,
    gtopt_dir: Path | None = None,
    planning_json: Path | None = None,
) -> None:
    """Print a Rich-formatted post-run results summary."""
    try:
        from gtopt_check_json._terminal import (  # noqa: PLC0415
            print_table,
        )
    except ImportError:
        # Fallback: plain text
        _report_solution_plain(results_dir, gtopt_dir)
        return

    # ── Solution status ──
    sol = check_solution(results_dir)
    status: int | None = sol.get("status")
    obj: float | None = sol.get("objective")
    status_map = {0: "OPTIMAL", 1: "NON-OPTIMAL / INFEASIBLE"}
    status_str = status_map.get(
        status if status is not None else -1, f"UNKNOWN ({status})"
    )

    # ── Energy indicators (fast) ──
    indicators = _compute_energy_indicators(results_dir, planning_json)
    scale_obj = indicators.get("scale_objective", 1000.0)

    # Scale objective back to real $
    true_obj: float | None = None
    if obj is not None:
        true_obj = obj * scale_obj

    # ── Build rows ──
    rows: list[tuple[str, str]] = []
    rows.append(("Status", status_str))
    if true_obj is not None:
        rows.append(("Objective value", f"${true_obj:,.2f}"))

    if "generated_twh" in indicators:
        rows.append(("Generated energy", f"{indicators['generated_twh']:.3f} TWh"))
    if "served_twh" in indicators:
        rows.append(("Served demand", f"{indicators['served_twh']:.3f} TWh"))
    if "shed_twh" in indicators:
        rows.append(("Load shedding", f"{indicators['shed_twh']:.3f} TWh"))

    # Avg generation cost = total cost / generated energy
    gen_twh = indicators.get("generated_twh", 0.0)
    if true_obj is not None and gen_twh > 0:
        gen_mwh = gen_twh * 1e6
        avg_cost = true_obj / gen_mwh
        rows.append(("Avg generation cost", f"${avg_cost:.2f}/MWh"))

    # LCOE = discounted total cost / discounted generated energy
    # The solver objective IS the sum of discounted costs (each stage's
    # cost is already multiplied by discount_factor in the LP).
    disc_twh = indicators.get("discounted_energy_twh", 0.0)
    if true_obj is not None and disc_twh > 0:
        disc_mwh = disc_twh * 1e6
        lcoe = true_obj / disc_mwh
        rows.append(("LCOE", f"${lcoe:.2f}/MWh"))

    # ── File inventory ──
    file_counts = _count_output_files(results_dir)
    total_size = _total_dir_size(results_dir)
    if file_counts:
        total_files = sum(file_counts.values())
        ext_str = ", ".join(f"{ext}: {cnt}" for ext, cnt in sorted(file_counts.items()))
        rows.append(("Output files", f"{total_files} ({_format_size(total_size)})"))
        rows.append(("  by type", ext_str))

    classes = _count_output_classes(results_dir)
    if classes:
        rows.append(("Element classes", ", ".join(classes)))

    # ── Paths ──
    rows.append(("Results", str(results_dir)))
    log_path = results_dir / "logs" / "gtopt.log"
    if log_path.is_file():
        rows.append(("Log file", str(log_path)))

    print_table(
        headers=["Property", "Value"],
        rows=rows,
        aligns=["left", "right"],
        title="Results Summary",
    )


def _report_solution_plain(results_dir: Path, gtopt_dir: Path | None = None) -> None:
    """Fallback plain-text results summary."""
    sol = check_solution(results_dir)
    status_map = {0: "OPTIMAL", 1: "NON-OPTIMAL / INFEASIBLE"}
    status: int | None = sol.get("status")
    status_str = (
        status_map.get(status, f"UNKNOWN ({status})")
        if status is not None
        else "UNKNOWN (None)"
    )
    print(f"  Status: {status_str}")
    obj = sol.get("objective")
    if obj is not None:
        print(f"  Objective: {obj:.6g}")
    print(f"  Results: {results_dir}")
