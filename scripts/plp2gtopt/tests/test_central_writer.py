#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for CentralWriter class."""

import typing
import json
import tempfile
from pathlib import Path
import pytest
from ..central_writer import CentralWriter
from ..central_parser import CentralParser
from .conftest import get_example_file


@pytest.fixture
def sample_central_file():
    """Fixture providing path to sample central file."""
    return get_example_file("plpcnfce.dat")


@pytest.fixture
def sample_central_writer(sample_central_file, tmp_path):
    """Fixture providing initialized CentralWriter with sample data."""
    parser = CentralParser(sample_central_file)
    parser.parse()
    options = {"output_dir": tmp_path}
    return CentralWriter(parser, options=options)


def test_central_writer_initialization(sample_central_file, tmp_path):  # pylint: disable=redefined-outer-name
    """Test CentralWriter initialization."""
    parser = CentralParser(sample_central_file)
    parser.parse()
    options = {"output_dir": tmp_path}
    writer = CentralWriter(parser, options=options)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_centrals


def test_to_json_array(sample_central_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of centrals to JSON array format."""
    json_centrals = sample_central_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_centrals, list)
    assert len(json_centrals) > 0

    # Verify each central has required fields
    for central in json_centrals:
        assert "uid" in central
        assert "name" in central
        assert "bus" in central
        assert "gcost" in central
        assert "capacity" in central
        assert isinstance(central["uid"], int)
        assert isinstance(central["name"], str)
        assert isinstance(central["bus"], int)
        assert isinstance(central["gcost"], float)
        assert isinstance(central["capacity"], float)


def test_write_to_file(sample_central_writer):  # pylint: disable=redefined-outer-name
    """Test writing central data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_central_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_write_empty_centrals(tmp_path):
    """Test handling of empty central list."""

    # Create parser with no centrals
    # Create a mock parser with empty centrals list
    class MockCentralParser:
        def __init__(self):
            self._centrals = []
            self._num_centrals = 0

        @property
        def centrals(self):
            return self._centrals

        @property
        def num_centrals(self):
            return self._num_centrals

        def get_centrals(self):
            return self._centrals

        def get_all(self):
            return self._centrals

    mock_parser: typing.Any = MockCentralParser()

    options = {"output_dir": tmp_path}
    writer = CentralWriter(mock_parser, options=options)
    json_centrals = writer.to_json_array()
    assert not json_centrals

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []


def _make_central_parser_from_list(centrals):
    """Build a mock CentralParser from a list of central dicts."""

    class _Mock:
        def __init__(self, items):
            self._centrals = items
            self._num_centrals = len(items)

        @property
        def centrals(self):
            return self._centrals

        @property
        def num_centrals(self):
            return self._num_centrals

        def get_centrals(self):
            return self._centrals

        def get_all(self):
            return self._centrals

    return _Mock(centrals)


def test_suspected_solar_description_when_no_tech_detect(tmp_path):
    """When auto_detect_tech=False, suspected solar adds description note."""
    centrals = [
        {
            "name": "SolarAlmeyda",
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 100.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": False}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()

    assert len(result) == 1
    gen = result[0]
    assert gen["type"] == "thermal"  # type NOT changed
    assert gen["description"] == "suspected solar"


def test_suspected_wind_description_when_no_tech_detect(tmp_path):
    """When auto_detect_tech=False, suspected wind adds description note."""
    centrals = [
        {
            "name": "EolicaCanela",
            "number": 2,
            "bus": 1,
            "type": "pasada",
            "pmax": 50.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": False}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()

    assert len(result) == 1
    gen = result[0]
    assert gen["type"] == "hydro_ror"  # type NOT changed
    assert gen["description"] == "suspected wind"


def test_no_description_when_tech_detect_enabled(tmp_path):
    """When auto_detect_tech=True, no description note — type is changed."""
    centrals = [
        {
            "name": "SolarAlmeyda",
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 100.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": True}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()

    assert len(result) == 1
    gen = result[0]
    assert gen["type"] == "solar"  # type IS changed
    assert "description" not in gen


def test_no_description_for_non_suspect_name(tmp_path):
    """Generic termica name gets no description note."""
    centrals = [
        {
            "name": "GenericPlant",
            "number": 3,
            "bus": 1,
            "type": "termica",
            "pmax": 200.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": False}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()

    assert len(result) == 1
    gen = result[0]
    assert gen["type"] == "thermal"
    assert "description" not in gen


# ---------------------------------------------------------------------------
# Issue #524 — last-resort renewable:hydro fallback for PLP-only thermals
# that have no fuel-cost signal and auto-detect didn't find a name match.
# ---------------------------------------------------------------------------


def test_524_fallback_hr0_no_pattern_with_autodetect(tmp_path):
    """PLP termica + gcost=0 + no pattern match + auto-detect ON
    → fallback to renewable:hydro (small distributed unit).
    """
    centrals = [
        {
            "name": "ALTO_HOSPICIO",  # not in CEN _FV / _EO suffix patterns
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 5.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": True}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()
    assert result[0]["type"] == "renewable:hydro"


def test_524_no_fallback_when_autodetect_off(tmp_path):
    """Same input but auto-detect OFF → legacy behaviour preserved
    (type stays "thermal", description gets the suspected note).
    """
    centrals = [
        {
            "name": "ALTO_HOSPICIO",
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 5.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": False}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()
    assert result[0]["type"] == "thermal"  # unchanged
    assert "description" not in result[0]  # no suspected pattern match


def test_524_no_fallback_when_gcost_nonzero(tmp_path):
    """Real thermal with gcost>0 keeps type=thermal even with
    auto-detect on (fuel-cost signal indicates real combustion)."""
    centrals = [
        {
            "name": "DIESEL_PEAKER",
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 50.0,
            "pmin": 0.0,
            "gcost": 200.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": True}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()
    # detect_technology may refine via name (none here) but bare
    # "thermal" + gcost>0 must NOT trigger the renewable fallback
    assert result[0]["type"] in ("thermal", "diesel")  # tolerant on pattern hit


def test_524_no_fallback_when_gcost_is_schedule(tmp_path, monkeypatch):
    """A thermal whose ``gcost`` is a time-varying cost SCHEDULE (the
    string ``"gcost"`` referencing a parquet column) DOES burn fuel and
    must stay ``thermal`` — never reclassified to ``renewable:hydro``.

    Regression for the coal-as-hydro bug: the old check treated a string
    gcost as "no fuel cost" (it is not an int/float), so real coal / gas /
    diesel plants (Guacolda, Angamos, Kelar, …) with scheduled fuel costs
    were mislabeled as hydro, hiding coal from the technology breakdown and
    mis-attributing their fuel cost to "hydro".
    """
    centrals = [
        {
            "name": "GUACOLDA_1",  # coal, no _FV/_EO pattern → bare "thermal"
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 128.3,
            "pmin": 0.0,
            "gcost": 0.0,  # scalar in the .dat, but overridden by the schedule
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": True}
    writer = CentralWriter(parser, options=options)
    # Force the time-varying-cost path: the central's parquet column is in
    # the "gcost" set → gcost becomes the string "gcost" inside to_json_array.
    pcol = writer.pcol_name("GUACOLDA_1", 1)
    monkeypatch.setattr(
        writer,
        "_write_parquet_files",
        lambda: {"gcost": [pcol], "pmin": [], "pmax": []},
    )
    result = writer.to_json_array()
    assert result[0]["gcost"] == "gcost"  # schedule reference, not a scalar
    assert result[0]["type"] == "thermal"  # NOT renewable:hydro


def test_524_pattern_match_beats_fallback(tmp_path):
    """When auto-detect catches the name (_FV → solar), the
    fallback never runs."""
    centrals = [
        {
            "name": "CAPRICORNIO_FV",
            "number": 1,
            "bus": 1,
            "type": "termica",
            "pmax": 100.0,
            "pmin": 0.0,
            "gcost": 0.0,
        },
    ]
    parser: typing.Any = _make_central_parser_from_list(centrals)
    options: dict = {"output_dir": tmp_path, "auto_detect_tech": True}
    writer = CentralWriter(parser, options=options)
    result = writer.to_json_array()
    assert result[0]["type"] == "solar"  # _FV suffix detection wins
