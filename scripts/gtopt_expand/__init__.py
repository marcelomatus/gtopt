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

__version__ = "0.1.0"

__all__ = [
    "LajaAgreement",
    "MauleAgreement",
    "expand_lng",
    "expand_lng_from_file",
    "__version__",
]
