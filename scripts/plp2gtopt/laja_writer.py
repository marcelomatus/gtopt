# -*- coding: utf-8 -*-

"""Backward-compatibility shim for ``LajaWriter``.

The Laja agreement transform now lives in ``gtopt_irrigation`` (Stage 2 of
the irrigation pipeline — see ``project_irrigation_pipeline.md``).  This
module re-exports the new ``LajaAgreement`` class under the legacy
``LajaWriter`` name so that existing plp2gtopt callers and tests keep
working unchanged.
"""

from gtopt_irrigation.laja_agreement import (  # noqa: F401
    LajaAgreement as LajaWriter,
    _zones_to_bound_rule_segments,
)

__all__ = ["LajaWriter", "_zones_to_bound_rule_segments"]
