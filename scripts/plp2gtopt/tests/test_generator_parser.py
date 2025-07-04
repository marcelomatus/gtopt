#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for generator_parser.py"""

import tempfile
import unittest
from pathlib import Path

from ..generator_parser import GeneratorParser


class TestGeneratorParser(unittest.TestCase):
    """Test cases for GeneratorParser class."""

    def setUp(self):
        """Create temporary test files."""
        # Context manager would prevent cleanup in tearDown
        self.test_dir = tempfile.TemporaryDirectory()  # pylint: disable=consider-using-with
        self.test_path = Path(self.test_dir.name)
        
        # Create valid test file in plpcnfce.dat format
        self.valid_file = self.test_path / "valid_gen.dat"
        with open(self.valid_file, "w", encoding="utf-8") as f:
            f.write("# Test generator file\n")
            f.write("    1 'TEST_GEN1'                                       1    F       F       F       F           F          0           0\n")
            f.write("          PotMin PotMax VertMin VertMax\n")
            f.write("           010.0  100.0   000.0   000.0\n")
            f.write("          CosVar  Rendi  Barra Genera Vertim\n")
            f.write("             5.0  1.000      1      0      0\n")
            f.write("    2 'TEST_GEN2'                                       1    F       F       F       F           F          0           0\n")
            f.write("          PotMin PotMax VertMin VertMax\n")
            f.write("           020.0  200.0   000.0   000.0\n")
            f.write("          CosVar  Rendi  Barra Genera Vertim\n")
            f.write("            10.0  0.900      2      0      0\n")

        # Create empty test file
        self.empty_file = self.test_path / "empty_gen.dat"
        self.empty_file.touch()

        # Create malformed test file
        self.bad_file = self.test_path / "bad_gen.dat"
        with open(self.bad_file, "w", encoding="utf-8") as f:
            f.write("    1 'BAD_GEN'                                       1    F       F       F       F           F          0           0\n")
            f.write("          PotMin PotMax VertMin VertMax\n")
            f.write("           010.0\n")  # Incomplete line

    def tearDown(self):
        """Clean up temporary files."""
        self.test_dir.cleanup()

    def test_parse_valid_file(self):
        """Test parsing a valid generator file."""
        parser = GeneratorParser(self.valid_file)
        parser.parse()
        self.assertEqual(parser.get_num_generators(), 2)
        generators = parser.get_generators()
        self.assertEqual(len(generators), 2)
        
        # Test first generator
        gen1 = generators[0]
        self.assertEqual(gen1["id"], "1")
        self.assertEqual(gen1["name"], "TEST_GEN1")
        self.assertEqual(gen1["bus"], "1")
        self.assertEqual(gen1["p_min"], 10.0)
        self.assertEqual(gen1["p_max"], 100.0)
        self.assertEqual(gen1["variable_cost"], 5.0)
        self.assertEqual(gen1["efficiency"], 1.0)
        self.assertEqual(gen1["is_battery"], False)
        
        # Test second generator
        gen2 = generators[1]
        self.assertEqual(gen2["id"], "2")
        self.assertEqual(gen2["name"], "TEST_GEN2")
        self.assertEqual(gen2["bus"], "2")
        self.assertEqual(gen2["p_min"], 20.0)
        self.assertEqual(gen2["p_max"], 200.0)
        self.assertEqual(gen2["variable_cost"], 10.0)
        self.assertEqual(gen2["efficiency"], 0.9)
        self.assertEqual(gen2["is_battery"], False)

    def test_get_generators_by_bus(self):
        """Test getting generators by bus ID."""
        parser = GeneratorParser(self.valid_file)
        parser.parse()
        bus1_gens = parser.get_generators_by_bus("1")
        self.assertEqual(len(bus1_gens), 1)
        self.assertEqual(bus1_gens[0]["id"], "1")
        bus2_gens = parser.get_generators_by_bus("2")
        self.assertEqual(len(bus2_gens), 1)
        self.assertEqual(bus2_gens[0]["id"], "2")
        empty_gens = parser.get_generators_by_bus("999")
        self.assertEqual(len(empty_gens), 0)

    def test_parse_nonexistent_file(self):
        """Test parsing a non-existent file."""
        parser = GeneratorParser(self.test_path / "nonexistent.dat")
        with self.assertRaises(FileNotFoundError):
            parser.parse()

    def test_parse_empty_file(self):
        """Test parsing an empty file."""
        parser = GeneratorParser(self.empty_file)
        parser.parse()  # Should not raise for empty file
        self.assertEqual(parser.get_num_generators(), 0)

    def test_parse_malformed_file(self):
        """Test parsing a malformed file."""
        parser = GeneratorParser(self.bad_file)
        with self.assertRaises(ValueError):
            parser.parse()


if __name__ == "__main__":
    unittest.main()
