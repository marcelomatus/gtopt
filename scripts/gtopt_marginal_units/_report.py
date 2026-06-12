# SPDX-License-Identifier: BSD-3-Clause
"""Markdown digest generator. Master plan §4.6.5.

The report is a small, readable Markdown file the operator can scan
in one minute. Sections:
* Executive summary — cells, zones, attribution rates.
* Top-N marginal units by frequency.
* Saturation summary (when there is any).
* Unattributed cells (always — even if zero, we flag it).
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import pandas as pd


def write_report(
    out_path: Path,
    *,
    per_bus: pd.DataFrame,
    per_zone: pd.DataFrame,
    saturated_lines: Optional[pd.DataFrame] = None,
    unattributed: Optional[pd.DataFrame] = None,
    title: str = "Marginal-unit attribution report",
    extras: Optional[dict] = None,
    top_n_marginal: int = 10,
    top_n_lines: int = 10,
    top_n_reasons: int = 5,
) -> Path:
    """Render a small Markdown digest to ``out_path``.

    ``top_n_*`` parameters control table truncation; defaults match
    legacy behaviour.  When the underlying data has > N rows, the
    report explicitly notes the truncation count so the reader knows
    how many entries were elided instead of silently dropping them.
    """
    sections: list[str] = [f"# {title}", ""]

    # Executive summary.
    n_cells = (
        per_zone[["scenario", "stage", "block"]].drop_duplicates().shape[0]
        if {"scenario", "stage", "block"}.issubset(per_zone.columns)
        else len(per_zone)
    )
    n_zones = per_zone["zone_id"].nunique() if "zone_id" in per_zone.columns else 0
    n_unattributed = (
        len(unattributed)
        if (unattributed is not None and not unattributed.empty)
        else 0
    )
    sections.append("## Executive summary\n")
    sections.append(f"- Cells:     {n_cells}")
    sections.append(f"- Zones:     {n_zones}")
    sections.append(f"- Per-bus rows:   {len(per_bus)}")
    sections.append(f"- Unattributed cells: {n_unattributed}")
    if extras:
        for k, v in extras.items():
            sections.append(f"- {k}: {v}")
    sections.append("")

    # Top marginal units.
    if "gen_uid" in per_bus.columns and "is_marginal" in per_bus.columns:
        marginal = per_bus[per_bus["is_marginal"].fillna(False)]
        if not marginal.empty:
            counts = (
                marginal.groupby(
                    [c for c in ("gen_uid", "gen_name") if c in marginal.columns]
                )
                .size()
                .reset_index(name="frequency")
            )
            total_marg = len(counts)
            counts = counts.sort_values("frequency", ascending=False).head(
                top_n_marginal
            )
            sections.append(f"## Top {top_n_marginal} marginal units (by cell-count)\n")
            if total_marg > top_n_marginal:
                sections.append(
                    f"_Showing {top_n_marginal} of {total_marg} distinct "
                    f"marginal units; raise ``--report-top-marginal`` to "
                    f"see more._\n"
                )
            sections.append("| gen_uid | gen_name | cells |")
            sections.append("|---:|---|---:|")
            for _, r in counts.iterrows():
                name = r.get("gen_name") if "gen_name" in r else ""
                sections.append(
                    f"| {int(r['gen_uid'])} | {name} | {int(r['frequency'])} |"
                )
            sections.append("")

    # Saturated lines.
    if saturated_lines is not None and not saturated_lines.empty:
        sat_summary = (
            saturated_lines.groupby(
                [c for c in ("line_uid",) if c in saturated_lines.columns]
            )
            .size()
            .reset_index(name="cells_saturated")
        )
        total_sat = len(sat_summary)
        sat_summary = sat_summary.sort_values("cells_saturated", ascending=False).head(
            top_n_lines
        )
        sections.append(f"## Saturated lines (top {top_n_lines})\n")
        if total_sat > top_n_lines:
            sections.append(
                f"_Showing {top_n_lines} of {total_sat} saturated lines; "
                f"raise ``--report-top-lines`` to see more._\n"
            )
        sections.append("| line_uid | cells |")
        sections.append("|---:|---:|")
        for _, r in sat_summary.iterrows():
            sections.append(f"| {int(r['line_uid'])} | {int(r['cells_saturated'])} |")
        sections.append("")

    # Unattributed.
    if unattributed is not None and not unattributed.empty:
        sections.append("## Unattributed cells\n")
        sections.append(f"{len(unattributed)} cells could not be attributed. ")
        sections.append("Inspect ``audit/unattributed.parquet`` for full reasons.\n")
        if "reason" in unattributed.columns:
            all_reasons = (
                unattributed.groupby("reason")
                .size()
                .reset_index(name="cells")
                .sort_values("cells", ascending=False)
            )
            total_reasons = len(all_reasons)
            reasons = all_reasons.head(top_n_reasons)
            if total_reasons > top_n_reasons:
                sections.append(
                    f"_Showing {top_n_reasons} of {total_reasons} distinct "
                    f"reasons; raise ``--report-top-reasons`` to see more._\n"
                )
            sections.append("| reason | cells |")
            sections.append("|---|---:|")
            for _, r in reasons.iterrows():
                sections.append(f"| {r['reason']} | {int(r['cells'])} |")
            sections.append("")

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(sections) + "\n", encoding="utf-8")
    return out_path
