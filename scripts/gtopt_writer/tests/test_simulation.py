# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_writer.simulation` (issue #507 Phase 3)."""

from __future__ import annotations

import dataclasses

import pytest

from gtopt_writer.simulation import (
    BlockSpec,
    PhaseSpec,
    ScenarioSpec,
    SimulationSpec,
    StageSpec,
    build_simulation,
    heterogeneous_blocks,
    multi_stage_uniform,
    single_stage_uniform,
)


def test_build_simulation_minimal_skeleton() -> None:
    """Minimal: 1 block × 1 stage × 1 scenario, no chronological tag."""
    spec = SimulationSpec(
        blocks=(BlockSpec(uid=1, duration=1.0),),
        stages=(StageSpec(uid=1, first_block=0, count_block=1, active=1),),
        scenarios=(ScenarioSpec(uid=1, probability_factor=1.0),),
    )
    out = build_simulation(spec)
    assert out == {
        "block_array": [{"uid": 1, "duration": 1.0}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1.0}],
    }
    # phase_array must NOT be present when no phases were supplied.
    assert "phase_array" not in out


def test_build_simulation_emits_chronological_when_set() -> None:
    """``chronological`` field is emitted only when explicitly set."""
    spec = SimulationSpec(
        blocks=(BlockSpec(uid=1, duration=1.0),),
        stages=(
            StageSpec(
                uid=1,
                first_block=0,
                count_block=1,
                active=1,
                chronological=True,
            ),
        ),
        scenarios=(ScenarioSpec(uid=1, probability_factor=1.0),),
    )
    out = build_simulation(spec)
    assert out["stage_array"][0]["chronological"] is True


def test_build_simulation_emits_phases_when_set() -> None:
    """``phase_array`` is emitted only when phases are populated."""
    spec = SimulationSpec(
        blocks=(BlockSpec(uid=1, duration=1.0),),
        stages=(StageSpec(uid=1, first_block=0, count_block=1, active=1),),
        scenarios=(ScenarioSpec(uid=1, probability_factor=1.0),),
        phases=(PhaseSpec(uid=1, first_stage=0, count_stage=1),),
    )
    out = build_simulation(spec)
    assert out["phase_array"] == [{"uid": 1, "first_stage": 0, "count_stage": 1}]


def test_single_stage_uniform_matches_pp2gtopt_shape() -> None:
    """``single_stage_uniform`` reproduces pp2gtopt's inline simulation."""
    spec = single_stage_uniform(num_blocks=1, block_duration_h=1.0)
    out = build_simulation(spec)
    assert out["block_array"] == [{"uid": 1, "duration": 1.0}]
    assert out["stage_array"] == [
        {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ]
    assert out["scenario_array"] == [{"uid": 1, "probability_factor": 1.0}]


def test_multi_stage_uniform_matches_sddp2gtopt_shape() -> None:
    """``multi_stage_uniform`` reproduces sddp2gtopt's stage-blocks shape."""
    spec = multi_stage_uniform(
        num_stages=3,
        blocks_per_stage=2,
        stage_duration_h=730.0,  # one PSR monthly stage
    )
    out = build_simulation(spec)
    assert len(out["block_array"]) == 6
    assert all(b["duration"] == 365.0 for b in out["block_array"])
    assert [b["uid"] for b in out["block_array"]] == [1, 2, 3, 4, 5, 6]
    assert out["stage_array"] == [
        {"uid": 1, "first_block": 0, "count_block": 2, "active": 1},
        {"uid": 2, "first_block": 2, "count_block": 2, "active": 1},
        {"uid": 3, "first_block": 4, "count_block": 2, "active": 1},
    ]


def test_heterogeneous_blocks_matches_plexos_native_shape() -> None:
    """``heterogeneous_blocks`` reproduces plexos block_layout aggregation."""
    spec = heterogeneous_blocks(block_durations=(3.0, 5.0, 7.0))
    out = build_simulation(spec)
    assert out["block_array"] == [
        {"uid": 1, "duration": 3.0},
        {"uid": 2, "duration": 5.0},
        {"uid": 3, "duration": 7.0},
    ]
    # Chronological default in this constructor is True.
    assert out["stage_array"][0]["chronological"] is True


def test_multi_scenario_probability_normalisation() -> None:
    """Multi-scenario constructors split probability evenly."""
    spec = single_stage_uniform(num_blocks=1, block_duration_h=1.0, scenarios=4)
    out = build_simulation(spec)
    weights = [s["probability_factor"] for s in out["scenario_array"]]
    assert weights == [0.25, 0.25, 0.25, 0.25]
    uids = [s["uid"] for s in out["scenario_array"]]
    assert uids == [1, 2, 3, 4]


def test_build_simulation_with_empty_blocks_emits_empty_arrays() -> None:
    """Degenerate spec with empty tuples emits empty arrays cleanly."""
    spec = SimulationSpec(blocks=(), stages=(), scenarios=())
    out = build_simulation(spec)
    assert out == {
        "block_array": [],
        "stage_array": [],
        "scenario_array": [],
    }
    assert "phase_array" not in out


def test_single_stage_uniform_scenarios_zero_raises_value_error() -> None:
    """``scenarios=0`` is rejected with a clear ValueError, not ZeroDivisionError."""
    with pytest.raises(ValueError, match="scenarios must be > 0"):
        single_stage_uniform(num_blocks=1, block_duration_h=1.0, scenarios=0)


def test_multi_stage_uniform_blocks_per_stage_zero_raises_value_error() -> None:
    """``blocks_per_stage=0`` is rejected before the division kicks in."""
    with pytest.raises(ValueError, match="blocks_per_stage must be > 0"):
        multi_stage_uniform(num_stages=2, blocks_per_stage=0, stage_duration_h=24.0)


def test_multi_stage_uniform_zero_stages_produces_empty_arrays() -> None:
    """``num_stages=0`` yields empty stages and blocks (valid degenerate case)."""
    spec = multi_stage_uniform(num_stages=0, blocks_per_stage=2, stage_duration_h=24.0)
    out = build_simulation(spec)
    assert out["block_array"] == []
    assert out["stage_array"] == []
    assert len(out["scenario_array"]) == 1  # default scenarios=1


def test_heterogeneous_blocks_empty_durations_produces_empty_block_array() -> None:
    """Empty ``block_durations`` yields a stage with ``count_block=0``."""
    spec = heterogeneous_blocks(block_durations=())
    out = build_simulation(spec)
    assert out["block_array"] == []
    assert out["stage_array"][0]["count_block"] == 0


def test_heterogeneous_blocks_scenarios_zero_raises() -> None:
    """``scenarios=0`` rejected in heterogeneous_blocks too."""
    with pytest.raises(ValueError, match="scenarios must be > 0"):
        heterogeneous_blocks(block_durations=(1.0,), scenarios=0)


def test_single_stage_uniform_chronological_false_emits_key() -> None:
    """``chronological=False`` explicitly emits ``chronological: false`` in JSON."""
    spec = single_stage_uniform(num_blocks=1, block_duration_h=1.0, chronological=False)
    out = build_simulation(spec)
    assert out["stage_array"][0]["chronological"] is False


def test_specs_are_frozen() -> None:
    """All spec dataclasses are frozen so callers can't mutate after build."""
    block = BlockSpec(uid=1, duration=1.0)
    assert dataclasses.is_dataclass(block)
    # ``frozen=True`` raises ``FrozenInstanceError`` on attribute assignment.
    with pytest.raises(dataclasses.FrozenInstanceError):
        block.duration = 2.0  # type: ignore[misc]
