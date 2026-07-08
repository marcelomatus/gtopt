# -*- coding: utf-8 -*-

"""AR(1) inflow-model estimation from PLP hydrology ensembles.

Implements the ``--inflow-model ar1`` estimation step of
``docs/formulation/sddp-ar-inflows.md`` §4: per central, the lag-1
autocorrelation of the stage-mean residuals of the ``plpaflce.dat``
hydrology ensemble, emitted as the ``inflow_model`` object on the
generated ``Flow`` element.  The mean series (``mu_t``) is the existing
``discharge`` schedule and is left untouched — the model only adds the
memory coefficient ``phi`` (and the residual std-dev ``sigma`` as
tooling metadata).
"""

from typing import Any, Dict, Optional

import numpy as np

#: phi is clamped into the open stationarity interval (-1, 1).
PHI_CLAMP = 0.99

#: Minimum number of stages (lag pairs + 1) for a meaningful fit.
MIN_STAGES = 3


def _stage_mean_series(
    flows: np.ndarray,
    blocks: np.ndarray,
    block_parser: Optional[Any],
) -> np.ndarray:
    """Aggregate the block-level ensemble to per-stage means.

    ``flows`` has shape ``(num_blocks, num_hydrologies)``.  When a
    ``BlockParser`` is available and covers every block, rows are
    averaged per stage (PLP months); otherwise the block-level series is
    returned unchanged (the intra-stage repetition then biases ``phi``
    upward, but no stage map exists to do better).
    """
    if block_parser is None or blocks.size != flows.shape[0]:
        return flows
    stages = np.asarray(block_parser.get_stage_numbers(blocks.tolist()))
    if stages.size != flows.shape[0] or np.any(stages < 0):
        return flows
    uniq = np.unique(stages)  # sorted stage numbers = chronological
    if uniq.size < MIN_STAGES:
        return flows
    agg = np.empty((uniq.size, flows.shape[1]), dtype=float)
    for i, stage_num in enumerate(uniq):
        agg[i] = flows[stages == stage_num].mean(axis=0)
    return agg


def estimate_ar1_from_aflce(
    aflce_item: Dict[str, Any],
    block_parser: Optional[Any] = None,
) -> Optional[Dict[str, Any]]:
    """Fit ``inflow_model = {"type": "ar1", "phi", "sigma"}`` for one central.

    Estimation (design doc §4):

    - ``mu_t`` = cross-hydrology mean per stage;
    - residuals ``x[h, t] = q[h, t] − mu_t``;
    - ``phi`` = pooled lag-1 autocorrelation
      ``Σ x_t·x_{t−1} / Σ x_{t−1}²`` over all hydrologies, clamped to
      ``[-PHI_CLAMP, PHI_CLAMP]``;
    - ``sigma`` = std-dev of the AR(1) residuals ``x_t − phi·x_{t−1}``.

    Returns ``None`` for degenerate series (constant across hydrologies,
    fewer than :data:`MIN_STAGES` stages, or missing data) — the caller
    then emits the Flow without a model (today's behaviour).
    """
    flows = np.asarray(aflce_item.get("flow", []), dtype=float)
    if flows.ndim != 2 or flows.shape[0] < MIN_STAGES or flows.shape[1] < 1:
        return None
    if not np.all(np.isfinite(flows)):
        return None

    blocks = np.asarray(aflce_item.get("block", []))
    series = _stage_mean_series(flows, blocks, block_parser)
    if series.shape[0] < MIN_STAGES:
        return None

    mu = series.mean(axis=1, keepdims=True)
    resid = series - mu
    x_prev = resid[:-1].ravel()
    x_next = resid[1:].ravel()
    denom = float(np.dot(x_prev, x_prev))
    if denom <= 0.0:
        # No cross-hydrology spread — a single (or constant) hydrology
        # carries no memory signal.
        return None
    phi = float(np.dot(x_next, x_prev) / denom)
    phi = float(np.clip(phi, -PHI_CLAMP, PHI_CLAMP))
    sigma = float(np.std(x_next - phi * x_prev))
    return {"type": "ar1", "phi": round(phi, 6), "sigma": round(sigma, 6)}
