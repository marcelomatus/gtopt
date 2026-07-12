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
    add_emissions_arguments,
    add_lift_line_caps_argument,
    add_line_losses_mode_argument,
    add_loss_cost_eps_argument,
    add_loss_secant_segments_argument,
    add_scale_objective_argument,
    add_use_kirchhoff_argument,
    add_use_single_bus_argument,
)
from gtopt_shared.bus_kv import (
    SEN_KV_LEVELS,
    parse_bus_kv,
)
from gtopt_shared.emissions import (
    DEFAULT_EMISSIONS_FILE,
    EmissionDefaults,
    EmissionFactor,
    EmissionReport,
    apply_emission_defaults,
    apply_emission_defaults_from_file,
    load_emission_defaults,
)
from gtopt_shared.json_utils import (
    DEFAULT_INF_OMIT_KEYS,
    sanitize_inf,
    strip_internal_keys,
)
from gtopt_shared.pampl_ident import (
    PENALTY_TIER_NAMES,
    pampl_ident,
    penalty_param_name,
)
from gtopt_shared.pampl_rhs import pampl_rhs_vector

__all__ = [
    "DEFAULT_EMISSIONS_FILE",
    "DEFAULT_INF_OMIT_KEYS",
    "LINE_LOSSES_MODE_CHOICES",
    "EmissionDefaults",
    "EmissionFactor",
    "EmissionReport",
    "PENALTY_TIER_NAMES",
    "SEN_KV_LEVELS",
    "add_aperture_chunk_size_argument",
    "add_common_arguments",
    "add_demand_fail_cost_argument",
    "add_emissions_arguments",
    "add_lift_line_caps_argument",
    "add_line_losses_mode_argument",
    "add_loss_cost_eps_argument",
    "add_loss_secant_segments_argument",
    "add_scale_objective_argument",
    "add_use_kirchhoff_argument",
    "add_use_single_bus_argument",
    "apply_emission_defaults",
    "apply_emission_defaults_from_file",
    "load_emission_defaults",
    "pampl_ident",
    "pampl_rhs_vector",
    "parse_bus_kv",
    "penalty_param_name",
    "sanitize_inf",
    "strip_internal_keys",
]
