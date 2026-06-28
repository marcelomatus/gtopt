# SPDX-License-Identifier: BSD-3-Clause
"""Output validation checks and power-system indicators."""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd

from ._reader import (
    dataset_layout,
    dataset_uid_cols,
    get_block_durations,
    get_generator_info,
    get_generator_profile_info,
    get_line_info,
    open_dataset,
    streaming_abs_weighted_sum_per_uid,
    streaming_pairwise_weighted_sum,
    streaming_sol_weighted_sum,
    streaming_sol_weighted_sum_per_uid,
    streaming_sqr_weighted_sum_per_uid,
    streaming_uid_stats,
    streaming_uid_sum,
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
        ("Line/flow_sol", "line flows (signed)"),
    ]

    # Cheap existence probe — DO NOT call `read_table` here.  On wide
    # hive-partitioned streams (Generator/generation_sol, etc.)
    # `pd.read_parquet` would materialise the entire ~70 GB dataset in
    # pandas RAM just to answer "does this exist?".  A filesystem
    # `exists()` / glob check is the right tool.
    def _stem_present(stem: str) -> bool:
        pq = results_dir / (stem + ".parquet")
        if pq.is_dir() or pq.is_file():
            return True
        parent = results_dir / Path(stem).parent
        name = Path(stem).name
        for ext in (".csv", ".csv.zst", ".csv.gz"):
            if (results_dir / (stem + ext)).is_file():
                return True
            if any(parent.glob(f"{name}_s*_p*{ext}")):
                return True
        return False

    for stem, label in required:
        if not _stem_present(stem):
            findings.append(
                Finding(
                    "expected_files",
                    "CRITICAL",
                    f"missing required output: {label} ({stem})",
                )
            )

    for stem, label in expected:
        if not _stem_present(stem):
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
    """Check for unserved demand (load shedding).  Streaming aggregation —
    never materialises the full Demand/fail_sol dataset in memory."""
    findings: list[Finding] = []
    fail_ds = open_dataset(results_dir, "Demand/fail_sol")
    if fail_ds is None:
        return findings

    stats = streaming_uid_stats(fail_ds)
    if not stats:
        return findings
    total_shed = stats["sum"]
    max_shed = stats["max"]

    if total_shed > 0.01:
        # Energy = Σ fail × duration(block).  Stream over partition batches
        # so even a 70 GB pandas equivalent stays at one batch (~80 MB)
        # in memory at a time.
        durations = get_block_durations(planning)
        energy_shed = streaming_sol_weighted_sum(fail_ds, durations)
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
    """Compare total generation to total demand served.  Streaming —
    aggregates `sum()` directly from per-partition record batches."""
    findings: list[Finding] = []
    gen_ds = open_dataset(results_dir, "Generator/generation_sol")
    load_ds = open_dataset(results_dir, "Demand/load_sol")

    if gen_ds is None or load_ds is None:
        return findings

    gen_total = streaming_uid_sum(gen_ds)
    load_total = streaming_uid_sum(load_ds)

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
    """Compute energy production (MWh) by generator type — streaming.

    Old version materialised `Generator/generation_sol` as a wide pandas
    DataFrame (6.6M rows × 1335 cols × 8 bytes ≈ 71 GB) and then walked
    every cell via a per-uid Python `for idx, val in df[col].items()`
    inner loop.  Both the materialisation and the row-by-row walk were
    catastrophic on the 2-year case — the 80 GB WSL OOM.  The new path
    asks `pyarrow.dataset.scanner()` to yield per-partition record
    batches (~80 MB each), aggregates per-uid sums in C++-backed
    Arrow code, then multiplies by the per-block duration via a
    weighted streaming sum.
    """
    findings: list[Finding] = []
    gen_ds = open_dataset(results_dir, "Generator/generation_sol")
    if gen_ds is None:
        return findings, {}

    gen_info = get_generator_info(planning)
    durations = get_block_durations(planning)

    # Build uid → type mapping
    uid_type: dict[int, str] = {}
    for _, row in gen_info.iterrows():
        uid_type[int(row["uid"])] = row["type"]

    # Per-uid energy = Σ value(s,t,b) · duration(b).  Pass `coef_per_uid=1`
    # to get the duration-weighted per-uid totals.  pyarrow streams over
    # partitions; peak memory is one record batch.
    uid_energy = streaming_sol_weighted_sum_per_uid(gen_ds, durations)
    if not uid_energy:
        return findings, {}

    energy_by_type: dict[str, float] = {}
    for uid, energy in uid_energy.items():
        gtype = uid_type.get(uid, "unknown")
        energy_by_type[gtype] = energy_by_type.get(gtype, 0.0) + energy

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


def _streaming_uid_abs_max_per_col(dataset) -> dict[int, float]:
    """Per-uid `max(|value|)` aggregated via pyarrow streaming.

    Layout-aware (mirrors ``_reader.streaming_uid_sum_per_col``): the
    long-form path groups ``|value|`` by ``uid`` per batch, the wide-form
    path iterates the ``uid:N`` columns.  Long form drops zero rows, so a
    uid that is identically zero is simply absent from the result — which
    is the right semantics for the congestion ranking (a never-flowing
    line has zero utilization and is not congested)."""
    if dataset is None:
        return {}
    import pyarrow as pa  # noqa: PLC0415
    import pyarrow.compute as pc  # noqa: PLC0415

    out: dict[int, float] = {}
    if dataset_layout(dataset) == "long":
        for batch in dataset.scanner(columns=["uid", "value"]).to_batches():
            tbl = pa.table({"uid": batch["uid"], "absv": pc.abs(batch["value"])})
            grouped = tbl.group_by("uid").aggregate([("absv", "max")])
            uids = grouped["uid"].to_pylist()
            maxes = grouped["absv_max"].to_pylist()
            for u, m in zip(uids, maxes):
                if u is None or m is None:
                    continue
                ui = int(u)
                if float(m) > out.get(ui, 0.0):
                    out[ui] = float(m)
        return out
    cols = dataset_uid_cols(dataset)
    out = {int(c.split(":")[1]): 0.0 for c in cols}
    for batch in dataset.scanner(columns=cols).to_batches():
        for col in cols:
            arr = pc.abs(batch[col])
            mx = pc.max(arr).as_py()
            if mx is not None and mx > out[int(col.split(":")[1])]:
                out[int(col.split(":")[1])] = float(mx)
    return out


def compute_congestion_ranking(
    results_dir: Path, planning: dict, top_n: int = 10
) -> list[Finding]:
    """Rank transmission lines by congestion (utilization of capacity).
    Streaming: per-uid max(|flow|) accumulated across record batches —
    never materialises the wide Line/flow_sol table.

    Prefers the unified ``Line/flow_sol`` signed primal; falls back to
    the legacy directional pair ``Line/flowp_sol`` + ``Line/flown_sol``
    when reading a pre-unified-flow gtopt output.
    """
    findings: list[Finding] = []
    flow_ds = open_dataset(results_dir, "Line/flow_sol")
    if flow_ds is not None:
        fp_max = _streaming_uid_abs_max_per_col(flow_ds)
        fn_max: dict[int, float] = {}
    else:
        flowp_ds = open_dataset(results_dir, "Line/flowp_sol")
        if flowp_ds is None:
            return findings
        flown_ds = open_dataset(results_dir, "Line/flown_sol")
        fp_max = _streaming_uid_abs_max_per_col(flowp_ds)
        fn_max = (
            _streaming_uid_abs_max_per_col(flown_ds) if flown_ds is not None else {}
        )

    line_info = get_line_info(planning)
    if line_info.empty:
        return findings

    uid_tmax: dict[int, float] = {}
    uid_name: dict[int, str] = {}
    for _, row in line_info.iterrows():
        uid_tmax[int(row["uid"])] = float(row["tmax"]) if row["tmax"] > 0 else 1e9
        uid_name[int(row["uid"])] = row["name"]

    utilizations: list[tuple[str, float, float]] = []
    for uid, fp in fp_max.items():
        tmax = uid_tmax.get(uid, 1e9)
        name = uid_name.get(uid, f"uid:{uid}")
        max_flow = max(fp, fn_max.get(uid, 0.0))
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
    """Compute LMP (locational marginal price) statistics per bus.
    Streaming: aggregates min/max/sum/count/negative-count per record
    batch via pyarrow; never materialises the wide LMP table."""
    findings: list[Finding] = []
    lmp_ds = open_dataset(results_dir, "Bus/balance_dual")
    if lmp_ds is None:
        return findings

    stats = streaming_uid_stats(lmp_ds)
    if not stats or stats.get("count", 0) == 0:
        return findings

    overall_mean = stats["mean"]
    overall_max = stats["max"]
    overall_min = stats["min"]
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
    neg_count = stats.get("n_neg", 0)
    if neg_count > 0:
        findings.append(
            Finding(
                "lmp_stats",
                "WARNING",
                f"{neg_count} negative LMP values detected (possible over-generation)",
            )
        )

    return findings


def _per_block_durations(planning: dict, df: pd.DataFrame) -> pd.Series:
    """Map each row's `block` column to its duration (hours)."""
    block_dur = get_block_durations(planning)
    if "block" in df.columns:
        return df["block"].map(block_dur).fillna(1.0).astype(float)
    return pd.Series(1.0, index=df.index, dtype=float)


def _scalar_from_planning_field(value: object, fallback: float = 0.0) -> float:
    """Extract a scalar from a planning-JSON cost field.  Returns
    `fallback` when the field is a parquet file reference, a TB
    schedule, or anything other than a single number — those need
    per-(stage, block) handling which the cost breakdown does not
    attempt (file references would also force loading per-element
    parquets, which is out of scope for this top-level summary)."""
    if isinstance(value, (int, float)):
        return float(value)
    return fallback


def compute_cost_breakdown(results_dir: Path, planning: dict) -> list[Finding]:
    """Total system cost broken down by component.

    Each component is computed from the LP primal solution multiplied
    by its cost coefficient and the block duration:

        cost_component = Σ_{s,t,b,uid}
            sol(s,t,b,uid) · coefficient(uid, s,t,b) · duration(b)

    Earlier versions summed the per-column REDUCED COSTS from
    `*_cost.parquet`.  That is wrong: for an interior basic LP column
    the reduced cost is zero by complementary slackness, so summing
    rcs systematically understates the real component cost (and the
    "total system cost" headline never matched the LP objective).
    Switching to `sol × coefficient × duration` also lets this check
    work without the per-element reduced-cost streams, so a leaner
    `--write-out` flag is enough to produce a correct breakdown.

    Cost coefficients sourced from the planning JSON:
      * Generator → uses `Generator/srmc_sol.parquet` (already in
        physical $/MWh and segment-aware for piecewise heat rate).
      * Demand    → `model_options.demand_fail_cost` (per-system
        scalar) times `Demand/fail_sol.parquet`.
      * Line      → `tcost` per line (scalar or skipped if file-ref).

    Fields stored as a parquet file reference / per-(stage, block)
    schedule are treated as zero contribution here — they need
    per-block resolution that the top-level cost summary does not
    attempt.  The full per-block cost surface is still recoverable
    downstream by joining `*_sol` with the corresponding cost
    schedule (or, for Generator, by reading `vom_cost_sol` and
    `fuel_cost_sol` opt-in via `--write-out extras`).
    """
    findings: list[Finding] = []
    components: dict[str, float] = {}
    durations = get_block_durations(planning)

    # ─── Generation cost: Σ srmc · generation · duration ────────────────
    # Streaming: per partition, multiply srmc batch × generation batch
    # × duration vector and accumulate.  Skips the pandas wide-DF
    # materialisation that previously consumed ~140 GB on the 2y case
    # (srmc + generation = two ~70 GB wide DataFrames plus a product).
    srmc_ds = open_dataset(results_dir, "Generator/srmc_sol")
    gen_ds = open_dataset(results_dir, "Generator/generation_sol")
    if srmc_ds is not None and gen_ds is not None:
        gen_total = streaming_pairwise_weighted_sum(srmc_ds, gen_ds, durations)
        if abs(gen_total) > 0.01:
            components["generation"] = gen_total

    # ─── Load-shedding penalty: demand_fail_cost · fail · duration ─────
    fail_ds = open_dataset(results_dir, "Demand/fail_sol")
    if fail_ds is not None:
        model_opts = planning.get("options", {}).get("model_options", {})
        dfc = _scalar_from_planning_field(
            model_opts.get("demand_fail_cost"), fallback=0.0
        )
        if dfc > 0.0:
            fail_total = streaming_sol_weighted_sum(fail_ds, durations) * dfc
            if abs(fail_total) > 0.01:
                components["load shedding penalty"] = fail_total

    # ─── Transmission cost: tcost · flow · duration ────────────────────
    line_tcost: dict[int, float] = {}
    for ln in planning.get("system", {}).get("line_array", []):
        line_tcost[int(ln.get("uid", 0))] = _scalar_from_planning_field(
            ln.get("tcost"), fallback=0.0
        )
    # Unified signed flow when available (one stem covers both directions
    # — ``tcost × |flow|`` integrates the magnitude regardless of sign);
    # fall back to the legacy ``flowp_sol`` / ``flown_sol`` directional
    # pair for older gtopt outputs.
    flow_ds_unified = open_dataset(results_dir, "Line/flow_sol")
    if flow_ds_unified is not None:
        total = streaming_sol_weighted_sum(
            flow_ds_unified, durations, line_tcost, abs_value=True
        )
        if abs(total) > 0.01:
            components["transmission"] = total
    else:
        for stem, label in (
            ("Line/flowp_sol", "transmission (A→B)"),
            ("Line/flown_sol", "transmission (B→A)"),
        ):
            flow_ds = open_dataset(results_dir, stem)
            if flow_ds is None:
                continue
            total = streaming_sol_weighted_sum(flow_ds, durations, line_tcost)
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
    """Check battery state-of-charge bounds (streaming)."""
    findings: list[Finding] = []
    soc_ds = open_dataset(results_dir, "Battery/energy_sol")
    if soc_ds is None:
        return findings

    stats = streaming_uid_stats(soc_ds)
    neg = stats.get("n_neg", 0) if stats else 0
    if neg > 0:
        findings.append(
            Finding("battery_soc", "WARNING", f"{neg} negative battery SoC values")
        )
    else:
        findings.append(Finding("battery_soc", "INFO", "battery SoC within bounds"))
    return findings


def check_transmission_losses(
    results_dir: Path, planning: dict
) -> tuple[list[Finding], dict]:
    """Estimate transmission losses two complementary ways and report
    the gap (= battery / storage internal losses + numerical residue).

    * **Balance method** (energy-conservation closure):

        losses_balance = Σ_gen − Σ_load
                       = Σ_gen + Σ_(bat_dis) − Σ_(load_real) − Σ_(bat_chg)

      where ``Generator/generation_sol`` is exported by gtopt with one
      phantom generator per battery (whose energy equals
      ``Battery/fout_sol``) and ``Demand/load_sol`` is exported with
      one phantom demand per battery (whose energy equals
      ``Battery/finp_sol``).  The phantoms cancel on both sides so a
      naive ``Σ_gen − Σ_load`` over the whole datasets gives the right
      closure (line losses + battery round-trip losses + reservoir /
      storage net change).

    * **Analytical method** (DC-OPF quadratic loss approximation):

        losses_analytical = Σ_lines (R / V²) · Σ_(s,t,b) flow²(s,t,b) · dur(b)

      where R is line resistance [Ω], V is the line nominal voltage [kV],
      and flow [MW] comes from ``Line/flow_sol``.  This captures
      transmission losses only (no battery, no reservoir).

    The difference ``balance − analytical`` is dominated by the battery
    round-trip loss ``Σ_(bat_chg − bat_dis)`` plus any storage
    net-change over the horizon.  A small residue after subtracting
    those is expected (DC-OPF linearisation + line-loss segmentation).
    """
    findings: list[Finding] = []
    indicators: dict = {}

    gen_ds = open_dataset(results_dir, "Generator/generation_sol")
    load_ds = open_dataset(results_dir, "Demand/load_sol")
    if gen_ds is None or load_ds is None:
        return findings, indicators

    durations = get_block_durations(planning)

    # --- Balance method (full datasets — phantoms cancel) -----------------
    gen_energy = streaming_sol_weighted_sum(gen_ds, durations)
    load_energy = streaming_sol_weighted_sum(load_ds, durations)
    losses_balance = gen_energy - load_energy

    # Battery round-trip loss (optional — appears as the bulk of the
    # balance-vs-analytical gap when batteries cycle a lot).
    bat_chg_ds = open_dataset(results_dir, "Battery/finp_sol")
    bat_dis_ds = open_dataset(results_dir, "Battery/fout_sol")
    bat_chg_e = streaming_sol_weighted_sum(bat_chg_ds, durations) if bat_chg_ds else 0.0
    bat_dis_e = streaming_sol_weighted_sum(bat_dis_ds, durations) if bat_dis_ds else 0.0
    battery_roundtrip_loss = bat_chg_e - bat_dis_e

    # --- Analytical method ------------------------------------------------
    flow_ds = open_dataset(results_dir, "Line/flow_sol")
    line_info = get_line_info(planning)
    losses_analytical: float | None = None
    loss_per_line: dict[int, float] = {}
    throughput_per_line: dict[int, float] = {}
    if flow_ds is not None and not line_info.empty:
        # Coefficient per line: R / V²  (MW per MW²).  Lines with
        # zero R or zero V contribute zero (skip — no loss model).
        coef_per_uid: dict[int, float] = {}
        for _, row in line_info.iterrows():
            R = float(row.get("resistance", 0.0))
            V = float(row.get("voltage", 0.0))
            if R > 0.0 and V > 0.0:
                coef_per_uid[int(row["uid"])] = R / (V * V)
        if coef_per_uid:
            # Per-line analytical loss energy (used both for the total
            # and for ranking + arbitrage detection).  The total is
            # just `sum(loss_per_line.values())` — no need for the
            # separate scalar streaming pass.
            loss_per_line = streaming_sqr_weighted_sum_per_uid(
                flow_ds, durations, coef_per_uid
            )
            losses_analytical = sum(loss_per_line.values())
            # Per-line throughput (Σ |flow| × dur) — used as the
            # denominator in loss / throughput ratio.  A line whose
            # ratio is much higher than its sister lines (or higher
            # than the loss-percentage you'd get from R · I_max² at
            # nameplate flow) is a loss-arbitrage candidate.
            throughput_per_line = streaming_abs_weighted_sum_per_uid(flow_ds, durations)

    # --- Report -----------------------------------------------------------
    findings.append(
        Finding(
            "losses",
            "INFO",
            f"transmission losses (balance method): {losses_balance:.1f} MWh "
            f"(gen − load = {losses_balance / max(load_energy, 1.0) * 100:.2f}% "
            f"of served demand)",
        )
    )
    if losses_analytical is not None:
        gap = losses_balance - losses_analytical
        pct_demand = losses_analytical / max(load_energy, 1.0) * 100
        findings.append(
            Finding(
                "losses",
                "INFO",
                f"transmission losses (analytical Σ R·P²/V²): "
                f"{losses_analytical:.1f} MWh ({pct_demand:.2f}% of demand)",
            )
        )
        # Most of the gap should be battery round-trip loss.
        residue = gap - battery_roundtrip_loss
        findings.append(
            Finding(
                "losses",
                "INFO",
                f"balance − analytical = {gap:.1f} MWh "
                f"(battery roundtrip = {battery_roundtrip_loss:.1f}; "
                f"residue = {residue:.1f})",
            )
        )
        # Sanity flag: large positive residue after battery accounting
        # suggests either DC-OPF linearisation error (large dur ≠ 1
        # blocks under heavy flow) or that the gtopt LP secant
        # over-estimates losses on the LP side.
        if abs(residue) > 0.05 * load_energy:
            findings.append(
                Finding(
                    "losses",
                    "WARNING",
                    f"large residue after battery roundtrip "
                    f"({residue:.1f} MWh = "
                    f"{abs(residue) / max(load_energy, 1.0) * 100:.1f}% of demand) — "
                    f"check line-loss model or reservoir / storage net change",
                )
            )

    # --- Per-line ranking + loss-arbitrage detection ------------------------
    # Two complementary per-line views:
    #   1. Top-10 lines by analytical loss energy — where physical losses
    #      concentrate (long high-flow corridors).
    #   2. Top-10 lines by loss / throughput ratio — where the LP may be
    #      cycling flow back-and-forth across a lossy line ("loss
    #      arbitrage"), inflating both directional flows beyond the net
    #      transfer the line actually accomplishes.  Without the
    #      per-direction `Line/loss_*_sol` streams in the output (this
    #      run uses `write_out: sol,dual,rc:Generator,Line` which omits
    #      them), the symptom shows up as a loss-fraction much larger
    #      than the line's `(R · tmax) / V² ≈ I²R` baseline.
    if loss_per_line:
        line_name = dict(zip(line_info["uid"], line_info["name"], strict=False))
        line_R = dict(zip(line_info["uid"], line_info["resistance"], strict=False))
        line_V = dict(zip(line_info["uid"], line_info["voltage"], strict=False))
        line_tmax = dict(zip(line_info["uid"], line_info["tmax"], strict=False))

        top_loss = sorted(loss_per_line.items(), key=lambda kv: -kv[1])[:10]
        findings.append(
            Finding(
                "losses",
                "INFO",
                "top 10 lines by analytical loss energy (per-line R·P²/V² · dur, MWh):",
            )
        )
        for uid, e in top_loss:
            findings.append(
                Finding(
                    "losses",
                    "INFO",
                    f"  {line_name.get(uid, f'uid:{uid}'):<35} {e:>12.1f} MWh  "
                    f"(R={float(line_R.get(uid, 0.0)):.2f} Ω, "
                    f"V={float(line_V.get(uid, 0.0)):.0f} kV)",
                )
            )

        # Loss-arbitrage candidates.  Baseline upper bound on physical
        # loss-fraction = (R · tmax) / V² ≈ I²R / P at nameplate flow.
        # A line whose `loss / throughput` exceeds this by a large
        # factor (heuristic: 3×) is suspicious — the LP is cycling
        # flow beyond what's needed to transfer power.
        candidates = []
        for uid, loss_e in loss_per_line.items():
            thr = throughput_per_line.get(uid, 0.0)
            if thr <= 0.0 or loss_e <= 0.0:
                continue
            ratio = loss_e / thr
            R = float(line_R.get(uid, 0.0))
            V = float(line_V.get(uid, 0.0))
            tmax = float(line_tmax.get(uid, 0.0))
            baseline = (R * tmax) / (V * V) if (V > 0 and tmax > 0) else 0.0
            # Skip tiny lines (avoid divide-by-near-zero noise).
            if thr < 1.0:
                continue
            if baseline > 0 and ratio > 3.0 * baseline:
                candidates.append((uid, loss_e, ratio, baseline, thr))
        if candidates:
            candidates.sort(key=lambda r: -r[2] / r[3] if r[3] > 0 else 0)
            findings.append(
                Finding(
                    "losses",
                    "WARNING",
                    f"potential loss-arbitrage candidates: {len(candidates)} "
                    f"line(s) with loss / throughput >> nameplate I²R baseline "
                    f"(top 10 shown)",
                )
            )
            for uid, loss_e, ratio, baseline, thr in candidates[:10]:
                factor = ratio / baseline if baseline > 0 else float("inf")
                findings.append(
                    Finding(
                        "losses",
                        "WARNING",
                        f"  {line_name.get(uid, f'uid:{uid}'):<35} "
                        f"loss={loss_e:>10.1f} MWh, "
                        f"flow_energy={thr:>10.1f} MWh, "
                        f"loss_frac={ratio * 100:.3f}% vs baseline "
                        f"{baseline * 100:.3f}% ({factor:.1f}×)",
                    )
                )

    indicators["losses"] = {
        "balance_method_MWh": losses_balance,
        "analytical_method_MWh": losses_analytical,
        "battery_roundtrip_MWh": battery_roundtrip_loss,
        "demand_MWh": load_energy,
        "loss_per_line_MWh": dict(loss_per_line),
        "throughput_per_line_MWh": dict(throughput_per_line),
    }
    return findings, indicators


def check_reservoir_levels(results_dir: Path, planning: dict) -> list[Finding]:
    """Check reservoir energy levels (streaming)."""
    findings: list[Finding] = []
    energy_ds = open_dataset(results_dir, "Reservoir/energy_sol")
    if energy_ds is None:
        return findings

    stats = streaming_uid_stats(energy_ds)
    neg = stats.get("n_neg", 0) if stats else 0
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

    spill_ds = open_dataset(results_dir, "GeneratorProfile/spillover_sol")
    if spill_ds is None:
        return findings, curtailment_data

    gen_ds = open_dataset(results_dir, "Generator/generation_sol")
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

    # Per-profile curtailment energy = Σ spillover · duration(block).
    # Per-linked-generator actual energy = Σ generation · duration(block).
    # Both via streaming aggregators — never materialises the wide
    # spillover or generation tables in memory.  The old per-row Python
    # iteration (`for idx, val in df[col].items()` × every uid) was the
    # second-worst memory + CPU offender after the wide-DF
    # materialisation itself.
    spill_energy_per_uid = streaming_sol_weighted_sum_per_uid(spill_ds, durations)
    gen_energy_per_uid = (
        streaming_sol_weighted_sum_per_uid(gen_ds, durations) if gen_ds else {}
    )

    if not spill_energy_per_uid:
        # Long-form output drops zero rows, so an empty spillover map means
        # zero curtailment — emit the same INFO finding as the explicit
        # all-zero wide-form case rather than returning silently.
        findings.append(
            Finding(
                "renewable_curtailment",
                "INFO",
                "no renewable curtailment (all profiled generators at full output)",
            )
        )
        # Populate ``curtailment_data`` with zero for every profile so
        # downstream consumers (e.g. the bat4b24 igtopt smoke test) can
        # assert ``data[profile_name] == 0`` without first checking
        # whether the spillover dataset had any rows.  Mirrors the
        # wide-form all-zero path below where each profile is added to
        # ``curtailment_data`` with its computed (zero) energy.
        for profile_uid, pname in profile_uid_name.items():
            curtailment_data[pname] = 0.0
        return findings, curtailment_data

    total_curtailment_energy = 0.0
    total_potential_energy = 0.0
    per_profile: list[tuple[str, str, float, float, float]] = []

    for profile_uid, curtail_energy in spill_energy_per_uid.items():
        pname = profile_uid_name.get(profile_uid, f"uid:{profile_uid}")
        gen_uid = profile_uid_gen.get(profile_uid, -1)
        gen_name = gen_uid_name.get(gen_uid, f"uid:{gen_uid}")

        actual_energy = gen_energy_per_uid.get(gen_uid, 0.0)
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

    losses_findings, losses_indicators = check_transmission_losses(
        results_dir, planning
    )
    report.findings.extend(losses_findings)
    report.indicators["losses"] = losses_indicators.get("losses", {})

    return report
