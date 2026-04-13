# -*- coding: utf-8 -*-

"""Backward-compatibility shim for ``MauleWriter``.

The Maule agreement transform now lives in ``gtopt_expand`` (Stage 2 of
the irrigation pipeline — see ``project_irrigation_pipeline.md``).  This
module re-exports the new ``MauleAgreement`` class under the legacy
``MauleWriter`` name so that existing plp2gtopt callers and tests keep
working unchanged.
"""

from gtopt_expand.maule_agreement import MauleAgreement as MauleWriter

__all__ = ["MauleWriter"]
