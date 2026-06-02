# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.cli_flags`.

The shared helpers exist to keep the eight canonical gtopt
converter flags in sync across ``plp2gtopt``, ``plexos2gtopt``,
``sddp2gtopt`` and ``pp2gtopt``.  Each helper is exercised here in
isolation against a throw-away ``argparse.ArgumentParser`` so a
single registrar change is caught even when no consumer test
covers the path yet.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pytest

from gtopt_shared.cli_flags import (
    LINE_LOSSES_MODE_CHOICES,
    add_aperture_chunk_size_argument,
    add_common_arguments,
    add_demand_fail_cost_argument,
    add_emissions_arguments,
    add_lift_line_caps_argument,
    add_line_losses_mode_argument,
    add_loss_cost_eps_argument,
    add_scale_objective_argument,
    add_use_kirchhoff_argument,
    add_use_single_bus_argument,
)


def _parser() -> argparse.ArgumentParser:
    """Return a fresh parser; tests must not share state."""
    return argparse.ArgumentParser()


# ---------------------------------------------------------------------------
# add_emissions_arguments — Phase-1 helper kept from feat/pmin-fcost
# ---------------------------------------------------------------------------


def _parser_with_emissions() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    add_emissions_arguments(parser)
    return parser


def test_emissions_defaults_off() -> None:
    args = _parser_with_emissions().parse_args([])
    assert args.emissions is False
    assert args.emissions_file is None
    assert args.emissions_report is None


def test_emissions_master_switch_on() -> None:
    args = _parser_with_emissions().parse_args(["--emissions"])
    assert args.emissions is True


def test_emissions_custom_file_and_report(tmp_path: Path) -> None:
    custom = tmp_path / "custom.json"
    report = tmp_path / "report.json"
    args = _parser_with_emissions().parse_args(
        [
            "--emissions",
            "--emissions-file",
            str(custom),
            "--emissions-report",
            str(report),
        ]
    )
    assert args.emissions_file == custom
    assert args.emissions_report == report


def test_emissions_default_override_via_defaults_dict() -> None:
    parser = argparse.ArgumentParser()
    add_emissions_arguments(parser, defaults={"emissions": True})
    args = parser.parse_args([])
    assert args.emissions is True


# ---------------------------------------------------------------------------
# scale_objective
# ---------------------------------------------------------------------------


def test_scale_objective_default_value() -> None:
    parser = _parser()
    add_scale_objective_argument(parser)
    args = parser.parse_args([])
    assert args.scale_objective == 1000.0


def test_scale_objective_default_override() -> None:
    parser = _parser()
    add_scale_objective_argument(parser, default=1.0)
    args = parser.parse_args([])
    assert args.scale_objective == 1.0


def test_scale_objective_explicit_value() -> None:
    parser = _parser()
    add_scale_objective_argument(parser, default=1.0)
    args = parser.parse_args(["--scale-objective", "2500.5"])
    assert args.scale_objective == 2500.5


# ---------------------------------------------------------------------------
# demand_fail_cost
# ---------------------------------------------------------------------------


def test_demand_fail_cost_default_value() -> None:
    parser = _parser()
    add_demand_fail_cost_argument(parser)
    args = parser.parse_args([])
    assert args.demand_fail_cost == 1000.0


def test_demand_fail_cost_default_none() -> None:
    """``default=None`` signals auto-derive."""
    parser = _parser()
    add_demand_fail_cost_argument(parser, default=None)
    args = parser.parse_args([])
    assert args.demand_fail_cost is None


def test_demand_fail_cost_explicit() -> None:
    parser = _parser()
    add_demand_fail_cost_argument(parser, default=None)
    args = parser.parse_args(["--demand-fail-cost", "500"])
    assert args.demand_fail_cost == 500.0


def test_demand_fail_cost_custom_help() -> None:
    parser = _parser()
    add_demand_fail_cost_argument(parser, default=None, help_text="custom help string")
    fmt = parser.format_help()
    assert "custom help string" in fmt


# ---------------------------------------------------------------------------
# use_kirchhoff
# ---------------------------------------------------------------------------


def test_use_kirchhoff_default_true() -> None:
    parser = _parser()
    add_use_kirchhoff_argument(parser)
    args = parser.parse_args([])
    assert args.use_kirchhoff is True


def test_use_kirchhoff_negation() -> None:
    parser = _parser()
    add_use_kirchhoff_argument(parser)
    args = parser.parse_args(["--no-use-kirchhoff"])
    assert args.use_kirchhoff is False


def test_use_kirchhoff_short_flag() -> None:
    parser = _parser()
    add_use_kirchhoff_argument(parser)
    args = parser.parse_args(["-k"])
    assert args.use_kirchhoff is True


# ---------------------------------------------------------------------------
# use_single_bus
# ---------------------------------------------------------------------------


def test_use_single_bus_boolean_optional_default() -> None:
    """plp2gtopt dialect: tri-state default=None."""
    parser = _parser()
    add_use_single_bus_argument(parser, dialect="boolean_optional")
    args = parser.parse_args([])
    assert args.use_single_bus is None


def test_use_single_bus_boolean_optional_on() -> None:
    parser = _parser()
    add_use_single_bus_argument(parser, dialect="boolean_optional")
    args = parser.parse_args(["-b"])
    assert args.use_single_bus is True


def test_use_single_bus_boolean_optional_off() -> None:
    parser = _parser()
    add_use_single_bus_argument(parser, dialect="boolean_optional")
    args = parser.parse_args(["--no-use-single-bus"])
    assert args.use_single_bus is False


def test_use_single_bus_store_true_default() -> None:
    """plexos2gtopt dialect: default=False, no short flag."""
    parser = _parser()
    add_use_single_bus_argument(parser, dialect="store_true")
    args = parser.parse_args([])
    assert args.use_single_bus is False


def test_use_single_bus_store_true_set() -> None:
    parser = _parser()
    add_use_single_bus_argument(parser, dialect="store_true")
    args = parser.parse_args(["--use-single-bus"])
    assert args.use_single_bus is True


def test_use_single_bus_unknown_dialect_raises() -> None:
    parser = _parser()
    with pytest.raises(ValueError, match="unknown dialect"):
        add_use_single_bus_argument(parser, dialect="bogus")


# ---------------------------------------------------------------------------
# line_losses_mode
# ---------------------------------------------------------------------------


def test_line_losses_mode_default_none() -> None:
    parser = _parser()
    add_line_losses_mode_argument(parser)
    args = parser.parse_args([])
    assert args.line_losses_mode is None


def test_line_losses_mode_default_override() -> None:
    parser = _parser()
    add_line_losses_mode_argument(parser, default="adaptive")
    args = parser.parse_args([])
    assert args.line_losses_mode == "adaptive"


def test_line_losses_mode_accepts_every_choice() -> None:
    """Every C++ enum value must round-trip through argparse."""
    for choice in LINE_LOSSES_MODE_CHOICES:
        parser = _parser()
        add_line_losses_mode_argument(parser)
        args = parser.parse_args(["--line-losses-mode", choice])
        assert args.line_losses_mode == choice


def test_line_losses_mode_rejects_unknown() -> None:
    parser = _parser()
    add_line_losses_mode_argument(parser)
    with pytest.raises(SystemExit) as excinfo:
        parser.parse_args(["--line-losses-mode", "not_a_mode"])
    assert excinfo.value.code == 2


def test_line_losses_mode_choices_match_cpp_enum() -> None:
    """Pin the canonical 8-mode list as published to the C++ enum."""
    assert LINE_LOSSES_MODE_CHOICES == (
        "none",
        "linear",
        "piecewise",
        "bidirectional",
        "adaptive",
        "dynamic",
        "piecewise_direct",
        "tangent_signed_flow",
    )


# ---------------------------------------------------------------------------
# loss_cost_eps
# ---------------------------------------------------------------------------


def test_loss_cost_eps_plp_default_none() -> None:
    parser = _parser()
    add_loss_cost_eps_argument(parser, dialect="plp")
    args = parser.parse_args([])
    assert args.loss_cost_eps is None


def test_loss_cost_eps_plp_explicit() -> None:
    parser = _parser()
    add_loss_cost_eps_argument(parser, dialect="plp")
    args = parser.parse_args(["--loss-cost-eps", "1e-6"])
    assert args.loss_cost_eps == 1e-6


def test_loss_cost_eps_plexos_default_zero() -> None:
    parser = _parser()
    add_loss_cost_eps_argument(parser, dialect="plexos")
    args = parser.parse_args([])
    assert args.loss_cost_eps == 0.0


def test_loss_cost_eps_plexos_default_override() -> None:
    parser = _parser()
    add_loss_cost_eps_argument(parser, dialect="plexos", default=1e-6)
    args = parser.parse_args([])
    assert args.loss_cost_eps == 1e-6


def test_loss_cost_eps_unknown_dialect_raises() -> None:
    parser = _parser()
    with pytest.raises(ValueError, match="unknown dialect"):
        add_loss_cost_eps_argument(parser, dialect="bogus")


# ---------------------------------------------------------------------------
# lift_line_caps
# ---------------------------------------------------------------------------


def test_lift_line_caps_plp_default_none() -> None:
    parser = _parser()
    add_lift_line_caps_argument(parser, dialect="plp")
    args = parser.parse_args([])
    assert args.lift_line_caps is None


def test_lift_line_caps_plp_explicit() -> None:
    parser = _parser()
    add_lift_line_caps_argument(parser, dialect="plp")
    args = parser.parse_args(["--lift-line-caps", "L1:3.0,L2"])
    assert args.lift_line_caps == "L1:3.0,L2"


def test_lift_line_caps_plexos_default_capricornio() -> None:
    parser = _parser()
    add_lift_line_caps_argument(parser, dialect="plexos")
    args = parser.parse_args([])
    assert args.lift_line_caps == "Capricornio110->LaNegra110"


def test_lift_line_caps_plexos_empty_opt_in_soft_mode() -> None:
    """Empty string activates the experimental SOFT-EL=1 mode."""
    parser = _parser()
    add_lift_line_caps_argument(parser, dialect="plexos")
    args = parser.parse_args(["--lift-line-caps", ""])
    assert args.lift_line_caps == ""


def test_lift_line_caps_unknown_dialect_raises() -> None:
    parser = _parser()
    with pytest.raises(ValueError, match="unknown dialect"):
        add_lift_line_caps_argument(parser, dialect="bogus")


# ---------------------------------------------------------------------------
# aperture_chunk_size
# ---------------------------------------------------------------------------


def test_aperture_chunk_size_default_none() -> None:
    parser = _parser()
    add_aperture_chunk_size_argument(parser)
    args = parser.parse_args([])
    assert args.aperture_chunk_size is None


def test_aperture_chunk_size_explicit() -> None:
    parser = _parser()
    add_aperture_chunk_size_argument(parser)
    args = parser.parse_args(["--aperture-chunk-size", "4"])
    assert args.aperture_chunk_size == 4


def test_aperture_chunk_size_serial_negative() -> None:
    """-1 = fully serial per scene."""
    parser = _parser()
    add_aperture_chunk_size_argument(parser)
    args = parser.parse_args(["--aperture-chunk-size", "-1"])
    assert args.aperture_chunk_size == -1


def test_aperture_chunk_size_rejects_non_int() -> None:
    parser = _parser()
    add_aperture_chunk_size_argument(parser)
    with pytest.raises(SystemExit):
        parser.parse_args(["--aperture-chunk-size", "auto"])


# ---------------------------------------------------------------------------
# Grouped registrar
# ---------------------------------------------------------------------------


def test_add_common_arguments_registers_all_flags() -> None:
    """Every flag must be present after a default-args call."""
    parser = _parser()
    add_common_arguments(parser)
    args = parser.parse_args([])
    for field in (
        "scale_objective",
        "demand_fail_cost",
        "use_kirchhoff",
        "use_single_bus",
        "line_losses_mode",
        "loss_cost_eps",
        "lift_line_caps",
        "aperture_chunk_size",
    ):
        assert hasattr(args, field), f"missing {field}"


def test_add_common_arguments_default_overrides() -> None:
    parser = _parser()
    add_common_arguments(
        parser,
        defaults={
            "scale_objective": 1.0,
            "demand_fail_cost": 250.0,
            "use_kirchhoff": False,
            "line_losses_mode": "adaptive",
            "aperture_chunk_size": 2,
        },
    )
    args = parser.parse_args([])
    assert args.scale_objective == 1.0
    assert args.demand_fail_cost == 250.0
    assert args.use_kirchhoff is False
    assert args.line_losses_mode == "adaptive"
    assert args.aperture_chunk_size == 2


def test_add_common_arguments_plexos_dialects() -> None:
    parser = _parser()
    add_common_arguments(
        parser,
        dialects={
            "use_single_bus": "store_true",
            "loss_cost_eps": "plexos",
            "lift_line_caps": "plexos",
        },
    )
    args = parser.parse_args([])
    assert args.use_single_bus is False  # store_true default
    assert args.loss_cost_eps == 0.0
    assert args.lift_line_caps == "Capricornio110->LaNegra110"


def test_add_common_arguments_skip() -> None:
    """``skip=`` omits the named flags."""
    parser = _parser()
    add_common_arguments(parser, skip={"aperture_chunk_size", "lift_line_caps"})
    args = parser.parse_args([])
    assert hasattr(args, "scale_objective")
    assert not hasattr(args, "aperture_chunk_size")
    assert not hasattr(args, "lift_line_caps")
