# SPDX-License-Identifier: BSD-3-Clause
"""Output validation checks and power-system indicators."""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd

from ._reader import (
    get_block_durations,
    get_generator_info,
    get_generator_profile_info,
    get_line_info,
    read_table,
)

log = logging.getLogger(__name__)


@dataclass
class Finding:
    """A single check result."""

    check: str
    severity: str  # "CRITICAL", "WARNING", "INFO"
    message: str


@dataclass
class OutputReport:
    """Aggregated output analysis report."""

    findings: list[Finding] = field(default_factory=list)
    indicators: dict = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not any(f.severity == "CRITICAL" for f in self.findings)


def _uid_cols(df: pd.DataFrame) -> list[str]:
    """Return uid:N columns from a DataFrame."""
    return [c for c in df.columns if c.startswith("uid:")]


def _to_wide(df: pd.DataFrame) -> pd.DataFrame:
    """Return only uid columns, ensuring numeric."""
    cols = _uid_cols(df)
    if not cols:
        # Try all non-index columns
        idx = {"scenario", "stage", "block"}
        cols = [c for c in df.columns if c not in idx]
    return df[cols].apply(pd.to_numeric, errors="coerce")


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------


def check_expected_files(results_dir: Path) -> list[Finding]:
    """Verify that key output files exist."""
    findings: list[Finding] = []
    required = [
        ("solution", "solution status"),
        ("Generator/generation_sol", "generator dispatch"),
        ("Bus/balance_dual", "bus marginal prices (LMPs)"),
    ]
    expected = [
        ("Demand/fail_sol", "load shedding"),
        ("Demand/load_sol", "demand served"),
        ("Line/flowp_sol", "line flows (A→B)"),
    ]

    for stem, label in required:
        df = read_table(results_dir, stem)
        if df is None:
            findings.append(
                Finding(
                    "expected_files",
                    "CRITICAL",
                    f"missing required output: {label} ({stem})",
                )
            )

    for stem, label in expected:
        df = read_table(results_dir, stem)
        if df is None:
            findings.append(
                Finding(
                    "expected_files",
                    "INFO",
                    f"optional output not found: {label} ({stem})",
                )
            )

    if not findings:
        findings.append(
            Finding("expected_files", "INFO", "all expected output files present")
        )
    return findings


def check_load_shedding(results_dir: Path, planning: dict) -> list[Finding]:
    """Check for unserved demand (load shedding)."""
    findings: list[Finding] = []
    fail_df = read_table(results_dir, "Demand/fail_sol")
    if fail_df is None:
        return findings

    vals = _to_wide(fail_df)
    total_shed = vals.sum().sum()
    max_shed = vals.max().max()

    if total_shed > 0.01:
        # Compute energy shed
        durations = get_block_durations(planning)
        energy_shed = 0.0
        if "block" in fail_df.columns:
            for _, row in fail_df.iterrows():
                blk = int(row.get("block", 0))
                dur = durations.get(blk, 1.0)
                energy_shed += vals.loc[row.name].sum() * dur

        findings.append(
            Finding(
                "load_shedding",
                "WARNING",
                f"load shedding detected: total={total_shed:.2f} MW, "
                f"max={max_shed:.2f} MW, energy={energy_shed:.1f} MWh",
            )
        )
    else:
        findings.append(
            Finding("load_shedding", "INFO", "no load shedding (all demand served)")
        )
    return findings


def check_generation_vs_demand(results_dir: Path, planning: dict) -> list[Finding]:
    """Compare total generation to total demand served."""
    findings: list[Finding] = []
    gen_df = read_table(results_dir, "Generator/generation_sol")
    load_df = read_table(results_dir, "Demand/load_sol")

    if gen_df is None or load_df is None:
        return findings

    gen_total = _to_wide(gen_df).sum().sum()
    load_total = _to_wide(load_df).sum().sum()

    if gen_total < load_total * 0.99:
        findings.append(
            Finding(
                "gen_vs_demand",
                "WARNING",
                f"total generation ({gen_total:.1f} MW) < demand ({load_total:.1f} MW)",
            )
        )
    else:
        ratio = gen_total / load_total if load_total > 0 else float("inf")
        findings.append(
            Finding(
                "gen_vs_demand",
                "INFO",
                f"generation/demand ratio: {ratio:.4f} "
                f"(gen={gen_total:.1f}, demand={load_total:.1f})",
            )
        )
    return findings


def compute_energy_by_type(
    results_dir: Path, planning: dict
) -> tuple[list[Finding], dict]:
    """Compute energy production (MWh) by generator type."""
    findings: list[Finding] = []
    gen_df = read_table(results_dir, "Generator/generation_sol")
    if gen_df is None:
        return findings, {}

    gen_info = get_generator_info(planning)
    durations = get_block_durations(planning)

    uid_cols = _uid_cols(gen_df)
    if not uid_cols:
        return findings, {}

    # Build uid → type mapping
    uid_type: dict[int, str] = {}
    for _, row in gen_info.iterrows():
        uid_type[int(row["uid"])] = row["type"]

    energy_by_type: dict[str, float] = {}
    for col in uid_cols:
        uid = int(col.split(":")[1])
        gtype = uid_type.get(uid, "unknown")
        col_energy = 0.0
        for idx, val in gen_df[col].items():
            blk = int(gen_df.at[idx, "block"]) if "block" in gen_df.columns else 0
            dur = durations.get(blk, 1.0)
            col_energy += float(val) * dur
        energy_by_type[gtype] = energy_by_type.get(gtype, 0.0) + col_energy

    # Import classify_type for category grouping
    try:
        from plp2gtopt.tech_classify import classify_type, type_label  # noqa: PLC0415
    except ImportError:

        def classify_type(gtype: str) -> str:
            return gtype

        def type_label(gtype: str) -> str:
            return gtype

    total = sum(energy_by_type.values())
    findings.append(
        Finding("energy_by_type", "INFO", f"total energy production: {total:.1f} MWh"),
    )

    # Per-type breakdown
    for gtype, energy in sorted(energy_by_type.items(), key=lambda x: -x[1]):
        pct = 100 * energy / total if total > 0 else 0
        label = type_label(gtype)
        findings.append(
            Finding(
                "energy_by_type",
                "INFO",
                f"  {label:30s}: {energy:12.1f} MWh ({pct:5.1f}%)",
            )
        )

    # Category summary (hydro / thermal / renewable)
    energy_by_cat: dict[str, float] = {}
    for gtype, energy in energy_by_type.items():
        cat = classify_type(gtype)
        energy_by_cat[cat] = energy_by_cat.get(cat, 0.0) + energy
    if len(energy_by_cat) > 1:
        findings.append(
            Finding("energy_by_type", "INFO", "  --- by category ---"),
        )
        for cat, energy in sorted(energy_by_cat.items(), key=lambda x: -x[1]):
            pct = 100 * energy / total if total > 0 else 0
            findings.append(
                Finding(
                    "energy_by_type",
                    "INFO",
                    f"  {cat:30s}: {energy:12.1f} MWh ({pct:5.1f}%)",
                )
            )
    return findings, energy_by_type


def compute_congestion_ranking(
    results_dir: Path, planning: dict, top_n: int = 10
) -> list[Finding]:
    """Rank transmission lines by congestion (utilization of capacity)."""
    findings: list[Finding] = []
    flowp = read_table(results_dir, "Line/flowp_sol")
    flown = read_table(results_dir, "Line/flown_sol")

    if flowp is None:
        return findings

    line_info = get_line_info(planning)
    if line_info.empty:
        return findings

    uid_tmax: dict[int, float] = {}
    uid_name: dict[int, str] = {}
    for _, row in line_info.iterrows():
        uid_tmax[int(row["uid"])] = float(row["tmax"]) if row["tmax"] > 0 else 1e9
        uid_name[int(row["uid"])] = row["name"]

    utilizations: list[tuple[str, float, float]] = []
    uid_cols = _uid_cols(flowp)
    for col in uid_cols:
        uid = int(col.split(":")[1])
        tmax = uid_tmax.get(uid, 1e9)
        name = uid_name.get(uid, col)

        fp = flowp[col].abs()
        fn = (
            flown[col].abs()
            if flown is not None and col in flown.columns
            else pd.Series([0])
        )
        max_flow = max(fp.max(), fn.max())
        utilization = max_flow / tmax if tmax > 0 else 0.0
        utilizations.append((name, utilization, max_flow))

    utilizations.sort(key=lambda x: -x[1])
    congested = [(n, u, f) for n, u, f in utilizations if u > 0.9]

    if congested:
        findings.append(
            Finding(
                "congestion",
                "WARNING",
                f"{len(congested)} line(s) with utilization > 90%",
            )
        )
    else:
        findings.append(
            Finding("congestion", "INFO", "no congested lines (all < 90% utilization)")
        )

    for name, util, flow in utilizations[:top_n]:
        findings.append(
            Finding(
                "congestion",
                "INFO",
                f"  {name:30s}: {util * 100:5.1f}% (max flow={flow:.1f} MW)",
            )
        )

    return findings


def compute_lmp_statistics(results_dir: Path, planning: dict) -> list[Finding]:
    """Compute LMP (locational marginal price) statistics per bus."""
    findings: list[Finding] = []
    lmp_df = read_table(results_dir, "Bus/balance_dual")
    if lmp_df is None:
        return findings

    vals = _to_wide(lmp_df)
    if vals.empty:
        return findings

    overall_mean = vals.mean().mean()
    overall_max = vals.max().max()
    overall_min = vals.min().min()
    spread = overall_max - overall_min

    findings.append(
        Finding(
            "lmp_stats",
            "INFO",
            f"LMP statistics: mean={overall_mean:.2f}, "
            f"min={overall_min:.2f}, max={overall_max:.2f}, spread={spread:.2f} $/MWh",
        )
    )

    if spread > 100:
        findings.append(
            Finding(
                "lmp_stats",
                "WARNING",
                f"high LMP spread ({spread:.1f} $/MWh) suggests network congestion",
            )
        )

    # Negative LMPs
    neg_count = (vals < -0.01).sum().sum()
    if neg_count > 0:
        findings.append(
            Finding(
                "lmp_stats",
                "WARNING",
                f"{neg_count} negative LMP values detected (possible over-generation)",
            )
        )

    return findings


def compute_cost_breakdown(results_dir: Path, planning: dict) -> list[Finding]:
    """Break down total cost by component."""
    findings: list[Finding] = []
    components: dict[str, float] = {}

    for stem, label in [
        ("Generator/generation_cost", "generation"),
        ("Demand/fail_cost", "load shedding penalty"),
        ("Line/flowp_cost", "transmission (A→B)"),
        ("Line/flown_cost", "transmission (B→A)"),
    ]:
        df = read_table(results_dir, stem)
        if df is not None:
            total = _to_wide(df).sum().sum()
            if abs(total) > 0.01:
                components[label] = total

    grand_total = sum(components.values())
    if grand_total > 0:
        findings.append(
            Finding("cost_breakdown", "INFO", f"total system cost: {grand_total:.2f}")
        )
        for label, cost in sorted(components.items(), key=lambda x: -abs(x[1])):
            pct = 100 * cost / grand_total if grand_total > 0 else 0
            findings.append(
                Finding(
                    "cost_breakdown",
                    "INFO",
                    f"  {label:30s}: {cost:12.2f} ({pct:5.1f}%)",
                )
            )
    return findings


def check_battery_soc(results_dir: Path, planning: dict) -> list[Finding]:
    """Check battery state-of-charge bounds."""
    findings: list[Finding] = []
    soc_df = read_table(results_dir, "Battery/energy_sol")
    if soc_df is None:
        return findings

    vals = _to_wide(soc_df)
    neg = (vals < -0.01).sum().sum()
    if neg > 0:
        findings.append(
            Finding("battery_soc", "WARNING", f"{neg} negative battery SoC values")
        )
    else:
        findings.append(Finding("battery_soc", "INFO", "battery SoC within bounds"))
    return findings


def check_reservoir_levels(results_dir: Path, planning: dict) -> list[Finding]:
    """Check reservoir energy levels."""
    findings: list[Finding] = []
    energy_df = read_table(results_dir, "Reservoir/energy_sol")
    if energy_df is None:
        return findings

    vals = _to_wide(energy_df)
    neg = (vals < -0.01).sum().sum()
    if neg > 0:
        findings.append(
            Finding("reservoir_levels", "WARNING", f"{neg} negative reservoir levels")
        )
    else:
        findings.append(
            Finding("reservoir_levels", "INFO", "reservoir levels within bounds")
        )
    return findings


def check_renewable_curtailment(
    results_dir: Path, planning: dict, top_n: int = 10
) -> tuple[list[Finding], dict]:
    """Check renewable energy curtailment (spillover) from generator profiles.

    Curtailment occurs when a profiled renewable generator (solar, wind)
    produces less than its available capacity.  The solver reports this
    as the ``GeneratorProfile/spillover_sol`` output.  This check reads
    that output and computes curtailment energy (MWh) per profile and
    as a percentage of potential generation.
    """
    findings: list[Finding] = []
    curtailment_data: dict[str, float] = {}

    spill_df = read_table(results_dir, "GeneratorProfile/spillover_sol")
    if spill_df is None:
        return findings, curtailment_data

    gen_df = read_table(results_dir, "Generator/generation_sol")
    profile_info = get_generator_profile_info(planning)
    gen_info = get_generator_info(planning)
    durations = get_block_durations(planning)

    if profile_info.empty:
        return findings, curtailment_data

    # Build uid→name mappings
    profile_uid_name: dict[int, str] = {}
    profile_uid_gen: dict[int, int] = {}
    for _, row in profile_info.iterrows():
        profile_uid_name[int(row["uid"])] = row["name"]
        profile_uid_gen[int(row["uid"])] = int(row["generator_uid"])

    gen_uid_name: dict[int, str] = {}
    for _, row in gen_info.iterrows():
        gen_uid_name[int(row["uid"])] = row["name"]

    uid_cols = _uid_cols(spill_df)
    if not uid_cols:
        return findings, curtailment_data

    total_curtailment_energy = 0.0
    total_potential_energy = 0.0
    per_profile: list[tuple[str, str, float, float, float]] = []

    for col in uid_cols:
        profile_uid = int(col.split(":")[1])
        pname = profile_uid_name.get(profile_uid, col)
        gen_uid = profile_uid_gen.get(profile_uid, -1)
        gen_name = gen_uid_name.get(gen_uid, f"uid:{gen_uid}")

        # Compute curtailment energy (spillover × duration)
        curtail_energy = 0.0
        for idx, val in spill_df[col].items():
            blk = int(spill_df.at[idx, "block"]) if "block" in spill_df.columns else 0
            dur = durations.get(blk, 1.0)
            curtail_energy += float(val) * dur

        # Compute actual generation energy for the linked generator
        actual_energy = 0.0
        gen_col = f"uid:{gen_uid}"
        if gen_df is not None and gen_col in gen_df.columns:
            for idx, val in gen_df[gen_col].items():
                blk = int(gen_df.at[idx, "block"]) if "block" in gen_df.columns else 0
                dur = durations.get(blk, 1.0)
                actual_energy += float(val) * dur

        potential_energy = actual_energy + curtail_energy
        pct = 100 * curtail_energy / potential_energy if potential_energy > 0 else 0.0

        per_profile.append(
            (pname, gen_name, curtail_energy, potential_energy, pct),
        )
        curtailment_data[pname] = curtail_energy
        total_curtailment_energy += curtail_energy
        total_potential_energy += potential_energy

    total_pct = (
        100 * total_curtailment_energy / total_potential_energy
        if total_potential_energy > 0
        else 0.0
    )

    if total_curtailment_energy > 0.01:
        findings.append(
            Finding(
                "renewable_curtailment",
                "WARNING",
                f"renewable curtailment detected: {total_curtailment_energy:.1f} MWh "
                f"({total_pct:.1f}% of potential generation)",
            )
        )
    else:
        findings.append(
            Finding(
                "renewable_curtailment",
                "INFO",
                "no renewable curtailment (all profiled generators at full output)",
            )
        )

    # Per-profile breakdown (sorted by curtailment energy descending)
    per_profile.sort(key=lambda x: -x[2])
    for pname, gen_name, curtail_e, potential_e, pct in per_profile[:top_n]:
        findings.append(
            Finding(
                "renewable_curtailment",
                "INFO",
                f"  {pname:20s} (gen={gen_name:15s}): "
                f"{curtail_e:10.1f} MWh curtailed "
                f"of {potential_e:10.1f} MWh potential ({pct:5.1f}%)",
            )
        )

    return findings, curtailment_data


# ---------------------------------------------------------------------------
# Orchestrator
# ---------------------------------------------------------------------------


def run_all_checks(results_dir: Path, planning: dict) -> OutputReport:
    """Run all output checks and return an aggregated report."""
    report = OutputReport()

    report.findings.extend(check_expected_files(results_dir))
    report.findings.extend(check_load_shedding(results_dir, planning))
    report.findings.extend(check_generation_vs_demand(results_dir, planning))

    energy_findings, energy_by_type = compute_energy_by_type(results_dir, planning)
    report.findings.extend(energy_findings)
    report.indicators["energy_by_type"] = energy_by_type

    curtailment_findings, curtailment_data = check_renewable_curtailment(
        results_dir, planning
    )
    report.findings.extend(curtailment_findings)
    report.indicators["renewable_curtailment"] = curtailment_data

    report.findings.extend(compute_congestion_ranking(results_dir, planning))
    report.findings.extend(compute_lmp_statistics(results_dir, planning))
    report.findings.extend(compute_cost_breakdown(results_dir, planning))
    report.findings.extend(check_battery_soc(results_dir, planning))
    report.findings.extend(check_reservoir_levels(results_dir, planning))

    return report
