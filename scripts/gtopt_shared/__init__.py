# SPDX-License-Identifier: BSD-3-Clause
"""Helpers shared between gtopt converters (plp2gtopt, plexos2gtopt, …).

Single source of truth for converter primitives that must agree
byte-for-byte across tools: identifier sanitisation, penalty tier
names, etc. Importing from a shared location prevents drift between
duplicated implementations.
"""

from gtopt_shared.pampl_ident import (
    PENALTY_TIER_NAMES,
    pampl_ident,
    penalty_param_name,
)

__all__ = ["PENALTY_TIER_NAMES", "pampl_ident", "penalty_param_name"]
