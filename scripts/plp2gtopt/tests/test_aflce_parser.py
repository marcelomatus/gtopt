#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for AflceParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..aflce_parser import AflceParser
from .conftest import get_example_file


@pytest.fixture
def sample_aflce_file():
    """Fixture providing path to sample flow file."""
    return get_example_file("plpaflce.dat")


def test_aflce_parser_initialization():
    """Test AflceParser initialization."""
    test_path = "test.dat"
    parser = AflceParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.flows
    assert parser.num_flows == 0


def test_get_flows(tmp_path):
    """Test get_flows returns properly structured flow data."""
    # Create a temporary test file
    test_file = tmp_path / "test_aflce.dat"
    test_file.write_text(
        """1
'test_central'
2
03 001 1.5 2.0
03 002 1.8 2.2"""
    )

    parser = AflceParser(str(test_file))
    parser.parse()

    flows = parser.flows
    assert len(flows) == 1
    flow = flows[0]

    # Verify structure and types
    assert flow["name"] == "test_central"
    assert isinstance(flow["blocks"], np.ndarray)
    assert isinstance(flow["flows"], np.ndarray)
    assert flow["blocks"].dtype == np.int16
    assert flow["flows"].dtype == np.float64
    assert flow["num_hydrologies"] == 2

    # Verify array contents
    np.testing.assert_array_equal(flow["blocks"], [1, 2])
    np.testing.assert_array_equal(flow["flows"], [[1.5, 2.0], [1.8, 2.2]])


def test_parse_sample_file(sample_aflce_file):
    """Test parsing of the sample flow file."""
    parser = AflceParser(str(sample_aflce_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_flows == 3
    flows = parser.flows
    assert len(flows) == 3

    # Verify all centrals have required fields
    for flow in flows:
        assert isinstance(flow["name"], str)
        assert flow["name"] != ""
        assert isinstance(flow["blocks"], np.ndarray)
        assert isinstance(flow["flows"], np.ndarray)
        assert len(flow["blocks"]) > 0
        assert len(flow["blocks"]) == len(flow["flows"])
        assert flow["num_hydrologies"] == 5

        # Verify array types and values
        assert flow["blocks"].dtype == np.int16
        assert flow["flows"].dtype == np.float64
        assert np.all(flow["blocks"] > 0)
        assert np.all(flow["flows"] >= 0)

    # Verify first central data
    flow1 = flows[0]
    assert flow1["name"] == "LOS_MORROS"
    assert len(flow1["blocks"]) == 5
    assert flow1["blocks"][0] == 1
    np.testing.assert_array_equal(flow1["flows"][0], [1.19]*5)

    # Verify second central data
    flow2 = flows[1]
    assert flow2["name"] == "MAITENES"
    assert len(flow2["blocks"]) == 3
    assert flow2["blocks"][0] == 1
    np.testing.assert_array_equal(flow2["flows"][0], [0.0]*5)


def test_get_flow_by_name(sample_aflce_file):
    """Test getting flow by central name."""
    parser = AflceParser(str(sample_aflce_file))
    parser.parse()

    # Test existing central
    flows = parser.flows
    first_central = flows[0]["name"]
    flow_data = parser.get_flow_by_name(first_central)
    assert flow_data is not None
    assert flow_data["name"] == first_central
    assert len(flow_data["blocks"]) > 0
    assert len(flow_data["flows"]) > 0

    # Test non-existent central
    missing = parser.get_flow_by_name("NonExistentCentral")
    assert missing is None


def test_parse_empty_file(tmp_path):
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()

    parser = AflceParser(str(empty_file))
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path):
    """Test handling of malformed flow file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'CENTRAL'\n2\n03 001")  # Missing flows

    parser = AflceParser(str(bad_file))
    with pytest.raises(ValueError):
        parser.parse()
