# SPDX-License-Identifier: BSD-3-Clause
"""Output writer — the 5-table parquet dataset documented in master §4.6.

Layout:
    marginal_units.parquet/
      attribution/per_bus.parquet
      attribution/per_zone.parquet
      merit_ladder.parquet
      bus_price_recipe.parquet
      bus_emission_intensity_recipe.parquet
      audit/saturated_lines.parquet     (when populated)
      audit/unattributed.parquet        (when populated)
      audit/out_of_merit.parquet        (when populated)
      manifest.json
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import pandas as pd

from gtopt_marginal_units._ladder import LadderRung
from gtopt_marginal_units._recipes import RecipeRow


_PARQUET_COMPRESSION = "zstd"
_VERSION = "1.0.0"


@dataclass(slots=True)
class WriteSummary:
    """Returned by ``write_dataset`` so the caller can populate exit codes."""

    rows_per_bus: int = 0
    rows_per_zone: int = 0
    rows_merit_ladder: int = 0
    rows_price_recipe: int = 0
    rows_emission_recipe: int = 0
    rows_audit_unattributed: int = 0
    rows_audit_saturated: int = 0
    rows_audit_out_of_merit: int = 0
    has_unattributed_cells: bool = False
    manifest_path: Optional[Path] = None


def write_dataset(
    root: Path,
    *,
    per_bus: pd.DataFrame,
    per_zone: pd.DataFrame,
    merit_ladder: list[LadderRung],
    price_recipe: list[RecipeRow],
    emission_recipe: list[RecipeRow],
    saturated_lines: Optional[pd.DataFrame] = None,
    unattributed: Optional[pd.DataFrame] = None,
    out_of_merit: Optional[pd.DataFrame] = None,
    extras: Optional[dict] = None,
) -> WriteSummary:
    """Write the full output dataset under ``root``.

    Caller is responsible for the per_bus / per_zone DataFrames already
    being in the canonical column shape (see master §4.6.1 / §4.6.2).
    """
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / "attribution").mkdir(exist_ok=True)
    (root / "audit").mkdir(exist_ok=True)

    summary = WriteSummary()

    _write(root / "attribution/per_bus.parquet", per_bus)
    _write(root / "attribution/per_zone.parquet", per_zone)
    summary.rows_per_bus = len(per_bus)
    summary.rows_per_zone = len(per_zone)

    if merit_ladder:
        ladder_df = pd.DataFrame([_ladder_to_dict(r) for r in merit_ladder])
        _write(root / "merit_ladder.parquet", ladder_df)
        summary.rows_merit_ladder = len(ladder_df)

    if price_recipe:
        price_df = pd.DataFrame([r.to_dict("lmp") for r in price_recipe])
        _write(root / "bus_price_recipe.parquet", price_df)
        summary.rows_price_recipe = len(price_df)

    if emission_recipe:
        em_df = pd.DataFrame([r.to_dict("emission_intensity") for r in emission_recipe])
        _write(root / "bus_emission_intensity_recipe.parquet", em_df)
        summary.rows_emission_recipe = len(em_df)

    if saturated_lines is not None and not saturated_lines.empty:
        _write(root / "audit/saturated_lines.parquet", saturated_lines)
        summary.rows_audit_saturated = len(saturated_lines)
    if unattributed is not None and not unattributed.empty:
        _write(root / "audit/unattributed.parquet", unattributed)
        summary.rows_audit_unattributed = len(unattributed)
        summary.has_unattributed_cells = True
    if out_of_merit is not None and not out_of_merit.empty:
        _write(root / "audit/out_of_merit.parquet", out_of_merit)
        summary.rows_audit_out_of_merit = len(out_of_merit)

    # Manifest.
    manifest = {
        "producer": "gtopt_marginal_units",
        "producer_version": _VERSION,
        "schema_version": _VERSION,
        "written_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "row_counts": _row_count_dict(summary),
        "extras": extras or {},
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8"
    )
    summary.manifest_path = manifest_path
    return summary


def _row_count_dict(summary: WriteSummary) -> dict[str, int]:
    return {
        "attribution/per_bus": summary.rows_per_bus,
        "attribution/per_zone": summary.rows_per_zone,
        "merit_ladder": summary.rows_merit_ladder,
        "bus_price_recipe": summary.rows_price_recipe,
        "bus_emission_intensity_recipe": summary.rows_emission_recipe,
        "audit/saturated_lines": summary.rows_audit_saturated,
        "audit/unattributed": summary.rows_audit_unattributed,
        "audit/out_of_merit": summary.rows_audit_out_of_merit,
    }


def _ladder_to_dict(r: LadderRung) -> dict[str, object]:
    scenario, stage, block, date_utc, hour, data_source = r.cell_key
    return {
        "scenario": scenario,
        "stage": stage,
        "block": block,
        "date_utc": date_utc,
        "hour": hour,
        "data_source": data_source,
        "zone_id": r.zone_id,
        "rank": r.rank,
        "gen_uid": r.gen_uid,
        "gen_name": r.gen_name,
        "declared_MC": r.declared_MC,
        "active_segment_MC": r.active_segment_MC,
        "dispatch": r.dispatch,
        "pmin": r.pmin,
        "pmax": r.pmax,
        "headroom_up_mw": r.headroom_up_mw,
        "headroom_down_mw": r.headroom_down_mw,
        "available": r.available,
        "hypothetical_lmp": r.hypothetical_lmp,
        "is_actual_marginal": r.is_actual_marginal,
    }


def _write(path: Path, df: pd.DataFrame) -> None:
    df.to_parquet(path, compression=_PARQUET_COMPRESSION, index=False)
