"""Unit tests for :mod:`plexos2gtopt.uc_audit`.

Cover parsing (PAMPL, planning JSON), classifier helpers, hard-list
loader, the minimal PLEXOS solution loader and an end-to-end
:func:`run_audit` invocation on a hand-built bundle.
"""

from __future__ import annotations

import json
from pathlib import Path

from plexos2gtopt.uc_audit import (
    AuditInputs,
    build_plexos_solution,
    categorise,
    is_hydro_minmax,
    load_hard_list,
    parse_json_ucs,
    parse_pampl_file,
    run_audit,
)


def test_parse_pampl_file_basic(tmp_path: Path) -> None:
    p = tmp_path / "uc_x.pampl"
    p.write_text(
        "param soft_op_penalty = 10;\n"
        "# a comment line\n"
        'constraint HardA: 1 * x("A") <= 5;\n'
        'constraint SoftB penalty soft_op_penalty: 1 * x("B") <= 7;\n'
        'inactive constraint InactC: 1 * x("C") <= 3;\n'
        'constraint VecD rhs [1.0, 2.0, 3.0]: 1 * x("D") <= 0;\n'
    )
    rows = parse_pampl_file(p)
    assert len(rows) == 4
    by_name = {r["name"]: r for r in rows}
    assert by_name["HardA"]["active"] is True
    assert by_name["HardA"]["penalty_value"] == 0.0
    assert by_name["SoftB"]["penalty_ident"] == "soft_op_penalty"
    assert by_name["SoftB"]["penalty_value"] == 10.0
    assert by_name["InactC"]["active"] is False
    assert by_name["InactC"]["penalty_value"] == 0.0
    assert by_name["VecD"]["rhs_profile"] == [1.0, 2.0, 3.0]
    assert by_name["VecD"]["penalty_value"] == 0.0


def test_parse_json_ucs_basic(tmp_path: Path) -> None:
    p = tmp_path / "planning.json"
    p.write_text(
        json.dumps(
            {
                "system": {
                    "user_constraint_array": [
                        {
                            "name": "UC_Active",
                            "expression": '1 * x("A") <= 5',
                            "active": True,
                            "penalty": 0,
                        },
                        {
                            "name": "UC_Inactive",
                            "expression": '1 * x("B") <= 7',
                            "active": False,
                            "penalty": 100,
                        },
                    ],
                }
            }
        )
    )
    rows = parse_json_ucs(p)
    assert len(rows) == 2
    by_name = {r["name"]: r for r in rows}
    assert by_name["UC_Active"]["active"] is True
    assert by_name["UC_Active"]["op"] == "<="
    assert by_name["UC_Active"]["rhs_scalar"] == 5.0
    assert by_name["UC_Inactive"]["active"] is False
    assert by_name["UC_Inactive"]["penalty_value"] == 100


def test_categorise_known_families() -> None:
    assert categorise("BatMaxCycDay_BAT_X") == "battery_cycle"
    assert categorise("Gas_MaxOpDay0_Colbun") == "gas_maxopday"
    assert categorise("FueMaxOffWeek_X") == "fuel_offtake_week"
    assert categorise("FOO_starting") == "commit_startup"
    assert categorise("XYZ") == "other"


def test_is_hydro_minmax() -> None:
    assert is_hydro_minmax("ANTUCOmin") is True
    assert is_hydro_minmax("MACHICURAlagrampdown") is True
    assert is_hydro_minmax("PANGUEramp") is True
    assert is_hydro_minmax("COLBUNmax") is True
    assert is_hydro_minmax("ANTUCOreserve") is True
    assert is_hydro_minmax("Diesel_OffTakeDay") is False
    assert is_hydro_minmax("BatMaxCycDay_BAT_X") is False
    assert is_hydro_minmax("Gas_MaxOpDay0_Colbun") is False


def test_load_hard_list_parses_annotations(tmp_path: Path) -> None:
    p = tmp_path / "hard.txt"
    p.write_text("Foo_HARD # HrsBind=10\n# comment line\nBar_HARD\n\n")
    assert load_hard_list(p) == {"Foo_HARD", "Bar_HARD"}


def _write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    lines = [",".join(header)]
    for r in rows:
        lines.append(",".join(str(c) for c in r))
    path.write_text("\n".join(lines) + "\n")


def test_build_plexos_solution_minimal(tmp_path: Path) -> None:
    _write_csv(
        tmp_path / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, "MyConstraint", 70]],
    )
    _write_csv(
        tmp_path / "t_membership.csv",
        [
            "membership_id",
            "collection_id",
            "child_class_id",
            "child_object_id",
        ],
        [[101, 700, 70, 42]],
    )
    _write_csv(
        tmp_path / "t_key.csv",
        ["key_id", "membership_id", "property_id"],
        [
            [1001, 101, 3069],  # activity
            [1002, 101, 3073],  # rhs
            [1003, 101, 3072],  # hours_binding
        ],
    )
    _write_csv(
        tmp_path / "t_data_0.csv",
        ["key_id", "value"],
        [
            [1001, 1.5],
            [1001, 2.5],
            [1002, 100.0],
            [1003, 4.0],
        ],
    )
    out = build_plexos_solution(tmp_path)
    assert "MyConstraint" in out
    rec = out["MyConstraint"]
    assert rec["activity_n"] == 2
    assert rec["activity_sum"] == 4.0
    assert rec["rhs_max"] == 100.0
    assert rec["hours_binding_sum"] == 4.0
    assert rec["plexos_active"] is True


def test_run_audit_end_to_end_minimal(tmp_path: Path) -> None:
    pampl_dir = tmp_path / "gtopt"
    pampl_dir.mkdir()
    (pampl_dir / "uc_x.pampl").write_text('constraint Shared: 1 * x("a") <= 50;\n')
    plan = pampl_dir / "planning.json"
    plan.write_text(json.dumps({"system": {"user_constraint_array": []}}))

    cache = tmp_path / "cache"
    cache.mkdir()
    _write_csv(
        cache / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, "Shared", 70]],
    )
    _write_csv(
        cache / "t_membership.csv",
        ["membership_id", "collection_id", "child_class_id", "child_object_id"],
        [[101, 700, 70, 42]],
    )
    _write_csv(
        cache / "t_key.csv",
        ["key_id", "membership_id", "property_id"],
        [[1001, 101, 3073]],
    )
    _write_csv(
        cache / "t_data_0.csv",
        ["key_id", "value"],
        [[1001, 50.0]],
    )

    inputs = AuditInputs(
        plexos_cache_dir=cache,
        gtopt_pampl_dir=pampl_dir,
        gtopt_json=plan,
        hard_list=None,
    )
    res = run_audit(inputs)
    assert res.intersection == ["Shared"]
    assert res.missing_from_gtopt == []
    assert res.synthetic_in_gtopt == []
    assert res.summary["n_plexos"] == 1
    assert res.summary["n_gtopt_total"] == 1
    assert res.summary["n_intersection"] == 1
