# SPDX-License-Identifier: BSD-3-Clause
"""Console (rich) and JSON renderers for gtopt_check_topology."""

from __future__ import annotations

import json
import math
from dataclasses import asdict
from typing import Any

from rich.console import Console

from gtopt_check_topology._analyzer import (
    AnalysisReport,
    BatteryEiniIssue,
    CycleInfo,
    IslandFeasibility,
    IslandInfo,
    LineRef,
    NegativeImpedance,
    ParallelCircuitIdentical,
    ReservoirCascadeIssue,
    ReserveZoneIssue,
    TimeVaryingCapacityIssue,
    VoltageMix,
)

# Severity glyphs -- chosen ASCII-only by default so terminals without an
# emoji font still render cleanly.  Rich colours the prefix.
_OK = "[bold green]ok[/bold green]"
_INFO = "[bold cyan]info[/bold cyan]"
_WARN = "[bold yellow]warn[/bold yellow]"
_ERR = "[bold red]err[/bold red]"


# ---------------------------------------------------------------------------
# Number formatting
# ---------------------------------------------------------------------------


def _fmt_ratio(value: float) -> str:
    if math.isinf(value):
        return "inf"
    if value >= 100:
        return f"{value:.0f}x"
    if value >= 10:
        return f"{value:.1f}x"
    return f"{value:.2f}x"


def _fmt_mw(value: float) -> str:
    if math.isinf(value) or math.isnan(value):
        return "n/a"
    return f"{value:.0f} MW"


def _cycle_label(cycle: CycleInfo, max_buses: int = 4) -> str:
    if len(cycle.buses) <= max_buses:
        return "-".join(cycle.buses)
    head = "-".join(cycle.buses[:max_buses])
    return f"{head}-... ({cycle.n_buses} buses)"


# ---------------------------------------------------------------------------
# Console rendering
# ---------------------------------------------------------------------------


def _print_islands(console: Console, report: AnalysisReport, quiet: bool) -> None:
    islands = report.islands
    no_gen = report.islands_no_generator

    if len(islands) <= 1 and not no_gen:
        if not quiet:
            biggest = islands[0].buses if islands else []
            console.print(
                f"{_OK} A1. Islands "
                f"(1 connected component, {len(biggest)}/{report.n_buses} buses)"
            )
        return

    console.print(f"{_WARN} A1. Islands: {len(islands)} connected components")
    for idx, isl in enumerate(islands):
        flag = "" if isl.has_generator else " [red](no generator)[/red]"
        bus_preview = ", ".join(isl.buses[:6])
        if len(isl.buses) > 6:
            bus_preview += f", ... (+{len(isl.buses) - 6})"
        console.print(f"  {idx + 1}. {len(isl.buses)} buses{flag}: {bus_preview}")
    if no_gen:
        console.print(
            f"  [red]-> {len(no_gen)} island(s) have NO generator "
            f"(LP infeasibility risk)[/red]"
        )


def _print_isolated(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.isolated_buses:
        if not quiet:
            console.print(f"{_OK} A2. Isolated buses: 0")
        return
    console.print(f"{_WARN} A2. Isolated buses: {len(report.isolated_buses)}")
    for name in report.isolated_buses[:20]:
        console.print(f"  - {name}")
    if len(report.isolated_buses) > 20:
        console.print(f"  ... (+{len(report.isolated_buses) - 20})")


def _print_stubs(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.stubs:
        if not quiet:
            console.print(f"{_OK} A3. Stub lines: 0")
        return
    if not quiet:
        console.print(
            f"{_INFO} A3. Stub lines (degree-1 endpoint): "
            f"{len(report.stubs)} - informational"
        )


def _print_cycles(
    console: Console,
    report: AnalysisReport,
    include_benign: bool,
    quiet: bool,
) -> None:
    if not report.cycles:
        if not quiet:
            console.print(f"{_OK} B1-B4. No fundamental cycles")
        return

    dangerous = report.dangerous_cycles
    benign = report.benign_cycles

    if dangerous:
        console.print(
            f"{_WARN} B2. Dangerous loops: {len(dangerous)} / "
            f"{report.n_cycles} (score >= {report.danger_threshold})"
        )
        top_n = min(5, len(dangerous))
        console.print(f"  Top {top_n} by danger score:")
        for rank, cyc in enumerate(dangerous[:top_n], start=1):
            console.print(
                f"    {rank}. {_cycle_label(cyc)} "
                f"({cyc.n_buses} buses, "
                f"asym_X={_fmt_ratio(cyc.asym_x)}, "
                f"asym_tmax={_fmt_ratio(cyc.asym_tmax)}, "
                f"min_tmax={_fmt_mw(cyc.min_tmax)}, "
                f"score={cyc.score})"
            )
        sos1 = report.sos1_candidates
        console.print(
            f"  [yellow]-> {len(sos1)} lines in dangerous cycles "
            f"-> candidates for SOS1[/yellow]"
        )
    elif not quiet:
        console.print(
            f"{_OK} B2. No dangerous loops "
            f"(0 / {report.n_cycles} above score {report.danger_threshold})"
        )

    if include_benign and benign:
        console.print(f"{_INFO} B3. Benign loops: {len(benign)}")
        for cyc in benign:
            console.print(
                f"    - {_cycle_label(cyc)} "
                f"(score={cyc.score}, asym_X={_fmt_ratio(cyc.asym_x)})"
            )


def _print_bridges(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.bridges:
        if not quiet:
            console.print(f"{_OK} B5. Bridge edges: 0")
        return
    console.print(
        f"{_WARN} B5. Bridge edges (single point of failure): {len(report.bridges)}"
    )
    for ref in report.bridges[:10]:
        console.print(f"  - {ref.name} ({ref.bus_a} <-> {ref.bus_b})")
    if len(report.bridges) > 10:
        console.print(f"  ... (+{len(report.bridges) - 10})")


def _print_dc(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.dc_lines:
        if not quiet:
            console.print(f"{_INFO} C1. DC links (reactance = 0): 0")
        return
    console.print(
        f"{_INFO} C1. DC links (reactance = 0): "
        f"{len(report.dc_lines)} (excluded from cycle analysis)"
    )
    for ref in report.dc_lines[:10]:
        console.print(f"  - {ref.name} ({ref.bus_a} <-> {ref.bus_b})")
    if len(report.dc_lines) > 10:
        console.print(f"  ... (+{len(report.dc_lines) - 10})")


def _print_negative(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.negative_impedance:
        if not quiet:
            console.print(f"{_OK} C2. No negative R/X")
        return
    console.print(
        f"{_ERR} C2. Negative R/X (data error): {len(report.negative_impedance)}"
    )
    for neg in report.negative_impedance[:10]:
        console.print(f"  - {neg.name} (R={neg.resistance}, X={neg.reactance})")


def _print_voltage(console: Console, report: AnalysisReport, quiet: bool) -> None:
    if not report.voltage_mixing:
        if not quiet:
            console.print(
                f"{_OK} C3. No voltage-mixing lines without explicit transformer"
            )
        return
    console.print(
        f"{_WARN} C3. Voltage mixing without explicit transformer: "
        f"{len(report.voltage_mixing)} line(s)"
    )
    for mix in report.voltage_mixing[:10]:
        console.print(
            f"  - bus_a.V={mix.bus_a_voltage}, "
            f"bus_b.V={mix.bus_b_voltage}  "
            f"({mix.bus_a} <-> {mix.bus_b}, line='{mix.name}')"
        )
    if len(report.voltage_mixing) > 10:
        console.print(f"  ... (+{len(report.voltage_mixing) - 10})")


def _print_reserve_zone_coherence(
    console: Console, report: AnalysisReport, quiet: bool
) -> None:
    issues = report.reserve_zone_coherence
    if not issues:
        if not quiet:
            console.print(f"{_OK} D1. Reserve-zone coherence: all zones OK")
        return
    console.print(f"{_WARN} D1. Reserve-zone coherence: {len(issues)} zone(s) flagged")
    for iss in issues[:20]:
        console.print(f"  - {iss.zone}: {iss.issue} (n_providers={iss.n_providers})")
    if len(issues) > 20:
        console.print(f"  ... (+{len(issues) - 20})")


def _print_island_feasibility(
    console: Console, report: AnalysisReport, quiet: bool
) -> None:
    issues = report.island_feasibility
    if not issues:
        if not quiet:
            console.print(f"{_OK} D2. Per-island feasibility: OK")
        return
    console.print(
        f"{_WARN} D2. Per-island feasibility: {len(issues)} island(s) flagged"
    )
    for iss in issues[:20]:
        console.print(
            f"  - island #{iss.island_idx}: {iss.issue} "
            f"(n_dem_buses={iss.n_demand_buses}, n_gen_buses={iss.n_gen_buses}, "
            f"sum_pmax={iss.sum_pmax:.0f} MW, sum_peak_load={iss.sum_peak_load:.0f} MW)"
        )
    if len(issues) > 20:
        console.print(f"  ... (+{len(issues) - 20})")


def _print_battery_eini(console: Console, report: AnalysisReport, quiet: bool) -> None:
    issues = report.battery_eini_issues
    if not issues:
        if not quiet:
            console.print(f"{_OK} D3. Battery initial-state consistency: OK")
        return
    console.print(
        f"{_WARN} D3. Battery initial-state issues: {len(issues)} battery(ies)"
    )
    for iss in issues[:20]:
        uc_hint = (
            f"  [in: {', '.join(iss.in_ucs[:3])}{'...' if len(iss.in_ucs) > 3 else ''}]"
            if iss.in_ucs
            else ""
        )
        console.print(f"  - {iss.name}: {iss.issue} (eini={iss.eini}){uc_hint}")
    if len(issues) > 20:
        console.print(f"  ... (+{len(issues) - 20})")


def _print_time_varying_capacity(
    console: Console, report: AnalysisReport, quiet: bool
) -> None:
    issues = report.time_varying_capacity_issues
    if not issues:
        if not quiet:
            console.print(f"{_OK} E1. Time-varying line capacity: OK")
        return
    console.print(
        f"{_WARN} E1. Time-varying line capacity: "
        f"{len(issues)} line(s) with mostly-zero blocks"
    )
    for iss in issues[:20]:
        console.print(
            f"  - {iss.name}: {iss.blocks_at_zero} blocks at zero tmax "
            f"/ {iss.blocks_total} total "
            f"(max_tmax={iss.max_tmax:.0f} MW)"
        )
    if len(issues) > 20:
        console.print(f"  ... (+{len(issues) - 20})")


def _print_reservoir_cascade(
    console: Console, report: AnalysisReport, quiet: bool
) -> None:
    issues = report.reservoir_cascade_issues
    if not issues:
        if not quiet:
            console.print(f"{_OK} E2. Reservoir cascade well-formedness: OK")
        return
    console.print(f"{_WARN} E2. Reservoir cascade issues: {len(issues)} item(s)")
    for iss in issues[:20]:
        suffix = f" -- {iss.detail}" if iss.detail else ""
        console.print(f"  - {iss.name}: {iss.issue}{suffix}")
    if len(issues) > 20:
        console.print(f"  ... (+{len(issues) - 20})")


def _print_parallel_identical(
    console: Console, report: AnalysisReport, quiet: bool
) -> None:
    items = report.parallel_circuit_identical
    if not items:
        if not quiet:
            console.print(f"{_OK} B6. No identical parallel circuits")
        return
    severe = [p for p in items if p.on_dangerous_cycle]
    severity = _WARN if severe else _INFO
    console.print(
        f"{severity} B6. Identical parallel circuits: {len(items)} bus pair(s) "
        f"({len(severe)} on a dangerous cycle)"
    )
    for it in items[:20]:
        flag = " [yellow](on dangerous cycle)[/yellow]" if it.on_dangerous_cycle else ""
        console.print(
            f"  - {it.n_circuits} parallel circuits between "
            f"{it.bus_a} <-> {it.bus_b} "
            f"with identical (X={it.reactance}, tmax={it.tmax:.0f}) "
            f"-- LP can split flow arbitrarily{flag}"
        )
    if len(items) > 20:
        console.print(f"  ... (+{len(items) - 20})")


def render_console_report(
    report: AnalysisReport,
    *,
    console: Console | None = None,
    quiet: bool = False,
    include_benign_loops: bool = False,
) -> None:
    """Print the human-readable topology report to ``console``.

    When ``quiet`` is True, OK-status checks are suppressed -- only
    flagged items appear.
    """
    if console is None:
        console = Console()

    title = report.bundle if report.bundle else "<planning>"
    console.print(f"\n[bold]== TOPOLOGY ANALYSIS: {title} ==[/bold]")
    console.print(
        f"Network: {report.n_buses} buses, {report.n_lines} lines, "
        f"{report.n_cycles} fundamental cycles\n"
    )

    _print_islands(console, report, quiet)
    _print_isolated(console, report, quiet)
    _print_stubs(console, report, quiet)
    console.print("")
    _print_cycles(console, report, include_benign_loops, quiet)
    _print_parallel_identical(console, report, quiet)
    console.print("")
    _print_bridges(console, report, quiet)
    console.print("")
    _print_dc(console, report, quiet)
    _print_negative(console, report, quiet)
    _print_voltage(console, report, quiet)
    console.print("")
    _print_reserve_zone_coherence(console, report, quiet)
    _print_island_feasibility(console, report, quiet)
    _print_battery_eini(console, report, quiet)
    console.print("")
    _print_time_varying_capacity(console, report, quiet)
    _print_reservoir_cascade(console, report, quiet)

    # ---- summary line ----
    parts: list[str] = []
    if report.dangerous_cycles:
        parts.append(f"{len(report.dangerous_cycles)} dangerous cycles")
    if report.sos1_candidates:
        parts.append(f"{len(report.sos1_candidates)} strict-direction candidates")
    if report.bridges:
        parts.append(f"{len(report.bridges)} bridges")
    if report.voltage_mixing:
        parts.append(f"{len(report.voltage_mixing)} voltage-mixing warnings")
    if report.islands_no_generator:
        parts.append(f"{len(report.islands_no_generator)} islands without generator")
    if report.negative_impedance:
        parts.append(f"{len(report.negative_impedance)} negative-R/X data errors")
    if report.reserve_zone_coherence:
        parts.append(f"{len(report.reserve_zone_coherence)} reserve-zone issues")
    if report.island_feasibility:
        parts.append(f"{len(report.island_feasibility)} island-feasibility issues")
    if report.battery_eini_issues:
        parts.append(f"{len(report.battery_eini_issues)} battery-eini issues")
    if report.time_varying_capacity_issues:
        parts.append(
            f"{len(report.time_varying_capacity_issues)} time-varying capacity issues"
        )
    if report.reservoir_cascade_issues:
        parts.append(f"{len(report.reservoir_cascade_issues)} reservoir-cascade issues")
    if report.parallel_circuit_identical:
        parts.append(
            f"{len(report.parallel_circuit_identical)} identical-parallel groups"
        )
    if not parts:
        parts.append("no issues")
    console.print(f"\n[bold]Summary:[/bold] {', '.join(parts)}")


# ---------------------------------------------------------------------------
# JSON serialization
# ---------------------------------------------------------------------------


def _line_to_dict(ref: LineRef) -> dict[str, Any]:
    return {
        "uid": ref.uid,
        "name": ref.name,
        "bus_a": ref.bus_a,
        "bus_b": ref.bus_b,
        "reactance": ref.reactance,
        "resistance": ref.resistance,
        "tmax": ref.tmax,
    }


def _island_to_dict(isl: IslandInfo) -> dict[str, Any]:
    return {
        "n_buses": len(isl.buses),
        "has_generator": isl.has_generator,
        "has_demand": isl.has_demand,
        "buses": isl.buses,
    }


def _cycle_to_dict(cyc: CycleInfo) -> dict[str, Any]:
    sat = cyc.kvl_saturation_threshold_mw
    return {
        "buses": cyc.buses,
        "n_buses": cyc.n_buses,
        "score": cyc.score,
        "asym_X": cyc.asym_x if not math.isinf(cyc.asym_x) else None,
        "asym_tmax": cyc.asym_tmax if not math.isinf(cyc.asym_tmax) else None,
        "min_tmax": cyc.min_tmax,
        "kvl_saturation_threshold_mw": (
            sat if sat is not None and not math.isinf(sat) else None
        ),
        "segments": [_line_to_dict(s) for s in cyc.segments],
    }


def _reserve_zone_issue_to_dict(iss: ReserveZoneIssue) -> dict[str, Any]:
    return {
        "zone": iss.zone,
        "issue": iss.issue,
        "n_providers": iss.n_providers,
    }


def _island_feasibility_to_dict(iss: IslandFeasibility) -> dict[str, Any]:
    return {
        "island_idx": iss.island_idx,
        "n_demand_buses": iss.n_demand_buses,
        "n_gen_buses": iss.n_gen_buses,
        "sum_pmax": iss.sum_pmax,
        "sum_peak_load": iss.sum_peak_load,
        "issue": iss.issue,
    }


def _battery_eini_to_dict(iss: BatteryEiniIssue) -> dict[str, Any]:
    return {
        "name": iss.name,
        "issue": iss.issue,
        "eini": iss.eini,
        "emin_first": iss.emin_first,
        "emax_first": iss.emax_first,
        "in_ucs": list(iss.in_ucs),
    }


def _tv_capacity_to_dict(iss: TimeVaryingCapacityIssue) -> dict[str, Any]:
    return {
        "uid": iss.uid,
        "name": iss.name,
        "bus_a": iss.bus_a,
        "bus_b": iss.bus_b,
        "blocks_at_zero": iss.blocks_at_zero,
        "blocks_near_zero": iss.blocks_near_zero,
        "blocks_total": iss.blocks_total,
        "max_tmax": iss.max_tmax,
    }


def _reservoir_cascade_to_dict(iss: ReservoirCascadeIssue) -> dict[str, Any]:
    return {
        "name": iss.name,
        "issue": iss.issue,
        "detail": iss.detail,
    }


def _parallel_identical_to_dict(iss: ParallelCircuitIdentical) -> dict[str, Any]:
    return {
        "bus_a": iss.bus_a,
        "bus_b": iss.bus_b,
        "n_circuits": iss.n_circuits,
        "X": iss.reactance,
        "tmax": iss.tmax,
        "uids": list(iss.uids),
        "on_dangerous_cycle": iss.on_dangerous_cycle,
    }


def _voltage_mix_to_dict(mix: VoltageMix) -> dict[str, Any]:
    return {
        "uid": mix.uid,
        "name": mix.name,
        "bus_a": mix.bus_a,
        "bus_b": mix.bus_b,
        "bus_a_V": mix.bus_a_voltage,
        "bus_b_V": mix.bus_b_voltage,
    }


def _negative_to_dict(neg: NegativeImpedance) -> dict[str, Any]:
    return asdict(neg)


def report_to_json(report: AnalysisReport) -> dict[str, Any]:
    """Convert an :class:`AnalysisReport` to a JSON-serialisable dict."""
    return {
        "bundle": report.bundle,
        "n_buses": report.n_buses,
        "n_lines": report.n_lines,
        "n_cycles": report.n_cycles,
        "danger_threshold": report.danger_threshold,
        "islands": [_island_to_dict(i) for i in report.islands],
        "isolated_buses": list(report.isolated_buses),
        "stubs": [_line_to_dict(s) for s in report.stubs],
        "dangerous_cycles": [_cycle_to_dict(c) for c in report.dangerous_cycles],
        "benign_cycles": [_cycle_to_dict(c) for c in report.benign_cycles],
        "sos1_candidates": [_line_to_dict(s) for s in report.sos1_candidates],
        "bridges": [_line_to_dict(b) for b in report.bridges],
        "dc_lines": [_line_to_dict(ref) for ref in report.dc_lines],
        "negative_impedance": [_negative_to_dict(n) for n in report.negative_impedance],
        "voltage_mixing": [_voltage_mix_to_dict(m) for m in report.voltage_mixing],
        "self_loops": [_line_to_dict(ref) for ref in report.self_loops],
        "skipped_unknown_bus": [
            _line_to_dict(ref) for ref in report.skipped_unknown_bus
        ],
        "reserve_zone_coherence": [
            _reserve_zone_issue_to_dict(r) for r in report.reserve_zone_coherence
        ],
        "island_feasibility": [
            _island_feasibility_to_dict(i) for i in report.island_feasibility
        ],
        "battery_eini_issues": [
            _battery_eini_to_dict(b) for b in report.battery_eini_issues
        ],
        "time_varying_capacity_issues": [
            _tv_capacity_to_dict(t) for t in report.time_varying_capacity_issues
        ],
        "reservoir_cascade_issues": [
            _reservoir_cascade_to_dict(r) for r in report.reservoir_cascade_issues
        ],
        "parallel_circuit_identical": [
            _parallel_identical_to_dict(p) for p in report.parallel_circuit_identical
        ],
    }


def sos1_candidates_payload(report: AnalysisReport) -> dict[str, Any]:
    """Return the converter-facing SOS1 candidate list.

    Schema:
        {
            "bundle": "<file>",
            "danger_threshold": 5,
            "count": N,
            "lines": [{"uid": ..., "name": ..., "bus_a": ..., "bus_b": ...}]
        }
    """
    return {
        "bundle": report.bundle,
        "danger_threshold": report.danger_threshold,
        "count": len(report.sos1_candidates),
        "lines": [
            {
                "uid": ref.uid,
                "name": ref.name,
                "bus_a": ref.bus_a,
                "bus_b": ref.bus_b,
            }
            for ref in report.sos1_candidates
        ],
    }


def write_json(payload: dict[str, Any], path: str) -> None:
    """Write ``payload`` to ``path`` as a UTF-8 JSON document (2-space indent)."""
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False, default=str)
        fh.write("\n")
