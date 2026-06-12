# SPDX-License-Identifier: BSD-3-Clause
"""§4.9 merit-ladder construction.

For each zone in each cell, emit ±K rungs around the rank-0 anchor
(the marginal unit picked by §4.5/§4.7 R3). Each rung carries
``hypothetical_lmp`` — the λ_z that *would* obtain if the anchor
moved to its bound and the rung became marginal.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from gtopt_canonical_feed import Generator
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units.constants import (
    DEFAULT_MERIT_LADDER_DEPTH,
    GeneratorKind,
    PROFILE_KINDS,
)


@dataclass(slots=True)
class LadderRung:
    cell_key: tuple[object, ...]
    zone_id: int
    rank: int  # 0 = current marginal anchor
    gen_uid: Optional[int]
    gen_name: Optional[str]
    declared_MC: Optional[float]
    active_segment_MC: Optional[float]
    dispatch: Optional[float]
    pmin: float
    pmax: float
    headroom_up_mw: float
    headroom_down_mw: float
    available: bool
    hypothetical_lmp: Optional[float]
    is_actual_marginal: bool


def build_ladder(
    *,
    cell_key: tuple[object, ...],
    zone_id: int,
    generators_in_zone: list[Generator],
    dispatch_by_uid: dict[int, float],
    zone_result: ZoneR3Result,
    depth: int = DEFAULT_MERIT_LADDER_DEPTH,
    merit_eligible_by_uid: Optional[dict[int, bool]] = None,
) -> list[LadderRung]:
    """Build the merit ladder for one zone.

    Returns a list of LadderRung sorted by rank; the rank-0 row is
    always present (it is the anchor — possibly synthetic for
    demand_fail / renewable_curtailment / unattributed).
    """
    if merit_eligible_by_uid is None:
        merit_eligible_by_uid = {
            g.uid: g.kind not in {k.value for k in PROFILE_KINDS}
            for g in generators_in_zone
        }

    # Filter to ladder-eligible units (per §4.9 step 1).
    eligible = [
        g
        for g in generators_in_zone
        if g.kind != GeneratorKind.PROFILE.value
        and merit_eligible_by_uid.get(g.uid, True)
    ]
    eligible.sort(
        key=lambda g: (
            float(g.declared_MC) if g.declared_MC is not None else float("inf"),
            g.uid,
        )
    )

    rungs: list[LadderRung] = []

    # rank-0 anchor: always emitted, even if synthetic.
    if zone_result.formula_kind in {
        "demand_fail",
        "renewable_curtailment",
        "unattributed",
    }:
        rungs.append(
            LadderRung(
                cell_key=cell_key,
                zone_id=zone_id,
                rank=0,
                gen_uid=None,
                gen_name=None,
                declared_MC=None,
                active_segment_MC=None,
                dispatch=None,
                pmin=0.0,
                pmax=0.0,
                headroom_up_mw=0.0,
                headroom_down_mw=0.0,
                available=False,
                hypothetical_lmp=zone_result.lambda_z,
                is_actual_marginal=True,
            )
        )
        return rungs

    # The anchor is the cheapest of the marginal_gen_uids (the first
    # tied unit when there's a tie — see §4.5).
    marginal_uids = list(zone_result.marginal_gen_uids)
    if not marginal_uids:
        # No anchor at all (shouldn't happen for non-synthetic kinds).
        return rungs

    anchor_uid = sorted(marginal_uids)[0]
    anchor_idx = next((i for i, g in enumerate(eligible) if g.uid == anchor_uid), -1)
    if anchor_idx == -1:
        # Anchor not in ladder-eligible list (e.g. a forced-pmin unit
        # excluded by merit_eligible). Add it manually as rank-0.
        anchor = next((g for g in generators_in_zone if g.uid == anchor_uid), None)
        if anchor is None:
            return rungs
        rungs.append(
            _rung_for_gen(
                cell_key,
                zone_id,
                0,
                anchor,
                dispatch_by_uid,
                hypothetical_lmp=zone_result.lambda_z,
                is_actual=True,
            )
        )
        return rungs

    # rank-0 from the eligible list.
    rungs.append(
        _rung_for_gen(
            cell_key,
            zone_id,
            0,
            eligible[anchor_idx],
            dispatch_by_uid,
            hypothetical_lmp=zone_result.lambda_z,
            is_actual=True,
        )
    )

    # +1, +2, …, +depth (next more expensive units).
    for offset in range(1, depth + 1):
        idx = anchor_idx + offset
        if idx >= len(eligible):
            break
        rungs.append(
            _rung_for_gen(
                cell_key,
                zone_id,
                +offset,
                eligible[idx],
                dispatch_by_uid,
                hypothetical_lmp=eligible[idx].declared_MC,
                is_actual=False,
            )
        )

    # -1, -2, …, -depth (next cheaper units).
    for offset in range(1, depth + 1):
        idx = anchor_idx - offset
        if idx < 0:
            break
        rungs.append(
            _rung_for_gen(
                cell_key,
                zone_id,
                -offset,
                eligible[idx],
                dispatch_by_uid,
                hypothetical_lmp=eligible[idx].declared_MC,
                is_actual=False,
            )
        )

    rungs.sort(key=lambda r: r.rank)
    return rungs


def _rung_for_gen(
    cell_key: tuple,
    zone_id: int,
    rank: int,
    gen: Generator,
    dispatch_by_uid: dict[int, float],
    *,
    hypothetical_lmp: Optional[float],
    is_actual: bool,
) -> LadderRung:
    d = dispatch_by_uid.get(gen.uid)
    headroom_up = max(0.0, gen.pmax - d) if d is not None else 0.0
    headroom_down = max(0.0, d - gen.pmin) if d is not None else 0.0
    available = d is not None and d <= gen.pmax  # simple availability proxy
    return LadderRung(
        cell_key=cell_key,
        zone_id=zone_id,
        rank=rank,
        gen_uid=gen.uid,
        gen_name=gen.name,
        declared_MC=gen.declared_MC,
        active_segment_MC=gen.declared_MC,  # v1: scalar units only
        dispatch=d,
        pmin=gen.pmin,
        pmax=gen.pmax,
        headroom_up_mw=headroom_up,
        headroom_down_mw=headroom_down,
        available=available,
        hypothetical_lmp=hypothetical_lmp,
        is_actual_marginal=is_actual,
    )
