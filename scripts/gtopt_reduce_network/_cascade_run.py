# SPDX-License-Identifier: BSD-3-Clause
"""Thin orchestration wrapper: reduce → run gtopt on the reduced case →
project results back to the nodal grid.

Pure subprocess driver — no LP smarts. Useful as the v1 path while
``CascadeLevel.system_override`` is unimplemented in C++.
"""

from __future__ import annotations

import logging
import shutil
import subprocess
import sys
from pathlib import Path

logger = logging.getLogger(__name__)


def cascade_run(
    case_path: str | Path,
    *,
    target_buses: int,
    output_dir: str | Path,
    gtopt_bin: str | Path | None = None,
    distance: str = "reactance-shortest-path",
    extra_gtopt_args: list[str] | None = None,
    skip_full: bool = True,
) -> dict[str, Path]:
    """Run the reduce → solve → project pipeline.

    Returns a dict with ``reduced_case``, ``reduced_dir``, ``projected_dir``,
    and (optionally) ``full_dir`` paths.
    """
    from gtopt_reduce_network._busmap import (
        save_aggregator,
        save_busmap,
        save_linemap,
        save_reducer_config,
    )
    from gtopt_reduce_network._io import load_case, save_case
    from gtopt_reduce_network._project_dispatch import project_results
    from gtopt_reduce_network._reduce import ReduceConfig, reduce_case

    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    reduced_path = out / "reduced.json"
    busmap_path = out / "busmap.csv"
    linemap_path = out / "linemap.csv"
    aggregator_path = out / "aggregator_table.csv"
    reducer_config_path = out / "reducer_config.json"

    case = load_case(case_path)
    cfg = ReduceConfig(target_buses=target_buses, distance=distance)
    result = reduce_case(case, cfg)
    save_case(result.case, reduced_path)
    save_busmap(result.busmap, busmap_path)
    save_linemap(result.linemap, linemap_path)
    save_aggregator(result.aggregator, aggregator_path)
    save_reducer_config(cfg.as_dict(), reducer_config_path)

    bin_path = _resolve_gtopt_bin(gtopt_bin)
    reduced_outdir = out / "reduced_out"
    reduced_outdir.mkdir(parents=True, exist_ok=True)
    cmd = [str(bin_path), str(reduced_path), "--output", str(reduced_outdir)]
    if extra_gtopt_args:
        cmd.extend(extra_gtopt_args)
    logger.info("running gtopt on reduced case: %s", " ".join(cmd))
    subprocess.run(cmd, check=True)

    projected_outdir = out / "projected_out"
    project_results(
        reduced_outdir,
        busmap=result.busmap,
        linemap=result.linemap,
        aggregator=result.aggregator,
        output_dir=projected_outdir,
    )

    paths = {
        "reduced_case": reduced_path,
        "reduced_dir": reduced_outdir,
        "projected_dir": projected_outdir,
    }

    if not skip_full:
        full_outdir = out / "full_out"
        full_outdir.mkdir(parents=True, exist_ok=True)
        full_cmd = [str(bin_path), str(case_path), "--output", str(full_outdir)]
        if extra_gtopt_args:
            full_cmd.extend(extra_gtopt_args)
        logger.info("running gtopt on full case: %s", " ".join(full_cmd))
        subprocess.run(full_cmd, check=True)
        paths["full_dir"] = full_outdir

    return paths


def _resolve_gtopt_bin(explicit: str | Path | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
    found = shutil.which("gtopt")
    if found:
        return Path(found)
    # Fall back to repo helper if importable.
    try:
        # pylint: disable=import-outside-toplevel
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        from get_gtopt_binary import resolve_gtopt_bin
    except (ImportError, ModuleNotFoundError) as exc:
        raise FileNotFoundError(
            "gtopt binary not found; pass --gtopt-bin or add it to $PATH"
        ) from exc
    return Path(resolve_gtopt_bin())
