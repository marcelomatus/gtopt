# SPDX-License-Identifier: BSD-3-Clause
"""Smoke tests: kernels run fast and report sane magnitudes."""

from gtopt_benchmark.main import _dgemm_gflops, _pyloop_mops, machine_bench


def test_kernels_report_positive() -> None:
    assert _dgemm_gflops(n=128, reps=1) > 0.1
    assert _pyloop_mops(n=100_000) > 0.1


def test_machine_bench_shape() -> None:
    res = machine_bench()
    assert res["dgemm_gflops"] > 0
    assert res["triad_gbps"] > 0
    assert "cplex_kticks_per_s" in res
