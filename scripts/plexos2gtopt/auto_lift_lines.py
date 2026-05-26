# SPDX-License-Identifier: BSD-3-Clause
"""Auto-detect lines that need ``enforce_level = 0``.

Runs a pandapower DC OPF on the FIRST (scenario, block) of a gtopt
bundle with **all line caps lifted**, then flags any line whose
resulting magnitude exceeds its original ``tmax_ab`` rating by more
than ``threshold`` (default 1.0 = strictly above rated cap).

Rationale: a radial line that the OPF "wants" to drive above its
rated cap is a structural over-cap line — typically a step-down to a
load pocket with no parallel transmission path.  Enforcing such a cap
in gtopt causes infeasibility (unserved demand at the receiver bus)
when the source-of-truth dispatch (PLEXOS, real-world operations)
treats the limit as voltage-conditional and ships flow above it
anyway.  Setting ``enforce_level = 0`` on these lines lets gtopt
mirror PLEXOS's actual cap-violating dispatch while preserving the
PWL-loss segment discretization (which still needs ``tmax_ab`` to
size its envelope).

The default ``--lift-line-caps`` flag in :mod:`plexos2gtopt.main`
hard-codes one such line for the CEN PCP daily bundle
(``Capricornio110->LaNegra110``).  This module derives the full set
from the data instead of relying on operator knowledge.
"""

from __future__ import annotations

import json
import logging
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

# How far above the rated cap a line must run in the unconstrained OPF
# before we declare it overloaded.  1.0 = strictly above rating.
DEFAULT_THRESHOLD = 1.0

# Multiplicative factor applied to every line's max_i_ka before the
# DC OPF runs.  Large enough that the OPF treats lines as unbounded
# (so we see the "natural" cost-minimising flow distribution), small
# enough not to numerically explode pandapower's PTDF matrix.
_LIFT_MULT = 1_000.0


@dataclass(frozen=True)
class OverloadedLine:
    """One line that exceeded its rated cap in the lifted-OPF run.

    Fields:
      name:     Line.name from the bundle JSON.
      flow_mw:  |MW| at the from-bus in the unconstrained DC OPF.
      rated_mw: Rated cap recovered from ``tmax_ab`` (via
                ``max_i_ka × from_kv × √3``).
      ratio:    ``flow_mw / rated_mw`` (≥ ``threshold`` to be listed).
    """

    name: str
    flow_mw: float
    rated_mw: float
    ratio: float


def detect_overloaded_lines(
    bundle_json_path: Path,
    *,
    threshold: float = DEFAULT_THRESHOLD,
    scenario_uid: int | None = None,
    block_uid: int | None = None,
) -> list[OverloadedLine]:
    """Run lifted-cap DC OPF on the first (scenario, block) and flag
    lines exceeding ``threshold × rated_cap``.

    Returns the sorted list of :class:`OverloadedLine` (worst ratio
    first).  Empty list when no line exceeds the threshold OR when
    pandapower/the OPF aren't available (the caller treats that as
    "no auto-lift signal" and falls back to the explicit
    ``--lift-line-caps`` list).
    """
    try:
        import pandapower as pp  # pylint: disable=import-outside-toplevel
    except ImportError:
        logger.warning(
            "pandapower unavailable; skipping auto-lift detection.  "
            "Install with `pip install pandapower` to enable."
        )
        return []

    try:
        # Reuse the bundle → pandapower converter shipped with gtopt2pp.
        # Avoids re-implementing all the unit conversions, bus mapping,
        # and generator profile resolution.
        # pylint: disable=import-outside-toplevel
        from gtopt2pp.convert import convert, load_gtopt_case
    except ImportError:
        logger.warning("gtopt2pp.convert unavailable; skipping auto-lift detection.")
        return []

    case = load_gtopt_case(bundle_json_path)

    # Sanity guard: detect_overloaded_lines is called BEFORE the bundle
    # is consumed by gtopt, so the JSON is in the converter's output
    # directory but the parquet input/output files don't exist yet.
    # gtopt2pp.convert's ``_resolve_field_sched_with_file`` swallows
    # missing files and falls back to the inline matrix in the JSON,
    # which is what CEN PCP ships (all schedules are inline matrices).
    # No further setup required.
    net = convert(case, scenario=scenario_uid, block=block_uid)

    # ── Lift line caps ─────────────────────────────────────────────────
    # Multiplying max_i_ka rather than overwriting preserves zeros
    # (lines that the converter already marked as "no rating").
    # Pandapower's DC OPF reads max_i_ka × from_bus.vn_kv × sqrt(3) as
    # the MVA limit; multiplying max_i_ka by 1000 effectively releases
    # the constraint without making it numerically dangerous.
    rated_max_i_ka = net.line["max_i_ka"].copy()
    net.line.loc[:, "max_i_ka"] = rated_max_i_ka * _LIFT_MULT

    # ── Run DC OPF ─────────────────────────────────────────────────────
    try:
        pp.rundcopp(net)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "pandapower DC OPF failed (%s); skipping auto-lift detection.",
            exc,
        )
        return []

    if net.res_line.empty:
        logger.warning(
            "pandapower DC OPF returned no line results; skipping auto-lift."
        )
        return []

    # ── Post-process: compare resulting flow to ORIGINAL rated cap ────
    out: list[OverloadedLine] = []
    for idx in net.line.index:
        rated_i = rated_max_i_ka.iloc[idx]
        if rated_i <= 0:
            continue
        from_bus = int(net.line.at[idx, "from_bus"])
        vn_kv = float(net.bus.at[from_bus, "vn_kv"])
        # Rated MVA ≈ rated MW for DC (cosφ=1).
        rated_mw = rated_i * vn_kv * math.sqrt(3.0)
        flow_mw = abs(float(net.res_line.at[idx, "p_from_mw"]))
        ratio = flow_mw / rated_mw if rated_mw > 0 else 0.0
        if ratio >= threshold:
            out.append(
                OverloadedLine(
                    name=str(net.line.at[idx, "name"]),
                    flow_mw=flow_mw,
                    rated_mw=rated_mw,
                    ratio=ratio,
                )
            )

    out.sort(key=lambda r: r.ratio, reverse=True)
    return out


def detect_overloaded_lines_via_gtopt(
    bundle_json_path: Path,
    *,
    threshold: float = DEFAULT_THRESHOLD,
    block_uid: int | None = None,
    gtopt_binary: str | Path | None = None,
    time_limit_s: int = 300,
) -> list[OverloadedLine]:
    """Same detection as :func:`detect_overloaded_lines`, but uses
    ``gtopt --no-mip`` (LP-relaxation) as the OPF engine instead of
    pandapower's ``rundcopp``.

    Avoids the pandapower / gtopt2pp Python dependency at the cost of
    one extra gtopt solve.  Trades ~10 s pandapower DC OPF for ~30 s
    gtopt LP-relax on CEN PCP weekly, but uses the SAME LP that
    downstream consumers will run — guaranteeing the line ratings,
    voltage assumptions, and PWL loss model match exactly.

    Pipeline:
      1. Materialise a sibling JSON (``<stem>_autolift.json`` next to
         the source bundle) with ``enforce_level = 0`` on every line.
         Sharing the parent dir means the LP can resolve any
         ``input_directory`` parquet schedules with no copy/symlink.
      2. Run ``gtopt -s <lifted.json> --no-mip --time-limit T`` with
         output in a fresh temp dir.
      3. Read ``output/Line/flowp_sol.parquet`` and ``flown_sol.parquet``,
         filter to the requested block (default = first), compute
         ``|flowp| + |flown|`` per line, compare to original
         ``tmax_ab`` from the source JSON.
      4. Flag any line where ``flow / rated ≥ threshold``.

    Returns an empty list when gtopt is unavailable or the solve fails.
    """
    import os
    import shutil
    import subprocess
    import tempfile

    # Locate the gtopt binary: explicit > $GTOPT_BIN env > $PATH.
    if gtopt_binary is None:
        gtopt_binary = os.environ.get("GTOPT_BIN") or shutil.which("gtopt")
    if not gtopt_binary or not Path(gtopt_binary).is_file():
        logger.warning(
            "gtopt binary not found (set GTOPT_BIN or pass `gtopt_binary`); "
            "skipping gtopt-engine auto-lift detection."
        )
        return []

    # Load source bundle + record original ratings BEFORE lifting.
    source = json.loads(bundle_json_path.read_text())
    src_lines = source.get("system", {}).get("line_array", [])
    if not src_lines:
        return []

    def _scalarise_tmax(value: Any) -> float:
        """Reduce ``tmax_ab`` (scalar or per-block matrix) to a single
        ``rated_mw`` for the comparison.  Per-block matrices are
        collapsed to their MAX (most permissive across the horizon —
        any line that exceeds even its peak rating is overloaded).
        Returns 0 when the field is unset."""
        if value is None:
            return 0.0
        if isinstance(value, (int, float)):
            return float(value)
        if isinstance(value, list) and value:
            inner = value[0] if isinstance(value[0], list) else value
            return max((float(x) for x in inner), default=0.0)
        return 0.0

    rated_by_name: dict[str, float] = {}
    for ln in src_lines:
        name = ln.get("name")
        rated = _scalarise_tmax(ln.get("tmax_ab"))
        if name and rated > 0:
            rated_by_name[name] = rated
    if not rated_by_name:
        return []

    # Build the lifted JSON in the SAME directory as the source so any
    # parquet ``input_directory`` references still resolve.  Use a
    # deterministic name so a stale lifted file from a prior run is
    # overwritten in place.
    lifted_path = bundle_json_path.with_name(bundle_json_path.stem + "_autolift.json")
    lifted = json.loads(bundle_json_path.read_text())
    for ln in lifted.get("system", {}).get("line_array", []):
        # Demote every line that carries a finite cap to EL=0 so the
        # LP solves the unconstrained network; leave already-EL=0
        # lines (no tmax_ab) alone — they're already unbounded.
        if _scalarise_tmax(ln.get("tmax_ab")) > 0:
            ln["enforce_level"] = 0
    lifted_path.write_text(json.dumps(lifted, indent=2))

    # Force LP-relaxation via ``--no-mip`` so commitments / segment
    # PWL stay continuous and the solve is fast (~30 s on CEN PCP
    # weekly vs ~10 min for the full MIP).
    out_dir = Path(tempfile.mkdtemp(prefix="plexos2gtopt_autolift_"))
    cmd = [
        str(gtopt_binary),
        "-s",
        str(lifted_path),
        "--no-mip",
        "--time-limit",
        str(int(time_limit_s)),
        "--set",
        f"output_directory={out_dir}",
    ]
    logger.info("auto-lift (gtopt-engine): running %s", " ".join(cmd))
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, check=False, timeout=time_limit_s + 60
        )
    except subprocess.TimeoutExpired:
        logger.warning("auto-lift (gtopt-engine): solve timed out; skipping.")
        return []
    except OSError as exc:
        logger.warning(
            "auto-lift (gtopt-engine): failed to launch gtopt (%s); skipping.",
            exc,
        )
        return []
    if proc.returncode != 0:
        logger.warning(
            "auto-lift (gtopt-engine): gtopt exited with code %d; skipping.\n"
            "stderr tail: %s",
            proc.returncode,
            (proc.stderr or "")[-400:],
        )
        return []

    # Parse the first-block flow per line from the output parquet.
    try:
        import pyarrow.parquet as pq  # pylint: disable=import-outside-toplevel
    except ImportError:
        logger.warning("pyarrow unavailable; cannot parse gtopt output for auto-lift.")
        return []

    name_by_uid = {int(ln["uid"]): ln["name"] for ln in src_lines if "uid" in ln}
    # Pick the block to inspect — default to the first block_uid in the bundle.
    first_block = None
    sim = source.get("simulation", {})
    block_array = sim.get("block_array", [])
    if block_array:
        first_block = int(block_array[0].get("uid", 1))
    blk = block_uid if block_uid is not None else first_block
    if blk is None:
        logger.warning("auto-lift (gtopt-engine): no block layout found; skipping.")
        return []

    def _sum_flow_at_block(rel: str) -> dict[int, float]:
        # ``--set output_directory=...`` makes gtopt write the class
        # subdirs (Line/, Generator/) directly under ``out_dir``, with
        # no intermediate ``output/`` parent — that level only exists
        # when output_directory is the default ``<case>/output``.  Try
        # both layouts so the same code works either way.
        for cand in (out_dir / rel, out_dir / "output" / rel):
            if cand.exists():
                df = pq.read_table(cand).to_pandas()
                sub = df[df["block"].astype(int) == int(blk)]
                return {
                    int(r["uid"]): abs(float(r["value"])) for _, r in sub.iterrows()
                }
        return {}

    flowp = _sum_flow_at_block("Line/flowp_sol.parquet")
    flown = _sum_flow_at_block("Line/flown_sol.parquet")

    out: list[OverloadedLine] = []
    for uid, name in name_by_uid.items():
        rated_mw = rated_by_name.get(name, 0.0)
        if rated_mw <= 0:
            continue
        flow_mw = flowp.get(uid, 0.0) + flown.get(uid, 0.0)
        ratio = flow_mw / rated_mw if rated_mw > 0 else 0.0
        if ratio >= threshold:
            out.append(
                OverloadedLine(
                    name=name,
                    flow_mw=flow_mw,
                    rated_mw=rated_mw,
                    ratio=ratio,
                )
            )

    out.sort(key=lambda r: r.ratio, reverse=True)
    return out


def patch_bundle_with_lifts(
    bundle_json_path: Path,
    overloaded: list[OverloadedLine],
) -> int:
    """Post-process the bundle JSON to set ``enforce_level = 0`` on
    every line listed in ``overloaded``.  Idempotent: lines already
    at EL=0 are left untouched, lines at EL=1/2 are demoted.

    Returns the number of lines patched.
    """
    if not overloaded:
        return 0

    by_name = {ol.name: ol for ol in overloaded}
    bundle: dict[str, Any] = json.loads(bundle_json_path.read_text())
    lines = bundle.get("system", {}).get("line_array", [])
    n_patched = 0
    for line in lines:
        name = line.get("name")
        if name not in by_name:
            continue
        prev = line.get("enforce_level", 2)
        if prev == 0:
            continue
        line["enforce_level"] = 0
        n_patched += 1
        logger.info(
            "auto-lift: '%s' (flow=%.1f MW vs rated=%.1f MW, %.2fx) "
            "demoted from EL=%s to EL=0",
            name,
            by_name[name].flow_mw,
            by_name[name].rated_mw,
            by_name[name].ratio,
            prev,
        )
    if n_patched:
        bundle_json_path.write_text(json.dumps(bundle, indent=2))
    return n_patched
