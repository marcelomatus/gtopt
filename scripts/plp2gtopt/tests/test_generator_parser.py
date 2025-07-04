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
        self.test_dir = tempfile.TemporaryDirectory()  # Will be cleaned up in tearDown
        self.test_path = Path(self.test_dir.name)
        
        # Create valid test file
        self.valid_file = self.test_path / "valid_gen.dat"
        with open(self.valid_file, "w", encoding="utf-8") as f:
            f.write("2\n")  # 2 generators
            f.write("GEN1 BUS1 10.0 100.0\n")
            f.write("GEN2 BUS2 20.0 200.0\n")

        # Create empty test file
        self.empty_file = self.test_path / "empty_gen.dat"
        self.empty_file.touch()

        # Create malformed test file
        self.bad_file = self.test_path / "bad_gen.dat"
        with open(self.bad_file, "w", encoding="utf-8") as f:
            f.write("1\n")  # Says 1 generator but provides invalid entry
            f.write("GEN1 BUS1\n")  # Missing p_min/p_max

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
        self.assertEqual(generators[0]["id"], "GEN1")
        self.assertEqual(generators[0]["bus"], "BUS1")
        self.assertEqual(generators[0]["p_min"], 10.0)
        self.assertEqual(generators[0]["p_max"], 100.0)
        
        self.assertEqual(generators[1]["id"], "GEN2")
        self.assertEqual(generators[1]["bus"], "BUS2")
        self.assertEqual(generators[1]["p_min"], 20.0)
        self.assertEqual(generators[1]["p_max"], 200.0)

    def test_get_generators_by_bus(self):
        """Test getting generators by bus ID."""
        parser = GeneratorParser(self.valid_file)
        parser.parse()
        
        bus1_gens = parser.get_generators_by_bus("BUS1")
        self.assertEqual(len(bus1_gens), 1)
        self.assertEqual(bus1_gens[0]["id"], "GEN1")
        
        bus2_gens = parser.get_generators_by_bus("BUS2")
        self.assertEqual(len(bus2_gens), 1)
        self.assertEqual(bus2_gens[0]["id"], "GEN2")
        
        empty_gens = parser.get_generators_by_bus("BUS3")
        self.assertEqual(len(empty_gens), 0)

    def test_parse_nonexistent_file(self):
        """Test parsing a non-existent file."""
        parser = GeneratorParser(self.test_path / "nonexistent.dat")
        with self.assertRaises(FileNotFoundError):
            parser.parse()

    def test_parse_empty_file(self):
        """Test parsing an empty file."""
        parser = GeneratorParser(self.empty_file)
        with self.assertRaises(IndexError):
            parser.parse()

    def test_parse_malformed_file(self):
        """Test parsing a malformed file."""
        parser = GeneratorParser(self.bad_file)
        with self.assertRaises(ValueError):
            parser.parse()


if __name__ == "__main__":
    unittest.main()
