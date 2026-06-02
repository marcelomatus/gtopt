# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""Shared JSON post-processing for gtopt converter output.

Two helpers used to land a planning dict on disk in a form gtopt's
C++ parser accepts:

* :func:`strip_internal_keys` — drop any top-level key starting with
  ``_`` (writer-side annotations gtopt's ``StrictParsePolicy`` would
  reject).
* :func:`sanitize_inf` — recursively replace ``math.inf`` /
  ``-math.inf`` with the daw-json-link sentinels ``"Infinity"`` /
  ``"-Infinity"`` (parseable on the C++ side under
  ``gtopt::NumberOptsWithInf``), with an opt-in set of keys where
  the cleanest representation is to omit the field entirely.

Both helpers were originally inlined in
``plp2gtopt/gtopt_writer.py``.  Issue #507 Phase 1 calls for moving
them to ``gtopt_shared`` so plexos2gtopt (and the other converters)
can adopt them without copy-paste.  plp2gtopt continues to re-export
the legacy ``_sanitize_inf`` / ``_strip_internal_keys`` names through
its own writer for back-compat.
"""

from __future__ import annotations

import math
from typing import Any


# ---------------------------------------------------------------------------
# Internal-key stripping
# ---------------------------------------------------------------------------


def strip_internal_keys(planning: dict[str, Any]) -> dict[str, Any]:
    """Return a shallow copy of ``planning`` with internal-only keys removed.

    The gtopt C++ parser uses ``StrictParsePolicy`` (daw::json
    ``UseExactMappingsByDefault=yes``), so any field not declared in the
    corresponding struct causes a parse error.  Python-side metadata
    (pipeline annotations, Excel hints, debug breadcrumbs) is kept on
    the writer instance but excluded from the emitted JSON via this
    helper.  Only TOP-LEVEL keys are filtered — nested ``_``-prefixed
    keys inside the system arrays are the caller's responsibility.
    """
    return {k: v for k, v in planning.items() if not k.startswith("_")}


# ---------------------------------------------------------------------------
# Infinity sanitization
# ---------------------------------------------------------------------------


#: Sentinel string the daw-json-link C++ parser recognises as ``+inf``
#: under ``gtopt::NumberOptsWithInf`` (``LiteralAsStringOpt::Maybe`` +
#: ``JsonNumberErrors::AllowNanInf``).  See
#: ``include/gtopt/json/json_parse_policy.hpp`` for the recipe.
_INF_JSON_SENTINEL = "Infinity"
_NEG_INF_JSON_SENTINEL = "-Infinity"

#: Default set of keys whose ``math.inf`` value should be OMITTED from
#: the JSON instead of being serialised as ``"Infinity"``.  For these
#: fields gtopt's C++ struct already defaults to an empty optional that
#: the LP flatten code reads as ``±DblMax`` → solver ±infinity, so
#: omitting the key is the cleanest representation.  Callers can extend
#: this set via the ``omit_keys`` parameter of :func:`sanitize_inf`.
DEFAULT_INF_OMIT_KEYS: frozenset[str] = frozenset(
    {
        "fmax",  # Waterway.fmax / FlowRight.fmax / VolumeRight.fmax
        "fmin",  # Waterway.fmin / FlowRight.fmin / VolumeRight.fmin
    }
)

#: PLP ships ``1e30`` as a "soft infinity" sentinel; the converter
#: treats anything ``≥ 1e20`` (in absolute value) inside an omit-key as
#: ``inf`` so the field is dropped instead of serialised as a literal
#: ``1e30``.  Other fields are passed through unchanged — gtopt's LP
#: flatten code clamps anything ``≥ DblMax`` to solver ±infinity.
_PLP_INF_THRESHOLD: float = 1e20


def sanitize_inf(
    obj: Any,
    *,
    omit_keys: frozenset[str] | None = None,
) -> Any:
    """Recursively make ``math.inf`` / ``-math.inf`` JSON-safe.

    Default rule: serialise as the quoted-string sentinels ``"Infinity"``
    / ``"-Infinity"`` so daw-json-link's ``AllowNanInf`` can parse them
    on the C++ side.

    Exception (preferred for select keys — listed in ``omit_keys``):
    omit the key entirely from its containing dict.  gtopt's struct
    defaults treat absent optional numeric fields as unbounded after
    the LP flatten clamp (e.g. ``Waterway.fmax`` defaults to an empty
    optional, which the flatten code reads as ``DblMax`` → ``+inf``).

    Walks dicts and lists in place; returns the (possibly mutated)
    object so callers can chain with ``json.dump``.

    Args:
        obj: The dict / list / scalar to sanitise.
        omit_keys: Optional set of dict keys whose inf values should be
            DROPPED rather than serialised.  Defaults to
            :data:`DEFAULT_INF_OMIT_KEYS` (``"fmax"``, ``"fmin"``).
    """
    keys = omit_keys if omit_keys is not None else DEFAULT_INF_OMIT_KEYS
    return _sanitize_inf_impl(obj, keys)


def _sanitize_inf_impl(obj: Any, omit_keys: frozenset[str]) -> Any:
    """Internal recursive worker for :func:`sanitize_inf`."""
    if isinstance(obj, dict):
        # Two-pass.  Pass 1 identifies keys whose value is ``math.inf``
        # AND the key is in the omit-set — those get dropped entirely.
        # Other inf values fall through to the per-value sanitisation
        # in pass 2 (which serialises them as the "Infinity" sentinel).
        drops: list[Any] = []
        for k, v in obj.items():
            if (
                k in omit_keys
                and isinstance(v, float)
                and (v == math.inf or v == -math.inf or abs(v) >= _PLP_INF_THRESHOLD)
            ):
                drops.append(k)
        for k in drops:
            del obj[k]
        for k, v in obj.items():
            obj[k] = _sanitize_inf_impl(v, omit_keys)
        return obj
    if isinstance(obj, list):
        for i, v in enumerate(obj):
            obj[i] = _sanitize_inf_impl(v, omit_keys)
        return obj
    if isinstance(obj, float):
        if obj == math.inf:
            return _INF_JSON_SENTINEL
        if obj == -math.inf:
            return _NEG_INF_JSON_SENTINEL
    return obj
