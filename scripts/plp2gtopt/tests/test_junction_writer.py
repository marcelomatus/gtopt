"""Unit tests for JunctionWriter class."""

from typing import Any, Dict, List, Optional

import pytest

from ..junction_writer import JunctionWriter
from ..central_parser import CentralParser
from ..extrac_parser import ExtracParser
from ..aflce_parser import AflceParser
from ..cenre_parser import CenreParser
from ..cenfi_parser import CenfiParser

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


class MockCenreParser(CenreParser):
    """Mock CenreParser for testing."""

    def __init__(self, efficiencies: List[Dict[str, Any]]):
        """Initialize with a list of efficiency data."""
        super().__init__("dummy.dat")
        self._mock_data = efficiencies

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all efficiency entries."""
        return self._mock_data

    @property
    def efficiencies(self) -> List[Dict[str, Any]]:
        """Return all efficiency entries."""
        return self._mock_data


class MockCenfiParser(CenfiParser):
    """Mock CenfiParser for testing."""

    def __init__(self, filtrations: List[Dict[str, Any]]):
        """Initialize with a list of filtration data."""
        super().__init__("dummy.dat")
        self._mock_data = filtrations

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all filtration entries."""
        return self._mock_data

    @property
    def filtrations(self) -> List[Dict[str, Any]]:
        """Return all filtration entries."""
        return self._mock_data


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
            "emin": 50,
            "emax": 200,
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
    assert reservoir["eini"] == 100
    assert reservoir["emax"] == 200
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


# ─── Filtration and efficiency tests ────────────────────────────────────────


def _make_hydro_parser() -> MockCentralParser:
    """Return a MockCentralParser with one reservoir and one turbine."""
    return MockCentralParser(
        [
            {
                "name": "Dam1",
                "number": 1,
                "type": "embalse",
                "bus": 0,
                "pmin": 0,
                "pmax": 100,
                "vert_min": 0,
                "vert_max": 50,
                "efficiency": 0.0,
                "ser_hid": 0,
                "ser_ver": 0,
                "afluent": 0.0,
                "vol_ini": 500.0,
                "vol_fin": 450.0,
                "emin": 100.0,
                "emax": 1000.0,
            },
            {
                "name": "Turbine1",
                "number": 2,
                "type": "serie",
                "bus": 10,
                "pmin": 0,
                "pmax": 80,
                "vert_min": 0,
                "vert_max": 50,
                "efficiency": 1.5,
                "ser_hid": 3,
                "ser_ver": 0,
                "afluent": 0.0,
            },
        ]
    )


def test_filtration_array_populated():
    """JunctionWriter creates filtration_array from CenfiParser data."""
    central_parser = _make_hydro_parser()
    cenfi_parser = MockCenfiParser(
        [
            {
                "name": "Turbine1",
                "reservoir": "Dam1",
                "slope": 0.001,
                "constant": 5.0,
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenfi_parser=cenfi_parser)
    result = writer.to_json_array()[0]

    assert "filtration_array" in result
    assert len(result["filtration_array"]) == 1
    filt = result["filtration_array"][0]
    assert filt["slope"] == pytest.approx(0.001)
    assert filt["constant"] == pytest.approx(5.0)
    assert filt["reservoir"] == 1  # Dam1 uid


def test_filtration_array_empty_when_no_parser():
    """filtration_array is empty when no CenfiParser is provided."""
    central_parser = _make_hydro_parser()
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert "filtration_array" in result
    assert result["filtration_array"] == []


def test_filtration_skips_unknown_central():
    """_process_filtrations skips entries whose central is not found."""
    central_parser = _make_hydro_parser()
    cenfi_parser = MockCenfiParser(
        [
            {
                "name": "NONEXISTENT",
                "reservoir": "Dam1",
                "slope": 0.001,
                "constant": 5.0,
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenfi_parser=cenfi_parser)
    result = writer.to_json_array()[0]
    # Unknown central → silently skipped
    assert result["filtration_array"] == []


def test_filtration_with_segments():
    """JunctionWriter propagates piecewise segments from cenfi_parser."""
    central_parser = _make_hydro_parser()
    cenfi_parser = MockCenfiParser(
        [
            {
                "name": "Turbine1",
                "reservoir": "Dam1",
                "slope": 0.001,
                "constant": 5.0,
                "segments": [
                    {"volume": 0.0, "slope": 0.001, "constant": 5.0},
                    {"volume": 500.0, "slope": 0.0002, "constant": 5.5},
                ],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenfi_parser=cenfi_parser)
    result = writer.to_json_array()[0]

    assert len(result["filtration_array"]) == 1
    filt = result["filtration_array"][0]
    assert filt["slope"] == pytest.approx(0.001)
    assert filt["constant"] == pytest.approx(5.0)
    assert "segments" in filt
    assert len(filt["segments"]) == 2
    assert filt["segments"][0]["volume"] == pytest.approx(0.0)
    assert filt["segments"][1]["volume"] == pytest.approx(500.0)


def test_filtration_without_segments_no_key():
    """Filtration without segments does not include the segments key."""
    central_parser = _make_hydro_parser()
    cenfi_parser = MockCenfiParser(
        [
            {
                "name": "Turbine1",
                "reservoir": "Dam1",
                "slope": 0.001,
                "constant": 5.0,
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenfi_parser=cenfi_parser)
    result = writer.to_json_array()[0]
    filt = result["filtration_array"][0]
    # When no segments present, the key should not be in the output
    assert "segments" not in filt


def test_reservoir_efficiency_array_populated():
    """JunctionWriter creates reservoir_efficiency_array from CenreParser data."""
    central_parser = _make_hydro_parser()
    cenre_parser = MockCenreParser(
        [
            {
                "name": "Turbine1",
                "reservoir": "Dam1",
                "mean_efficiency": 1.5,
                "segments": [
                    {"volume": 0.0, "slope": 0.0003, "constant": 1.2},
                    {"volume": 500.0, "slope": 0.0001, "constant": 1.5},
                ],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]

    assert "reservoir_efficiency_array" in result
    assert len(result["reservoir_efficiency_array"]) == 1
    eff = result["reservoir_efficiency_array"][0]
    assert eff["mean_efficiency"] == pytest.approx(1.5)
    assert eff["reservoir"] == 1  # Dam1 uid
    assert len(eff["segments"]) == 2
    assert eff["segments"][0]["slope"] == pytest.approx(0.0003)
    assert eff["segments"][1]["volume"] == pytest.approx(500.0)


def test_reservoir_efficiency_array_empty_when_no_parser():
    """reservoir_efficiency_array is empty when no CenreParser is provided."""
    central_parser = _make_hydro_parser()
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert "reservoir_efficiency_array" in result
    assert result["reservoir_efficiency_array"] == []


def test_efficiency_skips_unknown_central():
    """_process_reservoir_efficiencies skips entries whose central is not found."""
    central_parser = _make_hydro_parser()
    cenre_parser = MockCenreParser(
        [
            {
                "name": "NONEXISTENT",
                "reservoir": "Dam1",
                "mean_efficiency": 1.5,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]
    assert result["reservoir_efficiency_array"] == []


def test_efficiency_skips_central_without_turbine():
    """Efficiency entry is skipped when the central exists but has no turbine (bus<=0)."""
    central_parser = _make_hydro_parser()
    # Dam1 exists in central_parser with bus=0, so no turbine is created for it.
    cenre_parser = MockCenreParser(
        [
            {
                "name": "Dam1",
                "reservoir": "Dam1",
                "mean_efficiency": 1.2,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]
    # Dam1 has bus=0 so no turbine was created — efficiency entry must be skipped
    assert result["reservoir_efficiency_array"] == []
