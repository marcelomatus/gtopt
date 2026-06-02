# SPDX-License-Identifier: BSD-3-Clause
"""Helpers shared between gtopt converters (plp2gtopt, plexos2gtopt, …).

Single source of truth for converter primitives that must agree
byte-for-byte across tools: identifier sanitisation, penalty tier
names, etc. Importing from a shared location prevents drift between
duplicated implementations.
"""

from gtopt_shared.cli_flags import (
    LINE_LOSSES_MODE_CHOICES,
    add_aperture_chunk_size_argument,
    add_common_arguments,
    add_demand_fail_cost_argument,
    add_lift_line_caps_argument,
    add_line_losses_mode_argument,
    add_loss_cost_eps_argument,
    add_scale_objective_argument,
    add_use_kirchhoff_argument,
    add_use_single_bus_argument,
)
from gtopt_shared.pampl_ident import (
    PENALTY_TIER_NAMES,
    pampl_ident,
    penalty_param_name,
)

__all__ = [
    "LINE_LOSSES_MODE_CHOICES",
    "PENALTY_TIER_NAMES",
    "add_aperture_chunk_size_argument",
    "add_common_arguments",
    "add_demand_fail_cost_argument",
    "add_lift_line_caps_argument",
    "add_line_losses_mode_argument",
    "add_loss_cost_eps_argument",
    "add_scale_objective_argument",
    "add_use_kirchhoff_argument",
    "add_use_single_bus_argument",
    "pampl_ident",
    "penalty_param_name",
]
