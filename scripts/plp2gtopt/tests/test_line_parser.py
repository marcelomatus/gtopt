#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for line_parser.py"""

import tempfile
import unittest
from pathlib import Path

from ..line_parser import LineParser


class TestLineParser(unittest.TestCase):
    """Test cases for LineParser class."""

    @classmethod
    def setUpClass(cls):
        """Set up test data for all tests."""
        test_dir = Path(__file__).parent.parent.parent
        cls.test_file = test_dir / "cases/plp_dat_ex/plpcnfli.dat"
        cls.parser = LineParser(cls.test_file)
        cls.parser.parse()

    def test_parse_file(self):
        """Test parsing the line data file."""
        self.assertEqual(self.parser.get_num_lines(), 10)
        lines = self.parser.get_lines()
        self.assertEqual(len(lines), 10)

    def test_line_data(self):
        """Test line data structure."""
        line = self.parser.get_line_by_name("Andes220->Oeste220")
        self.assertIsNotNone(line)
        self.assertEqual(line["bus_a"], "Andes220")
        self.assertEqual(line["bus_b"], "Oeste220")
        self.assertEqual(line["voltage"], 220.0)
        self.assertEqual(line["bus_a_num"], 2)
        self.assertEqual(line["bus_b_num"], 65)
        self.assertAlmostEqual(line["r"], 3.431)
        self.assertAlmostEqual(line["x"], 15.802)
        self.assertTrue(line["has_losses"])
        self.assertEqual(line["num_sections"], 3)
        self.assertTrue(line["is_operational"])

    def test_get_lines_by_bus(self):
        """Test getting lines by bus name."""
        lines = self.parser.get_lines_by_bus("Andes220")
        self.assertEqual(len(lines), 2)
        names = {line["name"] for line in lines}
        self.assertIn("Andes220->Oeste220", names)
        self.assertIn("Andes345->Andes220", names)

    def test_get_lines_by_bus_num(self):
        """Test getting lines by bus number."""
        lines = self.parser.get_lines_by_bus_num(5)
        self.assertEqual(len(lines), 3)
        names = {line["name"] for line in lines}
        self.assertIn("Antofag110->Desalant110", names)
        self.assertIn("Antofag110->LaNegra110", names)
        self.assertIn("Capricornio110->Antofag110", names)

    def test_voltage_parsing(self):
        """Test voltage extraction from line names."""
        line = self.parser.get_line_by_name("Capricorn220->Capricorn110")
        self.assertEqual(line["voltage"], 220.0)
        line = self.parser.get_line_by_name("Antofag110->Desalant110")
        self.assertEqual(line["voltage"], 110.0)

    def test_operational_status(self):
        """Test operational status parsing."""
        lines = self.parser.get_lines()
        for line in lines:
            self.assertTrue(line["is_operational"])


if __name__ == "__main__":
    unittest.main()
