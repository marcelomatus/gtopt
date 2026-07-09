#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for the AR(1) inflow-model estimation (--inflow-model ar1)."""

import numpy as np

from ..inflow_model import MIN_STAGES, PHI_CLAMP, estimate_ar1_from_aflce
from ..main import make_parser


class _FakeBlockParser:
    """Minimal stand-in exposing ``get_stage_numbers``."""

    def __init__(self, stage_of_block):
        self._map = dict(stage_of_block)

    def get_stage_numbers(self, block_nums):
        return [self._map.get(int(b), -1) for b in block_nums]


def _ar1_ensemble(phi, num_stages, num_hydros, sigma=1.0, seed=7):
    """Synthesize an AR(1) residual ensemble around a seasonal mean."""
    rng = np.random.default_rng(seed)
    mu = 10.0 + 3.0 * np.sin(np.arange(num_stages))
    flows = np.empty((num_stages, num_hydros))
    for h in range(num_hydros):
        x = 0.0
        for t in range(num_stages):
            x = phi * x + rng.normal(0.0, sigma)
            flows[t, h] = mu[t] + x
    return flows


def test_estimate_recovers_known_phi():
    """A synthetic AR(1) ensemble yields phi close to the truth."""
    true_phi = 0.6
    flows = _ar1_ensemble(true_phi, num_stages=120, num_hydros=40)
    item = {"name": "c1", "block": np.arange(1, 121), "flow": flows}

    model = estimate_ar1_from_aflce(item)
    assert model is not None
    assert model["type"] == "ar1"
    assert abs(model["phi"] - true_phi) < 0.08
    assert model["sigma"] > 0.0


def test_estimate_zero_memory_series():
    """White-noise residuals give phi near zero (not None)."""
    flows = _ar1_ensemble(0.0, num_stages=200, num_hydros=30, seed=11)
    item = {"name": "c1", "block": np.arange(1, 201), "flow": flows}

    model = estimate_ar1_from_aflce(item)
    assert model is not None
    assert abs(model["phi"]) < 0.08


def test_estimate_degenerate_series_return_none():
    """Constant / too-short / missing series yield no model."""
    const = {"name": "c", "block": np.arange(1, 13), "flow": np.full((12, 5), 8.0)}
    assert estimate_ar1_from_aflce(const) is None

    short = {
        "name": "c",
        "block": np.arange(1, MIN_STAGES),
        "flow": np.ones((MIN_STAGES - 1, 5)),
    }
    assert estimate_ar1_from_aflce(short) is None

    assert estimate_ar1_from_aflce({"name": "c"}) is None

    nonfinite = {
        "name": "c",
        "block": np.arange(1, 13),
        "flow": np.full((12, 5), np.nan),
    }
    assert estimate_ar1_from_aflce(nonfinite) is None


def test_estimate_phi_is_clamped():
    """A random-walk-like ensemble stays inside the stationary interval."""
    flows = _ar1_ensemble(1.0, num_stages=60, num_hydros=10, seed=3)
    item = {"name": "c1", "block": np.arange(1, 61), "flow": flows}

    model = estimate_ar1_from_aflce(item)
    assert model is not None
    assert -PHI_CLAMP <= model["phi"] <= PHI_CLAMP


def test_stage_aggregation_uses_block_parser():
    """Blocks repeating each stage's value collapse to the stage series."""
    true_phi = 0.5
    stage_flows = _ar1_ensemble(true_phi, num_stages=80, num_hydros=25, seed=5)
    # Repeat every stage over 4 blocks (PLP months × blocks).
    blocks_per_stage = 4
    flows = np.repeat(stage_flows, blocks_per_stage, axis=0)
    block_nums = np.arange(1, flows.shape[0] + 1)
    stage_of_block = {int(b): 1 + (int(b) - 1) // blocks_per_stage for b in block_nums}
    item = {"name": "c1", "block": block_nums, "flow": flows}

    with_map = estimate_ar1_from_aflce(item, _FakeBlockParser(stage_of_block))
    assert with_map is not None
    assert abs(with_map["phi"] - true_phi) < 0.1

    # Without the stage map the intra-stage repetition inflates phi.
    without_map = estimate_ar1_from_aflce(item)
    assert without_map is not None
    assert without_map["phi"] > with_map["phi"]


def test_cli_flag_parses():
    """--inflow-model is registered with the 'ar1' choice."""
    parser = make_parser()
    args = parser.parse_args(["--inflow-model", "ar1", "input_dir"])
    assert args.inflow_model == "ar1"

    args_off = parser.parse_args(["input_dir"])
    assert args_off.inflow_model is None
