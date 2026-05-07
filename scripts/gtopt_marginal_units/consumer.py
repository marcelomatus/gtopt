# SPDX-License-Identifier: BSD-3-Clause
"""Re-export of the consumer API for stable import paths.

End users do::

    from gtopt_marginal_units.consumer import MarginalUnitDataset

The implementation lives in :mod:`gtopt_marginal_units._consumer`.
"""

from __future__ import annotations

from gtopt_marginal_units._consumer import MarginalUnitDataset

__all__ = ["MarginalUnitDataset"]
