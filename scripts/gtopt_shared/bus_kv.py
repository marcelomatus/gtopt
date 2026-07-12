# SPDX-License-Identifier: BSD-3-Clause
"""Recover a bus's nominal voltage level [kV] from its name.

CEN-style bundles (PLEXOS PCP daily, PLP) encode the real bus voltage
in the bus NAME (``Salar110`` → 110 kV, ``Tamaya033`` → 33 kV) rather
than in a dedicated field.  This helper is the converters' single
source of truth for that convention, feeding the gtopt ``Bus.voltage``
schema field (``include/gtopt/bus.hpp`` — nominal voltage level [kV]).

Matching strategy
-----------------

Every digit group in the name is considered, TRAILING group first
(``CNavia220_Aux_D`` → 220), and the first group that matches a known
SEN transmission level wins.  Leading zeros are tolerated (``033`` →
33).  Names whose digit groups match no SEN level (pure bus numbers
like ``b1`` or RTS-96's ``101``) yield ``None`` — the caller must OMIT
the voltage rather than fabricate one.

Note: ``gtopt_reduce_network._topology._bus_kv`` implements a looser
variant (with a non-validated trailing 2–3-digit fallback) for reducer
heuristics; that copy is intentionally left untouched.  Converters
must use this strict version so emitted data never invents a kV.
"""

from __future__ import annotations

import re

#: Nominal SEN (Chilean national grid) transmission voltage levels [kV].
SEN_KV_LEVELS: frozenset[int] = frozenset({23, 33, 66, 100, 110, 154, 220, 345, 500})

_NUM = re.compile(r"\d+")


def parse_bus_kv(name: str | None) -> float | None:
    """Parse the nominal kV level encoded in a bus ``name``.

    Returns the matched SEN level as a float, or ``None`` when the
    name carries no recognisable voltage (callers must then omit the
    ``voltage`` field instead of guessing).

    >>> parse_bus_kv("Salar110")
    110.0
    >>> parse_bus_kv("Tamaya033")  # leading zeros
    33.0
    >>> parse_bus_kv("CNavia220_Aux_D")  # embedded level
    220.0
    >>> parse_bus_kv("b1") is None  # bus number, not a kV
    True
    >>> parse_bus_kv(None) is None
    True
    """
    if not name:
        return None
    for group in reversed(_NUM.findall(name)):  # trailing group first
        if int(group) in SEN_KV_LEVELS:
            return float(int(group))
    return None
