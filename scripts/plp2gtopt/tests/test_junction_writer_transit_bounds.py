"""Tests for JunctionWriter transit-central waterway bound wiring.

Covers the behaviour added in junction_writer.py that propagates
plpmance.dat per-stage flow envelopes onto gen waterways of transit-only
centrals (``bus == 0``), writing ``Waterway/fmin.parquet`` and
``Waterway/fmax.parquet``.
"""

import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
import pyarrow.parquet as pq

from ..block_parser import BlockParser
from ..central_parser import CentralParser
from ..junction_writer import JunctionWriter
from ..mance_parser import ManceParser


# ---------------------------------------------------------------------------
# Minimal mock helpers
# ---------------------------------------------------------------------------


class MockCentralParser(CentralParser):
    """Minimal CentralParser backed by an explicit list."""

    def __init__(self, centrals: List[Dict[str, Any]]) -> None:
        """Build parser from an explicit list of central dicts.

        Calls ``_append`` for each central so that the BaseParser name/number
        index maps (used by ``get_item_by_name``) are populated.
        """
        super().__init__("dummy.dat")
        self._centrals = centrals
        self.centrals_of_type: Dict[str, List[Dict[str, Any]]] = {}
        for central in centrals:
            self._append(central)
            ctype = central.get("type", "serie")
            self.centrals_of_type.setdefault(ctype, []).append(central)

    def get_all(self) -> List[Dict[str, Any]]:
        """Return all centrals."""
        return self._centrals

    def get_central_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Look up a central by name."""
        for c in self._centrals:
            if c["name"] == name:
                return c
        return None


class MockManceParser(ManceParser):
    """Minimal ManceParser backed by an explicit list of mance dicts."""

    def __init__(self, mances: List[Dict[str, Any]]) -> None:
        """Build parser from an explicit list of mance dicts."""
        super().__init__("dummy.dat")
        for m in mances:
            self._append(m)

    def get_mance_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Return the mance dict whose ``name`` matches, or None."""
        return self.get_item_by_name(name)

    @property
    def mances(self) -> List[Dict[str, Any]]:
        """Return all mance entries."""
        return self.get_all()


class MockBlockParser(BlockParser):
    """Minimal BlockParser backed by an explicit stage-number map."""

    def __init__(self, stage_map: Dict[int, int]) -> None:
        """Build parser from an explicit {block_num: stage_num} dict."""
        super().__init__("dummy.dat")
        self.stage_number_map = stage_map
        for block_num, stage_num in stage_map.items():
            self._append({"number": block_num, "stage": stage_num, "duration": 1.0})

    def get_stage_number(self, block_num: int) -> int:
        """Return the stage number for a block."""
        return self.stage_number_map.get(int(block_num), -1)


# ---------------------------------------------------------------------------
# Shared test data builders
# ---------------------------------------------------------------------------

_TRANSIT_CENTRAL_SER_HID: Dict[str, Any] = {
    "name": "LMAULE",
    "number": 10,
    "bus": 0,
    "pmin": 0.0,
    "pmax": 100.0,
    "vert_min": 0.0,
    "vert_max": 0.0,
    "efficiency": 1.0,
    "ser_hid": 20,  # in-network: gen waterway goes to central 20
    "ser_ver": 0,
    "afluent": 0.0,
    "type": "serie",
}

_DOWNSTREAM_CENTRAL: Dict[str, Any] = {
    "name": "LOSANGELES",
    "number": 20,
    "bus": 101,
    "pmin": 0.0,
    "pmax": 50.0,
    "vert_min": 0.0,
    "vert_max": 0.0,
    "efficiency": 1.0,
    "ser_hid": 0,
    "ser_ver": 0,
    "afluent": 0.0,
    "type": "serie",
}

_MANCE_DATA: Dict[str, Any] = {
    "name": "LMAULE",
    "block": np.array([1, 2], dtype=np.int32),
    "pmin": np.array([50.0, 60.0], dtype=np.float64),
    # Deliberately differs from central pmax (100.0) so the column is not
    # suppressed by _create_dataframe's fill-value deduplication.
    "pmax": np.array([80.0, 90.0], dtype=np.float64),
}

_STAGE_MAP: Dict[int, int] = {1: 1, 2: 2}


def _make_jw(
    *,
    centrals: Optional[List[Dict[str, Any]]] = None,
    mance_entries: Optional[List[Dict[str, Any]]] = None,
    block_map: Optional[Dict[int, int]] = None,
    output_dir: Optional[Path] = None,
) -> JunctionWriter:
    """Construct a JunctionWriter with only the parsers needed for these tests."""
    if centrals is None:
        centrals = [_TRANSIT_CENTRAL_SER_HID, _DOWNSTREAM_CENTRAL]
    mance_parser = MockManceParser(mance_entries) if mance_entries is not None else None
    block_parser = (
        MockBlockParser(block_map or _STAGE_MAP) if mance_entries is not None else None
    )
    options: Dict[str, Any] = {}
    if output_dir is not None:
        options["output_dir"] = output_dir
    return JunctionWriter(
        central_parser=MockCentralParser(centrals),
        mance_parser=mance_parser,
        block_parser=block_parser,
        options=options,
    )


# ---------------------------------------------------------------------------
# Test 1 – Happy path: bus=0 central with mance data
# ---------------------------------------------------------------------------


def test_transit_bus0_with_mance_sets_fmin_fmax_refs() -> None:
    """bus=0 central with mance entry → gen waterway gets ``fmin: "fmin"`` / ``fmax: "fmax"``."""
    jw = _make_jw(mance_entries=[_MANCE_DATA])
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        result = jw.to_json_array()

    system = result[0]
    ww = next(w for w in system["waterway_array"] if "LMAULE_gen" in w["name"])
    assert ww["fmin"] == "fmin", "fmin should be string ref 'fmin'"
    assert ww["fmax"] == "fmax", "fmax should be string ref 'fmax'"


def test_transit_bus0_with_mance_registers_tuple() -> None:
    """bus=0 central with mance data → registry tuple (central_id, ww_uid, name) recorded."""
    jw = _make_jw(mance_entries=[_MANCE_DATA])
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        jw.to_json_array()

    assert len(jw._transit_gen_waterways) == 1
    cid, _ww_uid, name = jw._transit_gen_waterways[0]
    assert cid == 10
    assert name == "LMAULE"


# ---------------------------------------------------------------------------
# Test 2 – bus>0 central: NOT registered even with mance data
# ---------------------------------------------------------------------------


def test_bus_positive_not_registered_even_with_mance() -> None:
    """bus>0 central → gen waterway keeps numeric bounds; registry stays empty."""
    connected_central: Dict[str, Any] = {
        **_TRANSIT_CENTRAL_SER_HID,
        "bus": 101,
    }
    jw = _make_jw(
        centrals=[connected_central, _DOWNSTREAM_CENTRAL],
        mance_entries=[{**_MANCE_DATA, "name": "LMAULE"}],
    )
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        jw.to_json_array()

    assert not jw._transit_gen_waterways
    system = jw.to_json_array()
    ww = next(
        (w for w in system[0]["waterway_array"] if "LMAULE_gen" in w["name"]),
        None,
    )
    if ww is not None:
        assert ww.get("fmin") != "fmin"
        assert ww.get("fmax") != "fmax"


# ---------------------------------------------------------------------------
# Test 3 – bus=0 central WITHOUT mance data: NOT registered
# ---------------------------------------------------------------------------


def test_transit_bus0_without_mance_not_registered() -> None:
    """bus=0 central with no mance entry → NOT registered; numeric bounds kept."""
    jw = _make_jw(mance_entries=[])  # empty mance list
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        jw.to_json_array()

    assert not jw._transit_gen_waterways
    system = jw.to_json_array()
    ww = next(
        (w for w in system[0]["waterway_array"] if "LMAULE_gen" in w["name"]),
        None,
    )
    if ww is not None:
        assert ww.get("fmin") != "fmin"
        assert ww.get("fmax") != "fmax"


# ---------------------------------------------------------------------------
# Test 4 – No mance_parser provided: registry stays empty, no exception
# ---------------------------------------------------------------------------


def test_no_mance_parser_registry_empty_no_exception() -> None:
    """Without mance_parser, registry stays empty and to_json_array does not raise."""
    jw = JunctionWriter(
        central_parser=MockCentralParser(
            [_TRANSIT_CENTRAL_SER_HID, _DOWNSTREAM_CENTRAL]
        ),
        mance_parser=None,
        block_parser=None,
        options={},
    )
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        result = jw.to_json_array()

    assert result is not None
    assert not jw._transit_gen_waterways


# ---------------------------------------------------------------------------
# Test 5 – _write_transit_waterway_bounds no-op cases
# ---------------------------------------------------------------------------


def test_write_bounds_noop_empty_registry() -> None:
    """Empty registry → _write_transit_waterway_bounds is a no-op (no exception)."""
    jw = _make_jw(mance_entries=[_MANCE_DATA])
    # Do not call to_json_array so the registry stays empty
    jw._write_transit_waterway_bounds()  # must not raise


def test_write_bounds_noop_missing_block_parser() -> None:
    """Registry populated but block_parser=None → no-op, no exception."""
    jw = _make_jw(mance_entries=[_MANCE_DATA])
    # Manually inject a registry entry and then remove block_parser
    jw._transit_gen_waterways = [(10, 1, "LMAULE")]
    jw.block_parser = None
    jw._write_transit_waterway_bounds()  # must not raise


def test_write_bounds_noop_missing_mance_parser() -> None:
    """Registry populated but mance_parser=None → no-op, no exception."""
    jw = _make_jw(mance_entries=[_MANCE_DATA])
    jw._transit_gen_waterways = [(10, 1, "LMAULE")]
    jw.mance_parser = None
    jw._write_transit_waterway_bounds()  # must not raise


# ---------------------------------------------------------------------------
# Test 6 – Parquet output: columns renamed from uid:<central_id> to uid:<ww_uid>
# ---------------------------------------------------------------------------


def test_parquet_output_columns_renamed() -> None:
    """Full pipeline: Waterway/fmin.parquet and fmax.parquet exist with renamed cols."""
    with tempfile.TemporaryDirectory() as tmpdir:
        out = Path(tmpdir)
        jw = _make_jw(
            mance_entries=[_MANCE_DATA],
            output_dir=out,
        )
        result = jw.to_json_array()
        system = result[0]

        # Find the gen waterway uid for LMAULE
        ww = next(w for w in system["waterway_array"] if "LMAULE_gen" in w["name"])
        ww_uid = ww["uid"]

        fmin_path = out / "Waterway" / "fmin.parquet"
        fmax_path = out / "Waterway" / "fmax.parquet"
        assert fmin_path.exists(), f"Waterway/fmin.parquet not written to {out}"
        assert fmax_path.exists(), f"Waterway/fmax.parquet not written to {out}"

        tbl_fmin = pq.read_table(fmin_path)
        tbl_fmax = pq.read_table(fmax_path)

        expected_col = f"uid:{ww_uid}"
        assert expected_col in tbl_fmin.column_names, (
            f"Expected column '{expected_col}' in fmin, got {tbl_fmin.column_names}"
        )
        assert expected_col in tbl_fmax.column_names, (
            f"Expected column '{expected_col}' in fmax, got {tbl_fmax.column_names}"
        )

        # Original central-id column must NOT appear
        central_col = f"uid:{_TRANSIT_CENTRAL_SER_HID['number']}"
        assert central_col not in tbl_fmin.column_names
        assert central_col not in tbl_fmax.column_names


# ---------------------------------------------------------------------------
# Test 7 – Ocean-fallback gen waterway (ser_hid=0): registration fires
# ---------------------------------------------------------------------------


def test_ocean_fallback_gen_waterway_registered() -> None:
    """Transit central with ser_hid=0 → ocean gen waterway is also registered."""
    ocean_transit: Dict[str, Any] = {
        "name": "B_LAMINA",
        "number": 30,
        "bus": 0,
        "pmin": 0.0,
        "pmax": 80.0,
        "vert_min": 0.0,
        "vert_max": 0.0,
        "efficiency": 1.0,
        "ser_hid": 0,  # ocean fallback path
        "ser_ver": 0,
        "afluent": 0.0,
        "type": "serie",
    }
    mance_ocean: Dict[str, Any] = {
        "name": "B_LAMINA",
        "block": np.array([1, 2], dtype=np.int32),
        "pmin": np.array([10.0, 20.0], dtype=np.float64),
        "pmax": np.array([80.0, 80.0], dtype=np.float64),
    }

    # B_LAMINA must be referenced by another central so it is not skipped
    # as an isolated serie central (bus<=0, ser_hid=0, ser_ver=0).
    upstream: Dict[str, Any] = {
        "name": "UPSTREAM",
        "number": 31,
        "bus": 200,
        "pmin": 0.0,
        "pmax": 50.0,
        "vert_min": 0.0,
        "vert_max": 0.0,
        "efficiency": 1.0,
        "ser_hid": 30,  # references B_LAMINA
        "ser_ver": 0,
        "afluent": 5.0,
        "type": "serie",
    }

    jw = _make_jw(
        centrals=[ocean_transit, upstream],
        mance_entries=[mance_ocean],
    )
    with tempfile.TemporaryDirectory() as tmpdir:
        jw.options["output_dir"] = Path(tmpdir)
        result = jw.to_json_array()

    assert len(jw._transit_gen_waterways) == 1
    cid, _ww_uid, name = jw._transit_gen_waterways[0]
    assert cid == 30
    assert name == "B_LAMINA"

    system = result[0]
    ww = next(w for w in system["waterway_array"] if "B_LAMINA_gen" in w["name"])
    assert ww["fmin"] == "fmin"
    assert ww["fmax"] == "fmax"
