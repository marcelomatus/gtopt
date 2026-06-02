# SPDX-License-Identifier: BSD-3-Clause
"""Helpers shared between gtopt converters (plp2gtopt, plexos2gtopt, …).

Single source of truth for converter primitives that must agree
byte-for-byte across tools: identifier sanitisation, penalty tier
names, etc. Importing from a shared location prevents drift between
duplicated implementations.
"""

from gtopt_shared.cli_flags import (
    add_emissions_arguments,
    add_use_single_bus_argument,
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
    "EmissionDefaults",
    "EmissionFactor",
    "EmissionReport",
    "PENALTY_TIER_NAMES",
    "add_emissions_arguments",
    "add_use_single_bus_argument",
    "apply_emission_defaults",
    "apply_emission_defaults_from_file",
    "load_emission_defaults",
    "pampl_ident",
    "pampl_rhs_vector",
    "penalty_param_name",
    "sanitize_inf",
    "strip_internal_keys",
]
