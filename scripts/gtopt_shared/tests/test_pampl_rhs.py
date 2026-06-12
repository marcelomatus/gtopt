# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``gtopt_shared.pampl_rhs``."""

from __future__ import annotations

import pytest

from gtopt_shared.pampl_rhs import pampl_rhs_vector


def test_recognises_single_row_tb_matrix() -> None:
    assert pampl_rhs_vector([[1.0, 2.0, 3.0]]) == [1.0, 2.0, 3.0]


def test_promotes_ints_to_float() -> None:
    out = pampl_rhs_vector([[1, 2, 3]])
    assert out == [1.0, 2.0, 3.0]
    assert all(isinstance(v, float) for v in out)


@pytest.mark.parametrize(
    "rhs",
    [
        None,
        5.0,
        "stage_schedule",
        [],
        [1.0, 2.0, 3.0],  # one-level scalar list
        [[1.0], [2.0]],  # multi-row (per-stage block matrix)
        [["x", "y"]],  # non-numeric inner row
        [[1.0, "y"]],  # mixed-type inner row
        [[]],  # empty inner row
        {"file": "rhs.parquet"},  # file ref
    ],
)
def test_rejects_non_tb_matrix(rhs: object) -> None:
    assert pampl_rhs_vector(rhs) is None
