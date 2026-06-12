# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""Shared helpers for the PAMPL ``rhs`` encoding.

A :class:`UserConstraint`'s ``rhs`` field accepts several JSON shapes —
scalar, per-stage list, per-stage-per-block matrix, file ref, etc. — but
only the single-row TB-matrix form ``[[v0, v1, …]]`` has a ``rhs
[v0, v1, …]`` PAMPL encoding.  This module centralises the recognition
rule so every converter that emits ``.pampl`` files (plexos2gtopt today,
and per issue #507 Phase 1 the future ``gtopt_expand`` / ``plp2gtopt``
PAMPL emitters) agrees byte-for-byte on which shapes get inlined vs.
written out.

Symmetric to :mod:`gtopt_shared.pampl_ident` (identifier sanitisation)
and :mod:`gtopt_shared.emissions` (per-fuel CO2 factors).
"""

from __future__ import annotations

from typing import Any


def pampl_rhs_vector(rhs: Any) -> list[float] | None:
    """Return the per-block RHS vector if ``rhs`` is a TB-matrix profile.

    ``UserConstraint.rhs`` accepts several shapes; only the single-row
    TB-matrix form ``[[v0, v1, ...]]`` (what plexos2gtopt's
    ``build_user_constraint_array`` emits for a per-block profile) has a
    PAMPL ``rhs [v0, v1, ...]`` encoding.

    Returns the inner block vector for that shape, or ``None`` for
    scalar / per-stage / string / multi-stage forms that must stay
    inline in the JSON.
    """
    if (
        isinstance(rhs, list)
        and len(rhs) == 1
        and isinstance(rhs[0], list)
        and rhs[0]
        and all(isinstance(v, (int, float)) for v in rhs[0])
    ):
        return [float(v) for v in rhs[0]]
    return None
