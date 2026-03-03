# SPDX-License-Identifier: BSD-3-Clause
"""Unit and integration tests for pp2gtopt.convert."""

import argparse
import copy
import json
import math
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

import pp2gtopt.convert as _convert_mod
from pp2gtopt.convert import (
    _TMAX_UNLIMITED,
    _build_demands,
    _build_ext_grid_gen,
    _build_physical_lines,
    _build_transformers,
    _get_poly_cost,
    _line_tmax,
    convert,
    get_bus_base_kv,
    load_network,
    ohm_to_pu,
)
from pp2gtopt.main import main, make_parser


# ---------------------------------------------------------------------------
# Unit tests — pure functions (no pandapower required)
# ---------------------------------------------------------------------------


class TestOhmToPu:
    def test_basic_conversion(self):
        # z_base = 100^2 / 100 = 100 Ω
        assert ohm_to_pu(50.0, 100.0) == pytest.approx(0.5)

    def test_different_base_mva(self):
        # z_base = 10^2 / 50 = 2 Ω → 4 Ω / 2 = 2 p.u.
        assert ohm_to_pu(4.0, 10.0, base_mva=50.0) == pytest.approx(2.0)

    def test_zero_impedance(self):
        assert ohm_to_pu(0.0, 100.0) == pytest.approx(0.0)

    def test_scales_with_kv_squared(self):
        # Doubling kV quadruples z_base → halves p.u. value
        pu_100 = ohm_to_pu(10.0, 100.0)
        pu_200 = ohm_to_pu(10.0, 200.0)
        assert pu_100 == pytest.approx(4 * pu_200)


class TestLineTmax:
    def test_infinite_input_returns_sentinel(self):
        assert _line_tmax(float("inf"), 132.0) == _TMAX_UNLIMITED

    def test_large_value_returns_sentinel(self):
        # Exactly at the sentinel boundary
        assert _line_tmax(_TMAX_UNLIMITED, 132.0) == _TMAX_UNLIMITED

    def test_above_sentinel_returns_sentinel(self):
        assert _line_tmax(_TMAX_UNLIMITED + 1, 132.0) == _TMAX_UNLIMITED

    def test_limited_line_formula(self):
        max_i_ka, base_kv = 0.5, 132.0
        expected = round(max_i_ka * base_kv * math.sqrt(3), 1)
        assert _line_tmax(max_i_ka, base_kv) == pytest.approx(expected)

    def test_result_rounded_to_one_decimal(self):
        result = _line_tmax(0.123456, 110.0)
        assert result == round(result, 1)


# ---------------------------------------------------------------------------
# Integration tests — require pandapower
# ---------------------------------------------------------------------------

pytest.importorskip("pandapower", reason="pandapower not installed")
# pylint: disable=wrong-import-position,wrong-import-order
import pandapower as pp  # noqa: E402
import pandapower.networks as pn  # noqa: E402
# pylint: enable=wrong-import-position,wrong-import-order

@pytest.fixture(scope="module")
def net():
    """Load the IEEE 30-bus network once per test module."""
    return pn.case_ieee30()


@pytest.fixture(scope="module")
def ieee30b_json(tmp_path_factory):
    """Run convert() once and return the parsed JSON dict."""
    out = tmp_path_factory.mktemp("pp2gtopt") / "ieee30b.json"
    convert(out)
    with open(out, encoding="utf-8") as fh:
        return json.load(fh)


class TestGetBusBaseKv:
    def test_returns_float(self, net):
        kv = get_bus_base_kv(net, 0)
        assert isinstance(kv, float)
        assert kv > 0

    def test_bus0_is_132kv(self, net):
        assert get_bus_base_kv(net, 0) == pytest.approx(132.0)


class TestGetPolyCost:
    def test_ext_grid_returns_20(self, net):
        assert _get_poly_cost(net, "ext_grid", 0, default=99.0) == pytest.approx(20.0)

    def test_missing_element_returns_default(self, net):
        assert _get_poly_cost(net, "gen", 9999, default=42.5) == pytest.approx(42.5)

    def test_known_gen_cost(self, net):
        # Generator index 0 in ieee30b has a defined cost; it must exceed zero
        cost = _get_poly_cost(net, "gen", 0, default=0.0)
        assert cost > 0


class TestBuildExtGridGen:
    def test_uid_is_one(self, net):
        assert _build_ext_grid_gen(net)["uid"] == 1

    def test_name_is_g1(self, net):
        assert _build_ext_grid_gen(net)["name"] == "g1"

    def test_bus_is_one(self, net):
        assert _build_ext_grid_gen(net)["bus"] == 1

    def test_pmin_is_zero(self, net):
        assert _build_ext_grid_gen(net)["pmin"] == 0

    def test_capacity_equals_pmax(self, net):
        gen = _build_ext_grid_gen(net)
        assert gen["capacity"] == pytest.approx(gen["pmax"])

    def test_gcost_is_20(self, net):
        assert _build_ext_grid_gen(net)["gcost"] == pytest.approx(20.0)


class TestBuildDemands:
    def test_skips_zero_load(self, net):
        net_copy = copy.copy(net)
        net_copy.load = net.load.copy()
        net_copy.load.at[net_copy.load.index[0], "p_mw"] = 0.0
        demands = _build_demands(net_copy)
        assert len(demands) == len(_build_demands(net)) - 1

    def test_uids_are_sequential_after_skip(self, net):
        net_copy = copy.copy(net)
        net_copy.load = net.load.copy()
        net_copy.load.at[net_copy.load.index[0], "p_mw"] = 0.0
        demands = _build_demands(net_copy)
        assert [d["uid"] for d in demands] == list(range(1, len(demands) + 1))


class TestBuildTransformers:
    def test_count_ieee30b(self, net):
        trafos = _build_transformers(net, 100.0)
        assert len(trafos) == 7

    def test_reactance_formula(self, net):
        row = net.trafo.iloc[0]
        expected = round((row["vk_percent"] / 100.0) * (100.0 / row["sn_mva"]), 6)
        trafos = _build_transformers(net, 100.0)
        assert trafos[0]["reactance"] == pytest.approx(expected)

    def test_tmax_is_unlimited(self, net):
        for trafo in _build_transformers(net, 100.0):
            assert trafo["tmax_ab"] == _TMAX_UNLIMITED
            assert trafo["tmax_ba"] == _TMAX_UNLIMITED

    def test_no_uid_field(self, net):
        # UIDs are assigned by _build_lines, not _build_transformers
        for trafo in _build_transformers(net, 100.0):
            assert "uid" not in trafo


class TestBuildPhysicalLines:
    def test_count_ieee30b(self, net):
        lines = _build_physical_lines(net, 100.0)
        assert len(lines) == 34

    def test_no_uid_field(self, net):
        # UIDs are assigned by _build_lines, not _build_physical_lines
        for line in _build_physical_lines(net, 100.0):
            assert "uid" not in line

    def test_skips_degenerate_line(self, net):
        net_copy = copy.copy(net)
        net_copy.line = net.line.copy()
        net_copy.line.at[net_copy.line.index[0], "x_ohm_per_km"] = 0.0
        reduced = _build_physical_lines(net_copy, 100.0)
        assert len(reduced) == len(_build_physical_lines(net, 100.0)) - 1


class TestConvertStructure:
    def test_top_level_keys(self, ieee30b_json):
        assert set(ieee30b_json) == {"options", "simulation", "system"}

    def test_options(self, ieee30b_json):
        opts = ieee30b_json["options"]
        assert opts["use_kirchhoff"] is True
        assert opts["use_single_bus"] is False
        assert opts["output_compression"] == "uncompressed"
        assert opts["scale_objective"] == 1000
        assert opts["demand_fail_cost"] == 1000

    def test_simulation_arrays(self, ieee30b_json):
        sim = ieee30b_json["simulation"]
        assert len(sim["block_array"]) == 1
        assert len(sim["stage_array"]) == 1
        assert len(sim["scenario_array"]) == 1

    def test_system_name(self, ieee30b_json):
        assert ieee30b_json["system"]["name"] == "ieee30b"


class TestConvertBuses:
    def test_count(self, ieee30b_json):
        buses = ieee30b_json["system"]["bus_array"]
        assert len(buses) == 30

    def test_uids_sequential(self, ieee30b_json):
        uids = [b["uid"] for b in ieee30b_json["system"]["bus_array"]]
        assert uids == list(range(1, 31))

    def test_slack_bus_has_reference_theta(self, ieee30b_json):
        bus1 = next(b for b in ieee30b_json["system"]["bus_array"] if b["uid"] == 1)
        assert "reference_theta" in bus1
        assert bus1["reference_theta"] == 0

    def test_non_slack_buses_no_reference_theta(self, ieee30b_json):
        non_slack = [b for b in ieee30b_json["system"]["bus_array"] if b["uid"] != 1]
        for bus in non_slack:
            assert "reference_theta" not in bus


class TestConvertGenerators:
    def test_count(self, ieee30b_json):
        gens = ieee30b_json["system"]["generator_array"]
        # ext_grid + 5 PV generators = 6
        assert len(gens) == 6

    def test_g1_is_slack(self, ieee30b_json):
        g1 = ieee30b_json["system"]["generator_array"][0]
        assert g1["uid"] == 1
        assert g1["bus"] == 1  # slack bus

    def test_all_have_required_fields(self, ieee30b_json):
        for gen in ieee30b_json["system"]["generator_array"]:
            assert "uid" in gen
            assert "bus" in gen
            assert "pmin" in gen
            assert "pmax" in gen
            assert "gcost" in gen
            assert gen["pmin"] >= 0
            assert gen["pmax"] > 0
            assert gen["gcost"] > 0

    def test_g1_gcost_is_20(self, ieee30b_json):
        g1 = ieee30b_json["system"]["generator_array"][0]
        assert g1["gcost"] == pytest.approx(20.0)


class TestConvertDemands:
    def test_count(self, ieee30b_json):
        demands = ieee30b_json["system"]["demand_array"]
        # ieee30b has 21 loads with p_mw > 0
        assert len(demands) == 21

    def test_all_have_lmax(self, ieee30b_json):
        for dem in ieee30b_json["system"]["demand_array"]:
            assert "lmax" in dem
            assert isinstance(dem["lmax"], list)
            assert dem["lmax"][0][0] > 0


class TestConvertLines:
    def test_minimum_count(self, ieee30b_json):
        lines = ieee30b_json["system"]["line_array"]
        # 34 physical lines + 4 transformers = 38, but some may be skipped
        assert len(lines) >= 38

    def test_all_have_voltage_10(self, ieee30b_json):
        for line in ieee30b_json["system"]["line_array"]:
            assert line.get("voltage") == 10

    def test_all_have_reactance(self, ieee30b_json):
        for line in ieee30b_json["system"]["line_array"]:
            assert "reactance" in line
            assert line["reactance"] > 0

    def test_all_have_tmax(self, ieee30b_json):
        for line in ieee30b_json["system"]["line_array"]:
            assert "tmax_ab" in line
            assert "tmax_ba" in line
            assert line["tmax_ab"] > 0
            assert line["tmax_ba"] > 0

    def test_uids_sequential(self, ieee30b_json):
        lines = ieee30b_json["system"]["line_array"]
        uids = [ln["uid"] for ln in lines]
        assert uids == list(range(1, len(lines) + 1))


class TestConvertOutputFile:
    def test_writes_to_specified_path(self, tmp_path):
        out = tmp_path / "test_out.json"
        convert(out)
        assert out.exists()
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert "system" in data

    def test_default_path_in_package_dir(self, monkeypatch, tmp_path):
        """convert(None) writes ieee30b.json next to convert.py."""
        monkeypatch.setattr(_convert_mod, "__file__", str(tmp_path / "convert.py"))
        convert(None)
        assert (tmp_path / "ieee30b.json").exists()

    def test_custom_name_in_default_path(self, monkeypatch, tmp_path):
        """convert(None, name='mynet') writes mynet.json next to convert.py."""
        monkeypatch.setattr(_convert_mod, "__file__", str(tmp_path / "convert.py"))
        convert(None, name="mynet")
        assert (tmp_path / "mynet.json").exists()

    def test_custom_name_in_json(self, tmp_path):
        """convert(path, name='custom') embeds the given name in system.name."""
        out = tmp_path / "custom.json"
        convert(out, name="custom")
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert data["system"]["name"] == "custom"


# ---------------------------------------------------------------------------
# main.py — CLI argument parsing and entry point
# ---------------------------------------------------------------------------


class TestMakeParser:
    def test_returns_parser(self):
        assert isinstance(make_parser(), argparse.ArgumentParser)

    def test_default_network_is_none(self):
        # -n is in a mutually exclusive group; no default set → None
        args = make_parser().parse_args([])
        assert args.network is None

    def test_default_file_is_none(self):
        args = make_parser().parse_args([])
        assert args.file is None

    def test_default_output_is_none(self):
        args = make_parser().parse_args([])
        assert args.output is None

    def test_network_flag(self):
        args = make_parser().parse_args(["-n", "case14"])
        assert args.network == "case14"

    def test_file_flag(self, tmp_path):
        f = tmp_path / "net.json"
        f.touch()
        args = make_parser().parse_args(["-f", str(f)])
        assert args.file == f

    def test_file_and_network_mutually_exclusive(self, tmp_path):
        f = tmp_path / "net.json"
        f.touch()
        with pytest.raises(SystemExit):
            make_parser().parse_args(["-f", str(f), "-n", "case14"])

    def test_output_flag(self):
        args = make_parser().parse_args(["-o", "/tmp/out.json"])
        assert args.output == Path("/tmp/out.json")

    def test_version_exits(self):
        with pytest.raises(SystemExit) as exc:
            make_parser().parse_args(["--version"])
        assert exc.value.code == 0

    def test_invalid_network_exits(self):
        with pytest.raises(SystemExit):
            make_parser().parse_args(["-n", "nonexistent_network"])


class TestMain:
    def test_main_version(self, capsys):
        """main() --version prints version string and exits."""
        with pytest.raises(SystemExit) as exc:
            with patch.object(sys, "argv", ["pp2gtopt", "--version"]):
                main()
        assert exc.value.code == 0
        assert "pp2gtopt" in capsys.readouterr().out

    def test_main_list_networks(self, capsys):
        """main() --list-networks prints network names and exits."""
        with pytest.raises(SystemExit) as exc:
            with patch.object(sys, "argv", ["pp2gtopt", "--list-networks"]):
                main()
        assert exc.value.code == 0
        assert "ieee30b" in capsys.readouterr().out

    def test_main_default_conversion(self, tmp_path):
        """main() with no source flag defaults to ieee30b."""
        out = tmp_path / "out.json"
        with patch.object(sys, "argv", ["pp2gtopt", "-o", str(out)]):
            main()
        assert out.exists()
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert data["system"]["name"] == "ieee30b"
        assert len(data["system"]["bus_array"]) == 30

    def test_main_network_case14(self, tmp_path):
        """main() -n case14 writes a 14-bus JSON."""
        out = tmp_path / "case14.json"
        with patch.object(sys, "argv", ["pp2gtopt", "-n", "case14", "-o", str(out)]):
            main()
        assert out.exists()
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert data["system"]["name"] == "case14"
        assert len(data["system"]["bus_array"]) == 14

    def test_main_file_json(self, tmp_path):
        """main() -f <json_file> loads a saved pandapower JSON and converts it."""

        net = pn.case9()
        src = tmp_path / "case9.json"
        pp.to_json(net, str(src))

        out = tmp_path / "case9_gtopt.json"
        with patch.object(sys, "argv", ["pp2gtopt", "-f", str(src), "-o", str(out)]):
            main()

        assert out.exists()
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert data["system"]["name"] == "case9"
        assert len(data["system"]["bus_array"]) == 9


# ---------------------------------------------------------------------------
# load_network() — file-format detection
# ---------------------------------------------------------------------------


class TestLoadNetwork:
    def test_load_json(self, tmp_path):
        """load_network() reads a pandapower JSON file."""

        net_orig = pn.case9()
        src = tmp_path / "case9.json"
        pp.to_json(net_orig, str(src))

        net = load_network(src)
        assert len(net.bus) == 9

    def test_load_excel(self, tmp_path):
        """load_network() reads a pandapower Excel file."""
        pytest.importorskip("xlsxwriter", reason="xlsxwriter not installed")
        net_orig = pn.case9()
        src = tmp_path / "case9.xlsx"
        pp.to_excel(net_orig, str(src))

        net = load_network(src)
        assert len(net.bus) == 9

    def test_file_not_found(self, tmp_path):
        """load_network() raises FileNotFoundError for missing files."""
        with pytest.raises(FileNotFoundError, match="Network file not found"):
            load_network(tmp_path / "nonexistent.json")

    def test_unsupported_extension(self, tmp_path):
        """load_network() raises ValueError for unknown extensions."""
        f = tmp_path / "net.csv"
        f.write_text("dummy")
        with pytest.raises(ValueError, match="Unsupported file format"):
            load_network(f)

    def test_name_derived_from_stem(self, tmp_path):
        """convert() called with a file-loaded net uses the file stem as name."""

        net_orig = pn.case14()
        src = tmp_path / "mynetwork.json"
        pp.to_json(net_orig, str(src))

        net = load_network(src)
        out = tmp_path / "mynetwork_gtopt.json"
        convert(out, net=net, name=src.stem)
        with open(out, encoding="utf-8") as fh:
            data = json.load(fh)
        assert data["system"]["name"] == "mynetwork"
