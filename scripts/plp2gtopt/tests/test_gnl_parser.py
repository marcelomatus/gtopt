# -*- coding: utf-8 -*-

"""Tests for the GnlParser (plpcnfgnl.dat reader)."""

import pytest

from plp2gtopt.gnl_parser import GnlParser


def _write_gnl_file(tmp_path, content):
    """Write content to a plpcnfgnl.dat file and return the path."""
    path = tmp_path / "plpcnfgnl.dat"
    path.write_text(content, encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# Minimal valid plpcnfgnl.dat content
# ---------------------------------------------------------------------------
_MINIMAL_GNL = """\
# plpcnfgnl.dat - GNL terminal configuration
# header line 2
1
# Id  Name  VMax  Vini  CGnl  CVer  CReg  CAlm  GnlRen
1  GNL_Quintero  150000  80000  5.0  100.0  2.5  0.01  0.90
# NumLinkedGenerators
2
# GeneratorName  Efficiency
NEHUENCO_1  0.40
NEHUENCO_2  0.38
# NumDeliveryEntries
3
# StageIndex  DeliveryVolume
1  50000
3  50000
5  30000
"""

_TWO_TERMINALS = """\
# GNL configuration with two terminals
# header
2
# terminal 1
1  GNL_Quintero  150000  80000  5.0  100.0  2.5  0.01  0.90
# generators
1
NEHUENCO_1  0.40
# deliveries
2
1  50000
2  60000
# terminal 2
2  GNL_Mejillones  100000  40000  6.0  120.0  3.0  0.02  0.85
# generators
0
# deliveries
1
1  30000
"""


class TestGnlParserBasic:
    """Basic parsing of plpcnfgnl.dat format."""

    def test_single_terminal(self, tmp_path):
        path = _write_gnl_file(tmp_path, _MINIMAL_GNL)
        parser = GnlParser(path)
        parser.parse()

        assert parser.num_terminals == 1
        t = parser.terminals[0]
        assert t["name"] == "GNL_Quintero"
        assert t["number"] == 1
        assert t["vmax"] == 150000.0
        assert t["vini"] == 80000.0
        assert t["cgnl"] == 5.0
        assert t["cver"] == 100.0
        assert t["creg"] == 2.5
        assert t["calm"] == 0.01
        assert t["gnlren"] == 0.90

    def test_generators(self, tmp_path):
        path = _write_gnl_file(tmp_path, _MINIMAL_GNL)
        parser = GnlParser(path)
        parser.parse()

        gens = parser.terminals[0]["generators"]
        assert len(gens) == 2
        assert gens[0] == {"name": "NEHUENCO_1", "efficiency": 0.40}
        assert gens[1] == {"name": "NEHUENCO_2", "efficiency": 0.38}

    def test_deliveries(self, tmp_path):
        path = _write_gnl_file(tmp_path, _MINIMAL_GNL)
        parser = GnlParser(path)
        parser.parse()

        deliveries = parser.terminals[0]["deliveries"]
        assert len(deliveries) == 3
        assert deliveries[0] == {"stage": 1, "volume": 50000.0}
        assert deliveries[1] == {"stage": 3, "volume": 50000.0}
        assert deliveries[2] == {"stage": 5, "volume": 30000.0}

    def test_two_terminals(self, tmp_path):
        path = _write_gnl_file(tmp_path, _TWO_TERMINALS)
        parser = GnlParser(path)
        parser.parse()

        assert parser.num_terminals == 2
        assert parser.terminals[0]["name"] == "GNL_Quintero"
        assert parser.terminals[1]["name"] == "GNL_Mejillones"

        # Second terminal: no generators, one delivery
        assert len(parser.terminals[1]["generators"]) == 0
        assert len(parser.terminals[1]["deliveries"]) == 1

    def test_lookup_by_name(self, tmp_path):
        path = _write_gnl_file(tmp_path, _TWO_TERMINALS)
        parser = GnlParser(path)
        parser.parse()

        t = parser.get_terminal_by_name("GNL_Mejillones")
        assert t is not None
        assert t["vmax"] == 100000.0

    def test_lookup_by_number(self, tmp_path):
        path = _write_gnl_file(tmp_path, _TWO_TERMINALS)
        parser = GnlParser(path)
        parser.parse()

        t = parser.get_terminal_by_number(2)
        assert t is not None
        assert t["name"] == "GNL_Mejillones"

    def test_config_property(self, tmp_path):
        path = _write_gnl_file(tmp_path, _MINIMAL_GNL)
        parser = GnlParser(path)
        parser.parse()

        cfg = parser.config
        assert "terminals" in cfg
        assert len(cfg["terminals"]) == 1
        assert cfg["terminals"][0]["name"] == "GNL_Quintero"


class TestGnlParserErrors:
    """Error handling for malformed plpcnfgnl.dat files."""

    def test_empty_file(self, tmp_path):
        path = _write_gnl_file(tmp_path, "# only comments\n")
        parser = GnlParser(path)
        with pytest.raises(ValueError, match="empty or malformed"):
            parser.parse()

    def test_missing_file(self, tmp_path):
        parser = GnlParser(tmp_path / "nonexistent.dat")
        with pytest.raises(FileNotFoundError):
            parser.parse()

    def test_truncated_terminal_header(self, tmp_path):
        content = "1\n1  GNL_Short  150000\n"
        path = _write_gnl_file(tmp_path, content)
        parser = GnlParser(path)
        with pytest.raises(ValueError, match="Expected 9 fields"):
            parser.parse()

    def test_truncated_generator_line(self, tmp_path):
        content = """\
1
1  GNL_T  150000  80000  5.0  100.0  2.5  0.01  0.90
1
NEHUENCO_ONLY
"""
        path = _write_gnl_file(tmp_path, content)
        parser = GnlParser(path)
        with pytest.raises(ValueError, match="Expected 2 fields"):
            parser.parse()

    def test_truncated_delivery_line(self, tmp_path):
        content = """\
1
1  GNL_T  150000  80000  5.0  100.0  2.5  0.01  0.90
0
1
5
"""
        path = _write_gnl_file(tmp_path, content)
        parser = GnlParser(path)
        with pytest.raises(ValueError, match="Expected 2 fields"):
            parser.parse()
