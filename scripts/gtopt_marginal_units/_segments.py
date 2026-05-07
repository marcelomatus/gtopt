# SPDX-License-Identifier: BSD-3-Clause
"""Piecewise-linear MC lookup.

A unit's effective marginal cost depends on which cost segment is
active under its current dispatch. v1 only handles the scalar
``declared_MC`` case (the canonical-feed schema does not yet expose
per-segment data; the master plan documents this as a v1.1
enhancement). For scalar-MC units, the active segment is trivially
the unit itself and the slope is ``declared_MC``.

This module exists as the seam where v1.1 will plug in the
``segments=[(pmax_i, slope_i), ...]`` list from
``Topology.generator.segments``.
"""

from __future__ import annotations

from typing import NamedTuple, Optional


class ActiveSegment(NamedTuple):
    slope: Optional[float]  # MC at this segment, $/MWh; None if unset
    seg_index: int  # 0-based segment index, -1 for scalar units
    ambiguous: bool  # True at segment break points


def active_segment(
    *,
    dispatch: float,
    pmin: float,
    pmax: float,
    declared_MC: Optional[float],
    segments: Optional[list] = None,
    eps: float = 1.0e-4,
) -> ActiveSegment:
    """Return the active cost segment for ``dispatch``.

    For v1, only the scalar (segments=None) path is exercised.
    A unit with declared piecewise segments would feed
    ``segments=[(cum_max_i, slope_i), ...]`` sorted by cum_max and we
    would binary-search.

    Returns:
        ActiveSegment(slope, index, ambiguous).
        - ``slope`` is the MC at the active segment (or
          ``declared_MC`` for scalar units; or None if no MC data).
        - ``index`` is -1 for scalar units.
        - ``ambiguous`` is True when ``dispatch`` is exactly at a
          segment break (within ``eps``); two slopes are valid.
    """
    if segments is None or not segments:
        return ActiveSegment(slope=declared_MC, seg_index=-1, ambiguous=False)

    # Piecewise path — bisect into the segments by cumulative pmax.
    # segments is a list of (cum_pmax_upper_bound, slope) tuples.
    cum_bounds = [s[0] for s in segments]
    slopes = [s[1] for s in segments]
    # Find the lowest segment whose upper bound is >= dispatch.
    chosen = -1
    for i, ub in enumerate(cum_bounds):
        if dispatch <= ub + eps:
            chosen = i
            break
    if chosen == -1:
        # Above the last segment's upper bound — clamp to last.
        chosen = len(segments) - 1

    # Ambiguity: dispatch within eps of a break point (the chosen
    # segment's upper bound, except for the last segment).
    ambiguous = False
    if chosen < len(segments) - 1:
        if abs(dispatch - cum_bounds[chosen]) <= eps:
            ambiguous = True
    return ActiveSegment(
        slope=float(slopes[chosen]), seg_index=chosen, ambiguous=ambiguous
    )
