# SPDX-License-Identifier: BSD-3-Clause
"""Tests for _io.py: load/save, SingleId resolution, schedule classification."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from gtopt_reduce_network._io import (
    Case,
    aggregate_scalar,
    classify_schedule_field,
    iter_bus_referencing_elements,
    load_case,
    resolve_bus_ref,
    save_case,
)


def test_load_save_roundtrip(tmp_path: Path, ieee14_path: Path) -> None:
    case = load_case(ieee14_path)
    assert len(case.array("bus_array")) == 14
    assert len(case.array("line_array")) == 20
    out = tmp_path / "rt.json"
    save_case(case, out)
    again = load_case(out)
    assert again.array("bus_array") == case.array("bus_array")


def test_resolve_bus_ref_uid_and_name(ieee14_path: Path) -> None:
    case = load_case(ieee14_path)
    assert resolve_bus_ref(case, "b1") == 1
    assert resolve_bus_ref(case, 5) == 5
    assert resolve_bus_ref(case, None) is None
    with pytest.raises(KeyError):
        resolve_bus_ref(case, "no_such_bus")
    with pytest.raises(KeyError):
        resolve_bus_ref(case, 99999)
    with pytest.raises(TypeError):
        resolve_bus_ref(case, True)  # type: ignore[arg-type]


def test_iter_bus_referencing_elements(tiny_chain_case: Case) -> None:
    refs = iter_bus_referencing_elements(tiny_chain_case)
    # 1 generator + 1 demand = 2 ref entries.
    kinds = sorted({arr for arr, _, _ in refs})
    assert kinds == ["demand_array", "generator_array"]


def test_classify_schedule_field() -> None:
    assert classify_schedule_field(3.14) == "scalar"
    assert classify_schedule_field(7) == "scalar"
    assert classify_schedule_field([[1, 2], [3, 4]]) == "array"
    assert classify_schedule_field("Demand/lmax.parquet") == "file"
    assert classify_schedule_field(True) == "unknown"
    assert classify_schedule_field(None) == "unknown"


def test_aggregate_scalar_rules() -> None:
    assert aggregate_scalar([1.0, 2.0, 3.0], rule="sum") == pytest.approx(6.0)
    assert aggregate_scalar([1.0, 2.0, 3.0], rule="max") == pytest.approx(3.0)
    assert aggregate_scalar([1.0, 2.0, 3.0], rule="min") == pytest.approx(1.0)
    assert aggregate_scalar([2.0, 4.0], rule="mean") == pytest.approx(3.0)
    with pytest.raises(ValueError):
        aggregate_scalar([], rule="sum")
    with pytest.raises(ValueError):
        aggregate_scalar([1.0], rule="bogus")


def test_load_rejects_non_object(tmp_path: Path) -> None:
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
    with pytest.raises(ValueError, match="expected top-level JSON object"):
        load_case(bad)
