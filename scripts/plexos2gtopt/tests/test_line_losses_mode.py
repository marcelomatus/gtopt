"""Pin the converter's DEFAULT line-loss model.

Regression guard for the bug where ``build_line_array`` hard-coded
``Line.line_losses_mode = "piecewise"`` and the dynamic per-line layout,
silently SHADOWING the ``--line-losses-mode`` default (``tangent_signed_flow``
= Coffrin) and the ``--nseg-losses`` default (K=10).  A no-option conversion
must emit Coffrin + K=10 on lossy lines, with no PWL layout, and must honor an
explicit ``--line-losses-mode`` override.
"""

from __future__ import annotations

import pytest

from plexos2gtopt.entities import LineSpec
from plexos2gtopt.gtopt_writer import build_line_array


def _lossy_line() -> LineSpec:
    """A line with an enforced rating + resistance → carries a loss model."""
    return LineSpec(
        object_id=1,
        name="L",
        bus_from="a",
        bus_to="b",
        tmax_ab=100.0,
        tmin_ab=-100.0,
        resistance=0.01,
        reactance=0.1,
    )


def test_default_line_losses_mode_is_coffrin_tangent(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Clear the loss env so the K default (10) and layout are deterministic.
    monkeypatch.delenv("GTOPT_NSEG_LOSSES", raising=False)
    monkeypatch.delenv("GTOPT_LOSS_PWL_LAYOUT", raising=False)
    monkeypatch.delenv("GTOPT_LOSS_TANGENT_LINES", raising=False)
    out = build_line_array((_lossy_line(),))  # no mode → default
    entry = out[0]
    assert entry["line_losses_mode"] == "tangent_signed_flow", (
        "default loss mode must be Coffrin tangent_signed_flow, not the "
        f"over-counting piecewise; got {entry.get('line_losses_mode')!r}"
    )
    assert entry["loss_segments"] == 10, (
        f"default K must be --nseg-losses=10; got {entry.get('loss_segments')!r}"
    )
    # Coffrin is its own signed-flow model — it carries no PWL layout.
    assert "loss_pwl_layout" not in entry


def test_line_losses_mode_override_honored(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("GTOPT_NSEG_LOSSES", raising=False)
    monkeypatch.delenv("GTOPT_LOSS_PWL_LAYOUT", raising=False)
    out = build_line_array((_lossy_line(),), line_losses_mode="piecewise")
    assert out[0]["line_losses_mode"] == "piecewise"
