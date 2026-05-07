# SPDX-License-Identifier: BSD-3-Clause
"""Typed errors raised by gtopt_marginal_units."""

from __future__ import annotations


class MarginalUnitsError(Exception):
    """Base class for all gtopt_marginal_units errors."""


class InputValidationError(MarginalUnitsError):
    """Required input is missing or inconsistent for the chosen
    (--input-kind, --mode) combination. Maps to exit code 3."""


class ExpansionNotSupportedError(InputValidationError):
    """Planning JSON contains generators or lines with
    ``expansion=true``. v1 does not handle the
    ``capacityp_dual``/``capacityn_dual`` route per master §3.2 P0.1."""


class AttributionError(MarginalUnitsError):
    """Writer-side invariant violated — e.g. recipe-table
    recomputed_lmp does not match zone_lmp within tol_price.
    Maps to exit code 3."""
