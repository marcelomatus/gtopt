# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the relocated UC family taxonomy in plexos2gtopt.uc_families."""

from __future__ import annotations

from plexos2gtopt.uc_families import UC_FAMILY_NAMES, UC_FAMILY_ORDER, uc_family


# Verified (name, expected family) classifications — first match wins.
_CLASSIFICATIONS = (
    ("CC1_Uniq", "config_exclusivity"),
    ("ConfTG_x", "config_exclusivity"),
    ("Gas_MaxOp_A", "gas_offtake"),
    ("foo_starting", "commitment"),
    ("NorthSecurity", "commitment"),
    ("CPF_up", "reserve"),
    ("CSF_dn", "reserve"),
    ("RegRange1", "reserve"),
    ("SpecialNorth", "reserve"),
    ("SD_line1", "security"),
    ("2024084134_x", "security"),  # starts with a digit
    ("AnythingSecurity", "security"),
    ("FooComparison", "comparison"),
    ("FCF_term", "terminal_value"),
    ("alpha_fcf", "terminal_value"),
    ("ANTUCOmin", "operational"),
    ("RandomFloor", "operational"),
)


def test_uc_family_order_is_exact_tuple():
    assert UC_FAMILY_ORDER == (
        "config_exclusivity",
        "gas_offtake",
        "commitment",
        "reserve",
        "security",
        "comparison",
        "terminal_value",
        "operational",
    )


def test_uc_family_names_is_frozenset_of_order():
    assert isinstance(UC_FAMILY_NAMES, frozenset)
    assert UC_FAMILY_NAMES == frozenset(UC_FAMILY_ORDER)
    assert len(UC_FAMILY_NAMES) == 8


def test_uc_family_classifications():
    for name, expected in _CLASSIFICATIONS:
        assert uc_family(name) == expected, name


def test_every_classification_is_a_known_family():
    for name, _ in _CLASSIFICATIONS:
        assert uc_family(name) in UC_FAMILY_NAMES


def test_gtopt_writer_reexport_is_same_object():
    from plexos2gtopt.gtopt_writer import UC_FAMILY_NAMES as wfn
    from plexos2gtopt.gtopt_writer import uc_family as wf

    assert wf is uc_family
    assert wfn is UC_FAMILY_NAMES
