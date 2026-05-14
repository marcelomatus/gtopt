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

_LOSS_MODES = ("keep", "linear", "off", "uplift")
_COLLISION_MODES = ("replace", "add", "compound")


def apply_transport_only(case: Case) -> None:
    """Disable Kirchhoff so the reduced LP becomes a pure transport network.

    KCL (per-bus power balance) still binds; KVL (proportional flow via
    bus-angle theta) is dropped.  Capacity limits ``tmax_ab/tmax_ba``
    still apply.

    We do *not* delete reactance/resistance values — gtopt ignores them
    when ``use_kirchhoff=false`` and keeping them lets the case fall back
    cleanly if the flag is later toggled off.
    """
    case.options["use_kirchhoff"] = False
    logger.info("transport-only: set options.use_kirchhoff = false")


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
    * ``linear`` — set ``options.loss_segments = 1`` so each line has a
      single PWL segment (a linear loss curve).
    * ``off``    — set ``options.use_line_losses = false``; no loss
      variables / rows are emitted.
    * ``uplift`` — disable line losses and write a per-demand
      ``lossfactor = uplift_pct / 100`` so the bus-balance LP coefficient
      becomes ``-(1 + lossfactor)`` on every served load (see
      ``source/demand_lp.cpp:188``).  Schedule files referenced by
      ``lmax`` are NEVER touched — the uplift is purely a per-demand
      scalar field.

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
    if mode == "linear":
        case.options["loss_segments"] = 1
        logger.info("loss-mode=linear: set options.loss_segments = 1")
        return
    if mode == "off":
        case.options["use_line_losses"] = False
        case.options["loss_segments"] = 0
        logger.info("loss-mode=off: set options.use_line_losses=false, loss_segments=0")
        return
    # mode == "uplift"
    case.options["use_line_losses"] = False
    case.options["loss_segments"] = 0
    lf_new = float(uplift_pct) / 100.0
    n_set, n_skipped = _write_lossfactor(case, lf_new, collision=collision)
    logger.info(
        "loss-mode=uplift: lossfactor=%.4f on %d demands "
        "(collision=%s; %d skipped — non-scalar pre-existing lossfactor)",
        lf_new,
        n_set,
        collision,
        n_skipped,
    )


def _write_lossfactor(case: Case, lf_new: float, *, collision: str) -> tuple[int, int]:
    """Write the uplift value into every demand's ``lossfactor`` field.

    Returns ``(n_set, n_skipped)`` — counts of demands that received the
    new value vs. those skipped because their existing ``lossfactor`` is
    non-scalar (list or filename).
    """
    n_set = 0
    n_skipped = 0
    for d in case.array("demand_array"):
        lf_old = d.get("lossfactor")
        if lf_old is None or collision == "replace":
            d["lossfactor"] = lf_new
            n_set += 1
            continue
        if isinstance(lf_old, bool) or not isinstance(lf_old, (int, float)):
            # array or filename — skip with warning
            n_skipped += 1
            logger.warning(
                "demand uid=%s has non-scalar lossfactor %r; "
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
