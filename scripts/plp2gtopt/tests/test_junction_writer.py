"""Unit tests for JunctionWriter class."""

import logging
from typing import Any, Dict, List, Optional

import pytest

from ..junction_writer import JunctionWriter
from ..central_parser import CentralParser
from ..extrac_parser import ExtracParser
from ..aflce_parser import AflceParser
from ..cenre_parser import CenreParser
from ..cenfi_parser import CenfiParser

# Legacy options dict: opts back into the pre-default-suppress regime so
# tests asserting the historical ``_ver`` waterway shape keep their meaning.
# Tests covering the new default (``drop_spillway_waterway = True``) live
# in ``test_drop_spillway_waterway.py``.
_LEGACY_OPTS = {"drop_spillway_waterway": False}

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

    def __init__(self, seepages: List[Dict[str, Any]]):
        """Initialize with a list of seepage data."""
        super().__init__("dummy.dat")
        self._mock_data = seepages

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all seepage entries."""
        return self._mock_data

    @property
    def seepages(self) -> List[Dict[str, Any]]:
        """Return all seepage entries."""
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
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    assert len(result["junction_array"]) == 1
    junction = result["junction_array"][0]
    assert junction["uid"] == 1
    assert junction["name"] == "PlantA"
    assert not junction["drain"]

    # PlantA's generation flow is carried by the built-in Turbine
    # (``junction_a`` → ``junction_b``); only the ``_ver`` spillway
    # Waterway remains.
    assert len(result["waterway_array"]) == 1
    ver = result["waterway_array"][0]
    assert "_ver_" in ver["name"]
    assert ver["junction_a"] == "PlantA" and ver["junction_b"] == "2"

    assert len(result["turbine_array"]) == 1
    turbine = result["turbine_array"][0]
    assert turbine["uid"] == 1
    assert turbine["generator"] == "PlantA"
    assert "waterway" not in turbine
    assert turbine["junction_a"] == "PlantA"
    assert turbine["junction_b"] == "2"
    assert turbine["production_factor"] == 0.9

    assert len(result["flow_array"]) == 1
    flow = result["flow_array"][0]
    assert flow["uid"] == 1
    assert flow["junction"] == "PlantA"
    assert flow["discharge"] == 10.0


def test_irrigation_diversion_becomes_flowright():
    """Irrigation diversion (bus=0, ser_hid=0, ser_ver>0, serie) is modeled
    as a consumptive FlowRight on the central's own junction — NOT a free
    drain — while the ``_ver`` return arc to the downstream ``ser_ver``
    junction is preserved.  Mirrors RieSur123SCDZ / RIEGZACO / RieSaltos in
    the CEN case (the extraction is a water right; the remainder returns
    downstream).
    """
    central = {
        "name": "RieTest",
        "number": 1,
        "bus": 0,  # transit-only: no electrical generation
        "pmin": 0,
        "pmax": 1.0,
        "vert_min": 0,
        "vert_max": 9999.0,  # PLP no-limit sentinel
        "efficiency": 1.0,
        "ser_hid": 0,  # no downstream generation
        "ser_ver": 2,  # spill / return to downstream junction 2
        "afluent": 5.0,
        "type": "serie",
    }
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # The diversion junction must NOT carry a free drain.
    junction = next(j for j in result["junction_array"] if j["name"] == "RieTest")
    assert not junction.get("drain")
    assert "drain_capacity" not in junction
    assert "drain_cost" not in junction

    # A single consumptive FlowRight is emitted on the central's own junction.
    frs = [
        f
        for f in result.get("flow_right_array", [])
        if f.get("junction_a") == "RieTest"
    ]
    assert len(frs) == 1
    fr = frs[0]
    assert fr["consumptive"] is True
    assert fr["direction"] == -1
    assert fr["fmin"] == 0.0
    assert fr.get("fcost") is not None and fr["fcost"] > 0.0

    # The _ver return arc to the downstream ser_ver junction is preserved.
    vers = [
        w
        for w in result["waterway_array"]
        if "_ver_" in w["name"] and w["junction_a"] == "RieTest"
    ]
    assert len(vers) == 1
    assert vers[0]["junction_b"] == "2"


def _irrigation_central(**overrides: Any) -> Dict[str, Any]:
    """Build a serie irrigation-diversion central (bus=0, ser_hid=0,
    ser_ver>0), letting individual fields be overridden to probe the
    ``is_irrigation_diversion`` predicate and the FlowRight ``fmax`` port.
    """
    central: Dict[str, Any] = {
        "name": "RieTest",
        "number": 1,
        "bus": 0,
        "pmin": 0,
        "pmax": 50.0,
        "vert_min": 0,
        "vert_max": 9999.0,  # PLP no-limit sentinel
        "efficiency": 1.0,
        "ser_hid": 0,
        "ser_ver": 2,
        "afluent": 5.0,
        "type": "serie",
    }
    central.update(overrides)
    return central


def test_irrigation_diversion_fmax_bounded():
    """When the former gen-path drain capacity (``PotMax / Rendi``) is finite
    (below the PLP no-limit sentinel) it is ported to the FlowRight ``fmax``.
    """
    central = _irrigation_central(pmax=70.0, efficiency=2.0)  # gen_fmax = 35
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]), options=_LEGACY_OPTS
    )
    result = writer.to_json_array()[0]
    fr = next(f for f in result["flow_right_array"] if f.get("junction_a") == "RieTest")
    assert fr["fmax"] == pytest.approx(35.0)


def test_irrigation_diversion_fmax_unbounded():
    """A ``PotMax`` at/above the PLP no-limit sentinel (9000+) means there is
    no genuine diversion cap, so ``fmax`` is omitted (unbounded FlowRight).
    """
    central = _irrigation_central(pmax=9999.0)
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]), options=_LEGACY_OPTS
    )
    result = writer.to_json_array()[0]
    fr = next(f for f in result["flow_right_array"] if f.get("junction_a") == "RieTest")
    assert "fmax" not in fr


@pytest.mark.parametrize(
    "overrides",
    [
        {"ser_ver": 0},  # terminal: no _ver return arc
        {"bus": 5},  # has an electrical outlet
        {"ser_hid": 3},  # the gen arc has a real downstream junction
    ],
    ids=["terminal_ser_ver_0", "has_bus", "has_ser_hid"],
)
def test_serie_not_irrigation_diversion(overrides: Dict[str, Any]):
    """Each term of the ``is_irrigation_diversion`` signature is necessary:
    flipping any one of ser_ver>0 / bus==0 / ser_hid==0 must suppress the
    consumptive irrigation FlowRight.
    """
    central = _irrigation_central(**overrides)
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]), options=_LEGACY_OPTS
    )
    result = writer.to_json_array()[0]
    irr = [
        f
        for f in result.get("flow_right_array", [])
        if f.get("purpose") == "irrigation"
    ]
    assert irr == []


def test_irrigation_diversion_with_drop_spillway_waterway():
    """``--drop-spillway-waterway`` must NOT force a drain on an irrigation
    diversion: the option suppresses ``_ver`` spillways and drains the
    surplus on the source for ordinary serie/embalse/pasada centrals, but an
    irrigation diversion's ``_ver`` arc carries the river REMAINDER
    downstream (not a spill-to-sea).  So the ``_ver`` arc must be preserved,
    ``drain`` must stay False, and the consumptive FlowRight is still
    emitted (regression guard for the assertion that fired on RieSur123SCDZ).
    """
    central = _irrigation_central(name="RieDrop", ser_ver=7)
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]),
        options={"drop_spillway_waterway": True},
    )
    # Must not raise the irrigation-diversion drain=True assertion.
    result = writer.to_json_array()[0]

    junction = next(j for j in result["junction_array"] if j["name"] == "RieDrop")
    assert not junction.get("drain")

    # The _ver return arc to the downstream ser_ver junction is preserved.
    vers = [
        w
        for w in result["waterway_array"]
        if "_ver_" in w["name"] and w["junction_a"] == "RieDrop"
    ]
    assert len(vers) == 1
    assert vers[0]["junction_b"] == "7"

    # The consumptive FlowRight is still emitted.
    frs = [
        f
        for f in result.get("flow_right_array", [])
        if f.get("junction_a") == "RieDrop"
    ]
    assert len(frs) == 1
    assert frs[0]["consumptive"] is True
    assert frs[0]["direction"] == -1


def test_drain_junction():
    """Terminal central (ser_hid=0, ser_ver=0, vert_max>0): the spillway
    capacity is collapsed onto the SOURCE junction's own drain column
    (``drain = True`` + ``drain_capacity = VertMax``), while the gen path
    uses the built-in Turbine waterway (``Turbine.junction_a``).

    The synthetic ``<central>_ocean`` node and the ``_ver → ocean`` arc are
    eliminated: the productive water leaves via the turbine, and the spill /
    consumptive remainder is shed through the source junction's own drain
    column (capacity ported onto ``drain_capacity``).

    Two LP outlets:

    * **Turbine.junction_a** (debit, capped at ``PotMax / Rendi``) — the
      power-producing path.
    * **source-junction drain** (capped at ``VertMax`` via
      ``drain_capacity``) — the spillway path.
    """
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
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # ZERO synthetic ocean junctions — only the central's own junction.
    junctions = {j["name"]: j for j in result["junction_array"]}
    assert set(junctions) == {"PlantDrain"}
    plant_junction = junctions["PlantDrain"]
    assert plant_junction["uid"] == 5
    # The source junction is now the spill-exit drain, carrying VertMax on
    # its ``drain_capacity`` column.
    assert plant_junction["drain"] is True
    assert plant_junction["drain_capacity"] == pytest.approx(50.0)
    assert "drain_cost" not in plant_junction

    # No ``_ver → ocean`` spill waterway is emitted.
    assert len(result["waterway_array"]) == 0

    # Terminal turbine uses the built-in waterway form.
    turbines = result["turbine_array"]
    assert len(turbines) == 1
    assert turbines[0]["name"] == "PlantDrain"
    assert turbines[0]["junction_a"] == "PlantDrain"
    assert "waterway" not in turbines[0]
    assert "junction_b" not in turbines[0]


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
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    assert len(result["turbine_array"]) == 0
    # One waterway: `_gen` (to junction 7).  The `_ver` spill (ser_ver=0,
    # VertMax>0) is collapsed onto the source junction's own drain column
    # instead of a ``_ver → ocean`` arc, so NO synthetic ocean node and no
    # ``_ver`` waterway are emitted.
    waterway_names = sorted(w["name"] for w in result["waterway_array"])
    assert len(waterway_names) == 1
    assert any(n.startswith("PlantNoBus_gen") for n in waterway_names)
    assert not any(n.startswith("PlantNoBus_ver") for n in waterway_names)
    junctions = {j["name"]: j for j in result["junction_array"]}
    # ZERO synthetic ocean junctions.
    assert not any(name.endswith("_ocean") for name in junctions)
    src = junctions["PlantNoBus"]
    # The source junction is the spill-exit drain, carrying the VertMax cap
    # that used to live on the ``_ver → ocean`` arc.
    assert src["drain"] is True
    assert src["drain_capacity"] == pytest.approx(50.0)
    assert "drain_cost" not in src


def test_process_reservoirs(reservoir_parser):
    """Test processing of reservoir plants."""
    writer = JunctionWriter(central_parser=reservoir_parser)
    result = writer.to_json_array()[0]

    assert len(result["reservoir_array"]) == 1
    reservoir = result["reservoir_array"][0]
    assert reservoir["uid"] == 10
    assert reservoir["name"] == "ReservoirA"
    assert reservoir["junction"] == "ReservoirA"
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
        central_parser=central_parser,
        extrac_parser=sample_extrac_parser,
        options=_LEGACY_OPTS,
    )
    result = writer.to_json_array()[0]

    # Waterways from centrals
    assert len(result["waterway_array"]) == 5
    waterway = result["waterway_array"][0]
    assert waterway["junction_a"] == "PlantA"
    assert waterway["junction_b"] == "PlantB"
    assert waterway["fmin"] == 0.0

    waterway = result["waterway_array"][1]
    assert waterway["junction_a"] == "PlantA"
    assert waterway["junction_b"] == "5"
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
    assert flow["discharge"] == "discharge"


def test_multiple_plants_and_interactions(sample_central_parser, sample_extrac_parser):
    """Test a more complex scenario with multiple plants and extractions."""
    writer = JunctionWriter(
        central_parser=sample_central_parser,
        extrac_parser=sample_extrac_parser,
        options=_LEGACY_OPTS,
    )
    result = writer.to_json_array()[0]

    # PlantA + PlantB + PlantC junctions — ZERO synthetic ocean junctions.
    # PlantB used to need a synthetic ``PlantB_ocean`` for its terminal
    # gen path; with the built-in ``Turbine.junction_a`` waterway the
    # turbine debits PlantB directly and no ocean junction is needed.
    # PlantC (bus=0 transit-only, ser_hid=0 AND ser_ver=0) is a genuine
    # spill-exit point with no downstream consumer of its gen Waterway, so
    # both the gen and ver remainders collapse onto PlantC's own drain
    # column — no ``PlantC_ocean`` node, no PlantC waterways.
    assert len(result["junction_array"]) == 3
    assert not any(j["name"].endswith("_ocean") for j in result["junction_array"])

    # PlantA: ver (in-network → PlantC) only — gen path now on the
    #   built-in Turbine waterway (junction_a/junction_b, no Waterway).
    # PlantB: ver (in-network → PlantC) only — gen path likewise built-in.
    # PlantC (bus=0): no waterways — gen + ver remainders ride its own
    #   source-junction drain.
    # plus 1 extraction = 3 total.
    assert len(result["waterway_array"]) == 3
    # PlantC's own junction is the spill-exit drain.  Its drain_capacity is
    # the larger of the gen flow cap (pmax/eff = 70/0.95) and VertMax (50).
    plantc = next(j for j in result["junction_array"] if j["name"] == "PlantC")
    assert plantc["drain"] is True
    assert plantc["drain_capacity"] == pytest.approx(70.0 / 0.95)

    # PlantA (bus=101) + PlantB (bus=102) = 2 turbines (PlantC bus=0, no turbine)
    assert len(result["turbine_array"]) == 2

    # 2 flows: PlantA + PlantC (PlantC's afluent still injects at its junction)
    assert len(result["flow_array"]) == 2


# ─── ReservoirSeepage and efficiency tests ────────────────────────────────────────


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


def test_reservoir_seepage_array_populated():
    """JunctionWriter creates reservoir_seepage_array from CenfiParser data."""
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

    # Seepage is now in the system-level array
    seep_arr = result["reservoir_seepage_array"]
    assert len(seep_arr) == 1
    filt = seep_arr[0]
    assert filt["uid"] == 1
    assert filt["name"] == "Dam1_seepage_1"
    assert filt["waterway"] == "Turbine1_gen_2_3"
    assert filt["reservoir"] == "Dam1"
    assert filt["slope"] == pytest.approx(0.001)
    assert filt["constant"] == pytest.approx(5.0)


def test_reservoir_seepage_embedded_mode():
    """With embed_reservoir_constraints, seepage is inside the reservoir."""
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
    writer = JunctionWriter(
        central_parser=central_parser,
        cenfi_parser=cenfi_parser,
        options={"embed_reservoir_constraints": True},
    )
    result = writer.to_json_array()[0]

    # System-level array should be empty
    assert result["reservoir_seepage_array"] == []

    # Seepage should be embedded inside the reservoir dict
    dam1 = next(r for r in result["reservoir_array"] if r["name"] == "Dam1")
    assert len(dam1["seepage"]) == 1
    filt = dam1["seepage"][0]
    assert filt["name"] == "Dam1_seepage_1"
    assert filt["slope"] == pytest.approx(0.001)


def test_reservoir_seepage_empty_when_no_parser():
    """Reservoirs have no seepage field when no CenfiParser is provided."""
    central_parser = _make_hydro_parser()
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert result["reservoir_seepage_array"] == []


def test_seepage_skips_unknown_central():
    """_process_seepages skips entries whose central is not found."""
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
    # Unknown central → silently skipped, no seepage created
    assert result["reservoir_seepage_array"] == []


def test_seepage_with_segments():
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

    seep_arr = result["reservoir_seepage_array"]
    assert len(seep_arr) == 1
    filt = seep_arr[0]
    assert filt["slope"] == pytest.approx(0.001)
    assert filt["constant"] == pytest.approx(5.0)
    assert "segments" in filt
    assert len(filt["segments"]) == 2
    assert filt["segments"][0]["volume"] == pytest.approx(0.0)
    assert filt["segments"][1]["volume"] == pytest.approx(500.0)


def test_seepage_without_segments_no_key():
    """ReservoirSeepage without segments does not include the segments key."""
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
    filt = result["reservoir_seepage_array"][0]
    # When no segments present, the key should not be in the output
    assert "segments" not in filt


def test_reservoir_production_factor_array_populated():
    """JunctionWriter creates reservoir_production_factor_array from CenreParser data."""
    central_parser = _make_hydro_parser()
    cenre_parser = MockCenreParser(
        [
            {
                "name": "Turbine1",
                "reservoir": "Dam1",
                "mean_production_factor": 1.5,
                "segments": [
                    {"volume": 0.0, "slope": 0.0003, "constant": 1.2},
                    {"volume": 500.0, "slope": 0.0001, "constant": 1.5},
                ],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]

    # Production factor is now in the system-level array
    pfac_arr = result["reservoir_production_factor_array"]
    assert len(pfac_arr) == 1
    eff = pfac_arr[0]
    assert eff["uid"] == 1
    assert eff["name"] == "Dam1_pfac_1"
    assert eff["turbine"] == "Turbine1"
    assert eff["reservoir"] == "Dam1"
    assert eff["mean_production_factor"] == pytest.approx(1.5)
    assert len(eff["segments"]) == 2
    assert eff["segments"][0]["slope"] == pytest.approx(0.0003)
    assert eff["segments"][1]["volume"] == pytest.approx(500.0)


def test_reservoir_production_factor_empty_when_no_parser():
    """Reservoirs have no production_factor field when no CenreParser is provided."""
    central_parser = _make_hydro_parser()
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    assert result["reservoir_production_factor_array"] == []


def test_efficiency_skips_unknown_central():
    """_process_reservoir_efficiencies skips entries whose central is not found."""
    central_parser = _make_hydro_parser()
    cenre_parser = MockCenreParser(
        [
            {
                "name": "NONEXISTENT",
                "reservoir": "Dam1",
                "mean_production_factor": 1.5,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]
    assert result["reservoir_production_factor_array"] == []


def test_efficiency_skips_central_without_turbine():
    """Efficiency entry is skipped when the central exists but has no turbine (bus<=0)."""
    central_parser = _make_hydro_parser()
    # Dam1 exists in central_parser with bus=0, so no turbine is created for it.
    cenre_parser = MockCenreParser(
        [
            {
                "name": "Dam1",
                "reservoir": "Dam1",
                "mean_production_factor": 1.2,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]
    # Dam1 has bus=0 so no turbine was created — efficiency entry must be skipped
    assert result["reservoir_production_factor_array"] == []


# ── Ocean-junction ("RAPEL_ocean") tests ─────────────────────────────────────

_RAPEL_CENTRAL = {
    "name": "RAPEL",
    "number": 63,
    "type": "embalse",
    "bus": 177,
    "pmin": 0,
    "pmax": 362.0,
    "vert_min": 0,
    "vert_max": 6000.0,
    "efficiency": 0.9,
    "ser_hid": 0,  # no downstream generation junction → ocean fix required
    "ser_ver": 0,  # no downstream spillway junction  → ocean fix required
    "afluent": 0.0,
    "vol_ini": 285.0326,
    "vol_fin": 285.0326,
    "emin": 0.0,
    "emax": 563.2124,
}


def _rapel_parser() -> MockCentralParser:
    """Return a MockCentralParser containing only the RAPEL embalse central."""
    return MockCentralParser([_RAPEL_CENTRAL])


# ELTORO is the never-drain sentinel (``_DRAIN_KILLED_RESERVOIRS``): it gets
# NO reservoir storage-drain column, so its surplus must leave through the
# modelled topology.  With ``ser_ver = 0`` that exit is now collapsed onto
# ELTORO's OWN source-junction drain column (``drain = True`` +
# ``drain_capacity = VertMax``) instead of a ``_ver`` arc to a synthetic
# ``ELTORO_ocean`` node.  Every OTHER embalse drains via the reservoir
# storage column and likewise emits no ``_ver`` arc — see the
# ``_spillway_fields`` tests above.  We reuse the RAPEL geometry under the
# ELTORO name to exercise that spill-topology path.
_ELTORO_CENTRAL = {**_RAPEL_CENTRAL, "name": "ELTORO", "number": 37}


def _eltoro_parser() -> MockCentralParser:
    """Return a MockCentralParser containing only the ELTORO embalse central."""
    return MockCentralParser([_ELTORO_CENTRAL])


def test_embalse_ocean_junction_created():
    """ELTORO (never-drain, bus>0, ser_hid/ser_ver=0): spill-exit self-drain.

    ELTORO has NO reservoir storage-drain column (never-drain sentinel),
    so the spillway (``ser_ver = 0``, ``VertMax > 0``) must dispose of
    surplus water at a genuine basin exit.  Rather than synthesise an
    ``ELTORO_ocean`` drain junction + an ``ELTORO_ver`` arc, the spill is
    collapsed onto ELTORO's OWN junction drain column (``drain = True`` +
    ``drain_capacity = VertMax``).  ZERO synthetic ocean junctions remain.
    """
    writer = JunctionWriter(central_parser=_eltoro_parser())
    result = writer.to_json_array()[0]

    ocean_junctions = [j for j in result["junction_array"] if "ocean" in j["name"]]
    assert ocean_junctions == []
    eltoro_junction = next(j for j in result["junction_array"] if j["name"] == "ELTORO")
    assert eltoro_junction["drain"] is True


def test_embalse_ocean_junction_waterways_created():
    """ELTORO gen path is on the Turbine; spill rides the source self-drain.

    * gen via ``Turbine.junction_a`` (built-in waterway, no gen ocean arc)
    * spill collapsed onto ELTORO's own junction drain column
      (``drain_capacity = VertMax``) — no ``_ver → ocean`` arc, no ocean node.
    """
    writer = JunctionWriter(central_parser=_eltoro_parser(), options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # No spillway waterway terminates at a synthetic ocean drain.
    to_ocean = [
        w for w in result["waterway_array"] if w["junction_b"].endswith("_ocean")
    ]
    assert to_ocean == []

    # The ELTORO source junction IS the spill-exit drain, carrying VertMax.
    eltoro_junction = next(j for j in result["junction_array"] if j["name"] == "ELTORO")
    assert eltoro_junction["drain"] is True
    assert eltoro_junction["drain_capacity"] == pytest.approx(6000.0)
    assert "drain_cost" not in eltoro_junction


def test_embalse_ocean_junction_turbine_created():
    """The terminal embalse turbine uses ``Turbine.junction_a`` (built-in waterway).

    No synthetic ``<central>_ocean`` Junction, no ``<central>_gen``
    Waterway — the Turbine debits the embalse's own junction directly
    and drains terminal-style (``junction_b`` unset).
    """
    writer = JunctionWriter(central_parser=_rapel_parser())
    result = writer.to_json_array()[0]

    assert len(result["turbine_array"]) == 1
    turbine = result["turbine_array"][0]
    assert turbine["name"] == "RAPEL"
    assert turbine["generator"] == "RAPEL"
    assert turbine["production_factor"] == pytest.approx(0.9)
    assert turbine["junction_a"] == "RAPEL"
    assert "waterway" not in turbine
    assert "junction_b" not in turbine


def test_embalse_ocean_junction_enables_efficiency():
    """With the ocean junction fix, efficiency curves are applied to RAPEL."""
    cenre_parser = MockCenreParser(
        [
            {
                "name": "RAPEL",
                "reservoir": "RAPEL",
                "mean_production_factor": 1.2,
                "segments": [
                    {"volume": 100.0, "slope": 0.001, "constant": 0.9},
                    {"volume": 300.0, "slope": 0.0005, "constant": 1.1},
                ],
            }
        ]
    )
    writer = JunctionWriter(central_parser=_rapel_parser(), cenre_parser=cenre_parser)
    result = writer.to_json_array()[0]

    # Efficiency IS applied now — in system-level array
    pfac_arr = result["reservoir_production_factor_array"]
    assert len(pfac_arr) == 1
    eff = pfac_arr[0]
    assert eff["mean_production_factor"] == pytest.approx(1.2)
    assert len(eff["segments"]) == 2


def test_embalse_ocean_junction_no_warning(caplog):
    """No WARNING is emitted for RAPEL after the ocean-junction fix."""
    cenre_parser = MockCenreParser(
        [
            {
                "name": "RAPEL",
                "reservoir": "RAPEL",
                "mean_production_factor": 1.2,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=_rapel_parser(), cenre_parser=cenre_parser)
    with caplog.at_level(logging.WARNING, logger="plp2gtopt.junction_writer"):
        writer.to_json_array()
    rapel_warnings = [
        r
        for r in caplog.records
        if r.levelno == logging.WARNING and "RAPEL" in r.message
    ]
    assert rapel_warnings == [], f"Unexpected WARNING for RAPEL: {rapel_warnings}"


def test_embalse_with_bus_zero_self_drains_but_no_turbine():
    """Reservoir-only embalse (bus=0) sheds its surplus through its own
    source-junction drain — no synthetic ocean node, no turbine.

    Dam1 has bus=0, ser_hid=0, ser_ver=0.  Its spill rides the reservoir
    storage-drain column (it is a DRAINED reservoir) and its terminal gen
    path (no downstream consumer) collapses onto the Dam1 junction's own
    drain.  Neither a ``Dam1_ocean`` junction nor a ``Dam1_gen`` waterway
    is emitted.
    """
    writer = JunctionWriter(central_parser=_make_hydro_parser())
    result = writer.to_json_array()[0]

    # No synthetic ocean drain junction is created.
    dam1_ocean = [j for j in result["junction_array"] if j["name"] == "Dam1_ocean"]
    assert dam1_ocean == []

    # The Dam1 source junction itself drains (terminal, self-drain).
    dam1 = next(j for j in result["junction_array"] if j["name"] == "Dam1")
    assert dam1["drain"] is True

    # No gen waterway to ocean is emitted — the gen path is self-drained.
    dam1_gen_ww = [
        w for w in result["waterway_array"] if w["name"].startswith("Dam1_gen")
    ]
    assert dam1_gen_ww == []

    # No turbine for Dam1 (bus<=0)
    dam1_turbines = [t for t in result["turbine_array"] if t["name"] == "Dam1"]
    assert dam1_turbines == []


def test_embalse_no_ver_waterway_junction_is_drain():
    """ELTORO (never-drain, ser_ver==0) spills via its own source drain.

    Pre-86616b80 the spillway (ser_ver=0) was modelled by ``drain=True``
    on the central's own junction with no bound or cost — that free-drain
    shortcut caused LMAULE / RALCO cascade infeasibilities.  The fix that
    followed routed the spill via an explicit ``_ver`` arc to a synthetic
    ``<central>_ocean`` drain.  That synthetic node is now collapsed back
    onto the source junction: ELTORO is marked ``drain = True`` with the
    spill cap ported onto ``drain_capacity = VertMax`` (mass-conserving —
    only the surplus drains; productive water still leaves via the Turbine
    built-in waterway).  No ``_ver`` arc and no ``ELTORO_ocean`` node are
    emitted.  ELTORO is the never-drain SENTINEL only w.r.t. the reservoir
    STORAGE-drain column (it has none); its junction-level spill drain is
    the intended exit.
    """
    writer = JunctionWriter(central_parser=_eltoro_parser(), options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # ELTORO sheds its spill through its own source-junction drain.
    eltoro_junction = next(j for j in result["junction_array"] if j["name"] == "ELTORO")
    assert eltoro_junction["drain"] is True
    assert eltoro_junction["drain_capacity"] == pytest.approx(6000.0)
    # drain_cost is omitted when the test parser has no plpmat (no CVert).
    assert "drain_cost" not in eltoro_junction

    # No ``_ver`` arc and no synthetic ocean junction are emitted.
    ver_ww = [w for w in result["waterway_array"] if w["name"].startswith("ELTORO_ver")]
    assert ver_ww == []
    ocean = [j for j in result["junction_array"] if j["name"].endswith("_ocean")]
    assert ocean == []


def test_embalse_drained_reservoir_has_drain_column_and_no_ver_arc():
    """A DRAINED reservoir (any embalse except ELTORO) carries its spill on
    the reservoir storage-drain column and emits NO ``_ver`` arc / ocean
    junction — the double-escape path is removed."""
    writer = JunctionWriter(central_parser=_rapel_parser(), options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # The drain is on the reservoir element (spillway_cost == 0, capacity
    # omitted → C++ +6000 default).
    rapel = next(r for r in result["reservoir_array"] if r["name"] == "RAPEL")
    assert rapel.get("spillway_cost") == 0.0
    assert "spillway_capacity" not in rapel

    # No ``_ver`` arc and no synthetic ocean junction for RAPEL.
    rapel_ver = [
        w for w in result["waterway_array"] if w["name"].startswith("RAPEL_ver")
    ]
    assert rapel_ver == []
    rapel_ocean = [j for j in result["junction_array"] if j["name"] == "RAPEL_ocean"]
    assert rapel_ocean == []


def test_embalse_with_ver_waterway_junction_not_drain():
    """Embalse with a real ser_ver connection is NOT a drain junction."""
    central = {
        "name": "CIPRESES",
        "number": 50,
        "type": "embalse",
        "bus": 100,
        "pmin": 0,
        "pmax": 200.0,
        "vert_min": 0,
        "vert_max": 1000.0,
        "efficiency": 0.85,
        "ser_hid": 51,  # has downstream gen junction
        "ser_ver": 52,  # has downstream spillway junction
        "afluent": 0.0,
        "vol_ini": 100.0,
        "vol_fin": 100.0,
        "emin": 0.0,
        "emax": 500.0,
    }
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]),
        options=_LEGACY_OPTS,
    )
    result = writer.to_json_array()[0]

    cipreses_junction = next(
        j for j in result["junction_array"] if j["name"] == "CIPRESES"
    )
    assert cipreses_junction["drain"] is False
    # No ocean junction should be created (ser_hid != 0)
    ocean_junctions = [j for j in result["junction_array"] if "ocean" in j["name"]]
    assert ocean_junctions == []


def test_efficiency_debug_for_bus_zero_central(caplog):
    """DEBUG (not WARNING) is emitted when central has bus<=0 (reservoir-only)."""
    central_parser = _make_hydro_parser()
    # Dam1 exists in central_parser with bus=0
    cenre_parser = MockCenreParser(
        [
            {
                "name": "Dam1",
                "reservoir": "Dam1",
                "mean_production_factor": 1.2,
                "segments": [],
            }
        ]
    )
    writer = JunctionWriter(central_parser=central_parser, cenre_parser=cenre_parser)
    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.junction_writer"):
        writer.to_json_array()
    # Should be DEBUG, not WARNING
    warning_records = [
        r
        for r in caplog.records
        if r.levelno == logging.WARNING and "Dam1" in r.message
    ]
    assert warning_records == [], (
        f"Unexpected WARNING for bus<=0 central: {warning_records}"
    )


# ─── Isolated central tests (bus<=0, ser_hid=0, ser_ver=0) ──────────────


def test_isolated_serie_bus_zero_skipped():
    """Serie central with bus=0, ser_hid=0, ser_ver=0 produces no elements."""
    central = {
        "name": "IsolatedSerie",
        "number": 99,
        "type": "serie",
        "bus": 0,
        "pmin": 0,
        "pmax": 10,
        "vert_min": 0,
        "vert_max": 0,
        "efficiency": 1.0,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 5.0,
    }
    writer = JunctionWriter(central_parser=MockCentralParser([central]))
    result = writer.to_json_array()[0]
    assert not result["junction_array"]
    assert not result["waterway_array"]
    assert not result["flow_array"]
    assert not result["turbine_array"]


def test_isolated_pasada_bus_zero_skipped():
    """Pasada central with bus=0 is not included in hydro mode."""
    central = {
        "name": "IsolatedPasada",
        "number": 88,
        "type": "pasada",
        "bus": 0,
        "pmin": 0,
        "pmax": 5,
        "vert_min": 0,
        "vert_max": 0,
        "efficiency": 1.0,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 3.0,
    }
    writer = JunctionWriter(
        central_parser=MockCentralParser([central]),
        options={"pasada_hydro": True},
    )
    # Pasada bus<=0 filtered in items selection, so no items → empty result
    result = writer.to_json_array()
    assert not result


def test_serie_bus_zero_with_ser_hid_not_skipped():
    """Serie with bus=0 but ser_hid>0 is NOT skipped (part of hydro cascade)."""
    centrals = [
        {
            "name": "CascadeSerie",
            "number": 70,
            "type": "serie",
            "bus": 0,
            "pmin": 0,
            "pmax": 50,
            "vert_min": 0,
            "vert_max": 0,
            "efficiency": 0.9,
            "ser_hid": 71,  # connects to another junction
            "ser_ver": 72,
            "afluent": 10.0,
        },
        {
            "name": "DownstreamEmbalse",
            "number": 71,
            "type": "embalse",
            "bus": 100,
            "pmin": 0,
            "pmax": 200,
            "vert_min": 0,
            "vert_max": 0,
            "efficiency": 1.5,
            "ser_hid": 0,
            "ser_ver": 0,
            "afluent": 0.0,
            "vol_ini": 500,
            "vol_fin": 400,
            "emin": 100,
            "emax": 1000,
        },
    ]
    writer = JunctionWriter(central_parser=MockCentralParser(centrals))
    result = writer.to_json_array()[0]
    # CascadeSerie should create a junction (not skipped)
    names = {j["name"] for j in result["junction_array"]}
    assert "CascadeSerie" in names


def test_embalse_bus_zero_never_skipped():
    """Embalse with bus=0 is never skipped (always creates reservoir)."""
    central = {
        "name": "DamOnly",
        "number": 50,
        "type": "embalse",
        "bus": 0,
        "pmin": 0,
        "pmax": 0,
        "vert_min": 0,
        "vert_max": 0,
        "efficiency": 0.0,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
        "vol_ini": 100,
        "vol_fin": 80,
        "emin": 10,
        "emax": 200,
    }
    writer = JunctionWriter(central_parser=MockCentralParser([central]))
    result = writer.to_json_array()[0]
    # Embalse always creates junction + reservoir
    assert len(result["junction_array"]) >= 1
    assert len(result["reservoir_array"]) == 1


# Tests for _spillway_fields — spillway_cost / spillway_capacity logic.
#
# EVERY reservoir gets its storage-drain (spillway) column ENABLED EXCEPT
# the never-drain sentinels in ``_DRAIN_KILLED_RESERVOIRS`` (currently
# ``ELTORO``), mirroring plexos2gtopt's ``build_reservoir_array``.  For a
# drained reservoir ``_spillway_fields`` emits ``spillway_cost = 0.0``
# (free — a non-zero drain cost produces spurious negative marginal prices)
# and *omits* ``spillway_capacity`` (so the C++ ReservoirLP applies its
# finite +6000 m³/s default and the drain gate
# ``drain_cost.has_value() && capacity > 0`` is satisfied).  The spill is
# carried solely by this reservoir-spillway column — no draining ``_ver``
# waterway is emitted.  ELTORO gets no spillway at all (``{}`` → both
# fields omitted → C++ drain gate stays closed) so it keeps its ``_ver`` /
# ocean spill path and acts as a double-escape correctness sentinel.
# Membership in plpvrebemb.dat, the CVert plpmat default, the water-value
# resolver and ``--vrebemb-as-sink`` no longer affect the drain.


class _MockVrebembParser:
    """Minimal stand-in for VrebembParser in spillway tests."""

    def __init__(self, costs: Dict[str, float]):
        self._costs = costs

    def get_cost(self, name: str) -> Optional[float]:
        return self._costs.get(name)


class _MockPlpmatParser:
    """Minimal stand-in for PlpmatParser exposing only ``vert_cost``."""

    def __init__(self, vert_cost: float):
        self.vert_cost = vert_cost


def _make_jw(
    *,
    vrebemb: Optional[_MockVrebembParser] = None,
    plpmat: Optional[_MockPlpmatParser] = None,
) -> JunctionWriter:
    """Build a JunctionWriter wired only with the parsers under test."""
    return JunctionWriter(
        central_parser=MockCentralParser([]),
        vrebemb_parser=vrebemb,
        plpmat_parser=plpmat,
    )


def test_spillway_activates_drain_at_zero_cost():
    """Any reservoir except a never-drain sentinel → activate the drain.

    Emit ``spillway_cost = 0.0`` (free) and omit ``spillway_capacity`` so
    the C++ ReservoirLP applies its finite +6000 m³/s default and adds a
    per-block drain column.  Cost is 0, NOT the vrebemb Costo de Rebalse.
    """
    jw = _make_jw(vrebemb=_MockVrebembParser({"LMAULE": 5000.0}))
    fields = jw._spillway_fields("LMAULE")
    assert fields == {"spillway_cost": 0.0}
    assert "spillway_capacity" not in fields


def test_spillway_drain_enabled_for_non_vrebemb_reservoir():
    """A reservoir NOT in plpvrebemb.dat ALSO gets the drain (cost 0)."""
    jw = _make_jw(plpmat=_MockPlpmatParser(vert_cost=0.01))
    fields = jw._spillway_fields("RUNOFRIVER")
    assert fields == {"spillway_cost": 0.0}
    assert "spillway_capacity" not in fields


def test_spillway_eltoro_never_drains():
    """ELTORO is the never-drain sentinel → no spillway/drain at all.

    Both ``spillway_cost`` and ``spillway_capacity`` are omitted (``{}``)
    even though it IS listed in plpvrebemb.dat in the CEN data — the
    membership no longer matters; the sentinel set wins.  ELTORO keeps its
    ``_ver`` / ocean spill path so its water still has somewhere to go.
    """
    jw = _make_jw(vrebemb=_MockVrebembParser({"ELTORO": 5000.0}))
    fields = jw._spillway_fields("ELTORO")
    assert not fields
    # Case-insensitive sentinel match.
    assert not jw._spillway_fields("eltoro")


def test_spillway_no_plpmat_still_drains():
    """No plpmat / no vrebemb membership → drain still on (cost 0)."""
    jw = _make_jw()
    fields = jw._spillway_fields("RUNOFRIVER")
    assert fields == {"spillway_cost": 0.0}


def test_spillway_cost_is_always_zero_not_rebalse():
    """The drain cost is always 0; the vrebemb Costo de Rebalse is ignored."""
    jw = _make_jw(
        vrebemb=_MockVrebembParser({"LMAULE": 5000.0}),
        plpmat=_MockPlpmatParser(vert_cost=0.01),
    )
    fields = jw._spillway_fields("LMAULE")
    assert fields == {"spillway_cost": 0.0}


# ── Drain is sentinel-driven, independent of resolver / sink flags ──────────
# The reservoir storage-drain is now the spill mechanism for every
# reservoir except the never-drain sentinels, so neither
# ``--auto-water-fail-cost`` nor ``--vrebemb-as-sink`` change it — both
# flags acted on the now-removed ``_ver`` waterway path, not on the
# reservoir spillway column.


class _MockWaterValueResolver:
    """Mock the small subset of WaterValueResolver consumed by spillway code."""

    def __init__(self, *, is_active: bool):
        self.is_active = is_active


def test_spillway_drain_unaffected_by_resolver_active():
    """A reservoir keeps its zero-cost drain even when the resolver is on."""
    jw = _make_jw(vrebemb=_MockVrebembParser({"LMAULE": 5000.0}))
    jw._water_value_resolver = _MockWaterValueResolver(is_active=True)
    fields = jw._spillway_fields("LMAULE")
    assert fields == {"spillway_cost": 0.0}


def test_spillway_eltoro_no_drain_with_resolver_active():
    """ELTORO → still no drain regardless of resolver."""
    jw = _make_jw(
        vrebemb=_MockVrebembParser({"ELTORO": 5000.0}),
        plpmat=_MockPlpmatParser(vert_cost=0.01),
    )
    jw._water_value_resolver = _MockWaterValueResolver(is_active=True)
    fields = jw._spillway_fields("ELTORO")
    assert not fields


def test_spillway_drain_unaffected_by_vrebemb_as_sink():
    """--vrebemb-as-sink no longer changes the reservoir spillway column."""
    jw = _make_jw(vrebemb=_MockVrebembParser({"LMAULE": 5000.0}))
    jw._vrebemb_as_sink = True
    fields = jw._spillway_fields("LMAULE")
    assert fields == {"spillway_cost": 0.0}


def test_spillway_eltoro_no_drain_under_vrebemb_as_sink():
    """--vrebemb-as-sink does not give ELTORO a spillway."""
    jw = _make_jw(vrebemb=_MockVrebembParser({"ELTORO": 5000.0}))
    jw._vrebemb_as_sink = True
    fields = jw._spillway_fields("ELTORO")
    assert not fields


# ── _process_cenpmax tests (plpcenpmax.dat → ReservoirProductionFactor) ──────


class _MockCenpmaxParser:
    """Minimal stand-in for CenpmaxParser in _process_cenpmax tests.

    Exposes the ``pmax_curves`` attribute consumed by the writer without
    requiring a real plpcenpmax.dat file on disk.
    """

    def __init__(self, pmax_curves: List[Dict[str, Any]]):
        self.pmax_curves = pmax_curves


def _make_ralco_parser() -> MockCentralParser:
    """Return a MockCentralParser with a RALCO-like embalse + turbine.

    The reservoir sits on its own junction (``RALCO_dam``) and the turbine
    (``RALCO``) drains it via a generation waterway.  RALCO's PotMax=690 MW
    and efficiency=1.518 MW/(m³/s) yield a physical flow cap of
    690 / 1.518 = 454.5 m³/s.
    """
    return MockCentralParser(
        [
            {
                "name": "RALCO_dam",
                "number": 10,
                "type": "embalse",
                "bus": 0,
                "pmin": 0,
                "pmax": 0,
                "vert_min": 0,
                "vert_max": 50,
                "efficiency": 0.0,
                "ser_hid": 11,  # feeds RALCO turbine
                "ser_ver": 0,
                "afluent": 0.0,
                "vol_ini": 500.0,
                "vol_fin": 450.0,
                "emin": 100.0,
                "emax": 1000.0,
            },
            {
                "name": "RALCO",
                "number": 11,
                "type": "serie",
                "bus": 20,
                "pmin": 0,
                "pmax": 690.0,
                "vert_min": 0,
                "vert_max": 0,
                "efficiency": 1.518,
                "ser_hid": 12,
                "ser_ver": 0,
                "afluent": 0.0,
            },
        ]
    )


def test_cenpmax_emits_production_factor():
    """Cenpmax curve emits a scaled PF entry and pins waterway fmax."""
    central_parser = _make_ralco_parser()
    cenpmax = _MockCenpmaxParser(
        [
            {
                "name": "RALCO",
                "reservoir": "RALCO_dam",
                "segments": [
                    {"volume": 409.4, "slope": 0.4616, "constant": 339.83},
                    {"volume": 480.0, "slope": 0.3734, "constant": 382.17},
                ],
            }
        ]
    )
    writer = JunctionWriter(
        central_parser=central_parser,
        cenpmax_parser=cenpmax,
    )
    result = writer.to_json_array()[0]

    # One PF entry emitted from the cenpmax curve
    pfac_arr = result["reservoir_production_factor_array"]
    assert len(pfac_arr) == 1
    pf = pfac_arr[0]
    assert pf["turbine"] == "RALCO"
    assert pf["reservoir"] == "RALCO_dam"
    assert pf["name"] == "RALCO_dam_pmax_pfac_1"
    assert pf["mean_production_factor"] == pytest.approx(1.518)

    # Physical flow cap = 690 / 1.518 = 454.5 m³/s
    flow_ref = 690.0 / 1.518
    assert pf["segments"][0]["volume"] == pytest.approx(409.4)
    assert pf["segments"][0]["slope"] == pytest.approx(0.4616 / flow_ref)
    assert pf["segments"][0]["constant"] == pytest.approx(339.83 / flow_ref)
    assert pf["segments"][1]["volume"] == pytest.approx(480.0)
    assert pf["segments"][1]["slope"] == pytest.approx(0.3734 / flow_ref)
    assert pf["segments"][1]["constant"] == pytest.approx(382.17 / flow_ref)

    # Built-in Turbine waterway: the physical flow cap is pinned onto the
    # Turbine's ``capacity`` (no separate gen Waterway exists).
    turbine = next(t for t in result["turbine_array"] if t["name"] == "RALCO")
    assert "waterway" not in turbine
    assert turbine["capacity"] == pytest.approx(flow_ref)


def test_cenpmax_missing_central_skips(caplog):
    """Cenpmax entry for an unknown central is skipped with a warning."""
    central_parser = _make_ralco_parser()
    cenpmax = _MockCenpmaxParser(
        [
            {
                "name": "GHOST",
                "reservoir": "RALCO_dam",
                "segments": [
                    {"volume": 0.0, "slope": 0.1, "constant": 1.0},
                ],
            }
        ]
    )
    writer = JunctionWriter(
        central_parser=central_parser,
        cenpmax_parser=cenpmax,
    )
    with caplog.at_level(logging.WARNING):
        result = writer.to_json_array()[0]
    assert result["reservoir_production_factor_array"] == []
    assert any("GHOST" in rec.getMessage() for rec in caplog.records)


def test_cenpmax_zero_pot_max_skips(caplog):
    """Central with pmax=0 or efficiency=0 is skipped (flow_ref undefined)."""
    centrals = MockCentralParser(
        [
            {
                "name": "RALCO_dam",
                "number": 10,
                "type": "embalse",
                "bus": 0,
                "pmin": 0,
                "pmax": 0,
                "vert_min": 0,
                "vert_max": 50,
                "efficiency": 0.0,
                "ser_hid": 11,
                "ser_ver": 0,
                "afluent": 0.0,
                "vol_ini": 500.0,
                "vol_fin": 450.0,
                "emin": 100.0,
                "emax": 1000.0,
            },
            {
                "name": "BROKEN",
                "number": 11,
                "type": "serie",
                "bus": 20,
                "pmin": 0,
                "pmax": 0.0,  # zero pmax → skip
                "vert_min": 0,
                "vert_max": 0,
                "efficiency": 1.5,
                "ser_hid": 12,
                "ser_ver": 0,
                "afluent": 0.0,
            },
        ]
    )
    cenpmax = _MockCenpmaxParser(
        [
            {
                "name": "BROKEN",
                "reservoir": "RALCO_dam",
                "segments": [{"volume": 0.0, "slope": 0.1, "constant": 1.0}],
            }
        ]
    )
    writer = JunctionWriter(
        central_parser=centrals,
        cenpmax_parser=cenpmax,
    )
    with caplog.at_level(logging.WARNING):
        result = writer.to_json_array()[0]
    assert result["reservoir_production_factor_array"] == []
    assert any("BROKEN" in rec.getMessage() for rec in caplog.records)


def test_cenpmax_no_turbine_skips():
    """Central with bus<=0 has no turbine; cenpmax entry is skipped silently."""
    centrals = MockCentralParser(
        [
            {
                "name": "RALCO_dam",
                "number": 10,
                "type": "embalse",
                "bus": 0,  # no turbine → cannot pin waterway fmax
                "pmin": 0,
                "pmax": 100.0,
                "vert_min": 0,
                "vert_max": 50,
                "efficiency": 1.5,
                "ser_hid": 0,
                "ser_ver": 0,
                "afluent": 0.0,
                "vol_ini": 500.0,
                "vol_fin": 450.0,
                "emin": 100.0,
                "emax": 1000.0,
            },
        ]
    )
    cenpmax = _MockCenpmaxParser(
        [
            {
                "name": "RALCO_dam",
                "reservoir": "RALCO_dam",
                "segments": [{"volume": 0.0, "slope": 0.1, "constant": 1.0}],
            }
        ]
    )
    writer = JunctionWriter(
        central_parser=centrals,
        cenpmax_parser=cenpmax,
    )
    result = writer.to_json_array()[0]
    assert result["reservoir_production_factor_array"] == []


def test_cenpmax_coexists_with_cenre():
    """When plpcenre + plpcenpmax both define a PF curve, they are merged
    into a single MIN-envelope curve (not emitted as two separate entries).
    """
    central_parser = _make_ralco_parser()
    cenre = MockCenreParser(
        [
            {
                "name": "RALCO",
                "reservoir": "RALCO_dam",
                "mean_production_factor": 1.518,
                "segments": [
                    {"volume": 0.0, "slope": 0.0003, "constant": 1.2},
                ],
            }
        ]
    )
    cenpmax = _MockCenpmaxParser(
        [
            {
                "name": "RALCO",
                "reservoir": "RALCO_dam",
                "segments": [
                    {"volume": 409.4, "slope": 0.4616, "constant": 339.83},
                ],
            }
        ]
    )
    writer = JunctionWriter(
        central_parser=central_parser,
        cenre_parser=cenre,
        cenpmax_parser=cenpmax,
    )
    result = writer.to_json_array()[0]
    pfac_arr = result["reservoir_production_factor_array"]
    # MIN envelope merge: exactly one entry replacing both sources
    assert len(pfac_arr) == 1
    assert pfac_arr[0]["reservoir"] == "RALCO_dam"


# ────────────────────────────────────────────────────────────────────────────
# PLP "no limit" sentinel handling on PotMax / VertMax
# (re-introduced + extended from commit 8d1fff9b after the 86616b80 refactor;
# see junction_writer.py: _is_plp_no_limit, _PLP_NO_LIMIT_SENTINEL).
# ────────────────────────────────────────────────────────────────────────────


def test_is_plp_no_limit_helper():
    """The threshold predicate matches the documented 9000 m³/s cutoff.

    Real Chilean spillway / generator caps are at most a few thousand m³/s,
    so anything ≥ 9000 is treated as the PLP "essentially unbounded"
    sentinel and must be stripped from emitted waterway bounds.
    """
    from ..junction_writer import _is_plp_no_limit, _PLP_NO_LIMIT_SENTINEL

    assert _PLP_NO_LIMIT_SENTINEL == 9000.0

    # Below threshold — real bounds, kept as-is
    assert not _is_plp_no_limit(0.0)
    assert not _is_plp_no_limit(1.0)
    assert not _is_plp_no_limit(8999.99)

    # At and above threshold — sentinel, dropped
    assert _is_plp_no_limit(9000.0)
    assert _is_plp_no_limit(9999.0)  # most common PLP value
    assert _is_plp_no_limit(10042.0)  # 9000 + central_index pattern
    assert _is_plp_no_limit(99999.0)  # alternate PLP no-limit form


def _make_central_with_pmax_vertmax(name, pmax, vert_max, ser_hid=2, ser_ver=2):
    """Helper: build a central dict with controllable PotMax/VertMax."""
    return {
        "name": name,
        "number": 1,
        "bus": 101,
        "pmin": 0,
        "pmax": pmax,
        "vert_min": 0,
        "vert_max": vert_max,
        "efficiency": 1.0,  # gen_fmax = pmax / efficiency = pmax exactly
        "ser_hid": ser_hid,
        "ser_ver": ser_ver,
        "afluent": 10.0,
        "type": "serie",
    }


def test_pot_max_sentinel_dropped_on_gen_waterway():
    """When PotMax >= 9000 the synthesised gen waterway must drop fmax.

    Without this, a literal `gen_fmax = PotMax / Rendi = 9999 m³/s` upper
    bound is baked into the LP, inflating matrix kappa and yielding false
    binding-bound duals during SDDP cuts.
    """
    central = _make_central_with_pmax_vertmax(
        "PlantSentinel", pmax=9999.0, vert_max=50.0
    )
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser)
    result = writer.to_json_array()[0]

    # The generation flow is carried by the built-in Turbine; a sentinel
    # PotMax must leave the Turbine's ``capacity`` unset (not 9999) so no
    # literal cap is baked into the LP.
    turbine = next(t for t in result["turbine_array"] if t["name"] == "PlantSentinel")
    assert "waterway" not in turbine
    assert "capacity" not in turbine, (
        f"expected capacity dropped, got {turbine.get('capacity')!r}"
    )

    # Counter must reflect the normalisation
    assert writer._plp_no_limit_count >= 1


def test_vert_max_sentinel_dropped_on_ver_waterway():
    """When VertMax >= 9000 the spill (`_ver`) waterway must drop fmax.

    Pins the regression introduced by commit 86616b80 (which removed
    `_is_vert_sentinel` from the non-rebalse `_ver` branch when refactoring
    to physical fcost on _ver arcs).  Re-fixed in this change.
    """
    central = _make_central_with_pmax_vertmax(
        "PlantVertSentinel", pmax=50.0, vert_max=9999.0
    )
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    ver_ww = next(
        ww
        for ww in result["waterway_array"]
        if "_ver_" in ww["name"] and ww["junction_a"] == "PlantVertSentinel"
    )
    assert ver_ww.get("fmax") is None, (
        f"expected fmax dropped, got {ver_ww.get('fmax')!r}"
    )
    assert writer._plp_no_limit_count >= 1


def test_real_bound_under_threshold_preserved():
    """Below-threshold PotMax/VertMax must be passed through unchanged.

    Contrapositive sanity: 8999 m³/s is a legitimate (if uncommon) cap and
    must NOT be dropped.  Guards against an over-broad threshold change.
    """
    central = _make_central_with_pmax_vertmax(
        "PlantRealCap", pmax=8999.0, vert_max=8999.0
    )
    central_parser = MockCentralParser([central])
    writer = JunctionWriter(central_parser=central_parser, options=_LEGACY_OPTS)
    result = writer.to_json_array()[0]

    # The built-in Turbine carries NO generation cap: gen_fmax = PotMax/Rendi
    # is redundant with the generator pmax via the conversion row
    # (gen = rate*flow  =>  flow <= pmax/rate), so the turbine flow is left
    # unbounded (like a regular waterway flow).  The genuine spill cap stays
    # on the ``_ver`` Waterway ``fmax`` — that below-sentinel bound is what
    # this threshold test actually guards.
    turbine = next(t for t in result["turbine_array"] if t["name"] == "PlantRealCap")
    ver_ww = next(
        ww
        for ww in result["waterway_array"]
        if "_ver_" in ww["name"] and ww["junction_a"] == "PlantRealCap"
    )
    assert turbine.get("capacity") is None
    # gen_fmax = pmax / efficiency = 8999 / 1.0 = 8999, below the 9000 PLP
    # "no limit" sentinel, so the spill cap is preserved on the _ver waterway.
    assert ver_ww.get("fmax") == pytest.approx(8999.0)
    assert writer._plp_no_limit_count == 0


def test_plp_no_limit_log_emitted(caplog):
    """The end-of-run info log fires once per conversion when count > 0.

    Single log line per ``to_json_array`` so the user sees how many spurious
    bounds were dropped without a per-central spam.
    """
    centrals = [
        _make_central_with_pmax_vertmax("A", pmax=9999.0, vert_max=50.0),
        _make_central_with_pmax_vertmax("B", pmax=9999.0, vert_max=9999.0),
    ]
    central_parser = MockCentralParser(centrals)
    writer = JunctionWriter(central_parser=central_parser)

    with caplog.at_level(logging.INFO, logger="plp2gtopt.junction_writer"):
        writer.to_json_array()

    # Must contain exactly one line summarising the normalisations
    matching = [
        rec for rec in caplog.records if "PLP 'no limit' sentinel" in rec.message
    ]
    assert len(matching) == 1, f"expected exactly 1 summary log, got {len(matching)}"
    assert ">= 9000" in matching[0].message
    # Counter at end of run is the sum of normalisations across both centrals
    assert writer._plp_no_limit_count >= 2
