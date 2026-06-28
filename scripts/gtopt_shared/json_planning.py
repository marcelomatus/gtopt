"""Shared helpers for emitting a gtopt planning JSON.

Both ``sddp2gtopt`` and ``psse2gtopt`` (and any future single-file
converter) write a single ``{options, simulation, system}`` planning
dict.  This module centralises the two cross-cutting concerns:

* :func:`build_json_item` — drop ``None`` fields when assembling an item;
* :func:`write_planning` — sanitise Inf/NaN sentinels, strip converter-
  internal bookkeeping keys, and serialise — so the C++ ``daw::json``
  StrictParsePolicy parser accepts the output.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any


logger = logging.getLogger(__name__)


def build_json_item(**fields: Any) -> dict[str, Any]:
    """Assemble a JSON item, dropping keys whose value is ``None``."""
    return {k: v for k, v in fields.items() if v is not None}


def write_planning(planning: dict[str, Any], output_path: str | Path) -> Path:
    """Write a gtopt planning dict to ``output_path`` (parents created).

    Sanitises Inf / -Inf / NaN sentinels via
    :func:`gtopt_shared.json_utils.sanitize_inf` and strips converter-
    internal bookkeeping keys via
    :func:`gtopt_shared.json_utils.strip_internal_keys` before
    serialising — the C++ ``daw::json`` parser rejects literal
    ``Infinity`` and treats unknown keys as schema errors under
    StrictParsePolicy.
    """
    # Imported lazily so importing this module doesn't transitively pull
    # pyarrow when callers only need ``build_json_item``.
    from gtopt_shared.json_utils import (  # pylint: disable=import-outside-toplevel
        sanitize_inf,
        strip_internal_keys,
    )

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cleaned = sanitize_inf(strip_internal_keys(planning))
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(cleaned, fh, indent=2)
        fh.write("\n")
    logger.info("wrote gtopt planning: %s", output_path)
    return output_path
