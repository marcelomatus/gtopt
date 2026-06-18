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
    discover_gtopt_lp,
    is_hydro_minmax,
    is_provision_only_bess_reserve_zone,
    load_hard_list,
    parse_json_ucs,
    parse_lp_native_constraints,
    parse_pampl_file,
    primary_reserve_zone_for_bess,
    reserve_zone_requirement_series,
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
    assert not res.missing_from_gtopt
    assert not res.synthetic_in_gtopt
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


# ---------------------------------------------------------------------------
# B11 native-constraint LP parsing + RHS comparison
# ---------------------------------------------------------------------------
def _write_json(path: Path, reserve_zones: list[dict]) -> None:
    path.write_text(
        json.dumps(
            {
                "system": {
                    "user_constraint_array": [],
                    "reserve_zone_array": reserve_zones,
                }
            }
        )
    )


# A multi-line CPLEX-style LP snippet: a reserve-zone DOWN requirement row
# (uid 5 = CTF_LW), a commitment PMAX bound row (genname ``antuco``), and a
# generation row whose ``+`` continuation lines exercise the multi-line RHS
# recovery.  RHS sits on the row's final continuation line.
_LP_SNIPPET = """\\Problem name: demo

Minimize
 obj: x
Subject To
 reservezone_drequirement_5_1_1_1: reserveprovision_dprovision_7_1_1_1
                                 + reserveprovision_dprovision_8_1_1_1
                                 >= 94
 reservezone_drequirement_5_1_1_2: reserveprovision_dprovision_7_1_1_2
                                 >= 232
 antuco_pmax_constraint_1126_1_1_1: generator_generation_51_1_1_1
                                 + generator_generation_52_1_1_1
                                 - userconstraint_slack_ANTUCO_PMax_1126_1_1_1
                                 <= 137
 antuco_pmax_constraint_1126_1_1_2: generator_generation_51_1_1_2
                                 <= 137
End
"""


def test_parse_lp_native_constraints(tmp_path: Path) -> None:
    lp = tmp_path / "DATOS.lp"
    lp.write_text(_LP_SNIPPET)
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [{"uid": 5, "name": "CTF_LW"}])

    series = parse_lp_native_constraints(lp, plan)
    # Reserve-zone uid 5 -> name CTF_LW, dir down; two blocks 94, 232 (the
    # ENFORCED LP requirement row; B11 itself uses the JSON urreq/drreq).
    assert series[("reserve_zone", "CTF_LW", "down")] == [94.0, 232.0]
    # Commitment genname antuco, pmax; two blocks both 137 (flattened).
    assert series[("commitment", "antuco", "pmax")] == [137.0, 137.0]


def test_reserve_zone_requirement_series_flattens(tmp_path: Path) -> None:
    plan = tmp_path / "DATOS.json"
    _write_json(
        plan,
        [
            {"uid": 5, "name": "CTF_LW", "drreq": [[94.0], [232.0], [571.0]]},
            {"uid": 6, "name": "CTF_RS", "urreq": [10.0, 20.0]},
        ],
    )
    reqs = reserve_zone_requirement_series(plan)
    # Nested [[...]] schedule flattened into a per-block series.
    assert reqs[("CTF_LW", "down")] == [94.0, 232.0, 571.0]
    assert reqs[("CTF_RS", "up")] == [10.0, 20.0]


def test_discover_gtopt_lp(tmp_path: Path) -> None:
    assert discover_gtopt_lp(tmp_path) is None
    (tmp_path / "DATOS.lp").write_text("x")
    assert discover_gtopt_lp(tmp_path) == tmp_path / "DATOS.lp"


def _native_cache(
    tmp_path: Path,
    *,
    source_name: str,
    rhs_values: list[float],
    price: float,
    binding: float,
) -> Path:
    """A one-constraint PLEXOS cache (the B11 source) under ``tmp_path``."""
    cache = tmp_path / "cache"
    cache.mkdir()
    _write_csv(
        cache / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, source_name, 70]],
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
            [1004, 101, 3074],  # price (solution shadow price)
        ],
    )
    data = [[1002, v] for v in rhs_values]
    data.append([1003, binding])
    data.append([1004, price])
    _write_csv(cache / "t_data_0.csv", ["key_id", "value"], data)
    return cache


def test_b11_flags_flattened_reserve_zone(tmp_path: Path) -> None:
    # gtopt folds the CTF_LW down REQUIREMENT (JSON drreq) to a single 0.0
    # level while PLEXOS's redundant CTF_DownMinProvision carries a 3-level
    # binding profile -> B11 flags the native reserve_zone fold B2 never sees.
    # The compared gtopt value comes from the JSON drreq (the requirement),
    # NOT the enforced .lp row (which would fold in the urmin/drmin floor).
    lp = tmp_path / "DATOS.lp"
    lp.write_text(
        "Subject To\n"
        " reservezone_drequirement_5_1_1_1: rp_1 >= 183\n"
        " reservezone_drequirement_5_1_1_2: rp_2 >= 183\n"
        " reservezone_drequirement_5_1_1_3: rp_3 >= 571\n"
        "End\n"
    )
    plan = tmp_path / "DATOS.json"
    # gtopt requirement (drreq) is a flattened single 0.0 level; the enforced
    # LP row above carries the urmin floor (183) which B11 must NOT compare.
    _write_json(plan, [{"uid": 5, "name": "CTF_LW", "drreq": [0.0, 0.0, 0.0]}])
    cache = _native_cache(
        tmp_path,
        source_name="CTF_DownMinProvision",
        rhs_values=[94.0, 232.0, 571.0],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,  # no uc_*.pampl present
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=lp,
        )
    )
    items = res.buckets.get("B11_native_rhs_mismatch", [])
    assert [it["name"] for it in items] == ["CTF_LW"]
    it = items[0]
    assert it["element_kind"] == "reserve_zone"
    assert it["direction"] == "down"
    assert it["plexos_source"] == "CTF_DownMinProvision"
    assert it["flattened"] is True
    assert it["fixed_vs_variable"] is True
    assert it["gtopt_rhs_distinct"] == [0.0]
    assert it["plexos_rhs_distinct"] == [94.0, 232.0, 571.0]


def test_b11_no_flag_reserve_zone_when_requirement_matches(tmp_path: Path) -> None:
    # The urreq/drreq refinement: gtopt's enforced .lp row shows the urmin
    # FLOOR (183) but the JSON drreq REQUIREMENT exactly matches the PLEXOS
    # source profile [94, 232, 571].  B11 must NOT flag (it compares the
    # requirement, not the enforced floor) -- the CTF/CSF/CPF clear case.
    lp = tmp_path / "DATOS.lp"
    lp.write_text(
        "Subject To\n"
        " reservezone_drequirement_5_1_1_1: rp_1 >= 183\n"
        " reservezone_drequirement_5_1_1_2: rp_2 >= 232\n"
        " reservezone_drequirement_5_1_1_3: rp_3 >= 571\n"
        "End\n"
    )
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [{"uid": 5, "name": "CTF_LW", "drreq": [94.0, 232.0, 571.0]}])
    cache = _native_cache(
        tmp_path,
        source_name="CTF_DownMinProvision",
        rhs_values=[94.0, 232.0, 571.0],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=lp,
        )
    )
    assert res.buckets.get("B11_native_rhs_mismatch", []) == []


def test_b11_flags_flattened_commitment(tmp_path: Path) -> None:
    # gtopt enforces a single PMAX level (137) while the source per-plant
    # ANTUCOmax PLEXOS constraint carries a 3-level binding profile -> B11
    # flags the native commitment bound flatten (read from the .lp row, which
    # is correct for commitments -- no separate floor).
    lp = tmp_path / "DATOS.lp"
    lp.write_text(
        "Subject To\n"
        " antuco_pmax_constraint_1_1_1_1: g_1 <= 137\n"
        " antuco_pmax_constraint_1_1_1_2: g_2 <= 137\n"
        " antuco_pmax_constraint_1_1_1_3: g_3 <= 137\n"
        "End\n"
    )
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [])
    cache = _native_cache(
        tmp_path,
        source_name="ANTUCOmax",
        rhs_values=[85.0, 90.0, 137.0],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=lp,
        )
    )
    items = res.buckets.get("B11_native_rhs_mismatch", [])
    assert [it["name"] for it in items] == ["antuco"]
    it = items[0]
    assert it["element_kind"] == "commitment"
    assert it["direction"] == "pmax"
    assert it["plexos_source"] == "ANTUCOmax"
    assert it["flattened"] is True
    assert it["fixed_vs_variable"] is True
    assert it["gtopt_rhs_distinct"] == [137.0]


def test_b11_no_flag_when_rhs_matches(tmp_path: Path) -> None:
    # gtopt PMAX exactly equals the source RHS (single level both sides) -> no
    # B11 flag (the antuco-matches-ANTUCO_PMax real-data case).
    lp = tmp_path / "DATOS.lp"
    lp.write_text("Subject To\n antuco_pmax_constraint_1_1_1_1: g_1 <= 292.074\nEnd\n")
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [])
    cache = _native_cache(
        tmp_path,
        source_name="ANTUCO_PMax",
        rhs_values=[292.074, 292.074],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=lp,
        )
    )
    assert res.buckets.get("B11_native_rhs_mismatch", []) == []


def test_b11_defers_unmatched_source(tmp_path: Path) -> None:
    # An up-reserve zone whose UpMinProvision source isn't in the cache is
    # DEFERRED ("source not in solution cache"), never guessed into B11.  The
    # zone's urreq requirement is present in the JSON so it IS enumerated.
    lp = tmp_path / "DATOS.lp"
    lp.write_text("Subject To\n reservezone_urequirement_6_1_1_1: rp_1 >= 0\nEnd\n")
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [{"uid": 6, "name": "CTF_RS", "urreq": [10.0]}])
    # Cache has only the DOWN source, not the UP one the row needs.
    cache = _native_cache(
        tmp_path,
        source_name="CTF_DownMinProvision",
        rhs_values=[94.0, 232.0],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=lp,
        )
    )
    assert res.buckets.get("B11_native_rhs_mismatch", []) == []
    assert res.summary["n_native_deferred"] == 1
    assert res.native_deferred[0]["name"] == "CTF_RS"
    assert "not in solution cache" in res.native_deferred[0]["deferred"]


def test_b11_skipped_without_lp(tmp_path: Path) -> None:
    # No --gtopt-lp -> native check skipped gracefully (B11 absent, no error).
    plan = tmp_path / "DATOS.json"
    _write_json(plan, [{"uid": 5, "name": "CTF_LW", "drreq": [94.0, 232.0]}])
    cache = _native_cache(
        tmp_path,
        source_name="CTF_DownMinProvision",
        rhs_values=[94.0, 232.0],
        price=5.0,
        binding=4.0,
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=tmp_path,
            gtopt_json=plan,
            hard_list=None,
            gtopt_lp=None,
        )
    )
    assert "B11_native_rhs_mismatch" not in res.buckets
    assert res.summary["n_native_deferred"] == 0


# ---------------------------------------------------------------------------
# B6 refinement — fires only with a modelled input penalty (pid 4393)
# ---------------------------------------------------------------------------
def test_b6_fires_with_modelled_input_penalty(tmp_path: Path) -> None:
    # When the cache DOES carry a modelled input penalty price (pid 4393),
    # B6 legitimately fires on a row gtopt keeps hard but PLEXOS models soft.
    pampl_dir = tmp_path / "gtopt"
    pampl_dir.mkdir()
    (pampl_dir / "uc_x.pampl").write_text('constraint TrulySoft: 1 * x("a") <= 50;\n')
    plan = pampl_dir / "planning.json"
    plan.write_text(json.dumps({"system": {"user_constraint_array": []}}))

    cache = tmp_path / "cache"
    cache.mkdir()
    _write_csv(
        cache / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, "TrulySoft", 70]],
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
            [1004, 101, 3074],  # solution price
            [1005, 101, 3070],  # slack
            [1006, 101, 4393],  # MODELLED input penalty price
        ],
    )
    _write_csv(
        cache / "t_data_0.csv",
        ["key_id", "value"],
        [
            [1002, 50.0],
            [1003, 10.0],
            [1004, 999.0],
            [1005, 3.0],
            [1006, 1500.0],  # modelled soft penalty -> genuine soft constraint
        ],
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=pampl_dir,
            gtopt_json=plan,
            hard_list=None,
        )
    )
    assert res.summary["input_penalty_present"] is True
    items = res.buckets.get("B6_soft_in_plexos_hard_in_gtopt", [])
    assert [it["name"] for it in items] == ["TrulySoft"]
    assert items[0]["input_penalty_price"] == 1500.0


def test_b6_input_penalty_absent_keeps_b6_empty(tmp_path: Path) -> None:
    # Companion to the existing shadow-price false-positive test: assert the
    # summary flag is False when no modelled input penalty ships.
    pampl_dir = tmp_path / "gtopt"
    pampl_dir.mkdir()
    (pampl_dir / "uc_x.pampl").write_text('constraint SoftLike: 1 * x("a") <= 50;\n')
    plan = pampl_dir / "planning.json"
    plan.write_text(json.dumps({"system": {"user_constraint_array": []}}))

    cache = tmp_path / "cache"
    cache.mkdir()
    _write_csv(
        cache / "t_object.csv",
        ["object_id", "name", "class_id"],
        [[42, "SoftLike", 70]],
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
            [1004, 101, 3074],  # price (solution shadow price > 0)
            [1005, 101, 3070],  # slack > 0
        ],
    )
    _write_csv(
        cache / "t_data_0.csv",
        ["key_id", "value"],
        [
            [1002, 50.0],
            [1003, 10.0],
            [1004, 999.0],
            [1005, 3.0],
        ],
    )
    res = run_audit(
        AuditInputs(
            plexos_cache_dir=cache,
            gtopt_pampl_dir=pampl_dir,
            gtopt_json=plan,
            hard_list=None,
        )
    )
    assert res.summary["input_penalty_present"] is False
    assert res.buckets.get("B6_soft_in_plexos_hard_in_gtopt", []) == []


def test_is_provision_only_bess_reserve_zone() -> None:
    # The six PLEXOS ``*_BESS`` sub-trackers carry a zero requirement and must
    # be DEFERRED by B11, not flagged as a fold against the shared
    # ``*MinProvision`` constraint.
    for z in (
        "CPF_LW_BESS",
        "CPF_RS_BESS",
        "CSF_LW_BESS",
        "CSF_RS_BESS",
        "CTF_LW_BESS",
        "CTF_RS_BESS",
    ):
        assert is_provision_only_bess_reserve_zone(z) is True
    # The primary (non-BESS) twins carry the real requirement — never deferred.
    for z in ("CPF_LW", "CSF_RS", "CTF_LW", "CTF_RS"):
        assert is_provision_only_bess_reserve_zone(z) is False
    # Unrelated names (a BESS-cycle UC, a battery object) are not zones.
    assert is_provision_only_bess_reserve_zone("CPF_BESS_ANGAMOS") is False
    assert is_provision_only_bess_reserve_zone("BAT_TOCOPILLA") is False
    assert is_provision_only_bess_reserve_zone("CTF_LW_BESS_EXTRA") is False


def test_primary_reserve_zone_for_bess() -> None:
    # Each ``*_BESS`` sub-tracker maps to its non-BESS twin (the requirement
    # carrier sharing the ``*MinProvision`` source).
    assert primary_reserve_zone_for_bess("CTF_LW_BESS") == "CTF_LW"
    assert primary_reserve_zone_for_bess("CSF_RS_BESS") == "CSF_RS"
    assert primary_reserve_zone_for_bess("CPF_LW_BESS") == "CPF_LW"
    assert primary_reserve_zone_for_bess("CTF_LW") is None
    assert primary_reserve_zone_for_bess("CPF_BESS_ANGAMOS") is None
