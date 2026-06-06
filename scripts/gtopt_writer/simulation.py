# SPDX-License-Identifier: BSD-3-Clause
"""Shared ``build_simulation`` for the gtopt-writer framework (issue #507 Phase 3).

Unifies the block / stage / scenario / phase array assembly across
the four planning-writer converters (``plp2gtopt``, ``plexos2gtopt``,
``sddp2gtopt``, ``pp2gtopt``) plus the Excel writer (``igtopt``).
Each converter still owns the ADAPTER step that produces a
:class:`SimulationSpec` from its native input (PLEXOS bundle, PLP
study, SDDP psrclasses, pandapower net, igtopt Excel sheets) — this
module owns the JSON SHAPE.

Output is the ``simulation`` sub-object of a gtopt planning JSON:

  {
    "block_array":    [{"uid": int, "duration": float}, …],
    "stage_array":    [{"uid": int, "first_block": int, "count_block": int,
                       "active": int, ["chronological": bool], …}, …],
    "scenario_array": [{"uid": int, "probability_factor": float}, …],
    ["phase_array":   [{"uid": int, "first_stage": int, "count_stage": int}, …],]
  }

The phase array is optional — only emitted when the spec carries
phases (plp2gtopt's cascade method needs it; sddp / pp / plexos
single-stage cases do not).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True, slots=True)
class BlockSpec:
    """One block in the gtopt simulation block_array."""

    uid: int
    duration: float


@dataclass(frozen=True, slots=True)
class StageSpec:
    """One stage covering ``count_block`` consecutive blocks."""

    uid: int
    first_block: int
    count_block: int
    active: int = 1
    chronological: bool | None = None


@dataclass(frozen=True, slots=True)
class ScenarioSpec:
    """One scenario with a probability weight."""

    uid: int
    probability_factor: float = 1.0


@dataclass(frozen=True, slots=True)
class PhaseSpec:
    """One phase covering ``count_stage`` consecutive stages (optional)."""

    uid: int
    first_stage: int
    count_stage: int


@dataclass(frozen=True, slots=True)
class SimulationSpec:
    """All four arrays the gtopt ``simulation`` sub-object carries.

    ``phases`` is optional — only consumers that emit phase rows
    (plp2gtopt's cascade method) populate it.
    """

    blocks: tuple[BlockSpec, ...]
    stages: tuple[StageSpec, ...]
    scenarios: tuple[ScenarioSpec, ...]
    phases: tuple[PhaseSpec, ...] = field(default_factory=tuple)


def build_simulation(spec: SimulationSpec) -> dict[str, Any]:
    """Assemble the ``simulation`` JSON sub-object from a :class:`SimulationSpec`.

    Drops the ``chronological`` field from stages whose spec leaves it
    ``None`` (matching the historical gtopt JSON shape — the field is
    only emitted when explicitly set).  Drops ``phase_array`` entirely
    when the spec carries no phases.
    """

    def _stage_entry(s: StageSpec) -> dict[str, Any]:
        entry: dict[str, Any] = {
            "uid": s.uid,
            "first_block": s.first_block,
            "count_block": s.count_block,
            "active": s.active,
        }
        if s.chronological is not None:
            entry["chronological"] = s.chronological
        return entry

    out: dict[str, Any] = {
        "block_array": [{"uid": b.uid, "duration": b.duration} for b in spec.blocks],
        "stage_array": [_stage_entry(s) for s in spec.stages],
        "scenario_array": [
            {"uid": s.uid, "probability_factor": s.probability_factor}
            for s in spec.scenarios
        ],
    }
    if spec.phases:
        out["phase_array"] = [
            {"uid": p.uid, "first_stage": p.first_stage, "count_stage": p.count_stage}
            for p in spec.phases
        ]
    return out


# ── Convenience constructors for the common shapes ────────────────────


def _build_uniform_scenarios(scenarios: int) -> tuple[ScenarioSpec, ...]:
    """Build ``scenarios`` ScenarioSpecs with even probability.

    Raised to ValueError on ``scenarios <= 0`` (preserves a clear
    error path; the alternative ``1.0 / 0`` ZeroDivisionError is
    opaque at the call site).
    """
    if scenarios <= 0:
        raise ValueError(
            f"scenarios must be > 0; got {scenarios}.  Single-scenario "
            "cases pass scenarios=1 (the default)."
        )
    return tuple(
        ScenarioSpec(uid=i + 1, probability_factor=1.0 / scenarios)
        for i in range(scenarios)
    )


def single_stage_uniform(
    *,
    num_blocks: int,
    block_duration_h: float,
    scenarios: int = 1,
    chronological: bool | None = None,
) -> SimulationSpec:
    """One scenario × one stage × ``num_blocks`` uniform blocks.

    Matches the pp2gtopt and plexos2gtopt-hourly defaults.  When
    ``chronological`` is set, the stage carries the
    ``chronological`` flag (gtopt's commitment LP requires it).

    Raises ``ValueError`` on ``scenarios <= 0``.  ``num_blocks=0``
    is accepted and produces a degenerate stage with ``count_block=0``.
    """
    blocks = tuple(
        BlockSpec(uid=i + 1, duration=block_duration_h) for i in range(num_blocks)
    )
    stage = StageSpec(
        uid=1,
        first_block=0,
        count_block=num_blocks,
        active=1,
        chronological=chronological,
    )
    return SimulationSpec(
        blocks=blocks,
        stages=(stage,),
        scenarios=_build_uniform_scenarios(scenarios),
    )


def multi_stage_uniform(
    *,
    num_stages: int,
    blocks_per_stage: int,
    stage_duration_h: float,
    scenarios: int = 1,
) -> SimulationSpec:
    """``num_stages`` × ``blocks_per_stage`` uniform blocks per stage.

    Matches sddp2gtopt's hourly / weekly / monthly horizons where
    each stage carries the same number of equal-duration blocks.
    Block duration is computed as ``stage_duration_h /
    blocks_per_stage``.

    Raises ``ValueError`` on ``blocks_per_stage <= 0`` (would force a
    ``ZeroDivisionError`` on the block-duration computation) or
    ``scenarios <= 0``.  ``num_stages=0`` is accepted and produces
    empty ``blocks`` and ``stages`` tuples.
    """
    if blocks_per_stage <= 0:
        raise ValueError(
            f"blocks_per_stage must be > 0; got {blocks_per_stage}.  "
            "Use single_stage_uniform(num_blocks=0) for the degenerate "
            "no-block case."
        )
    block_h = stage_duration_h / blocks_per_stage
    blocks: list[BlockSpec] = []
    stages: list[StageSpec] = []
    bid = 1
    for s in range(num_stages):
        first_block = bid - 1
        for _ in range(blocks_per_stage):
            blocks.append(BlockSpec(uid=bid, duration=block_h))
            bid += 1
        stages.append(
            StageSpec(
                uid=s + 1,
                first_block=first_block,
                count_block=blocks_per_stage,
                active=1,
            )
        )
    return SimulationSpec(
        blocks=tuple(blocks),
        stages=tuple(stages),
        scenarios=_build_uniform_scenarios(scenarios),
    )


def heterogeneous_blocks(
    *,
    block_durations: tuple[float, ...],
    chronological: bool = True,
    scenarios: int = 1,
) -> SimulationSpec:
    """Single stage with explicit per-block durations.

    Matches plexos-native mode where each block aggregates a
    heterogeneous number of hourly intervals (e.g. 111 blocks
    across 7 days for CEN PCP).

    Raises ``ValueError`` on ``scenarios <= 0``.  Empty
    ``block_durations`` is accepted and produces a degenerate stage
    with ``count_block=0``.
    """
    blocks = tuple(
        BlockSpec(uid=i + 1, duration=float(d)) for i, d in enumerate(block_durations)
    )
    stage = StageSpec(
        uid=1,
        first_block=0,
        count_block=len(block_durations),
        active=1,
        chronological=chronological,
    )
    return SimulationSpec(
        blocks=blocks,
        stages=(stage,),
        scenarios=_build_uniform_scenarios(scenarios),
    )
