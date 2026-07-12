# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.bus_kv` (bus-name → nominal kV)."""

from __future__ import annotations

from gtopt_shared import SEN_KV_LEVELS, parse_bus_kv


def test_trailing_sen_level() -> None:
    assert parse_bus_kv("Salar110") == 110.0
    assert parse_bus_kv("Andes345") == 345.0
    assert parse_bus_kv("PAzucar220") == 220.0


def test_leading_zeros() -> None:
    assert parse_bus_kv("Tamaya033") == 33.0
    assert parse_bus_kv("Chuqui023") == 23.0


def test_embedded_level_with_suffix() -> None:
    assert parse_bus_kv("CNavia220_Aux_D") == 220.0
    assert parse_bus_kv("Ancoa500-S2") == 500.0


def test_trailing_group_preferred_over_leading() -> None:
    # Both groups are SEN levels — the TRAILING one is the bus's kV.
    assert parse_bus_kv("Charrua220_500") == 500.0


def test_no_kv_yields_none() -> None:
    assert parse_bus_kv("b1") is None  # bus number, not a voltage
    assert parse_bus_kv("NORTE") is None  # no digits at all
    assert parse_bus_kv("101") is None  # RTS-96 bus number, not SEN
    assert parse_bus_kv("") is None
    assert parse_bus_kv(None) is None


def test_sen_levels_are_the_documented_set() -> None:
    assert SEN_KV_LEVELS == frozenset({23, 33, 66, 100, 110, 154, 220, 345, 500})
