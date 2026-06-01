# SPDX-License-Identifier: BSD-3-Clause
"""PAMPL identifier sanitisation + canonical penalty tier names.

PAMPL identifiers are ``[A-Za-z_][A-Za-z0-9_]*``. PLEXOS / PLP names can
start with a digit or contain ``- . ( ) space + > /`` and similar. This
module is the single source of truth for the sanitiser — both
``plexos2gtopt`` and ``plp2gtopt`` must agree byte-for-byte so that the
``uc_audit`` post-conversion compare keys match.
"""

from __future__ import annotations

import re

# Canonical PAMPL ``param`` names for the converter's penalty tiers.
# Good-AMPL named constants instead of inline magic numbers. Any other
# distinct PLEXOS/PLP penalty value gets a value-derived name
# (``penalty_467_19`` …) via ``penalty_param_name``.
PENALTY_TIER_NAMES: dict[float, str] = {10.0: "soft_floor_penalty"}


def pampl_ident(name: str) -> str:
    """Sanitise an arbitrary constraint name into a PAMPL ``IDENT``.

    Replace every non ``[A-Za-z0-9_]`` character with ``_``. Prefix
    ``uc_`` when the result would not start with a letter/underscore.
    The original name should be preserved in the constraint's
    description for traceability.
    """
    safe = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not safe or not (safe[0].isalpha() or safe[0] == "_"):
        safe = "uc_" + safe
    return safe


def penalty_param_name(value: float) -> str:
    """PAMPL ``param`` name for a per-unit penalty value.

    Named tiers (``PENALTY_TIER_NAMES``) are returned by name; every
    other value is encoded as ``penalty_<digits>`` with ``.`` / ``-``
    folded to ``_``.
    """
    if value in PENALTY_TIER_NAMES:
        return PENALTY_TIER_NAMES[value]
    return "penalty_" + re.sub(r"[^0-9]", "_", f"{value:g}")
