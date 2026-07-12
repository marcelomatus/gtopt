# SPDX-License-Identifier: BSD-3-Clause
"""Tests for transport-only + loss-mode model simplifications."""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_reduce_network._io import Case, load_case
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case
from gtopt_reduce_network._simplify import (
    apply_loss_mode,
    apply_transport_only,
)


def _toy_case() -> Case:
    raw = {
        "options": {
            "use_kirchhoff": True,
            "use_line_losses": True,
            "loss_segments": 3,
        },
        "simulation": {},
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
            "line_array": [
                {
                    "uid": 10,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.1,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
            ],
            "generator_array": [
                {"uid": 1, "bus": 1, "pmax": 200, "capacity": 200},
            ],
            "demand_array": [
                {"uid": 1, "bus": 2, "lmax": 100.0},
                {"uid": 2, "bus": 2, "lmax": [[10.0, 20.0], [30.0, 40.0]]},
                {"uid": 3, "bus": 2, "lmax": "Demand/lmax.parquet"},
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    return case


def _toy_case_with_existing_lossfactor() -> Case:
    """Variant where every demand already carries a scalar lossfactor."""
    case = _toy_case()
    for d, lf in zip(case.array("demand_array"), [0.02, 0.04, 0.0]):
        d["lossfactor"] = lf
    return case


def test_apply_transport_only_sets_use_kirchhoff_false() -> None:
    case = _toy_case()
    apply_transport_only(case)
    assert case.options["use_kirchhoff"] is False
    # Reactance values are preserved (gtopt just ignores them).
    assert case.array("line_array")[0]["reactance"] == 0.1


def test_loss_mode_keep_is_noop() -> None:
    case = _toy_case()
    before = dict(case.options)
    apply_loss_mode(case, "keep")
    assert dict(case.options) == before


def test_loss_mode_linear_sets_one_segment() -> None:
    case = _toy_case()
    apply_loss_mode(case, "linear")
    assert case.options["loss_segments"] == 1
    # use_line_losses must NOT be touched in linear mode.
    assert case.options.get("use_line_losses") is True


def test_loss_mode_off_disables_losses() -> None:
    case = _toy_case()
    apply_loss_mode(case, "off")
    assert case.options["use_line_losses"] is False
    assert case.options["loss_segments"] == 0


# ─── uplift mode now writes per-demand lossfactor (NOT scaling lmax) ─────────


def test_loss_mode_uplift_writes_lossfactor_not_lmax() -> None:
    """uplift must NEVER scale lmax — only set per-demand lossfactor."""
    case = _toy_case()
    # Snapshot lmax values pre-uplift.
    orig_lmax = [d["lmax"] for d in case.array("demand_array")]

    apply_loss_mode(case, "uplift", uplift_pct=5.0)

    # use_line_losses must be off + loss_segments=0.
    assert case.options["use_line_losses"] is False
    assert case.options["loss_segments"] == 0
    # lmax untouched on every demand.
    for d, before in zip(case.array("demand_array"), orig_lmax):
        assert d["lmax"] == before, (
            "lmax must NEVER be modified by uplift mode — touching the "
            "schedule (e.g. a Parquet filename) is the bug we are avoiding"
        )
    # Per-demand lossfactor = uplift_pct / 100 on every demand.
    for d in case.array("demand_array"):
        assert d["lossfactor"] == pytest.approx(0.05)


def test_loss_mode_uplift_default_3pct() -> None:
    case = _toy_case()
    apply_loss_mode(case, "uplift")
    for d in case.array("demand_array"):
        assert d["lossfactor"] == pytest.approx(0.03)


def test_loss_mode_uplift_collision_replace() -> None:
    case = _toy_case_with_existing_lossfactor()
    apply_loss_mode(case, "uplift", uplift_pct=5.0, collision="replace")
    # All overwritten with the new value, regardless of old.
    for d in case.array("demand_array"):
        assert d["lossfactor"] == pytest.approx(0.05)


def test_loss_mode_uplift_collision_add() -> None:
    case = _toy_case_with_existing_lossfactor()
    apply_loss_mode(case, "uplift", uplift_pct=5.0, collision="add")
    demands = {d["uid"]: d for d in case.array("demand_array")}
    # uid=1 had 0.02 → 0.02 + 0.05 = 0.07.
    assert demands[1]["lossfactor"] == pytest.approx(0.07)
    # uid=2 had 0.04 → 0.09.
    assert demands[2]["lossfactor"] == pytest.approx(0.09)
    # uid=3 had 0.0 → 0.05.
    assert demands[3]["lossfactor"] == pytest.approx(0.05)


def test_loss_mode_uplift_collision_compound() -> None:
    case = _toy_case_with_existing_lossfactor()
    apply_loss_mode(case, "uplift", uplift_pct=5.0, collision="compound")
    demands = {d["uid"]: d for d in case.array("demand_array")}
    # (1 + 0.02)(1 + 0.05) - 1 = 0.071.
    assert demands[1]["lossfactor"] == pytest.approx(0.071)
    # (1 + 0.04)(1 + 0.05) - 1 = 0.092.
    assert demands[2]["lossfactor"] == pytest.approx(0.092)
    # (1 + 0.0)(1 + 0.05) - 1 = 0.05.
    assert demands[3]["lossfactor"] == pytest.approx(0.05)


def test_loss_mode_uplift_unknown_collision_raises() -> None:
    case = _toy_case()
    with pytest.raises(ValueError, match="unknown collision"):
        apply_loss_mode(case, "uplift", collision="bogus")


def test_loss_mode_uplift_skips_non_scalar_lossfactor(caplog) -> None:
    """Pre-existing non-scalar lossfactor must be skipped (not crash)."""
    case = _toy_case()
    case.array("demand_array")[0]["lossfactor"] = [0.01, 0.02, 0.03]  # array
    case.array("demand_array")[1]["lossfactor"] = "Demand/lf.parquet"  # file
    apply_loss_mode(case, "uplift", uplift_pct=5.0, collision="add")
    # Replace mode would overwrite even these — but collision='add' must
    # leave non-scalar entries alone (and warn).
    assert case.array("demand_array")[0]["lossfactor"] == [0.01, 0.02, 0.03]
    assert case.array("demand_array")[1]["lossfactor"] == "Demand/lf.parquet"
    # Demand uid=3 had no lossfactor → fresh write.
    assert case.array("demand_array")[2]["lossfactor"] == pytest.approx(0.05)


def test_loss_mode_unknown_raises() -> None:
    case = _toy_case()
    with pytest.raises(ValueError, match="unknown loss_mode"):
        apply_loss_mode(case, "bogus")


def test_reduce_config_validates_loss_mode() -> None:
    with pytest.raises(ValueError, match="loss_mode"):
        ReduceConfig(target_buses=5, loss_mode="bogus")


def test_reduce_config_validates_collision() -> None:
    with pytest.raises(ValueError, match="loss_uplift_collision"):
        ReduceConfig(target_buses=5, loss_uplift_collision="bogus")


def test_reduce_with_transport_only(ieee14_path: Path) -> None:
    case = load_case(ieee14_path)
    cfg = ReduceConfig(target_buses=7, transport_only=True)
    result = reduce_case(case, cfg)
    opts = result.case.options
    model = opts.get("model_options", opts)
    assert model["use_kirchhoff"] is False


def test_reduce_with_loss_uplift_writes_lossfactor(ieee14_path: Path) -> None:
    """End-to-end: reduce IEEE-14 with uplift → every demand has scalar lossfactor."""
    case = load_case(ieee14_path)
    orig_lmaxes = [d["lmax"] for d in case.array("demand_array")]

    cfg = ReduceConfig(target_buses=7, loss_mode="uplift", loss_uplift_pct=5.0)
    result = reduce_case(case, cfg)

    # lmax untouched on every demand in the reduced case.
    new_lmaxes = [d["lmax"] for d in result.case.array("demand_array")]
    assert new_lmaxes == orig_lmaxes
    # Every demand carries lossfactor = 0.05.
    for d in result.case.array("demand_array"):
        assert d["lossfactor"] == pytest.approx(0.05)
    opts = result.case.options
    model = opts.get("model_options", opts)
    assert model["use_line_losses"] is False


def test_reduce_config_as_dict_contains_simplify_fields() -> None:
    cfg = ReduceConfig(
        target_buses=10,
        transport_only=True,
        loss_mode="uplift",
        loss_uplift_pct=4.5,
        loss_uplift_collision="compound",
    )
    d = cfg.as_dict()
    assert d["transport_only"] is True
    assert d["loss_mode"] == "uplift"
    assert d["loss_uplift_pct"] == 4.5
    assert d["loss_uplift_collision"] == "compound"
