# SPDX-License-Identifier: BSD-3-Clause
"""CLI plumbing tests for the plp2gtopt modernization flags.

Covers:
- ``--aperture-chunk-size`` → ``options.sddp_options.aperture_chunk_size``
- ``--loss-cost-eps`` → ``options.model_options.loss_cost_eps``
- ``--lift-line-caps`` → ``options.lift_line_caps`` (dict)
- ``--line-losses-mode tangent_signed_flow`` accepted by choices
- LineWriter stamps ``loss_envelope`` when name is in the lift dict
"""

from __future__ import annotations

import pytest

from plp2gtopt.line_writer import LineWriter
from plp2gtopt.main import _parse_lift_line_caps, build_options, make_parser


# ---------------------------------------------------------------------------
# _parse_lift_line_caps
# ---------------------------------------------------------------------------


def test_parse_lift_line_caps_per_line_factor() -> None:
    assert _parse_lift_line_caps("L1:3.0,L2:1.5") == {"L1": 3.0, "L2": 1.5}


def test_parse_lift_line_caps_default_factor() -> None:
    # Bare name uses the converter's default factor (2.0).
    assert _parse_lift_line_caps("L1") == {"L1": 2.0}


def test_parse_lift_line_caps_mixed() -> None:
    assert _parse_lift_line_caps("L1:3.0,L2,L3:1.5") == {
        "L1": 3.0,
        "L2": 2.0,
        "L3": 1.5,
    }


def test_parse_lift_line_caps_strips_whitespace() -> None:
    assert _parse_lift_line_caps("  L1 : 3.0 , L2  ") == {"L1": 3.0, "L2": 2.0}


def test_parse_lift_line_caps_empty() -> None:
    assert not _parse_lift_line_caps("")


def test_parse_lift_line_caps_invalid_factor() -> None:
    with pytest.raises(ValueError, match="Invalid factor"):
        _parse_lift_line_caps("L1:not-a-number")


# ---------------------------------------------------------------------------
# argparse: choices + defaults
# ---------------------------------------------------------------------------


def test_line_losses_mode_accepts_tangent_signed_flow() -> None:
    p = make_parser()
    args = p.parse_args(["/tmp/x", "--line-losses-mode", "tangent_signed_flow"])
    assert args.line_losses_mode == "tangent_signed_flow"


def test_aperture_chunk_size_defaults_to_none() -> None:
    p = make_parser()
    args = p.parse_args(["/tmp/x"])
    assert args.aperture_chunk_size is None


def test_loss_cost_eps_defaults_to_one() -> None:
    # plp2gtopt now ships ``loss_cost_eps = 1.0 $/MWh`` by default —
    # strictly breaks the bidirectional-flow degeneracy even under
    # aggressive presolve / Ruiz scaling, and doubles as the v-pin the
    # tangent_signed_flow L-secant bracket needs when
    # loss_secant_segments > 1 without SOS2.  Pass ``--loss-cost-eps 0``
    # to opt out.
    p = make_parser()
    args = p.parse_args(["/tmp/x"])
    assert args.loss_cost_eps == 1.0


def test_lift_line_caps_defaults_to_none() -> None:
    p = make_parser()
    args = p.parse_args(["/tmp/x"])
    assert args.lift_line_caps is None


# ---------------------------------------------------------------------------
# build_options wiring (no parsers/writers — pure Namespace)
# ---------------------------------------------------------------------------


def _parse(*extra: str):
    """Parse a CLI list with a placeholder input directory."""
    p = make_parser()
    return p.parse_args(["/tmp/nonexistent", *extra])


def test_build_options_emits_aperture_chunk_size() -> None:
    args = _parse("--aperture-chunk-size", "4")
    opts = build_options(args)
    assert opts["sddp_options"]["aperture_chunk_size"] == 4


def test_build_options_omits_aperture_chunk_size_when_unset() -> None:
    # monolithic gets no iterative fast-path defaults, so an unset
    # --aperture-chunk-size leaves it absent.  (sddp/cascade deliberately
    # default it to -1 via the fast-path block — see
    # test_main_coverage.test_sddp_method_fast_path_defaults.)
    args = _parse("--method", "monolithic")
    opts = build_options(args)
    assert "aperture_chunk_size" not in opts.get("sddp_options", {})


def test_build_options_emits_loss_cost_eps() -> None:
    args = _parse("--loss-cost-eps", "1e-6")
    opts = build_options(args)
    assert opts["model_options"]["loss_cost_eps"] == 1e-6


def test_build_options_emits_default_loss_cost_eps() -> None:
    # The plp2gtopt default ``--loss-cost-eps 1.0`` is forwarded as
    # ``options.model_options.loss_cost_eps`` so the gtopt LP receives
    # the strict degeneracy-breaker out of the box (no opt-in needed).
    args = _parse()
    opts = build_options(args)
    assert opts["model_options"]["loss_cost_eps"] == 1.0


def test_build_options_omits_loss_secant_segments_by_default() -> None:
    # ``--loss-secant-segments`` defaults to UNSET: extra secants are
    # inert in pure LP (chord inactive at benign optima; arbitrage
    # ceiling S-independent), so the converter emits nothing and gtopt
    # resolves S=1.  S>1 is a per-line, SOS2-paired opt-in.
    args = _parse()
    opts = build_options(args)
    assert "loss_secant_segments" not in opts["model_options"]

    args_explicit = _parse("--loss-secant-segments", "4")
    opts_explicit = build_options(args_explicit)
    assert opts_explicit["model_options"]["loss_secant_segments"] == 4


def test_build_options_emits_explicit_zero_loss_cost_eps() -> None:
    # Setting ``--loss-cost-eps 0`` is the opt-out path — the explicit
    # ``0.0`` is forwarded (not omitted) so the planning JSON shows the
    # user explicitly asked for legacy behaviour, not "left at default".
    # The C++ side reads the explicit 0.0 the same as a missing field.
    args = _parse("--loss-cost-eps", "0")
    opts = build_options(args)
    assert opts["model_options"]["loss_cost_eps"] == 0.0


def test_build_options_emits_lift_line_caps_dict() -> None:
    args = _parse("--lift-line-caps", "L1:3.0,L2")
    opts = build_options(args)
    assert opts["lift_line_caps"] == {"L1": 3.0, "L2": 2.0}


def test_build_options_omits_lift_line_caps_when_empty() -> None:
    args = _parse()
    opts = build_options(args)
    assert "lift_line_caps" not in opts


# ---------------------------------------------------------------------------
# LineWriter loss_envelope stamping
# ---------------------------------------------------------------------------


def test_line_writer_stamps_loss_envelope_when_listed() -> None:
    items = [
        {
            "name": "L1",
            "number": 1,
            "bus_a": 1,
            "bus_b": 2,
            "r": 0.01,
            "x": 0.1,
            "tmax_ab": 100.0,
            "tmax_ba": 100.0,
            "voltage": 220.0,
            "operational": 1,
            "num_sections": 1,
            "mod_perdidas": False,
        },
        {
            "name": "L2",
            "number": 2,
            "bus_a": 2,
            "bus_b": 3,
            "r": 0.01,
            "x": 0.1,
            "tmax_ab": 50.0,
            "tmax_ba": 50.0,
            "voltage": 220.0,
            "operational": 1,
            "num_sections": 1,
            "mod_perdidas": False,
        },
    ]

    writer = LineWriter(options={"lift_line_caps": {"L1": 2.5}})
    out = writer.to_json_array(items=items)

    by_name = {line["name"]: line for line in out}
    assert by_name["L1"]["loss_envelope"] == pytest.approx(2.5 * 100.0)
    assert "loss_envelope" not in by_name["L2"]


def test_line_writer_no_envelope_when_tmax_is_parquet_ref() -> None:
    # When tmax_ab survives as a column name (parquet reference) we
    # cannot compute a numeric envelope here.  The lift list is
    # silently skipped for that line.
    items = [
        {
            "name": "L1",
            "number": 1,
            "bus_a": 1,
            "bus_b": 2,
            "r": 0.01,
            "x": 0.1,
            "tmax_ab": "tmax_ab",  # parquet column reference
            "tmax_ba": "tmax_ba",
            "voltage": 220.0,
            "operational": 1,
            "num_sections": 1,
            "mod_perdidas": False,
        },
    ]
    writer = LineWriter(options={"lift_line_caps": {"L1": 2.5}})
    out = writer.to_json_array(items=items)
    assert "loss_envelope" not in out[0]


def test_line_writer_no_envelope_without_option() -> None:
    items = [
        {
            "name": "L1",
            "number": 1,
            "bus_a": 1,
            "bus_b": 2,
            "r": 0.01,
            "x": 0.1,
            "tmax_ab": 100.0,
            "tmax_ba": 100.0,
            "voltage": 220.0,
            "operational": 1,
            "num_sections": 1,
            "mod_perdidas": False,
        },
    ]
    writer = LineWriter(options={})
    out = writer.to_json_array(items=items)
    assert "loss_envelope" not in out[0]
