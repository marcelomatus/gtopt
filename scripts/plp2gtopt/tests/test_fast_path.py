# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plp2gtopt._fast_path — the shared iterative SDDP fast-path defaults.

Locks the single source of truth used by both ``main.build_options`` (CLI)
and ``GTOptWriter.process_options`` (writer-direct), including the
setdefault / source-seed semantics and the deliberate omission of
``cut_sharing_mode`` (each caller owns that on its own target dict).
"""

from __future__ import annotations

from plp2gtopt._fast_path import FAST_PATH_METHODS, apply_iterative_fast_path


def test_fast_path_methods_membership() -> None:
    assert FAST_PATH_METHODS == {"sddp", "cascade", "cascade-reduced"}
    assert "monolithic" not in FAST_PATH_METHODS


def test_defaults_on_empty_dicts() -> None:
    model_opts: dict = {}
    sddp_opts: dict = {}
    apply_iterative_fast_path(model_opts, sddp_opts)
    assert model_opts["lp_reduction"] is True
    assert sddp_opts["aperture_solve_mode"] == "warm"
    assert sddp_opts["aperture_chunk_size"] == -1
    assert sddp_opts["forward_solver_options"] == {
        "algorithm": "dual",
        "advanced_basis": True,
    }
    assert sddp_opts["backward_solver_options"] == {"algorithm": "dual"}
    # The helper deliberately does NOT own cut_sharing_mode.
    assert "cut_sharing_mode" not in sddp_opts


def test_explicit_values_win() -> None:
    model_opts = {"lp_reduction": False}
    sddp_opts: dict = {
        "aperture_solve_mode": "cold",
        "aperture_chunk_size": 8,
        "backward_solver_options": {"threads": 4},
    }
    apply_iterative_fast_path(model_opts, sddp_opts)
    assert model_opts["lp_reduction"] is False
    assert sddp_opts["aperture_solve_mode"] == "cold"
    assert sddp_opts["aperture_chunk_size"] == 8
    # backward keeps an explicit field but still gains the dual algorithm.
    assert sddp_opts["backward_solver_options"] == {
        "threads": 4,
        "algorithm": "dual",
    }


def test_source_seeds_writer_direct_path() -> None:
    """Writer-direct path seeds defaults from the source planning dicts."""
    model_opts: dict = {}
    sddp_opts: dict = {}
    apply_iterative_fast_path(
        model_opts,
        sddp_opts,
        src_model={"lp_reduction": False},
        src_sddp={"aperture_chunk_size": 4, "aperture_solve_mode": "cold"},
    )
    assert model_opts["lp_reduction"] is False
    assert sddp_opts["aperture_chunk_size"] == 4
    assert sddp_opts["aperture_solve_mode"] == "cold"
