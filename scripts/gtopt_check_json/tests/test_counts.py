# SPDX-License-Identifier: BSD-3-Clause
"""Tests for compute_counts (element + user-constraint counts)."""

from pathlib import Path

from gtopt_check_json._info import (
    SystemCounts,
    compute_counts,
    _build_counts_sections,
)


def test_inline_user_constraints() -> None:
    """Inline UCs are counted, classified by family, and split active/inactive."""
    planning = {
        "system": {
            "bus_array": [{}, {}],
            "generator_array": [
                {"type": "thermal"},
                {"type": "thermal"},
                {"type": "solar"},
            ],
            "fuel_array": [{}, {}],
            "commitment_array": [{}],
            "decision_variable_array": [{"name": "alpha_fcf"}],
            "flow_right_array": [{}],
            "user_constraint_array": [
                {"name": "CC1_Uniq"},
                {"name": "Gas_MaxOp_X"},
                {"name": "SD_line1", "active": False},
                {"name": "RandomFloor"},
            ],
        },
        "simulation": {
            "block_array": [{} for _ in range(24)],
            "stage_array": [{}],
            "scenario_array": [{}],
        },
        "options": {},
    }
    c = compute_counts(planning)

    assert c.user_constraints_total == 4
    assert c.user_constraints_active == 3
    assert c.user_constraints_inactive == 1
    assert c.uc_inline == 4
    assert c.uc_in_pampl == 0
    assert c.uc_by_family == {
        "config_exclusivity": 1,
        "gas_offtake": 1,
        "security": 1,
        "operational": 1,
    }

    # Element counts taken straight from the planning arrays.
    assert c.buses == 2
    assert c.generators == 3
    assert c.gen_by_type == {"thermal": 2, "solar": 1}
    assert c.fuels == 2
    assert c.commitments == 1
    assert c.decision_variables == 1
    assert c.flow_rights == 1

    # Simulation dimensions.
    assert c.num_blocks == 24
    assert c.num_stages == 1
    assert c.num_scenarios == 1


def test_pampl_mode_and_boundary_cut(tmp_path: Path) -> None:
    """Modular .pampl UCs and a boundary-cut CSV are parsed from base_dir."""
    (tmp_path / "uc_gas_offtake.pampl").write_text(
        "constraint Gas_MaxOp_A: x <= 5;\nconstraint Gas_MaxOp_B: y <= 3;\n",
        encoding="utf-8",
    )
    (tmp_path / "uc_security.pampl").write_text(
        "inactive constraint SD_1: z <= 1;\n",
        encoding="utf-8",
    )
    (tmp_path / "boundary_cuts.csv").write_text(
        "scene,rhs,ResA,ResB\n0,1.0,2.0,3.0\n",
        encoding="utf-8",
    )

    planning = {
        "system": {
            "user_constraint_files": [
                "uc_gas_offtake.pampl",
                "uc_security.pampl",
            ]
        },
        "options": {"monolithic_options": {"boundary_cuts_file": "boundary_cuts.csv"}},
        "simulation": {},
    }
    c = compute_counts(planning, base_dir=str(tmp_path))

    assert c.user_constraints_total == 3
    assert c.uc_in_pampl == 3
    assert c.uc_inline == 0
    assert c.user_constraints_inactive == 1
    assert c.user_constraints_active == 2
    assert c.uc_by_family == {"gas_offtake": 2, "security": 1}

    # Boundary cut CSV: one data row, two slope (reservoir) columns.
    assert c.boundary_cuts == 1
    assert c.boundary_state_variables == 2


def test_build_counts_sections(tmp_path: Path) -> None:
    """_build_counts_sections emits PLEXOS + UC sections with per-family rows."""
    (tmp_path / "uc_gas_offtake.pampl").write_text(
        "constraint Gas_MaxOp_A: x <= 5;\nconstraint Gas_MaxOp_B: y <= 3;\n",
        encoding="utf-8",
    )
    (tmp_path / "uc_security.pampl").write_text(
        "inactive constraint SD_1: z <= 1;\n",
        encoding="utf-8",
    )
    (tmp_path / "boundary_cuts.csv").write_text(
        "scene,rhs,ResA,ResB\n0,1.0,2.0,3.0\n",
        encoding="utf-8",
    )

    planning = {
        "system": {
            "user_constraint_files": [
                "uc_gas_offtake.pampl",
                "uc_security.pampl",
            ]
        },
        "options": {"monolithic_options": {"boundary_cuts_file": "boundary_cuts.csv"}},
        "simulation": {},
    }
    counts = compute_counts(planning, base_dir=str(tmp_path))
    sections = _build_counts_sections(counts)

    titles = {title for title, _ in sections}
    assert "PLEXOS Elements" in titles
    assert "User Constraints" in titles

    uc_pairs = next(pairs for title, pairs in sections if title == "User Constraints")
    assert ("Total", "3") in uc_pairs
    assert ("  gas_offtake", "2") in uc_pairs


def test_empty_planning() -> None:
    """An empty planning yields zero UCs and no info sections."""
    c = compute_counts({"system": {}, "simulation": {}, "options": {}})

    assert isinstance(c, SystemCounts)
    assert c.user_constraints_total == 0
    assert not _build_counts_sections(c)
