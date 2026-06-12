# SPDX-License-Identifier: BSD-3-Clause
"""Backward-compat re-export shim for :mod:`gtopt_shared.options_meta`.

Lifted to ``gtopt_shared`` on 2026-06-06 as part of issue #507 Phase 1
(making it the canonical source consumed by every planning-writer
converter).  Existing imports of ``igtopt._options_meta.*`` continue to
work unchanged via this shim — new code should import directly from
``gtopt_shared.options_meta`` instead.
"""

from __future__ import annotations

from gtopt_shared.options_meta import (
    CASCADE_OPTION_KEYS,
    MODEL_OPTION_KEYS,
    MONOLITHIC_OPTION_KEYS,
    SDDP_OPTION_KEYS,
    SIMULATION_OPTION_KEYS,
    SOLVER_OPTION_KEYS,
    _OPTIONS_FIELDS,
)


__all__ = [
    "CASCADE_OPTION_KEYS",
    "MODEL_OPTION_KEYS",
    "MONOLITHIC_OPTION_KEYS",
    "SDDP_OPTION_KEYS",
    "SIMULATION_OPTION_KEYS",
    "SOLVER_OPTION_KEYS",
    "_OPTIONS_FIELDS",
]
