"""Unit tests for JunctionWriter class."""

from typing import Any, Dict, List, Optional

import pytest

from ..junction_writer import JunctionWriter
from ..central_parser import CentralParser
from ..extrac_parser import ExtracParser
from ..aflce_parser import AflceParser

# Mocks for parsers


class MockCentralParser(CentralParser):
    """Mock CentralParser for testing."""

    def __init__(self, centrals: List[Dict[str, Any]]):
        """Initialize with a list of central data."""
        super().__init__("dummy.dat")
        self._centrals = centrals
        self.centrals_of_type: Dict[str, List[Dict[str, Any]]] = {}
        for central in centrals:
            ctype = central.get("type", "serie")
            if ctype not in self.centrals_of_type:
                self.centrals_of_type[ctype] = []
            self.centrals_of_type[ctype].append(central)

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all centrals."""
        return self._centrals

    def get_central_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get a central by name."""
        for central in self._centrals:
            if central["name"] == name:
                return central
        return None


class MockExtracParser(ExtracParser):
    """Mock ExtracParser for testing."""

    def __init__(self, extracs: List[Dict[str, Any]]):
        """Initialize with a list of extraction data."""
        super().__init__("dummy.dat")
        self._mock_data = extracs

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all extracs."""
        return self._mock_data


class MockAflceParser(AflceParser):
    """Mock AflceParser for testing."""

    def __init__(self, aflces: List[Dict[str, Any]]):
        """Initialize with a list of aflce data."""
        super().__init__("dummy.dat")
        self._aflces = aflces

    def get_item_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get an item by name."""
        for aflce in self._aflces:
            if aflce["name"] == name:
                return aflce
        return None


# Fixtures for parsers


@pytest.fixture
def empty_central_parser() -> MockCentralParser:
    """Return a MockCentralParser with no data."""
    return MockCentralParser([])


@pytest.fixture
def sample_central_parser() -> MockCentralParser:
    """Return a MockCentralParser with sample data."""
    centrals = [
        {
            "name": "PlantA",
            "number": 1,
            "bus": 101,
            "pmin": 0,
            "pmax": 50,
            "vert_min": 0,
            "vert_max": 50,
            "efficiency": 0.9,
            "ser_hid": 2,
            "ser_ver": 3,
            "afluent": 10.0,
            "type": "serie",
        },
        {
            "name": "PlantB",
            "number": 2,
            "bus": 102,
            "pmin": 0,
            "pmax": 60,
            "vert_min": 0,
            "vert_max": 50,
            "efficiency": 0.85,
            "ser_hid": 0,
            "ser_ver": 3,
            "afluent": 0.0,
            "type": "serie",
        },
        {
            "name": "PlantC",
            "number": 3,
            "bus": 0,
            "pmin": 0,
            "pmax": 70,
            "vert_min": 0,
            "vert_max": 50,
            "efficiency": 0.95,
            "ser_hid": 0,
            "ser_ver": 0,
            "afluent": 5.0,
            "type": "serie",
        },
    ]
    return MockCentralParser(centrals)


@pytest.fixture
def reservoir_parser() -> MockCentralParser:
    """Return a MockCentralParser with reservoir data."""
    reservoirs = [
        {
            "name": "ReservoirA",
            "number": 10,
            "bus": 0,
            "pmin": 0,
            "pmax": 100,
            "vert_min": 0,
            "vert_max": 50,
            "efficiency": 0,
            "ser_hid": 0,
            "ser_ver": 0,
            "afluent": 0.0,
            "type": "embalse",
            "vol_ini": 100,
            "vol_fin": 100,
            "vmin": 50,
            "vmax": 200,
        }
    ]
    return MockCentralParser(reservoirs)


@pytest.fixture
def sample_extrac_parser() -> MockExtracParser:
    """Return a MockExtracParser with sample data."""
    extracs = [{"name": "PlantA", "downstream": "PlantB", "max_extrac": 15.0}]
    return MockExtracParser(extracs)


@pytest.fixture
def sample_aflce_parser() -> MockAflceParser:
    """Return a MockAflceParser with sample data."""
    aflces = [{"name": "PlantA"}]
    return MockAflceParser(aflces)


# Test functions


def test_junction_writer_initialization(empty_central_parser):
    """Test JunctionWriter initialization."""
    writer = JunctionWriter(central_parser=empty_central_parser)
    assert writer.parser == empty_central_parser
    assert writer.aflce_parser is None
    assert writer.extrac_parser is None


def test_to_json_array_empty(empty_central_parser):
    """Test to_json_array with no input data."""
    writer = JunctionWriter(central_parser=empty_central_parser)
    result = writer.to_json_array()
    assert not result


def test_to_json_array_single_plant():
    """Test processing a single plant with all features."""
    central = {
        "name": "PlantA",
        "number": 1,
        "bus": 101,
        "pmin": 0,
        "pmax": 50,
        "vert_min": 0,
        "vert_max": 50,
        "efficiency": 0.9,
        "ser_hid": 2,
        "ser_ver": 2,
        "afluent": 10.0,
        "type": "serie",
    }
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert len(result["junction_array"]) == 1
    junction = result["junction_array"][0]
    assert junction["uid"] == 1
    assert junction["name"] == "PlantA"
    assert not junction["drain"]

    assert len(result["waterway_array"]) == 2
    ww1, ww2 = result["waterway_array"]
    assert ww1["junction_a"] == 1 and ww1["junction_b"] == 2
    assert ww2["junction_a"] == 1 and ww2["junction_b"] == 2

    assert len(result["turbine_array"]) == 1
    turbine = result["turbine_array"][0]
    assert turbine["uid"] == 1
    assert turbine["generator"] == 1
    assert turbine["waterway"] == ww1["uid"]
    assert turbine["conversion_rate"] == 0.9

    assert len(result["flow_array"]) == 1
    flow = result["flow_array"][0]
    assert flow["uid"] == 1
    assert flow["junction"] == 1
    assert flow["discharge"] == 10.0


def test_drain_junction():
    """Test that a junction is marked as drain if it has no downstream connections."""
    central = {
        "name": "PlantDrain",
        "number": 5,
        "bus": 101,
        "pmin": 0,
        "pmax": 50,
        "vert_min": 0,
        "vert_max": 50,
        "efficiency": 0.9,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 10.0,
        "type": "serie",
    }
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert len(result["junction_array"]) == 1
    junction = result["junction_array"][0]
    assert junction["uid"] == 5
    assert junction["drain"] is True
    assert len(result["waterway_array"]) == 0


def test_no_turbine_creation():
    """Test that no turbine is created if plant is not connected to a bus."""
    central = {
        "name": "PlantNoBus",
        "number": 6,
        "bus": 0,
        "pmin": 0,
        "pmax": 50,
        "vert_min": 0,
        "vert_max": 50,
        "efficiency": 0.9,
        "ser_hid": 7,
        "ser_ver": 0,
        "afluent": 0.0,
        "type": "serie",
    }
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert len(result["turbine_array"]) == 0
    assert len(result["waterway_array"]) == 1


def test_process_reservoirs(reservoir_parser):
    """Test processing of reservoir plants."""
    writer = JunctionWriter(central_parser=reservoir_parser)
    result = writer.to_json_array()[0]

    assert len(result["reservoir_array"]) == 1
    reservoir = result["reservoir_array"][0]
    assert reservoir["uid"] == 10
    assert reservoir["name"] == "ReservoirA"
    assert reservoir["junction"] == 10
    assert reservoir["vini"] == 100
    assert reservoir["vmax"] == 200
    assert reservoir["capacity"] == 200


def test_process_extractions(sample_extrac_parser):
    """Test processing of extraction data into waterways."""
    centrals = [
        {
            "name": "PlantA",
            "number": 1,
            "type": "serie",
            "bus": 0,
            "pmin": 0,
            "pmax": 15,
            "vert_min": 0,
            "vert_max": 50,
            "ser_hid": 2,
            "ser_ver": 5,
            "efficiency": 0,
        },
        {
            "name": "PlantB",
            "number": 2,
            "type": "serie",
            "bus": 0,
            "pmin": 0,
            "pmax": 50,
            "vert_min": 0,
            "vert_max": 50,
            "ser_hid": 3,
            "ser_ver": 7,
            "efficiency": 0,
        },
    ]
    central_parser = MockCentralParser(centrals)

    writer = JunctionWriter(
        central_parser=central_parser, extrac_parser=sample_extrac_parser
    )
    result = writer.to_json_array()[0]

    # Waterways from centrals
    assert len(result["waterway_array"]) == 5
    waterway = result["waterway_array"][0]
    assert waterway["junction_a"] == 1
    assert waterway["junction_b"] == 2
    assert waterway["fmin"] == 0.0

    waterway = result["waterway_array"][1]
    assert waterway["junction_a"] == 1
    assert waterway["junction_b"] == 5
    assert waterway["fmin"] == 0.0
    assert waterway["fmax"] == 50.0


def test_get_plant_flow_with_aflce(sample_aflce_parser):
    """Test that flow is identified as 'afluent' when aflce data is present."""
    central = {
        "name": "PlantA",
        "number": 1,
        "afluent": 10.0,
        "type": "serie",
        "bus": 101,
        "pmin": 0,
        "pmax": 50,
        "vert_min": 0,
        "vert_max": 50,
        "efficiency": 0.9,
        "ser_hid": 0,
        "ser_ver": 0,
    }
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=central_parser, aflce_parser=sample_aflce_parser
    )
    result = writer.to_json_array()[0]

    assert len(result["flow_array"]) == 1
    flow = result["flow_array"][0]
    assert flow["discharge"] == "Afluent@afluent"


def test_multiple_plants_and_interactions(sample_central_parser, sample_extrac_parser):
    """Test a more complex scenario with multiple plants and extractions."""
    writer = JunctionWriter(
        central_parser=sample_central_parser, extrac_parser=sample_extrac_parser
    )
    result = writer.to_json_array()[0]

    # 3 junctions from central parser
    assert len(result["junction_array"]) == 3

    # 2 waterways from PlantA, 1 from PlantB, 1 from extraction
    assert len(result["waterway_array"]) == 4

    # 1 turbine for PlantA (PlantB has no generation waterway)
    assert len(result["turbine_array"]) == 1

    # 2 flows for PlantA and PlantC
    assert len(result["flow_array"]) == 2
