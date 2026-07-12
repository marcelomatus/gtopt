# SPDX-License-Identifier: BSD-3-Clause
"""Optional model-side simplifications applied to the reduced case.

These do not change the bus/line topology — they only adjust gtopt's
*formulation* (Kirchhoff on/off, loss segments, demand uplift) so the
reduced LP is even cheaper to solve.  Drive from
:class:`ReduceConfig.transport_only`, :class:`ReduceConfig.loss_mode`,
and :class:`ReduceConfig.loss_uplift_pct`.
"""

from __future__ import annotations

import logging

from gtopt_reduce_network._io import Case

logger = logging.getLogger(__name__)

_LOSS_MODES = ("keep", "linear", "off", "uplift", "gen-lossfactor")
_COLLISION_MODES = ("replace", "add", "compound")


def _model_options(case: Case) -> dict:
    """Return the dict that holds model-formulation keys.

    gtopt's strict JSON parser accepts ``use_kirchhoff`` /
    ``use_line_losses`` / ``loss_segments`` inside the nested
    ``options.model_options`` block; cases written with the nested layout
    (e.g. plexos2gtopt) reject those keys at the top level.  Write into
    the nested block when it exists, else keep the legacy flat layout.
    """
    block = case.options.get("model_options")
    if isinstance(block, dict):
        return block
    return case.options


def apply_transport_only(case: Case) -> None:
    """Disable Kirchhoff so the reduced LP becomes a pure transport network.

    KCL (per-bus power balance) still binds; KVL (proportional flow via
    bus-angle theta) is dropped.  Capacity limits ``tmax_ab/tmax_ba``
    still apply.

    We do *not* delete reactance/resistance values — gtopt ignores them
    when ``use_kirchhoff=false`` and keeping them lets the case fall back
    cleanly if the flag is later toggled off.
    """
    _model_options(case)["use_kirchhoff"] = False
    logger.info("transport-only: set model use_kirchhoff = false")


def apply_loss_mode(
    case: Case,
    mode: str,
    uplift_pct: float = 3.0,
    *,
    collision: str = "replace",
) -> None:
    """Apply a loss-model simplification to the reduced case.

    Modes:

    * ``keep``   — leave the loss formulation untouched.
    * ``linear`` — set ``loss_segments = 1`` so each line has a single
      PWL segment (a linear loss curve).  Note this keeps TWO directional
      flow variables per line (f⁺/f⁻) and the phantom-circulation
      arbitrage channel under negative prices.
    * ``off``    — set ``use_line_losses = false``; no loss variables /
      rows are emitted and each line keeps ONE signed flow variable.
    * ``uplift`` — disable line losses and write a per-demand
      ``lossfactor = uplift_pct / 100`` so the bus-balance LP coefficient
      becomes ``-(1 + lossfactor)`` on every served load (see
      ``source/demand_lp.cpp:188``).  Schedule files referenced by
      ``lmax`` are NEVER touched — the uplift is purely a per-demand
      scalar field.
    * ``gen-lossfactor`` — disable line losses and write a per-generator
      ``lossfactor = uplift_pct / 100``: the bus-balance coefficient on
      generation becomes ``(1 - lossfactor)`` (``generator_lp.cpp``), the
      classic injection penalty-factor of uninodal economic dispatch
      (PLP factores de penalización / PLEXOS marginal loss factors).
      Keeps demand truthful (no inflated VoLL / reserve exposure) and
      admits per-generator differentiation later.

    ``collision`` controls how to combine the uplift with a demand's
    pre-existing ``lossfactor`` (only meaningful for ``mode="uplift"``):

    * ``replace`` (default): overwrite the existing value;
    * ``add``: ``lf_new = lf_old + uplift``;
    * ``compound``: ``lf_new = (1 + lf_old) * (1 + uplift) - 1``.

    Non-scalar pre-existing ``lossfactor`` values (arrays / file refs)
    are skipped with a warning regardless of collision mode.
    """
    if mode not in _LOSS_MODES:
        raise ValueError(f"unknown loss_mode={mode!r}; supported: {_LOSS_MODES}")
    if collision not in _COLLISION_MODES:
        raise ValueError(
            f"unknown collision={collision!r}; supported: {_COLLISION_MODES}"
        )
    if mode == "keep":
        return
    model = _model_options(case)
    if mode == "linear":
        model["loss_segments"] = 1
        logger.info("loss-mode=linear: set model loss_segments = 1")
        return
    model["use_line_losses"] = False
    model["loss_segments"] = 0
    if mode == "off":
        logger.info("loss-mode=off: set model use_line_losses=false, loss_segments=0")
        return
    # mode == "uplift" (demand side) or "gen-lossfactor" (generation side)
    array = "demand_array" if mode == "uplift" else "generator_array"
    lf_new = float(uplift_pct) / 100.0
    n_set, n_skipped = _write_lossfactor(case, array, lf_new, collision=collision)
    logger.info(
        "loss-mode=%s: lossfactor=%.4f on %d %s "
        "(collision=%s; %d skipped — non-scalar pre-existing lossfactor)",
        mode,
        lf_new,
        n_set,
        "demands" if mode == "uplift" else "generators",
        collision,
        n_skipped,
    )


def _write_lossfactor(
    case: Case, array: str, lf_new: float, *, collision: str
) -> tuple[int, int]:
    """Write the uplift value into every element's ``lossfactor`` field.

    Returns ``(n_set, n_skipped)`` — counts of elements that received the
    new value vs. those skipped because their existing ``lossfactor`` is
    non-scalar (list or filename).
    """
    n_set = 0
    n_skipped = 0
    for d in case.array(array):
        lf_old = d.get("lossfactor")
        if lf_old is None or collision == "replace":
            d["lossfactor"] = lf_new
            n_set += 1
            continue
        if isinstance(lf_old, bool) or not isinstance(lf_old, (int, float)):
            # array or filename — skip with warning
            n_skipped += 1
            logger.warning(
                "element uid=%s has non-scalar lossfactor %r; "
                "skipped uplift collision=%s",
                d.get("uid"),
                lf_old,
                collision,
            )
            continue
        old_f = float(lf_old)
        if collision == "add":
            d["lossfactor"] = old_f + lf_new
        elif collision == "compound":
            d["lossfactor"] = (1.0 + old_f) * (1.0 + lf_new) - 1.0
        else:  # pragma: no cover - guarded by apply_loss_mode
            raise ValueError(f"unknown collision={collision!r}")
        n_set += 1
    return n_set, n_skipped
