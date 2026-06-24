"""Unit tests for the cascade-reduced builder in GTOptWriter."""

from __future__ import annotations

from unittest.mock import MagicMock

import pytest

from plp2gtopt.gtopt_writer import GTOptWriter


def _writer_with_phases(n_apertures_per_phase: int = 16) -> GTOptWriter:
    """Build a GTOptWriter with a synthetic phase_array carrying apertures
    so the L2 aperture computation has something to inspect."""
    w = GTOptWriter(MagicMock())
    w.planning["simulation"]["phase_array"] = [
        {
            "uid": 1,
            "first_stage": 0,
            "count_stage": 1,
            "apertures": list(range(n_apertures_per_phase)),
        },
    ]
    return w


# ─── Method normalisation + dispatch ─────────────────────────────────────


def test_normalize_method_accepts_cascade_reduced() -> None:
    assert GTOptWriter._normalize_method("cascade-reduced") == "cascade-reduced"
    assert GTOptWriter._normalize_method("cascade_reduced") == "cascade-reduced"


def test_process_options_cascade_reduced_emits_4_levels() -> None:
    w = _writer_with_phases()
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    opts = w.planning["options"]
    # JSON-facing method is "cascade" so gtopt's MethodType enum accepts
    # it; the cascade-reduced flavour is signalled by system_file on
    # individual levels and is otherwise indistinguishable on the C++ side.
    assert opts["method"] == "cascade"
    cascade = opts["cascade_options"]
    levels = cascade["level_array"]
    names = [lvl["name"] for lvl in levels]
    assert names == ["uninodal", "reduced_transport", "reduced_dcopf", "full_network"]


# ─── Level structure ──────────────────────────────────────────────────────


def test_l0_is_single_bus_with_1_head_aperture() -> None:
    w = _writer_with_phases()
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    l0 = w.planning["options"]["cascade_options"]["level_array"][0]
    assert l0["name"] == "uninodal"
    assert l0["model_options"]["use_single_bus"] is True
    so = l0["sddp_options"]
    assert so["num_apertures"] == 1
    assert so["aperture_selection_mode"] == "head"


def test_l1_is_reduced_transport_no_kvl_no_losses_stride() -> None:
    w = _writer_with_phases(n_apertures_per_phase=16)
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    l1 = w.planning["options"]["cascade_options"]["level_array"][1]
    assert l1["name"] == "reduced_transport"
    mo = l1["model_options"]
    assert mo["use_single_bus"] is False
    assert mo["use_kirchhoff"] is False
    assert mo["use_line_losses"] is False
    so = l1["sddp_options"]
    # ONA = 16, default ratio = 4 → num_apertures = 4.
    assert so["num_apertures"] == 4
    assert so["aperture_selection_mode"] == "stride"
    assert l1["system_file"] == "case.L1.json"


def test_l2_is_reduced_dcopf_kvl_on_no_explicit_losses_stride() -> None:
    w = _writer_with_phases(n_apertures_per_phase=16)
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    l2 = w.planning["options"]["cascade_options"]["level_array"][2]
    assert l2["name"] == "reduced_dcopf"
    mo = l2["model_options"]
    assert mo["use_single_bus"] is False
    assert mo["use_kirchhoff"] is True
    assert mo["use_line_losses"] is False
    so = l2["sddp_options"]
    # ONA = 16, default ratio = 2 → num_apertures = 8.
    assert so["num_apertures"] == 8
    assert so["aperture_selection_mode"] == "stride"
    assert l2["system_file"] == "case.L2.json"


def test_l3_is_full_network_no_aperture_overrides() -> None:
    w = _writer_with_phases()
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    l3 = w.planning["options"]["cascade_options"]["level_array"][3]
    assert l3["name"] == "full_network"
    so = l3["sddp_options"]
    assert "num_apertures" not in so
    assert "aperture_selection_mode" not in so


# ─── Aperture-ratio knob ──────────────────────────────────────────────────


def test_aperture_ratios_floor_at_1() -> None:
    """Small ONA + large ratio → still ≥ 1 aperture on both reduced levels."""
    w = _writer_with_phases(n_apertures_per_phase=2)
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "cascade_reduced_opts": {
                "l1_aperture_ratio": 16,
                "l2_aperture_ratio": 16,
            },
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    assert levels[1]["sddp_options"]["num_apertures"] == 1
    assert levels[2]["sddp_options"]["num_apertures"] == 1


def test_aperture_ratios_override_defaults() -> None:
    w = _writer_with_phases(n_apertures_per_phase=24)
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "cascade_reduced_opts": {
                "l1_aperture_ratio": 6,
                "l2_aperture_ratio": 3,
            },
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    # 24 / 6 = 4 at L1; 24 / 3 = 8 at L2.
    assert levels[1]["sddp_options"]["num_apertures"] == 4
    assert levels[2]["sddp_options"]["num_apertures"] == 8


def test_default_aperture_ratios_l1_4_l2_2() -> None:
    """L1 = ONA/4 stride, L2 = ONA/2 stride (canonical defaults)."""
    w = _writer_with_phases(n_apertures_per_phase=16)
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    # L1: 16 / 4 = 4 apertures.
    assert levels[1]["sddp_options"]["num_apertures"] == 4
    assert levels[1]["sddp_options"]["aperture_selection_mode"] == "stride"
    # L2: 16 / 2 = 8 apertures.
    assert levels[2]["sddp_options"]["num_apertures"] == 8
    assert levels[2]["sddp_options"]["aperture_selection_mode"] == "stride"


# ─── Disable flags ────────────────────────────────────────────────────────


def test_disable_l1_skips_level() -> None:
    w = _writer_with_phases()
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "cascade_reduced_opts": {"disable_l1": True},
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    names = [lvl["name"] for lvl in levels]
    assert names == ["uninodal", "reduced_dcopf", "full_network"]
    # All non-name fields still well-formed.
    assert levels[1]["system_file"] == "case.L2.json"


def test_disable_l2_skips_level() -> None:
    w = _writer_with_phases()
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "cascade_reduced_opts": {"disable_l2": True},
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    names = [lvl["name"] for lvl in levels]
    assert names == ["uninodal", "reduced_transport", "full_network"]
    assert levels[1]["system_file"] == "case.L1.json"


def test_disable_both_falls_back_to_uninodal_full_only() -> None:
    w = _writer_with_phases()
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "cascade_reduced_opts": {"disable_l1": True, "disable_l2": True},
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    names = [lvl["name"] for lvl in levels]
    assert names == ["uninodal", "full_network"]


# ─── Transitions + cuts ───────────────────────────────────────────────────


def test_l1_and_l2_carry_transition_with_inherit_cuts() -> None:
    w = _writer_with_phases()
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "cascade-reduced"}
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    # L1, L2, L3 all have transition with -1 inheritance.
    for lvl in levels[1:]:
        tr = lvl["transition"]
        assert tr["inherit_optimality_cuts"] == -1
        assert "inherit_targets" not in tr


# ─── Path stem comes from output_file ─────────────────────────────────────


def test_system_file_paths_derive_from_output_file_stem() -> None:
    w = _writer_with_phases()
    w.process_options(
        {
            "output_dir": "/tmp/work",
            "output_file": "/tmp/work/juan.json",
            "method": "cascade-reduced",
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    l1 = next(lvl for lvl in levels if lvl["name"] == "reduced_transport")
    l2 = next(lvl for lvl in levels if lvl["name"] == "reduced_dcopf")
    # Relative basenames (cascade resolves them next to the parent JSON).
    assert l1["system_file"] == "juan.L1.json"
    assert l2["system_file"] == "juan.L2.json"


# ─── Iteration budget ─────────────────────────────────────────────────────


def test_iteration_budget_split() -> None:
    """L0=N, L1=N/2, L2=N/2, L3=N/4 (cheap LPs at L1/L2 → more budget)."""
    w = _writer_with_phases()
    w.process_options(
        {
            "output_dir": "out",
            "output_file": "case.json",
            "method": "cascade-reduced",
            "max_iterations": 120,
        }
    )
    levels = w.planning["options"]["cascade_options"]["level_array"]
    assert levels[0]["sddp_options"]["max_iterations"] == 120
    assert levels[1]["sddp_options"]["max_iterations"] == 60
    assert levels[2]["sddp_options"]["max_iterations"] == 60
    assert levels[3]["sddp_options"]["max_iterations"] == 30
    # Cascade-global = N + N/2 + N/2 + N/4 = 270.
    assert (
        w.planning["options"]["cascade_options"]["sddp_options"]["max_iterations"]
        == 270
    )


def test_method_sddp_does_not_emit_cascade_options() -> None:
    """Sanity: only cascade* methods emit cascade_options."""
    w = _writer_with_phases()
    w.process_options(
        {"output_dir": "out", "output_file": "case.json", "method": "sddp"}
    )
    assert "cascade_options" not in w.planning["options"]


@pytest.mark.parametrize(
    ("method_in", "expected"),
    [
        ("cascade-reduced", "cascade-reduced"),
        ("cascade_reduced", "cascade-reduced"),
        ("cascade", "cascade"),
        ("sddp", "sddp"),
        ("mono", "monolithic"),
        ("monolithic", "monolithic"),
        ("bogus", "sddp"),  # unknown → default
    ],
)
def test_normalize_method_table(method_in: str, expected: str) -> None:
    assert GTOptWriter._normalize_method(method_in) == expected
