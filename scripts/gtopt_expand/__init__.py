# -*- coding: utf-8 -*-
"""gtopt expansion transforms.

Stage-2 of the expansion pipeline: consume canonical agreement/config
descriptions (``laja.json``, ``maule.json``, ``lng.json``, etc.) and
produce gtopt entities (``FlowRight``, ``VolumeRight``,
``UserConstraint``, ``LngTerminal``) plus companion PAMPL files.

The agreement classes accept either an in-memory dict (matching the
schema documented in their respective module docstrings) or a JSON file
path via ``LajaAgreement.from_json`` / ``MauleAgreement.from_json``.
"""

from gtopt_expand.laja_agreement import LajaAgreement
from gtopt_expand.lng_expand import expand_lng, expand_lng_from_file
from gtopt_expand.maule_agreement import MauleAgreement
from gtopt_expand.pmin_flowright_expand import (
    PminFlowRightSpec,
    expand_pmin_flowright,
    expand_pmin_flowright_from_file,
    parse_pmin_flowright_file,
)
from gtopt_expand.pumped_storage_expand import (
    default_config as pumped_storage_default_config,
    expand_pumped_storage,
    expand_pumped_storage_from_file,
)
from gtopt_expand.ror_expand import (
    RorSpec,
    parse_ror_equivalence_file,
    parse_ror_selection,
)

__version__ = "0.1.0"

__all__ = [
    "LajaAgreement",
    "MauleAgreement",
    "PminFlowRightSpec",
    "RorSpec",
    "expand_lng",
    "expand_lng_from_file",
    "expand_pmin_flowright",
    "expand_pmin_flowright_from_file",
    "expand_pumped_storage",
    "expand_pumped_storage_from_file",
    "parse_pmin_flowright_file",
    "parse_ror_equivalence_file",
    "parse_ror_selection",
    "pumped_storage_default_config",
    "__version__",
]
