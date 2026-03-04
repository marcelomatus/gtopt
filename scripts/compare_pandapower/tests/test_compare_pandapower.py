# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for compare_pandapower."""

import csv
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

from compare_pandapower.main import (
    _CASES,
    _NET_BUILDERS,
    _SCALE_OBJECTIVE,
    main,
    read_gtopt_cost,
    read_gtopt_generation,
    read_gtopt_lmps,
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


# ---------------------------------------------------------------------------
# CLI — argument parsing via main()
# ---------------------------------------------------------------------------


class TestMainArgParsing:
    def test_missing_case_exits(self):
        with pytest.raises(SystemExit):
            with patch.object(
                sys, "argv", ["compare_pandapower", "--gtopt-output", "/tmp"]
            ):
                main()

    def test_missing_output_exits(self):
        """--gtopt-output is required when not using --save-pandapower-file alone."""
        with pytest.raises(SystemExit):
            with patch.object(sys, "argv", ["compare_pandapower", "--case", "s1b"]):
                main()

    def test_invalid_case_exits(self):
        with pytest.raises(SystemExit):
            with patch.object(
                sys,
                "argv",
                [
                    "compare_pandapower",
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
                    "compare_pandapower",
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
                    "compare_pandapower",
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
                    "compare_pandapower",
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
        from compare_pandapower.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.bus) == 1

    def test_two_generators(self):
        from compare_pandapower.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.gen) == 2

    def test_one_load(self):
        from compare_pandapower.main import build_net_s1b

        net = build_net_s1b()
        assert len(net.load) == 1
        assert net.load["p_mw"].sum() == pytest.approx(250.0)

    def test_g1_capacity_200(self):
        from compare_pandapower.main import build_net_s1b

        net = build_net_s1b()
        assert net.gen.loc[0, "max_p_mw"] == pytest.approx(200.0)

    def test_g2_capacity_300(self):
        from compare_pandapower.main import build_net_s1b

        net = build_net_s1b()
        assert net.gen.loc[1, "max_p_mw"] == pytest.approx(300.0)


class TestBuildNetIeee4bOri:
    def test_four_buses(self):
        from compare_pandapower.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.bus) == 4

    def test_two_generators(self):
        from compare_pandapower.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.gen) == 2

    def test_five_lines(self):
        from compare_pandapower.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert len(net.line) == 5

    def test_total_load_250(self):
        from compare_pandapower.main import build_net_ieee_4b_ori

        net = build_net_ieee_4b_ori()
        assert net.load["p_mw"].sum() == pytest.approx(250.0)


class TestBuildNetIeee30b:
    def test_thirty_buses(self):
        from compare_pandapower.main import build_net_ieee30b

        net = build_net_ieee30b()
        assert len(net.bus) == 30

    def test_quadratic_cost_zeroed(self):
        from compare_pandapower.main import build_net_ieee30b

        net = build_net_ieee30b()
        assert (net.poly_cost["cp2_eur_per_mw2"] == 0.0).all()
