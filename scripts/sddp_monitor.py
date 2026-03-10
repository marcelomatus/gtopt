#!/usr/bin/env python3
"""
sddp_monitor.py — Interactive SDDP solver monitoring dashboard.

Polls the JSON status file written by the gtopt SDDP solver and displays
live line-graphs in two figure windows:

  Figure 1 – Real-time / time-series charts (updated every POLL_INTERVAL s):
      • CPU load (%)          vs wall-clock seconds
      • Active worker threads vs wall-clock seconds

  Figure 2 – Iteration-indexed charts (updated every POLL_INTERVAL s):
      • Objective function upper-bound per scene   vs iteration
      • Objective function lower-bound per scene   vs iteration
      • Convergence gap (UB-LB)/max(1,|UB|)       vs iteration

Usage
-----
  python sddp_monitor.py [--status-file PATH] [--poll SECONDS] [--no-gui]

  --status-file PATH   Path to the sddp_status.json file produced by gtopt.
                       Default: output/sddp_status.json
  --poll SECONDS       Polling interval in seconds (default: 1.0).
  --no-gui             Print status to stdout instead of opening a GUI window
                       (useful in headless / CI environments).

The script exits when the solver reports "converged" or when you press Ctrl-C.

Requirements
------------
  pip install matplotlib
"""

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def load_status(path: Path) -> dict[str, Any] | None:
    """Load and parse the SDDP status JSON file.  Returns None on any error."""
    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text)  # type: ignore[no-any-return]
    except (OSError, json.JSONDecodeError):
        return None


def print_status(data: dict[str, Any]) -> None:
    """Print a brief status summary to stdout."""
    status = data.get("status", "unknown")
    iteration = data.get("iteration", 0)
    gap = data.get("gap", float("nan"))
    lb = data.get("lower_bound", float("nan"))
    ub = data.get("upper_bound", float("nan"))
    elapsed = data.get("elapsed_s", 0.0)
    print(
        f"  [{elapsed:7.1f}s]  iter={iteration:4d}  "
        f"LB={lb:14.4f}  UB={ub:14.4f}  gap={gap:.6f}  [{status}]"
    )


# ---------------------------------------------------------------------------
# GUI mode (matplotlib)
# ---------------------------------------------------------------------------


def run_gui(status_file: Path, poll_interval: float) -> None:
    """Open two matplotlib figure windows and update them interactively."""
    try:
        import matplotlib.pyplot as plt
        import matplotlib.ticker as mticker
    except ImportError:
        print(
            "matplotlib is not installed.  Install it with:\n"
            "    pip install matplotlib\n"
            "Or run with --no-gui to use text-only mode.",
            file=sys.stderr,
        )
        sys.exit(1)

    plt.ion()  # Interactive (non-blocking) mode

    # ── Figure 1: Real-time charts ──────────────────────────────────────────
    fig1, (ax_cpu, ax_workers) = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    fig1.suptitle("SDDP Monitor — Real-time Workpool", fontsize=13)

    (line_cpu,) = ax_cpu.plot(
        [], [], color="tab:red", linewidth=1.5, label="CPU load (%)"
    )
    ax_cpu.set_ylabel("CPU load (%)")
    ax_cpu.set_ylim(0, 105)
    ax_cpu.yaxis.set_major_formatter(mticker.FormatStrFormatter("%g%%"))
    ax_cpu.grid(True, linestyle="--", alpha=0.5)
    ax_cpu.legend(loc="upper left")

    (line_workers,) = ax_workers.plot(
        [], [], color="tab:blue", linewidth=1.5, label="Active workers"
    )
    ax_workers.set_ylabel("Active workers")
    ax_workers.set_xlabel("Wall-clock time (s)")
    ax_workers.grid(True, linestyle="--", alpha=0.5)
    ax_workers.legend(loc="upper left")

    fig1.tight_layout()

    # ── Figure 2: Iteration charts ───────────────────────────────────────────
    fig2, (ax_ub, ax_lb, ax_gap) = plt.subplots(3, 1, figsize=(10, 9), sharex=True)
    fig2.suptitle("SDDP Monitor — Convergence by Iteration", fontsize=13)

    scene_ub_lines: list[Any] = []
    scene_lb_lines: list[Any] = []
    (line_gap,) = ax_gap.plot(
        [], [], color="black", linewidth=2.0, label="Gap (UB-LB)/max(1,|UB|)"
    )
    ax_ub.set_ylabel("Upper bound per scene")
    ax_ub.grid(True, linestyle="--", alpha=0.5)
    ax_lb.set_ylabel("Lower bound per scene")
    ax_lb.grid(True, linestyle="--", alpha=0.5)
    ax_gap.set_ylabel("Convergence gap")
    ax_gap.set_xlabel("Iteration")
    ax_gap.set_yscale("log")
    ax_gap.grid(True, which="both", linestyle="--", alpha=0.5)
    ax_gap.legend(loc="upper right")

    fig2.tight_layout()

    last_iter = -1
    tab_colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]

    print(f"Monitoring {status_file} (poll every {poll_interval:.1f}s)…")
    print("Close either window or press Ctrl-C to stop.\n")

    try:
        while True:
            # Check if windows are still open
            if not plt.fignum_exists(fig1.number) or not plt.fignum_exists(fig2.number):
                print("Window closed — exiting.")
                break

            data = load_status(status_file)
            if data is None:
                plt.pause(poll_interval)
                continue

            print_status(data)

            # ── Update real-time charts ───────────────────────────────────
            rt = data.get("realtime", {})
            ts = rt.get("timestamps", [])
            cpus = rt.get("cpu_loads", [])
            workers = rt.get("active_workers", [])

            if ts:
                line_cpu.set_data(ts, cpus)
                line_workers.set_data(ts, workers)

                ax_cpu.relim()
                ax_cpu.autoscale_view(scaley=False)
                ax_workers.relim()
                ax_workers.autoscale_view(scaley=True)
                ax_workers.set_ylim(bottom=0)
                ax_workers.xaxis.set_major_formatter(
                    mticker.FormatStrFormatter("%.0fs")
                )
                fig1.canvas.draw_idle()

            # ── Update iteration charts ───────────────────────────────────
            history = data.get("history", [])
            if history:
                iterations = [r["iteration"] for r in history]
                gaps = [r.get("gap", float("nan")) for r in history]

                # Determine number of scenes from first result with non-empty
                # scene_upper_bounds
                num_scenes = 0
                for r in history:
                    n = len(r.get("scene_upper_bounds", []))
                    if n > 0:
                        num_scenes = n
                        break

                # Add scene lines if we haven't yet (or if scene count grew)
                while len(scene_ub_lines) < num_scenes:
                    si = len(scene_ub_lines)
                    color = tab_colors[si % len(tab_colors)]
                    (l_ub,) = ax_ub.plot(
                        [], [], color=color, linewidth=1.5, label=f"Scene {si}"
                    )
                    (l_lb,) = ax_lb.plot(
                        [], [], color=color, linewidth=1.5, label=f"Scene {si}"
                    )
                    scene_ub_lines.append(l_ub)
                    scene_lb_lines.append(l_lb)
                if num_scenes > 0 and len(scene_ub_lines) <= num_scenes:
                    ax_ub.legend(loc="upper right", fontsize=8)
                    ax_lb.legend(loc="upper right", fontsize=8)

                # Update per-scene data
                default_bounds = [0.0] * num_scenes
                for si in range(num_scenes):
                    ubs = [
                        r.get("scene_upper_bounds", default_bounds)[si] for r in history
                    ]
                    lbs = [
                        r.get("scene_lower_bounds", default_bounds)[si] for r in history
                    ]
                    scene_ub_lines[si].set_data(iterations, ubs)
                    scene_lb_lines[si].set_data(iterations, lbs)

                # Update gap line
                line_gap.set_data(iterations, [max(g, 1e-10) for g in gaps])

                for ax in (ax_ub, ax_lb, ax_gap):
                    ax.relim()
                    ax.autoscale_view()
                ax_gap.set_xlim(left=1)

                # Annotate convergence tolerance if present
                conv_tol = None
                # (not exposed in the status file yet; add if needed)
                _ = conv_tol

                fig2.canvas.draw_idle()
                last_iter = history[-1]["iteration"]

            status = data.get("status", "running")
            if status == "converged":
                ax_gap.set_title(f"Converged at iteration {last_iter}", color="green")
                fig2.canvas.draw_idle()
                print(
                    f"\nSolver converged at iteration {last_iter}.  Press Ctrl-C to exit."
                )

            plt.pause(poll_interval)

    except KeyboardInterrupt:
        print("\nMonitoring stopped.")

    plt.ioff()


# ---------------------------------------------------------------------------
# Text-only mode
# ---------------------------------------------------------------------------


def run_text(status_file: Path, poll_interval: float) -> None:
    """Poll the status file and print summaries to stdout."""
    print(f"Monitoring {status_file} (poll every {poll_interval:.1f}s)…")
    print("Press Ctrl-C to stop.\n")
    print(
        f"  {'Time':>8s}  {'Iter':>4s}  {'LB':>14s}  {'UB':>14s}  {'Gap':>10s}  Status"
    )
    print("  " + "-" * 70)
    try:
        while True:
            data = load_status(status_file)
            if data is not None:
                print_status(data)
                if data.get("status") == "converged":
                    print("\n  Solver converged.")
                    break
            time.sleep(poll_interval)
    except KeyboardInterrupt:
        print("\nMonitoring stopped.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> None:
    """Parse arguments and start the appropriate monitoring mode."""
    parser = argparse.ArgumentParser(
        description="Interactive SDDP solver monitoring dashboard",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--status-file",
        default="output/sddp_status.json",
        help="Path to the sddp_status.json file (default: output/sddp_status.json)",
    )
    parser.add_argument(
        "--poll",
        type=float,
        default=1.0,
        metavar="SECONDS",
        help="Polling interval in seconds (default: 1.0)",
    )
    parser.add_argument(
        "--no-gui",
        action="store_true",
        help="Print status to stdout instead of opening a GUI window",
    )
    args = parser.parse_args()

    status_file = Path(args.status_file)

    if args.no_gui:
        run_text(status_file, args.poll)
    else:
        run_gui(status_file, args.poll)


if __name__ == "__main__":
    main()
