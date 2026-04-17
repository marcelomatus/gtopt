# SPDX-License-Identifier: BSD-3-Clause
"""Read gtopt output and input files into pandas DataFrames."""

from __future__ import annotations

import json
import logging
from pathlib import Path

import pandas as pd

log = logging.getLogger(__name__)


def read_table(directory: Path, stem: str) -> pd.DataFrame | None:
    """Read a parquet or CSV file from *directory*/*stem*.{ext}.

    Handles two layouts transparently:

    * Single-file: ``{stem}.{ext}`` — the legacy layout.
    * Per-(scene, phase) shards: ``{stem}_s*_p*.{ext}`` — concatenated
      into one frame in scene-then-phase order.

    Shards are preferred when present.  Tries parquet first, then csv
    variants.  Returns ``None`` if nothing matches.
    """
    parent = directory / Path(stem).parent
    name = Path(stem).name
    for ext in (".parquet", ".csv", ".csv.zst", ".csv.gz"):
        shards = sorted(parent.glob(f"{name}_s*_p*{ext}"))
        if shards:
            try:
                frames = [
                    pd.read_parquet(f) if ext == ".parquet" else pd.read_csv(f)
                    for f in shards
                ]
                return pd.concat(frames, ignore_index=True) if frames else None
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                log.warning("failed to read shards for %s: %s", stem, exc)
                return None

        fpath = directory / (stem + ext)
        if fpath.is_file():
            try:
                if ext == ".parquet":
                    return pd.read_parquet(fpath)
                return pd.read_csv(fpath)
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                log.warning("failed to read %s: %s", fpath, exc)
                return None
    return None


def load_planning(json_path: Path) -> dict:
    """Load a planning JSON file."""
    with open(json_path, encoding="utf-8") as f:
        return json.load(f)


def get_block_durations(planning: dict) -> dict[int, float]:
    """Return {block_uid: duration_hours} from the planning dict."""
    blocks = planning.get("simulation", {}).get("block_array", [])
    result: dict[int, float] = {}
    for blk in blocks:
        uid = blk.get("uid", 0)
        dur = blk.get("duration", 1.0)
        result[uid] = dur
    return result


def get_generator_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with generator uid, name, type, bus, pmax."""
    gens = planning.get("system", {}).get("generator_array", [])
    rows = []
    for g in gens:
        uid = g.get("uid", 0)
        name = g.get("name", f"uid:{uid}")
        gtype = g.get("type", "unknown")
        bus = g.get("bus", "")
        pmax = g.get("pmax", 0)
        if isinstance(pmax, (int, float)):
            pmax_val = float(pmax)
        else:
            pmax_val = 0.0  # file-referenced
        rows.append(
            {
                "uid": uid,
                "name": name,
                "type": gtype,
                "bus": bus,
                "pmax": pmax_val,
            }
        )
    return pd.DataFrame(rows)


def get_line_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with line uid, name, bus_a, bus_b, tmax."""
    lines = planning.get("system", {}).get("line_array", [])
    rows = []
    for ln in lines:
        uid = ln.get("uid", 0)
        name = ln.get("name", f"uid:{uid}")
        bus_a = ln.get("bus_a", "")
        bus_b = ln.get("bus_b", "")
        tmax = ln.get("tmax_ab", ln.get("tmax", 0))
        if isinstance(tmax, (int, float)):
            tmax_val = float(tmax)
        else:
            tmax_val = 0.0
        rows.append(
            {
                "uid": uid,
                "name": name,
                "bus_a": bus_a,
                "bus_b": bus_b,
                "tmax": tmax_val,
            }
        )
    return pd.DataFrame(rows)


def get_generator_profile_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with generator profile uid, name, generator uid."""
    profiles = planning.get("system", {}).get("generator_profile_array", [])
    gen_info = get_generator_info(planning)
    # Build name→uid mapping for generators
    gen_name_to_uid: dict[str, int] = {}
    for _, row in gen_info.iterrows():
        gen_name_to_uid[row["name"]] = int(row["uid"])

    rows = []
    for gp in profiles:
        uid = gp.get("uid", 0)
        name = gp.get("name", f"uid:{uid}")
        gen_ref = gp.get("generator", "")
        # generator can be a uid (int) or a name (str)
        if isinstance(gen_ref, int):
            gen_uid = gen_ref
        elif isinstance(gen_ref, str) and gen_ref.isdigit():
            gen_uid = int(gen_ref)
        else:
            gen_uid = gen_name_to_uid.get(str(gen_ref), -1)
        rows.append({"uid": uid, "name": name, "generator_uid": gen_uid})
    return pd.DataFrame(rows)


def get_demand_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with demand uid, name, bus."""
    demands = planning.get("system", {}).get("demand_array", [])
    rows = []
    for d in demands:
        uid = d.get("uid", 0)
        name = d.get("name", f"uid:{uid}")
        bus = d.get("bus", "")
        rows.append({"uid": uid, "name": name, "bus": bus})
    return pd.DataFrame(rows)
