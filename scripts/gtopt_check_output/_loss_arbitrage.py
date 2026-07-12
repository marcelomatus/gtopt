# SPDX-License-Identifier: BSD-3-Clause
"""Loss-arbitrage detector for gtopt solution outputs.

Flags per-(line, block) symptoms of fictitious transmission losses —
the LP exploiting the loss model as an energy sink when a bus-dual
pair-sum goes negative (see ``include/gtopt/line_enums.hpp`` and
``source/line_losses.cpp`` for the channel taxonomy):

* **Phantom circulation** (channel A): both directional flows are
  simultaneously positive and their sum dwarfs the net transfer.
  Requires the directional split (``Line/flown_sol`` — emitted with
  ``--write-out ...,extras:Line`` — or the legacy
  ``Line/flowp_sol``/``Line/flown_sol`` pair).
* **Loss inflation** (channels B/C/D): booked loss exceeds the
  physical quadratic ``(R/V²)·f²`` at the net flow.
* **Idle-line loss** (channel D at f=0 / channel E): booked loss on a
  line carrying (near-)zero flow.

For every flagged line the detector also reports the worst bus-dual
pair-sum ``π_a + π_b`` across the flagged blocks — the sizing input
for the ``loss_cost_eps`` counter-measure: the loss sink is profitable
only while ``ε < −(π_a + π_b)/2``, so an override of
``ceil(|worst pair-sum| / 2 × safety)`` closes the channel on that
line.

.. warning::
   The sink **MIGRATES**: pricing the flagged lines pushes the LP to
   the next-cheapest lossy line.  Merge the overrides, re-solve, and
   re-run this check until no new lines are flagged.
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd

from ._reader import (
    dataset_layout,
    get_block_durations,
    get_line_info,
    open_dataset,
)

log = logging.getLogger(__name__)

# Symptom labels (row precedence: phantom > idle > inflation).
SYMPTOM_PHANTOM = "phantom-circulation"
SYMPTOM_IDLE = "idle-line-loss"
SYMPTOM_INFLATION = "loss-inflation"

MIGRATION_NOTE = (
    "the fictitious-loss sink MIGRATES to the next-cheapest line: "
    "merge the loss_cost_eps overrides into the case, re-solve, and "
    "re-run this check until no new lines are flagged"
)

_KEY = ["scenario", "stage", "block", "uid"]


@dataclass
class LineDiagnosis:
    """Per-line loss-arbitrage diagnosis."""

    uid: int
    name: str
    worst_symptom: str
    arbitrage_mwh: float
    worst_pair_sum: float | None
    n_flagged: int


@dataclass
class LossArbitrageReport:
    """Aggregated loss-arbitrage report."""

    diagnoses: list[LineDiagnosis] = field(default_factory=list)
    total_fictitious_mwh: float = 0.0
    notes: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        """True when no line shows arbitrage symptoms."""
        return not self.diagnoses


def _load_long(results_dir: Path, stem: str) -> pd.DataFrame | None:
    """Load an output stream as a long-form ``(scenario, stage, block,
    uid, value)`` DataFrame regardless of the on-disk layout.

    Wide datasets (one ``uid:N`` column per element) are melted; long
    datasets pass through.  Line-class streams are small (lines ×
    blocks), so materialising them is fine — this is NOT for the wide
    Generator streams.
    """
    dataset = open_dataset(results_dir, stem)
    if dataset is None:
        return None
    names = set(dataset.schema.names)
    id_cols = [c for c in ("scenario", "stage", "block") if c in names]
    if dataset_layout(dataset) == "long":
        df = dataset.to_table(columns=id_cols + ["uid", "value"]).to_pandas()
    else:
        uid_cols = [n for n in dataset.schema.names if n.startswith("uid:")]
        if not uid_cols:
            return None
        df = dataset.to_table(columns=id_cols + uid_cols).to_pandas()
        df = df.melt(
            id_vars=id_cols,
            value_vars=uid_cols,
            var_name="uid",
            value_name="value",
        )
        df["uid"] = df["uid"].str.split(":").str[1].astype("int64")
    for col in ("scenario", "stage", "block"):
        if col not in df.columns:
            df[col] = 1
    df["uid"] = pd.to_numeric(df["uid"], errors="coerce").fillna(0).astype("int64")
    df["value"] = pd.to_numeric(df["value"], errors="coerce").fillna(0.0)
    return df[_KEY + ["value"]]


def _bus_uid_map(planning: dict) -> dict[str, int]:
    """Map bus name AND stringified uid → bus uid."""
    out: dict[str, int] = {}
    for bus in planning.get("system", {}).get("bus_array", []):
        uid = int(bus.get("uid", 0))
        out[str(uid)] = uid
        name = bus.get("name")
        if name is not None:
            out[str(name)] = uid
    return out


def _resolve_bus_uid(ref: object, name_to_uid: dict[str, int]) -> int | None:
    """Resolve a line's ``bus_a``/``bus_b`` reference (uid or name)."""
    if isinstance(ref, bool):
        return None
    if isinstance(ref, int):
        return ref
    return name_to_uid.get(str(ref))


def _load_flows(
    results_dir: Path,
) -> tuple[pd.DataFrame | None, bool, list[str]]:
    """Return ``(flows, has_directional, notes)``.

    ``flows`` carries columns ``scenario, stage, block, uid, flow`` and
    — when the directional split is available — ``flow_p`` / ``flow_n``.
    """
    notes: list[str] = []
    flow = _load_long(results_dir, "Line/flow_sol")
    flown = _load_long(results_dir, "Line/flown_sol")
    if flow is not None:
        flow = flow.rename(columns={"value": "flow"})
        if flown is not None:
            flown = flown.rename(columns={"value": "flow_n"})
            flow = flow.merge(flown, on=_KEY, how="outer")
            flow[["flow", "flow_n"]] = flow[["flow", "flow_n"]].fillna(0.0)
            flow["flow_p"] = flow["flow"] + flow["flow_n"]
            return flow, True, notes
        notes.append(
            "Line/flown_sol absent (rerun with --write-out ...,extras:Line) "
            "— phantom-circulation channel skipped"
        )
        return flow, False, notes
    # Legacy directional pair (pre-unified-flow outputs).
    flowp = _load_long(results_dir, "Line/flowp_sol")
    if flowp is None:
        return None, False, notes
    flowp = flowp.rename(columns={"value": "flow_p"})
    if flown is not None:
        flown = flown.rename(columns={"value": "flow_n"})
        flows = flowp.merge(flown, on=_KEY, how="outer")
    else:
        flows = flowp
        flows["flow_n"] = 0.0
    flows[["flow_p", "flow_n"]] = flows[["flow_p", "flow_n"]].fillna(0.0)
    flows["flow"] = flows["flow_p"] - flows["flow_n"]
    return flows, flown is not None, notes


def _attach_pair_sums(
    flagged: pd.DataFrame,
    duals: pd.DataFrame | None,
    bus_a_uid: dict[int, int | None],
    bus_b_uid: dict[int, int | None],
) -> pd.Series:
    """Per flagged row, the bus-dual pair-sum ``π_a + π_b``.

    Long-form dual outputs drop zero rows, so a missing dual at a
    flagged (scenario, stage, block) is a true zero — filled as such.
    Rows whose line has no resolvable bus mapping get NaN.
    """
    if duals is None:
        return pd.Series(float("nan"), index=flagged.index)
    stb = ["scenario", "stage", "block"]
    duals = duals.rename(columns={"uid": "bus_uid", "value": "pi"})
    duals = duals.drop_duplicates(subset=stb + ["bus_uid"], keep="last")
    duals["bus_uid"] = duals["bus_uid"].astype("float64")
    out = flagged[stb + ["uid"]].copy()
    # float64 keys so None-mapped buses become NaN (never merge-match).
    out["bus_a_uid"] = pd.to_numeric(out["uid"].map(bus_a_uid), errors="coerce")
    out["bus_b_uid"] = pd.to_numeric(out["uid"].map(bus_b_uid), errors="coerce")
    merged = out.merge(
        duals.rename(columns={"bus_uid": "bus_a_uid", "pi": "pi_a"}),
        on=stb + ["bus_a_uid"],
        how="left",
    ).merge(
        duals.rename(columns={"bus_uid": "bus_b_uid", "pi": "pi_b"}),
        on=stb + ["bus_b_uid"],
        how="left",
    )
    pi_a = merged["pi_a"].fillna(0.0)
    pi_b = merged["pi_b"].fillna(0.0)
    pair = pi_a + pi_b
    # No bus mapping at all → NaN (cannot size the override).
    no_map = merged["bus_a_uid"].isna() & merged["bus_b_uid"].isna()
    pair[no_map.to_numpy()] = float("nan")
    pair.index = flagged.index
    return pair


def detect_loss_arbitrage(
    results_dir: Path,
    planning: dict,
    *,
    tol: float = 1e-3,
    circulation_factor: float = 3.0,
) -> LossArbitrageReport:
    """Detect loss-arbitrage symptoms per (line, block).

    :param tol: flow / loss tolerance (MW per block; also the loss
        excess slack over the analytical ``(R/V²)·f²`` quadratic).
    :param circulation_factor: ``k`` in the phantom-circulation test
        ``flow_p + flow_n > k × |net flow|``.
    """
    report = LossArbitrageReport()

    flows, has_directional, notes = _load_flows(results_dir)
    report.notes.extend(notes)
    if flows is None:
        report.notes.append(
            "Line/flow_sol (and legacy flowp_sol) absent — check skipped"
        )
        return report

    loss = _load_long(results_dir, "Line/loss_sol")
    has_loss = loss is not None
    if loss is not None:
        loss = loss.rename(columns={"value": "loss"})
        df = flows.merge(loss, on=_KEY, how="outer")
    else:
        df = flows.copy()
        df["loss"] = 0.0
        report.notes.append(
            "Line/loss_sol absent — loss-inflation / idle-line channels "
            "skipped; fictitious energy uses the circulating-flow proxy"
        )
    num_cols = [c for c in ("flow", "flow_p", "flow_n", "loss") if c in df.columns]
    df[num_cols] = df[num_cols].fillna(0.0)

    line_info = get_line_info(planning)
    if line_info.empty:
        report.notes.append("no line_array in the planning JSON — check skipped")
        return report

    # Per-line R/V² coefficient (V defaults to 1.0 when absent/zero).
    coef: dict[int, float] = {}
    name_of: dict[int, str] = {}
    for _, row in line_info.iterrows():
        uid = int(row["uid"])
        resistance = float(row["resistance"])
        voltage = float(row["voltage"]) if float(row["voltage"]) > 0.0 else 1.0
        coef[uid] = resistance / (voltage * voltage)
        name_of[uid] = str(row["name"])

    df["analytic"] = df["uid"].map(coef).fillna(0.0) * df["flow"] * df["flow"]
    durations = get_block_durations(planning)
    df["dur"] = df["block"].map(durations).fillna(1.0)

    # ── Channels ────────────────────────────────────────────────────────
    if has_directional:
        min_dir = df[["flow_p", "flow_n"]].min(axis=1)
        phantom = (min_dir > tol) & (
            (df["flow_p"] + df["flow_n"]) > circulation_factor * df["flow"].abs()
        )
    else:
        min_dir = pd.Series(0.0, index=df.index)
        phantom = pd.Series(False, index=df.index)
    if has_loss:
        idle = (df["loss"] > tol) & (df["flow"].abs() < tol)
        inflation = (df["loss"] > df["analytic"] + tol) & ~idle
    else:
        idle = pd.Series(False, index=df.index)
        inflation = pd.Series(False, index=df.index)

    flagged_mask = phantom | idle | inflation
    if not bool(flagged_mask.any()):
        return report

    # Fictitious loss per row (MW): booked loss beyond the physical
    # quadratic at the net flow; circulating-flow proxy when the loss
    # stream is absent (phantom-only detection).
    if has_loss:
        excess = (df["loss"] - df["analytic"]).clip(lower=0.0)
    else:
        excess = 2.0 * min_dir.clip(lower=0.0)
    df["fictitious_mwh"] = excess * df["dur"]

    # Row symptom label, precedence: phantom > idle > inflation.
    df["symptom"] = ""
    df.loc[inflation, "symptom"] = SYMPTOM_INFLATION
    df.loc[idle, "symptom"] = SYMPTOM_IDLE
    df.loc[phantom, "symptom"] = SYMPTOM_PHANTOM

    flagged = df[flagged_mask].copy()

    # Worst bus-dual pair-sum across the flagged blocks of each line.
    bus_map = _bus_uid_map(planning)
    bus_a_uid: dict[int, int | None] = {}
    bus_b_uid: dict[int, int | None] = {}
    for line in planning.get("system", {}).get("line_array", []):
        uid = int(line.get("uid", 0))
        bus_a_uid[uid] = _resolve_bus_uid(line.get("bus_a"), bus_map)
        bus_b_uid[uid] = _resolve_bus_uid(line.get("bus_b"), bus_map)
    duals = _load_long(results_dir, "Bus/balance_dual")
    if duals is None:
        report.notes.append(
            "Bus/balance_dual absent — pair-sums (and loss_cost_eps "
            "override sizing) unavailable"
        )
    flagged["pair_sum"] = _attach_pair_sums(flagged, duals, bus_a_uid, bus_b_uid)

    for uid, grp in flagged.groupby("uid", sort=True):
        uid_i = int(uid)
        arbitrage_mwh = float(grp["fictitious_mwh"].sum())
        # Worst symptom: the channel that books the most fictitious
        # energy; count-based fallback when the excess ties at zero.
        by_symptom = grp.groupby("symptom")["fictitious_mwh"].agg(["sum", "size"])
        worst_symptom = str(
            by_symptom.sort_values(["sum", "size"], ascending=False).index[0]
        )
        pair_min = grp["pair_sum"].min()
        worst_pair: float | None = None if pd.isna(pair_min) else float(pair_min)
        report.diagnoses.append(
            LineDiagnosis(
                uid=uid_i,
                name=name_of.get(uid_i, f"uid:{uid_i}"),
                worst_symptom=worst_symptom,
                arbitrage_mwh=arbitrage_mwh,
                worst_pair_sum=worst_pair,
                n_flagged=int(len(grp)),
            )
        )

    report.diagnoses.sort(key=lambda d: -d.arbitrage_mwh)
    report.total_fictitious_mwh = float(flagged["fictitious_mwh"].sum())
    report.notes.append(MIGRATION_NOTE)
    return report


def format_table_lines(report: LossArbitrageReport) -> list[str]:
    """Human table: one row per flagged line plus a footer with the
    total fictitious-loss energy."""
    if report.ok:
        return ["no loss-arbitrage symptoms detected"]
    header = (
        f"{'uid':>5}  {'name':<24} {'worst symptom':<20} "
        f"{'arbitrage MWh':>14}  {'worst pi_a+pi_b':>16}"
    )
    lines = [header, "-" * len(header)]
    for d in report.diagnoses:
        pair = (
            f"{d.worst_pair_sum:16.2f}"
            if d.worst_pair_sum is not None
            else (f"{'n/a':>16}")
        )
        lines.append(
            f"{d.uid:>5}  {d.name:<24.24} {d.worst_symptom:<20} "
            f"{d.arbitrage_mwh:>14.2f}  {pair}"
        )
    lines.append(f"total fictitious-loss energy: {report.total_fictitious_mwh:.2f} MWh")
    return lines


def build_overrides(
    report: LossArbitrageReport, *, safety: float = 1.3
) -> dict[str, list[dict[str, float]]]:
    """Build the ``{"line_array": [{"uid": N, "loss_cost_eps": E}]}``
    JSON snippet, ready to merge into the case.

    ``E = ceil(|worst pair-sum| / 2 × safety)`` — the loss sink profits
    at a rate of ``−(π_a + π_b)/2`` per fictitious MWh, so any ε above
    that (plus margin) makes the arbitrage unprofitable on that line.
    Lines whose worst pair-sum is non-negative (or unavailable) are
    skipped: there is no negative-LMP driver to price against.
    """
    entries: list[dict[str, float]] = []
    for d in report.diagnoses:
        if d.worst_pair_sum is None or d.worst_pair_sum >= 0.0:
            continue
        eps = math.ceil(abs(d.worst_pair_sum) / 2.0 * safety)
        entries.append({"uid": d.uid, "loss_cost_eps": eps})
    return {"line_array": entries}
