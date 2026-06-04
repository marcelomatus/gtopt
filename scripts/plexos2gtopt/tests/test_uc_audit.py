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
    # In lock-step with parsers._is_hydro_min_max_uc — the names the
    # converter strips from the hard list under hydro-min-mode=soft.
    assert is_hydro_minmax("ANTUCOmin") is True
    assert is_hydro_minmax("MACHICURAlagrampdown") is True
    assert is_hydro_minmax("PANGUEramp") is True
    assert is_hydro_minmax("COLBUNmax") is True
    # Case-sensitive ``_PMax`` and ``caudal_min_diario`` suffixes the
    # earlier naive ``endswith`` heuristic missed (this was the cause
    # of the audit's 2 false-positive B5 entries pre-fix).
    assert is_hydro_minmax("ANTUCO_PMax") is True
    assert is_hydro_minmax("PANGUEcaudal_min_diario") is True
    assert is_hydro_minmax("Diesel_OffTakeDay") is False
    assert is_hydro_minmax("BatMaxCycDay_BAT_X") is False
    assert is_hydro_minmax("Gas_MaxOpDay0_Colbun") is False
    # ``*reserve`` (e.g. ``CANUTILLARreserve``) is a reservoir-energy
    # constraint, NOT a hydro min/max — the converter's regex doesn't
    # strip it from the hard list, so the audit must NOT silence it.
    assert is_hydro_minmax("CANUTILLARreserve") is False


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


# ---------------------------------------------------------------------------
# B2 RHS-scale-mismatch bucket — range-overlap + binding guard
# ---------------------------------------------------------------------------
def _run_audit_buckets(
    tmp_path: Path,
    *,
    name: str,
    pampl_def: str,
    rhs_values: list[float],
    price: float = 5.0,
    binding: float = 4.0,
) -> dict:
    """Build a one-constraint bundle and return ALL audit buckets.

    ``rhs_values`` are the per-block PLEXOS RHS rows (their min/max form
    the PLEXOS value range); ``price``/``binding`` populate the shadow
    price and binding-hours the guards check.
    """
    pampl_dir = tmp_path / "gtopt"
    pampl_dir.mkdir()
    (pampl_dir / "uc_x.pampl").write_text(pampl_def + "\n")
    plan = pampl_dir / "planning.json"
    plan.write_text(json.dumps({"system": {"user_constraint_array": []}}))

    cache = tmp_path / "cache"
    cache.mkdir()
    _write_csv(
        cache / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, name, 70]],
    )
    _write_csv(
        cache / "t_membership.csv",
        ["membership_id", "collection_id", "child_class_id", "child_object_id"],
        [[101, 700, 70, 42]],
    )
    _write_csv(
        cache / "t_key.csv",
        ["key_id", "membership_id", "property_id"],
        [
            [1002, 101, 3073],  # rhs
            [1003, 101, 3072],  # hours_binding
            [1004, 101, 3074],  # price
        ],
    )
    data = [[1002, v] for v in rhs_values]
    data.append([1003, binding])
    data.append([1004, price])
    _write_csv(cache / "t_data_0.csv", ["key_id", "value"], data)

    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=pampl_dir,
            gtopt_json=plan,
            hard_list=None,
        )
    )
    return res.buckets


def _run_b2(
    tmp_path: Path,
    *,
    name: str,
    pampl_def: str,
    rhs_values: list[float],
    price: float = 5.0,
    binding: float = 4.0,
) -> list[dict]:
    """Return only the ``B2_rhs_mismatch`` items for the one-constraint bundle."""
    return _run_audit_buckets(
        tmp_path,
        name=name,
        pampl_def=pampl_def,
        rhs_values=rhs_values,
        price=price,
        binding=binding,
    ).get("B2_rhs_mismatch", [])


def test_b2_flags_true_scale_mismatch(tmp_path: Path) -> None:
    # gtopt floor 137 vs PLEXOS floor 83.3 — a real unit/scale error on a
    # binding constraint → the distinct-value comparison flags it.
    items = _run_b2(
        tmp_path,
        name="ScaleBug",
        pampl_def='constraint ScaleBug: 1 * x("a") >= 137;',
        rhs_values=[83.3, 83.3],
    )
    assert [it["name"] for it in items] == ["ScaleBug"]
    assert items[0]["gtopt_rhs_distinct"] == [137.0]
    assert items[0]["plexos_rhs_distinct"] == [83.3]
    assert items[0]["flattened"] is False


def test_b2_flags_flattened_profile(tmp_path: Path) -> None:
    # PLEXOS carries a per-day-varying floor (7 distinct values) but gtopt
    # emitted a single scalar — the profile→scalar bug class.  The direct
    # comparison flags it (range-overlap silently passed it: the scalar sits
    # inside PLEXOS's band).
    items = _run_b2(
        tmp_path,
        name="FlatBug",
        pampl_def='constraint FlatBug: 1 * x("a") >= 83.30;',
        rhs_values=[83.30, 83.03, 82.75, 82.56, 82.35, 82.41, 82.84],
    )
    assert [it["name"] for it in items] == ["FlatBug"]
    assert items[0]["flattened"] is True
    assert items[0]["gtopt_rhs_distinct"] == [83.30]
    assert len(items[0]["plexos_rhs_distinct"]) == 7


def test_b2_suppresses_le_nolimit_sentinel(tmp_path: Path) -> None:
    # gtopt cap 400 sits INSIDE PLEXOS's [400, 10000] band (10000 is the
    # contingency-off no-limit sentinel) → ranges overlap → no flag, even
    # though PLEXOS binds the row.
    items = _run_b2(
        tmp_path,
        name="SD_x_Foo_Bar",
        pampl_def='constraint SD_x_Foo_Bar: 1 * x("a") <= 400;',
        rhs_values=[400.0, 10000.0],
    )
    assert not items


def test_b2_flags_distinct_rhs_level(tmp_path: Path) -> None:
    # gtopt's lowest RHS level (187.42) differs from PLEXOS's (207) by ~9% —
    # a genuine value difference, not numerical error.  The DIRECT
    # distinct-value comparison flags it (the old range-overlap test silently
    # passed it because the two bands [187.42, 320] / [207, 320] overlapped).
    items = _run_b2(
        tmp_path,
        name="ZoneCap",
        pampl_def=(
            'constraint ZoneCap rhs [320, 232, 187.42, 320]: 1 * x("a") <= 320;'
        ),
        rhs_values=[207.0, 232.0, 320.0],
    )
    assert [it["name"] for it in items] == ["ZoneCap"]
    assert items[0]["gtopt_rhs_distinct"] == [187.42, 232.0, 320.0]
    assert items[0]["plexos_rhs_distinct"] == [207.0, 232.0, 320.0]


def test_b2_suppresses_never_binding(tmp_path: Path) -> None:
    # Disjoint ranges (gtopt -10000 vs PLEXOS 20) but PLEXOS never binds
    # (price = 0, binding = 0) → immaterial, suppressed (PANGUEpriority).
    items = _run_b2(
        tmp_path,
        name="PANGUEpriority",
        pampl_def='constraint PANGUEpriority: 1 * x("a") >= -10000;',
        rhs_values=[20.0, 20.0],
        price=0.0,
        binding=0.0,
    )
    assert not items


def test_b2_skips_battery_shutoff_activity_echo(tmp_path: Path) -> None:
    # GEN_BAT_*/LOAD_BAT_* are charge/discharge shut-off rows gtopt correctly
    # emits as rhs 0.  PLEXOS echoes the battery's moving ACTIVITY back as the
    # row "RHS" (pid 3073 == activity), so a naive compare reads gtopt-fixed-0
    # vs PLEXOS-varying.  B2 must skip the family (same as B9).
    items = _run_b2(
        tmp_path,
        name="GEN_BAT_BOLERO_FV",
        pampl_def='constraint GEN_BAT_BOLERO_FV: 1 * x("a") <= 0;',
        rhs_values=[0.0, 13.537, 46.989, 77.615, 93.977],
    )
    assert not items


def test_b2_skips_gtopt_date_window_sentinel(tmp_path: Path) -> None:
    # gtopt's date-window overlay emits the real limit on active blocks and a
    # no-limit sentinel (100000) on inactive ones; PLEXOS reports only the
    # active value.  The sentinel must be filtered from BOTH sides, else
    # gtopt's [450, 100000] vs PLEXOS [450] is a spurious mismatch.
    items = _run_b2(
        tmp_path,
        name="SDCF_Rx1_norte_a_sur",
        pampl_def='constraint SDCF_Rx1_norte_a_sur rhs [450, 100000]: 1 * x("a") <= 450;',
        rhs_values=[450.0, 450.0],
    )
    assert not items


def test_b6_no_false_positive_from_solution_shadow_price(tmp_path: Path) -> None:
    # PLEXOS reports a non-zero SOLUTION shadow price (its global
    # infeasibility-relaxation dual) on a hard gtopt constraint — but ships no
    # INPUT penalty.  B6 must NOT flag it from the shadow price alone (it is
    # gated on ``input_penalty``, absent from the RES solution cache).
    buckets = _run_audit_buckets(
        tmp_path,
        name="Tx_Jadresic_I",
        pampl_def='constraint Tx_Jadresic_I: 1 * x("a") <= 100;',
        rhs_values=[100.0, 100.0],
        price=320.7,
        binding=11.0,
    )
    assert not buckets.get("B6_soft_in_plexos_hard_in_gtopt", [])
