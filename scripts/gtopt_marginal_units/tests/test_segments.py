# SPDX-License-Identifier: BSD-3-Clause
"""Piecewise-MC active-segment tests."""

from __future__ import annotations

import pytest

from gtopt_marginal_units._segments import active_segment


def test_scalar_unit_returns_declared_mc():
    seg = active_segment(dispatch=50.0, pmin=0, pmax=100, declared_MC=20.0)
    assert seg.slope == pytest.approx(20.0)
    assert seg.seg_index == -1
    assert not seg.ambiguous


def test_scalar_unit_with_no_mc_returns_none():
    seg = active_segment(dispatch=50.0, pmin=0, pmax=100, declared_MC=None)
    assert seg.slope is None
    assert seg.seg_index == -1


def test_piecewise_first_segment():
    segments = [(30.0, 10.0), (60.0, 20.0), (100.0, 80.0)]
    seg = active_segment(
        dispatch=15.0, pmin=0, pmax=100, declared_MC=None, segments=segments
    )
    assert seg.slope == 10.0
    assert seg.seg_index == 0
    assert not seg.ambiguous


def test_piecewise_middle_segment():
    segments = [(30.0, 10.0), (60.0, 20.0), (100.0, 80.0)]
    seg = active_segment(
        dispatch=45.0, pmin=0, pmax=100, declared_MC=None, segments=segments
    )
    assert seg.slope == 20.0
    assert seg.seg_index == 1


def test_piecewise_last_segment():
    segments = [(30.0, 10.0), (60.0, 20.0), (100.0, 80.0)]
    seg = active_segment(
        dispatch=85.0, pmin=0, pmax=100, declared_MC=None, segments=segments
    )
    assert seg.slope == 80.0
    assert seg.seg_index == 2


def test_piecewise_at_break_point_flags_ambiguous():
    segments = [(30.0, 10.0), (60.0, 20.0), (100.0, 80.0)]
    seg = active_segment(
        dispatch=30.0,
        pmin=0,
        pmax=100,
        declared_MC=None,
        segments=segments,
        eps=0.1,
    )
    assert seg.slope == 10.0  # chosen segment
    assert seg.ambiguous is True  # but flag the break


def test_piecewise_above_last_segment_clamps():
    segments = [(30.0, 10.0), (60.0, 20.0), (100.0, 80.0)]
    seg = active_segment(
        dispatch=200.0, pmin=0, pmax=100, declared_MC=None, segments=segments
    )
    # Above the last upper bound — we clamp to the last segment.
    assert seg.seg_index == 2
    assert seg.slope == 80.0
