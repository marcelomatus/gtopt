# SPDX-License-Identifier: BSD-3-Clause
"""The FCF end-of-horizon cost-to-go is provided SOLELY by gtopt's native
boundary-cut loader (``boundary_cuts.csv`` + ``monolithic_options.
boundary_cuts_file``).  plexos2gtopt must therefore NEVER emit either:

  * the ``alpha_fcf`` α-column as a DecisionVariable (direct JSON), or
  * the ``FCF_future_cost`` boundary cut as a UserConstraint (JSON *or*
    ``.pampl`` / AMPL),

because doing so double-counts the cost-to-go (two α columns, two cuts, a
2× terminal-storage gradient) and leaves a free LP column.  These tests
pin the filters in ``build_decision_variable_array`` /
``build_user_constraint_array`` / ``write_user_constraint_pampl``.
"""

from __future__ import annotations

from plexos2gtopt.entities import DecisionVariableSpec, UserConstraintSpec
from plexos2gtopt.gtopt_writer import (
    build_decision_variable_array,
    build_user_constraint_array,
    write_user_constraint_pampl,
)


def test_alpha_fcf_decision_variable_is_never_emitted() -> None:
    dvs = (
        DecisionVariableSpec(name="CPF_LW_VoRS", cost=10000.0),
        DecisionVariableSpec(name="alpha_fcf", cost=1000.0),
        DecisionVariableSpec(name="BESS_RS", lower_bound=0.0),
    )
    out = build_decision_variable_array(dvs)
    names = [d["name"] for d in out]
    assert "alpha_fcf" not in names
    # The other DVs are kept and contiguously renumbered (no uid gap).
    assert names == ["CPF_LW_VoRS", "BESS_RS"]
    assert [d["uid"] for d in out] == [1, 2]


def test_alpha_fcf_only_yields_empty_decision_variable_array() -> None:
    out = build_decision_variable_array((DecisionVariableSpec(name="alpha_fcf"),))
    assert not out


def test_fcf_future_cost_user_constraint_is_never_emitted_to_json() -> None:
    ucs = (
        UserConstraintSpec(name="GasBudget", expression="x <= 5"),
        UserConstraintSpec(
            name="FCF_future_cost",
            expression='1000 * decision_variable("alpha_fcf").value >= 1',
        ),
    )
    out = build_user_constraint_array(ucs)
    names = [u["name"] for u in out]
    assert "FCF_future_cost" not in names
    assert names == ["GasBudget"]


def test_fcf_future_cost_is_never_written_to_pampl(tmp_path) -> None:
    # The PAMPL writer must drop the cut even if it reaches this path
    # directly (alternate call site).  After filtering, the FCF row is gone
    # from both the per-family ``.pampl`` files and the inline JSON
    # remainder.
    uc_array = [
        {
            "uid": 1,
            "name": "FCF_future_cost",
            "expression": (
                '1000 * decision_variable("alpha_fcf").value '
                '+ 8565.0 * reservoir("L_Maule").efin >= 1.0'
            ),
        },
    ]
    files, json_remaining = write_user_constraint_pampl(uc_array, tmp_path)
    body = "".join((tmp_path / f).read_text(encoding="utf-8") for f in files)
    assert "FCF_future_cost" not in body
    assert "alpha_fcf" not in body
    assert all(u.get("name") != "FCF_future_cost" for u in json_remaining)
