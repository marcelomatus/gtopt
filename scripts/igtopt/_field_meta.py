# SPDX-License-Identifier: BSD-3-Clause
"""Backward-compat re-export shim for :mod:`gtopt_shared.field_meta`.

Lifted to ``gtopt_shared`` on 2026-06-06 as part of issue #507 Phase 1.
Existing imports of ``igtopt._field_meta.FIELD_META`` continue to work
unchanged via this shim — new code should import directly from
``gtopt_shared.field_meta`` instead.
"""

from __future__ import annotations

from gtopt_shared.field_meta import FIELD_META


__all__ = ["FIELD_META"]
