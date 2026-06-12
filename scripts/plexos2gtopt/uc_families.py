"""User-constraint family taxonomy — the single source of truth.

A PLEXOS ``Constraint`` object is routed to one gtopt ``.pampl`` *family*
based purely on its **original** name (before PAMPL-ident sanitisation).
This classifier is intentionally tiny and dependency-free so it can be
imported by both the heavy :mod:`plexos2gtopt.gtopt_writer` (which emits the
modular ``uc_<family>.pampl`` files) and by :mod:`gtopt_check_json` (which
re-derives the family of every emitted constraint to count it on the
gtopt-JSON side).  Keeping it here guarantees both sides classify
identically.

The families (ordered, first match wins; anything unmatched lands in
``operational``):

``config_exclusivity``
    Combined-cycle turbine mutexes (``*_Uniq`` / ``ConfTG``) — at most one
    configuration committed (HARD).
``gas_offtake``
    Daily gas/GNL fuel-operation caps (``Gas_MaxOp*``, day-scoped soft tier).
``commitment``
    Unit-commitment scheduling rows driving the PLEXOS dispatch.
``reserve``
    Ancillary-services / reserve products (CPF/CSF/CTF, RegRange, provisions).
``security``
    N-1 / contingency security rows (``SD_*`` and dated numeric names).
``comparison``
    PLEXOS↔gtopt validation rows (diagnostic, usually inactive).
``terminal_value``
    End-of-horizon future-cost (FCF / SDDP terminal value) rows.
``operational``
    Default: genuine generation/flow floors, ramps and caps.
"""

from __future__ import annotations

import re
from collections.abc import Callable

# Ordered — FIRST match wins — so the specific families precede the broad
# ones; anything unmatched lands in ``operational``.  Routed on the ORIGINAL
# PLEXOS constraint name (before PAMPL-ident sanitisation).
_UC_FAMILIES: tuple[tuple[str, Callable[[str], bool]], ...] = (
    # Combined-cycle turbine mutexes — at most one config committed (HARD).
    ("config_exclusivity", lambda n: n.endswith("_Uniq") or "ConfTG" in n),
    # Daily gas/GNL fuel-operation caps (day-scoped, soft fuel-cap tier).
    ("gas_offtake", lambda n: n.startswith("Gas_MaxOp")),
    # Unit-commitment scheduling rows that drive the PLEXOS dispatch (HARD).
    ("commitment", lambda n: n.endswith("_starting") or n == "NorthSecurity"),
    # Ancillary-services / reserve: CPF/CSF/CTF products, RegRange limits,
    # Special* north groupings, per-generator provision rows.
    (
        "reserve",
        lambda n: bool(
            re.search(
                r"(CTF|CSF|CPF|RegRange|Provision|MinUnits|MAXCSF|SSCC)|^Special", n
            )
        ),
    ),
    # N-1 / contingency security rows (``SD_*`` and the dated numeric form
    # ``2024…``); most ship ``inactive`` (excluded from the ST schedule).
    (
        "security",
        lambda n: n.startswith("SD_") or "Security" in n or bool(re.match(r"^\d", n)),
    ),
    # PLEXOS↔gtopt validation/comparison rows (diagnostic, usually inactive).
    ("comparison", lambda n: "Comparison" in n),
    # End-of-horizon future-cost (FCF / SDDP terminal value) rows.
    ("terminal_value", lambda n: n.startswith("FCF") or n.startswith("alpha")),
)

# Default family for the remaining operational floors / ramps / caps.
_UC_DEFAULT_FAMILY = "operational"

# All known UC family names (for --pampl-uc-only / --pampl-uc-off validation
# and for stable per-family table ordering in the comparison report).
UC_FAMILY_NAMES: frozenset[str] = frozenset(
    [name for name, _ in _UC_FAMILIES] + [_UC_DEFAULT_FAMILY]
)

# Stable display order: specific families first (declaration order), then the
# default.  Used by the comparison tables so PLEXOS and gtopt rows line up.
UC_FAMILY_ORDER: tuple[str, ...] = tuple(name for name, _ in _UC_FAMILIES) + (
    _UC_DEFAULT_FAMILY,
)


def uc_family(name: str) -> str:
    """Map a PLEXOS constraint name to its emitted ``.pampl`` family."""
    return next((f for f, pred in _UC_FAMILIES if pred(name)), _UC_DEFAULT_FAMILY)


__all__ = [
    "UC_FAMILY_NAMES",
    "UC_FAMILY_ORDER",
    "uc_family",
]
