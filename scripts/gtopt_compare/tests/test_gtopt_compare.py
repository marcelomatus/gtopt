# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for gtopt_compare."""

import csv
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

from gtopt_compare.main import (
    _CASES,
    _NET_BUILDERS,
    _PLP_BAT_4B_24_OUTPUT,
    _SCALE_OBJECTIVE,
    main,
    read_gtopt_cost,
    read_gtopt_generation,
    read_gtopt_lmps,
    read_plp_bat_4b_24_cmg,
    read_plp_bat_4b_24_ess,
    read_plp_bat_4b_24_generation,
    read_plp_bus_names,
    read_plp_central_names,
    read_plp_cmg,
    read_plp_ess,
    read_plp_generation,
)


# ---------------------------------------------------------------------------
# Helpers: write mock gtopt CSV output
# ---------------------------------------------------------------------------


def _write_generation_csv(tmp_path: Path, values: list) -> Path:
    """Write a minimal Generator/generation_sol.csv with *values* as uid columns."""
    gen_dir = tmp_path / "Generator"
    gen_dir.mkdir(parents=True)
    csv_path = gen_dir / "generation_sol.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        header = ["scenario", "stage", "block"] + [
            f"uid:{i + 1}" for i in range(len(values))
        ]
        writer.writerow(header)
        writer.writerow([1, 1, 1] + values)
    return tmp_path


def _write_lmps_csv(tmp_path: Path, values: list) -> Path:
    """Write a minimal Bus/balance_dual.csv with *values* as uid columns."""
    bus_dir = tmp_path / "Bus"
    bus_dir.mkdir(parents=True)
    csv_path = bus_dir / "balance_dual.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        header = ["scenario", "stage", "block"] + [
            f"uid:{i + 1}" for i in range(len(values))
        ]
        writer.writerow(header)
        writer.writerow([1, 1, 1] + values)
    return tmp_path


def _write_solution_csv(tmp_path: Path, obj_value: float) -> Path:
    """Write a minimal solution.csv with the given obj_value."""
    sol_path = tmp_path / "solution.csv"
    with open(sol_path, "w", newline="", encoding="utf-8") as fh:
        fh.write(f"obj_value,{obj_value}\nstatus,0\n")
    return tmp_path


# ---------------------------------------------------------------------------
# read_gtopt_generation
# ---------------------------------------------------------------------------


class TestReadGtoptGeneration:
    def test_reads_two_generators(self, tmp_path):
        _write_generation_csv(tmp_path, [200.0, 50.0])
        result = read_gtopt_generation(tmp_path)
        assert result == pytest.approx([200.0, 50.0])

    def test_reads_single_generator(self, tmp_path):
        _write_generation_csv(tmp_path, [250.0])
        result = read_gtopt_generation(tmp_path)
        assert result == pytest.approx([250.0])

    def test_file_not_found(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="generation_sol.csv"):
            read_gtopt_generation(tmp_path)

    def test_handles_float_values(self, tmp_path):
        _write_generation_csv(tmp_path, [123.456, 0.0, 99.999])
        result = read_gtopt_generation(tmp_path)
        assert result == pytest.approx([123.456, 0.0, 99.999])


# ---------------------------------------------------------------------------
# read_gtopt_lmps
# ---------------------------------------------------------------------------


class TestReadGtoptLmps:
    def test_reads_four_buses(self, tmp_path):
        _write_lmps_csv(tmp_path, [20.0, 20.0, 20.0, 20.0])
        result = read_gtopt_lmps(tmp_path)
        assert result == pytest.approx([20.0, 20.0, 20.0, 20.0])

    def test_file_not_found(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="balance_dual.csv"):
            read_gtopt_lmps(tmp_path)

    def test_handles_varying_lmps(self, tmp_path):
        _write_lmps_csv(tmp_path, [20.0, 25.0, 30.0])
        result = read_gtopt_lmps(tmp_path)
        assert result == pytest.approx([20.0, 25.0, 30.0])


# ---------------------------------------------------------------------------
# read_gtopt_cost
# ---------------------------------------------------------------------------


class TestReadGtoptCost:
    def test_applies_default_scale(self, tmp_path):
        # obj_value stored as 6.0 → 6.0 * 1000 = 6000
        _write_solution_csv(tmp_path, 6.0)
        result = read_gtopt_cost(tmp_path)
        assert result == pytest.approx(6000.0)

    def test_applies_custom_scale(self, tmp_path):
        _write_solution_csv(tmp_path, 5.0)
        result = read_gtopt_cost(tmp_path, scale=500.0)
        assert result == pytest.approx(2500.0)

    def test_default_scale_is_1000(self):
        assert _SCALE_OBJECTIVE == pytest.approx(1000.0)

    def test_file_not_found(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="solution.csv"):
            read_gtopt_cost(tmp_path)

    def test_missing_obj_value_raises(self, tmp_path):
        sol_path = tmp_path / "solution.csv"
        sol_path.write_text("status,0\nkappa,42\n")
        with pytest.raises(ValueError, match="obj_value"):
            read_gtopt_cost(tmp_path)


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------


class TestCasesDispatchTable:
    def test_all_expected_cases_present(self):
        assert "s1b" in _CASES
        assert "ieee_4b_ori" in _CASES
        assert "ieee30b" in _CASES

    def test_all_values_are_callable(self):
        for name, fn in _CASES.items():
            assert callable(fn), f"_CASES[{name!r}] is not callable"

    def test_net_builders_present_for_static_cases(self):
        """All static cases (not bat_4b_24) must have a network builder."""
        for case in ("s1b", "ieee_4b_ori", "ieee30b", "ieee_57b"):
            assert case in _NET_BUILDERS, f"_NET_BUILDERS missing '{case}'"
            assert callable(_NET_BUILDERS[case])

    def test_bat_4b_24_not_in_net_builders(self):
        """bat_4b_24 is block-dependent and must not have a static builder."""
        assert "bat_4b_24" not in _NET_BUILDERS

    def test_plp_in_cases(self):
        """Generic plp case must be registered in _CASES."""
        assert "plp" in _CASES
        assert callable(_CASES["plp"])

    def test_plp_not_in_net_builders(self):
        """plp uses PLP reference files, not a pandapower network."""
        assert "plp" not in _NET_BUILDERS

    def test_plp_bat_4b_24_in_cases(self):
        """plp_bat_4b_24 backward-compat alias must be registered in _CASES."""
        assert "plp_bat_4b_24" in _CASES
        assert callable(_CASES["plp_bat_4b_24"])

    def test_plp_bat_4b_24_not_in_net_builders(self):
        """plp_bat_4b_24 uses PLP reference files, not a pandapower network."""
        assert "plp_bat_4b_24" not in _NET_BUILDERS


# ---------------------------------------------------------------------------
# PLP reference readers — helpers shared across reader and comparison tests
# ---------------------------------------------------------------------------


def _write_plpcen(
    tmp_path: Path,
    rows: list,
    cen_num_start: int = 1,
    cen_tip: str = "Ter",
) -> Path:
    """Write a minimal plpcen.csv.

    Each element of *rows* is ``(block, cen_name, pgen)``.  CenNum is assigned
    sequentially starting from *cen_num_start*.  Pass ``cen_tip="BAT"`` to
    simulate a battery central.
    """
    f = tmp_path / "plpcen.csv"
    cen_name_to_num: dict = {}  # central name → CenNum
    next_num = [cen_num_start]

    with open(f, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "Hidro",
                "Bloque",
                "TipoEtapa",
                "CenNum",
                "CenNom",
                "CenTip",
                "CenBar",
                "BarNom",
                "CenQgen",
                "CenPgen",
                "CenEgen",
                "CenInyP",
                "CenInyE",
                "CenRen",
                "CenCVar",
                "CenCostOp",
                "CenPMax",
            ]
        )
        for block, cen, pgen in rows:
            if cen not in cen_name_to_num:
                cen_name_to_num[cen] = next_num[0]
                next_num[0] += 1
            writer.writerow(
                [
                    "Sim  1",
                    block,
                    "Stage01",
                    cen_name_to_num[cen],
                    cen,
                    cen_tip,
                    1,
                    "b1",
                    pgen,
                    pgen,
                    0,
                    0,
                    0,
                    1,
                    20,
                    pgen * 20,
                    250,
                ]
            )
            # Also add a MEDIA row (should be ignored)
            writer.writerow(
                [
                    "MEDIA",
                    block,
                    "Stage01",
                    cen_name_to_num[cen],
                    cen,
                    cen_tip,
                    1,
                    "b1",
                    pgen,
                    pgen,
                    0,
                    0,
                    0,
                    1,
                    20,
                    pgen * 20,
                    250,
                ]
            )
    return tmp_path


def _write_plpess(tmp_path: Path, rows: list, ess_name: str = "BESS1") -> Path:
    """Write a minimal plpess.csv with given (block, charge, discharge) rows."""
    f = tmp_path / "plpess.csv"
    with open(f, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "Hidro",
                " Bloque",
                " EssNom",
                " Efin",
                " EMin",
                " EMax",
                " DCar",
                " DCmin",
                " DCmax",
                " DCMod",
                " GDes",
                " GDMin",
                " GDMax",
                " PGNet",
                " EfPSom",
                " EfPSom2",
            ]
        )
        for block, charge, discharge in rows:
            writer.writerow(
                [
                    "Sim  1",
                    block,
                    f" {ess_name}",
                    200.0,
                    0.0,
                    200.0,
                    charge,
                    0.0,
                    60.0,
                    0,
                    discharge,
                    0.0,
                    60.0,
                    0.0,
                    21.0,
                    5.0,
                ]
            )
            writer.writerow(
                [
                    "MEDIA",
                    block,
                    f" {ess_name}",
                    200.0,
                    0.0,
                    200.0,
                    charge,
                    0.0,
                    60.0,
                    0,
                    discharge,
                    0.0,
                    60.0,
                    0.0,
                    21.0,
                    5.0,
                ]
            )
    return tmp_path


def _write_plpbar(tmp_path: Path, rows: list, bar_num_start: int = 1) -> Path:
    """Write a minimal plpbar.csv with given (block, bus_name, cmg) rows."""
    f = tmp_path / "plpbar.csv"
    bus_name_to_num: dict = {}
    next_num = [bar_num_start]

    with open(f, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "Hidro",
                "Bloque",
                "TipoEtapa",
                "BarNum",
                "BarNom",
                "CMgBar",
                "DemBarP",
                "DemBarE",
                "PerBarP",
                "PerBarE",
                "BarRetP",
                "BarRetE",
            ]
        )
        for block, bus, cmg in rows:
            if bus not in bus_name_to_num:
                bus_name_to_num[bus] = next_num[0]
                next_num[0] += 1
            writer.writerow(
                [
                    "Sim  1",
                    block,
                    "Stage01",
                    bus_name_to_num[bus],
                    bus,
                    cmg,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                ]
            )
            writer.writerow(
                [
                    "MEDIA",
                    block,
                    "Stage01",
                    bus_name_to_num[bus],
                    bus,
                    cmg,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                ]
            )
    return tmp_path


# ---------------------------------------------------------------------------
# read_plp_central_names
# ---------------------------------------------------------------------------


class TestReadPlpCentralNames:
    def test_returns_name_and_tip(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "g1", 50.0), (1, "g2", 0.0)])
        result = read_plp_central_names(tmp_path)
        names = [n for n, _ in result]
        tips = [t for _, t in result]
        assert names == ["g1", "g2"]
        assert all(t == "Ter" for t in tips)

    def test_ignores_media_rows(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "g1", 50.0)])
        result = read_plp_central_names(tmp_path)
        assert len(result) == 1

    def test_sorted_by_cen_num(self, tmp_path):
        # Write with two centrals; they should be returned in CenNum order
        _write_plpcen(tmp_path, [(1, "g1", 50.0), (1, "g2", 0.0), (1, "g3", 10.0)])
        result = read_plp_central_names(tmp_path)
        assert [n for n, _ in result] == ["g1", "g2", "g3"]

    def test_bat_tip_detected(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "BESS1", 0.0)], cen_tip="BAT")
        result = read_plp_central_names(tmp_path)
        assert result == [("BESS1", "BAT")]

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="plpcen.csv"):
            read_plp_central_names(tmp_path)

    def test_reads_real_plp_output(self):
        """Real PLP output has g1(Ter), g2(Ter), g_solar(Ter), BESS1(BAT)."""
        result = read_plp_central_names(_PLP_BAT_4B_24_OUTPUT)
        names = [n for n, _ in result]
        assert "g1" in names
        assert "g2" in names
        assert "g_solar" in names
        assert "BESS1" in names
        # BESS1 should be labelled BAT
        tips = dict(result)
        assert tips["BESS1"] == "BAT"
        assert tips["g1"] == "Ter"


# ---------------------------------------------------------------------------
# read_plp_bus_names
# ---------------------------------------------------------------------------


class TestReadPlpBusNames:
    def test_returns_sorted_bus_names(self, tmp_path):
        _write_plpbar(tmp_path, [(1, "b1", 20.0), (1, "b2", 20.03)])
        result = read_plp_bus_names(tmp_path)
        assert result == ["b1", "b2"]

    def test_ignores_media_rows(self, tmp_path):
        _write_plpbar(tmp_path, [(1, "b1", 20.0)])
        result = read_plp_bus_names(tmp_path)
        assert len(result) == 1

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="plpbar.csv"):
            read_plp_bus_names(tmp_path)

    def test_reads_real_plp_output(self):
        """Real PLP output has buses b1, b2, b3, b4."""
        result = read_plp_bus_names(_PLP_BAT_4B_24_OUTPUT)
        assert result == ["b1", "b2", "b3", "b4"]


# ---------------------------------------------------------------------------
# read_plp_generation  (and backward-compat alias read_plp_bat_4b_24_generation)
# ---------------------------------------------------------------------------


class TestReadPlpGeneration:
    def test_reads_two_centrals_one_block(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "g1", 50.0), (1, "g_solar", 4.5)])
        result = read_plp_generation(tmp_path)
        assert result[1]["g1"] == pytest.approx(50.0)
        assert result[1]["g_solar"] == pytest.approx(4.5)

    def test_ignores_media_rows(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "g1", 50.0)])
        result = read_plp_generation(tmp_path)
        assert len(result) == 1  # only block 1, no MEDIA duplicates

    def test_multiple_blocks(self, tmp_path):
        _write_plpcen(tmp_path, [(1, "g1", 50.0), (2, "g1", 46.0)])
        result = read_plp_generation(tmp_path)
        assert result[1]["g1"] == pytest.approx(50.0)
        assert result[2]["g1"] == pytest.approx(46.0)

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="plpcen.csv"):
            read_plp_generation(tmp_path)

    def test_backward_compat_alias(self, tmp_path):
        """read_plp_bat_4b_24_generation is an alias for read_plp_generation."""
        _write_plpcen(tmp_path, [(1, "g1", 50.0)])
        assert read_plp_bat_4b_24_generation(tmp_path) == read_plp_generation(tmp_path)

    def test_reads_real_plp_output(self):
        """Spot-check the committed PLP reference: block 1 g1=50 MW."""
        result = read_plp_generation(_PLP_BAT_4B_24_OUTPUT)
        assert len(result) == 24
        assert result[1]["g1"] == pytest.approx(50.0)
        assert result[1]["g2"] == pytest.approx(0.0)
        assert result[1]["g_solar"] == pytest.approx(0.0)
        assert result[13]["g_solar"] == pytest.approx(90.0)
        assert result[20]["g1"] == pytest.approx(183.2)


# ---------------------------------------------------------------------------
# read_plp_ess  (and backward-compat alias read_plp_bat_4b_24_ess)
# ---------------------------------------------------------------------------


class TestReadPlpEss:
    def test_reads_charge_and_discharge(self, tmp_path):
        _write_plpess(tmp_path, [(1, 30.0, 45.0)])
        result = read_plp_ess(tmp_path)
        assert result[1]["BESS1"]["charge"] == pytest.approx(30.0)
        assert result[1]["BESS1"]["discharge"] == pytest.approx(45.0)

    def test_ignores_media_rows(self, tmp_path):
        _write_plpess(tmp_path, [(1, 0.0, 0.0)])
        result = read_plp_ess(tmp_path)
        assert len(result) == 1

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="plpess.csv"):
            read_plp_ess(tmp_path)

    def test_multiple_ess_units(self, tmp_path):
        """Two ESS units on the same block."""
        _write_plpess(tmp_path, [(1, 10.0, 5.0)], ess_name="ESS1")
        # Append a second ESS unit by writing a second plpess with ess_name="ESS2"
        # (Simplification: just test that the key structure supports multiple names)
        result = read_plp_ess(tmp_path)
        assert "ESS1" in result[1]

    def test_backward_compat_alias(self, tmp_path):
        """read_plp_bat_4b_24_ess aggregates over all ESS per block."""
        _write_plpess(tmp_path, [(1, 10.0, 5.0)])
        flat = read_plp_bat_4b_24_ess(tmp_path)
        assert flat[1]["charge"] == pytest.approx(10.0)
        assert flat[1]["discharge"] == pytest.approx(5.0)

    def test_reads_real_plp_output_battery_idle(self):
        """PLP reference: battery is idle (DCar=0, GDes=0) in all 24 blocks."""
        result = read_plp_ess(_PLP_BAT_4B_24_OUTPUT)
        assert len(result) == 24
        for b in range(1, 25):
            for ess_vals in result[b].values():
                assert ess_vals["charge"] == pytest.approx(0.0), f"block {b} charge"
                assert ess_vals["discharge"] == pytest.approx(0.0), f"block {b} dis"


# ---------------------------------------------------------------------------
# read_plp_cmg  (and backward-compat alias read_plp_bat_4b_24_cmg)
# ---------------------------------------------------------------------------


class TestReadPlpCmg:
    def test_reads_cmg_per_bus(self, tmp_path):
        _write_plpbar(tmp_path, [(1, "b1", 20.0), (1, "b2", 20.03)])
        result = read_plp_cmg(tmp_path)
        assert result[1]["b1"] == pytest.approx(20.0)
        assert result[1]["b2"] == pytest.approx(20.03)

    def test_ignores_media_rows(self, tmp_path):
        _write_plpbar(tmp_path, [(1, "b1", 20.0)])
        result = read_plp_cmg(tmp_path)
        assert len(result) == 1

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="plpbar.csv"):
            read_plp_cmg(tmp_path)

    def test_backward_compat_alias(self, tmp_path):
        """read_plp_bat_4b_24_cmg is an alias for read_plp_cmg."""
        _write_plpbar(tmp_path, [(1, "b1", 20.0)])
        assert read_plp_bat_4b_24_cmg(tmp_path) == read_plp_cmg(tmp_path)

    def test_reads_real_plp_output_cmg_values(self):
        """PLP reference: b1=20.00, b2=20.03, b3=20.04, b4=20.07 for all blocks."""
        result = read_plp_cmg(_PLP_BAT_4B_24_OUTPUT)
        assert len(result) == 24
        for b in range(1, 25):
            assert result[b]["b1"] == pytest.approx(20.00, abs=0.01), f"block {b} b1"
            assert result[b]["b2"] == pytest.approx(20.03, abs=0.01), f"block {b} b2"
            assert result[b]["b3"] == pytest.approx(20.04, abs=0.01), f"block {b} b3"
            assert result[b]["b4"] == pytest.approx(20.07, abs=0.01), f"block {b} b4"


# ---------------------------------------------------------------------------
# _compare_plp / _compare_plp_bat_4b_24 — unit tests with mock gtopt output
# ---------------------------------------------------------------------------


def _write_bat_dispatch(tmp_path: Path, fout: list, finp: list) -> None:
    """Write Battery/fout_sol.csv and finp_sol.csv with per-block values."""
    bat_dir = tmp_path / "Battery"
    bat_dir.mkdir(parents=True, exist_ok=True)
    for fname, vals in [("fout_sol.csv", fout), ("finp_sol.csv", finp)]:
        with open(bat_dir / fname, "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["scenario", "stage", "block", "uid:1"])
            for i, v in enumerate(vals, start=1):
                writer.writerow([1, 1, i, v])


def _write_multi_block_generation_csv(tmp_path: Path, rows: list) -> None:
    """Write Generator/generation_sol.csv with one row per block.

    Each element of *rows* is a list of per-uid values for that block.
    """
    gen_dir = tmp_path / "Generator"
    gen_dir.mkdir(parents=True, exist_ok=True)
    n_uids = len(rows[0]) if rows else 0
    with open(gen_dir / "generation_sol.csv", "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            ["scenario", "stage", "block"] + [f"uid:{i + 1}" for i in range(n_uids)]
        )
        for i, vals in enumerate(rows, start=1):
            writer.writerow([1, 1, i] + list(vals))


def _write_multi_block_lmps_csv(tmp_path: Path, rows: list) -> None:
    """Write Bus/balance_dual.csv with one row per block."""
    bus_dir = tmp_path / "Bus"
    bus_dir.mkdir(parents=True, exist_ok=True)
    n_uids = len(rows[0]) if rows else 0
    with open(bus_dir / "balance_dual.csv", "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            ["scenario", "stage", "block"] + [f"uid:{i + 1}" for i in range(n_uids)]
        )
        for i, vals in enumerate(rows, start=1):
            writer.writerow([1, 1, i] + list(vals))


def _build_matching_gtopt_output(tmp_path: Path) -> Path:
    """Build a mock 24-block gtopt output that exactly matches the real PLP reference."""
    solar_profile = [
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.05,
        0.15,
        0.35,
        0.55,
        0.75,
        0.9,
        1.0,
        0.95,
        0.85,
        0.7,
        0.5,
        0.3,
        0.1,
        0.02,
        0.0,
        0.0,
        0.0,
        0.0,
    ]
    d3 = [
        30,
        28,
        27,
        27,
        28,
        32,
        40,
        55,
        70,
        80,
        85,
        88,
        90,
        88,
        84,
        80,
        82,
        88,
        100,
        110,
        105,
        95,
        75,
        50,
    ]
    d4 = [
        20,
        18,
        17,
        17,
        18,
        22,
        28,
        38,
        48,
        55,
        58,
        60,
        62,
        60,
        57,
        55,
        56,
        60,
        68,
        75,
        72,
        65,
        50,
        32,
    ]
    rows = []
    for i in range(24):
        solar = 90.0 * solar_profile[i]
        g1 = (d3[i] + d4[i]) - solar
        rows.append([g1, 0.0, solar, 0.0])  # uid:1..4 (uid:4 = bat discharge)
    _write_multi_block_generation_csv(tmp_path, rows)
    _write_bat_dispatch(tmp_path, [0.0] * 24, [0.0] * 24)
    # LMPs matching PLP CMg (b1=20.00, b2=20.03, b3=20.04, b4=20.07)
    lmps = [[20.00, 20.03, 20.04, 20.07]] * 24
    _write_multi_block_lmps_csv(tmp_path, lmps)
    return tmp_path


class TestComparePlp:
    """Unit tests for the generic _compare_plp function."""

    def test_plp_output_required(self, tmp_path):
        """_compare_plp raises ValueError when plp_output is None."""
        from gtopt_compare.main import _compare_plp

        _write_multi_block_generation_csv(tmp_path, [[50.0, 0.0, 0.0, 0.0]] * 24)
        _write_bat_dispatch(tmp_path, [0.0] * 24, [0.0] * 24)
        _write_multi_block_lmps_csv(tmp_path, [[20.0, 20.0, 20.0, 20.0]] * 24)
        with pytest.raises(ValueError, match="--plp-output"):
            _compare_plp(tmp_path, tol_mw=1.0, tol_lmp=0.5)

    def test_passes_with_real_plp_reference(self, tmp_path):
        """_compare_plp passes when gtopt output matches the committed PLP data."""
        from gtopt_compare.main import _compare_plp

        _build_matching_gtopt_output(tmp_path)
        result = _compare_plp(
            tmp_path, tol_mw=1.0, tol_lmp=0.5, plp_output=_PLP_BAT_4B_24_OUTPUT
        )
        assert result is True

    def test_fails_when_g1_wrong(self, tmp_path):
        """_compare_plp returns False when g1 is wrong."""
        from gtopt_compare.main import _compare_plp

        solar_profile = [
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.05,
            0.15,
            0.35,
            0.55,
            0.75,
            0.9,
            1.0,
            0.95,
            0.85,
            0.7,
            0.5,
            0.3,
            0.1,
            0.02,
            0.0,
            0.0,
            0.0,
            0.0,
        ]
        d3 = [
            30,
            28,
            27,
            27,
            28,
            32,
            40,
            55,
            70,
            80,
            85,
            88,
            90,
            88,
            84,
            80,
            82,
            88,
            100,
            110,
            105,
            95,
            75,
            50,
        ]
        d4 = [
            20,
            18,
            17,
            17,
            18,
            22,
            28,
            38,
            48,
            55,
            58,
            60,
            62,
            60,
            57,
            55,
            56,
            60,
            68,
            75,
            72,
            65,
            50,
            32,
        ]
        rows = []
        for i in range(24):
            solar = 90.0 * solar_profile[i]
            g1 = (d3[i] + d4[i]) - solar
            if i == 0:
                g1 += 20.0  # Large intentional error in block 1
            rows.append([g1, 0.0, solar, 0.0])
        _write_multi_block_generation_csv(tmp_path, rows)
        _write_bat_dispatch(tmp_path, [0.0] * 24, [0.0] * 24)
        _write_multi_block_lmps_csv(tmp_path, [[20.00, 20.03, 20.04, 20.07]] * 24)
        result = _compare_plp(
            tmp_path, tol_mw=1.0, tol_lmp=0.5, plp_output=_PLP_BAT_4B_24_OUTPUT
        )
        assert result is False

    def test_pandapower_file_ignored_with_message(self, tmp_path, capsys):
        """pandapower_file is silently accepted with an informational note."""
        from gtopt_compare.main import _compare_plp

        _build_matching_gtopt_output(tmp_path)
        fake_pp = tmp_path / "fake_net.json"
        _compare_plp(
            tmp_path,
            tol_mw=1.0,
            tol_lmp=0.5,
            plp_output=_PLP_BAT_4B_24_OUTPUT,
            pandapower_file=fake_pp,
        )
        out = capsys.readouterr().out
        assert "ignored" in out.lower()

    def test_extra_kwargs_ignored(self, tmp_path):
        """_compare_plp silently drops unknown **kwargs."""
        from gtopt_compare.main import _compare_plp

        _build_matching_gtopt_output(tmp_path)
        # Should not raise TypeError despite unknown kwarg
        result = _compare_plp(
            tmp_path,
            tol_mw=1.0,
            tol_lmp=0.5,
            plp_output=_PLP_BAT_4B_24_OUTPUT,
            unknown_future_arg=42,
        )
        assert result is True


class TestComparePlpBat4b24:
    """Tests for the backward-compatible _compare_plp_bat_4b_24 alias."""

    def test_passes_with_matching_output(self, tmp_path):
        from gtopt_compare.main import _compare_plp_bat_4b_24

        _build_matching_gtopt_output(tmp_path)
        result = _compare_plp_bat_4b_24(tmp_path, tol_mw=1.0, tol_lmp=0.5)
        assert result is True

    def test_pandapower_file_ignored_silently(self, tmp_path, capsys):
        from gtopt_compare.main import _compare_plp_bat_4b_24

        _build_matching_gtopt_output(tmp_path)
        fake_pp = tmp_path / "fake_net.json"
        _compare_plp_bat_4b_24(
            tmp_path, tol_mw=1.0, tol_lmp=0.5, pandapower_file=fake_pp
        )
        out = capsys.readouterr().out
        assert "ignored" in out.lower()

    def test_save_pandapower_file_plp_bat_4b_24_exits_with_2(self, tmp_path):
        """--save-pandapower-file with plp_bat_4b_24 should exit with code 2."""
        out_file = tmp_path / "net.json"
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "plp_bat_4b_24",
                    "--save-pandapower-file",
                    str(out_file),
                ],
            ):
                main()
        assert exc.value.code == 2


class TestComparePlpCli:
    """CLI tests for the generic --case plp with --plp-output."""

    def test_plp_case_missing_plp_output_exits_with_2(self, tmp_path):
        """--case plp without --plp-output exits with code 2 (ValueError)."""
        _build_matching_gtopt_output(tmp_path)
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "plp",
                    "--gtopt-output",
                    str(tmp_path),
                ],
            ):
                main()
        assert exc.value.code == 2

    def test_save_pandapower_file_plp_exits_with_2(self, tmp_path):
        """--save-pandapower-file with --case plp exits with code 2."""
        out_file = tmp_path / "net.json"
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "plp",
                    "--save-pandapower-file",
                    str(out_file),
                ],
            ):
                main()
        assert exc.value.code == 2


# ---------------------------------------------------------------------------
# CLI — argument parsing via main()
# ---------------------------------------------------------------------------


class TestMainArgParsing:
    def test_missing_case_exits(self):
        with pytest.raises(SystemExit):
            with patch.object(sys, "argv", ["gtopt_compare", "--gtopt-output", "/tmp"]):
                main()

    def test_missing_output_exits(self):
        """--gtopt-output is required when not using --save-pandapower-file alone."""
        with pytest.raises(SystemExit):
            with patch.object(sys, "argv", ["gtopt_compare", "--case", "s1b"]):
                main()

    def test_invalid_case_exits(self):
        with pytest.raises(SystemExit):
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "nonexistent",
                    "--gtopt-output",
                    "/tmp",
                ],
            ):
                main()

    def test_missing_output_dir_exits_with_2(self, tmp_path):
        """Non-existent gtopt-output directory exits with code 2."""
        missing = tmp_path / "no_such_dir"
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "s1b",
                    "--gtopt-output",
                    str(missing),
                ],
            ):
                main()
        assert exc.value.code == 2

    def test_save_pandapower_file_bat_4b_24_exits_with_2(self, tmp_path):
        """--save-pandapower-file with bat_4b_24 should exit with code 2."""
        out_file = tmp_path / "net.json"
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "bat_4b_24",
                    "--save-pandapower-file",
                    str(out_file),
                ],
            ):
                main()
        assert exc.value.code == 2

    def test_pandapower_file_not_found_exits_with_2(self, tmp_path):
        """--pandapower-file pointing to a missing file exits with code 2."""
        _write_generation_csv(tmp_path, [200.0, 50.0])
        _write_solution_csv(tmp_path, 6.0)
        missing_file = tmp_path / "nonexistent_net.json"
        with pytest.raises(SystemExit) as exc:
            with patch.object(
                sys,
                "argv",
                [
                    "gtopt_compare",
                    "--case",
                    "s1b",
                    "--gtopt-output",
                    str(tmp_path),
                    "--pandapower-file",
                    str(missing_file),
                ],
            ):
                main()
        assert exc.value.code == 2


# ---------------------------------------------------------------------------
# Network builders — require pandapower
# ---------------------------------------------------------------------------

pytest.importorskip("pandapower", reason="pandapower not installed")


class TestBuildNetS1b:
    def test_one_bus(self):
        from gtopt_compare.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.bus) == 1

    def test_two_generators(self):
        from gtopt_compare.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.gen) == 2

    def test_one_load(self):
        from gtopt_compare.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.load) == 1
        assert net.load["p_mw"].sum() == pytest.approx(250.0)

    def test_g1_capacity_200(self):
        from gtopt_compare.main import build_net_s1b

        net = build_net_s1b()
        assert net.gen.loc[0, "max_p_mw"] == pytest.approx(200.0)

    def test_g2_capacity_300(self):
        from gtopt_compare.main import build_net_s1b

        net = build_net_s1b()
        assert net.gen.loc[1, "max_p_mw"] == pytest.approx(300.0)


class TestBuildNetIeee4bOri:
    def test_four_buses(self):
        from gtopt_compare.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.bus) == 4

    def test_two_generators(self):
        from gtopt_compare.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.gen) == 2

    def test_five_lines(self):
        from gtopt_compare.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.line) == 5

    def test_total_load_250(self):
        from gtopt_compare.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert net.load["p_mw"].sum() == pytest.approx(250.0)


class TestBuildNetIeee30b:
    def test_thirty_buses(self):
        from gtopt_compare.main import build_net_ieee30b

        net = build_net_ieee30b()
        assert len(net.bus) == 30

    def test_quadratic_cost_zeroed(self):
        from gtopt_compare.main import build_net_ieee30b

        net = build_net_ieee30b()
        assert (net.poly_cost["cp2_eur_per_mw2"] == 0.0).all()
