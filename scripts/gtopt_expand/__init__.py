# -*- coding: utf-8 -*-
"""gtopt irrigation agreement transforms.

Stage-2 of the irrigation pipeline (see
``project_irrigation_pipeline.md``): consume canonical ``laja.json`` /
``maule.json`` agreement descriptions and produce gtopt entities
(``FlowRight``, ``VolumeRight``, ``UserConstraint``) plus the companion
``laja.pampl`` / ``maule.pampl`` files.

The agreement classes accept either an in-memory dict (matching the
schema documented in their respective module docstrings) or a JSON file
path via ``LajaAgreement.from_json`` / ``MauleAgreement.from_json``.
"""

from gtopt_irrigation.laja_agreement import LajaAgreement
from gtopt_irrigation.maule_agreement import MauleAgreement

__version__ = "0.1.0"

__all__ = ["LajaAgreement", "MauleAgreement", "__version__"]
