# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``plexos2gtopt.parsers._fuel_subtype_from_name``.

The subtype helper is the parser-side bridge to the IPCC sub-grade
distinction (pipeline NG vs LNG) that PLEXOS Fuel objects don't
carry as a structured property in CEN-Chile schemas.  Verified on
PLEXOS20260412 (2026-04-12 PCP bundle): 221 Fuel objects × 76
properties, no "Fuel Type" / sub-category — name pattern is the
only signal.
"""

from __future__ import annotations

import pytest

from plexos2gtopt.parsers import _fuel_subtype_from_name


@pytest.mark.parametrize(
    ("name", "expected"),
    [
        # Gas → LNG (default) vs pipeline NG (_GN_ suffix marker)
        ("Gas_GNLQuintero_A", "lng"),
        ("Gas_EnelMejillones_B", "lng"),
        ("Gas_Kelar_INF", "lng"),
        ("Gas_Colbun_GN_A", "natural_gas"),
        ("Gas_NuevaRenca_GN_B", "natural_gas"),
        ("Gas_EnelMejillones_GN_C", "natural_gas"),
        # No subtype detected for other families (fall back to family-prefix
        # match in the IPCC defaults at lookup time).
        ("Carbon_Andina", ""),
        ("Diesel_AguasBlancas", ""),
        ("FuelOil_Andes", ""),
        ("Biomasa_Celco_B1", ""),
        ("Biogas_LomaLosColorados", ""),
        ("GLP_TenoGas", ""),
        ("Otros_Noracid", ""),
        # Empty / malformed
        ("", ""),
        ("just_a_name", ""),
    ],
)
def test_fuel_subtype_from_name(name: str, expected: str) -> None:
    assert _fuel_subtype_from_name(name) == expected


def test_fuel_subtype_case_sensitive_on_prefix() -> None:
    """The ``Gas_`` prefix match is case-sensitive (matches the
    CEN convention literally).  ``gas_*`` / ``GAS_*`` are NOT
    treated as Gas family — they'd be unusual in a real bundle.
    """
    assert _fuel_subtype_from_name("gas_GNLQuintero_A") == ""
    assert _fuel_subtype_from_name("GAS_GNLQuintero_A") == ""


def test_fuel_subtype_gn_substring_only_in_gas_family() -> None:
    """The ``_GN_`` infix is only consulted for ``Gas_*`` names — a
    different family that happens to contain ``_GN_`` would not be
    re-routed (defensive: the convention is Chile-specific to Gas).
    """
    assert _fuel_subtype_from_name("Carbon_GN_something") == ""
    assert _fuel_subtype_from_name("Diesel_GN_anything") == ""
