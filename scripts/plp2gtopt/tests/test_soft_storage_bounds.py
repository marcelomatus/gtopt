"""Unit tests for the ``--soft-storage-bounds`` emission path.

When the option is enabled, ``JunctionWriter._apply_soft_storage_bounds``:

  1. sets ``reservoir.efin_cost`` from the per-reservoir cost cascade
     (``plpvrebemb.dat`` → global ``CVert`` → CLI default → fallback
     1000.0);
  2. routes the per-stage maintenance ``emin`` schedule
     (``plpmanem.dat``) into ``soft_emin`` / ``soft_emin_cost``,
     leaving the static ``emin`` box floor in place — *unless*
     ``plpminembh.dat`` already populated ``soft_emin``, in which case
     the manem path is skipped.

When the option is disabled (``--no-soft-storage-bounds``), neither
``efin_cost`` nor any soft re-routing happens.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

import numpy as np

from ..junction_writer import JunctionWriter
from ..central_parser import CentralParser
from ..manem_parser import ManemParser
from ..stage_parser import StageParser
from ..vrebemb_parser import VrebembParser


# ─── Mocks ────────────────────────────────────────────────────────────────


class MockCentralParser(CentralParser):
    """Mock CentralParser with one reservoir."""

    def __init__(self, centrals: List[Dict[str, Any]]):
        super().__init__("dummy.dat")
        self._centrals = centrals
        self.centrals_of_type: Dict[str, List[Dict[str, Any]]] = {}
        for central in centrals:
            ctype = central.get("type", "embalse")
            self.centrals_of_type.setdefault(ctype, []).append(central)

    def get_all(self) -> List[Dict[str, Any]]:
        return self._centrals


class MockStageParser(StageParser):
    """Mock StageParser with a fixed stage count."""

    def __init__(self, num: int):
        super().__init__("dummy.dat")
        self._num = num

    @property
    def num_stages(self) -> int:
        return self._num


class MockManemParser(ManemParser):
    """Mock ManemParser holding a single reservoir's per-stage emin/emax."""

    def __init__(self, entry: Optional[Dict[str, Any]] = None):
        super().__init__("dummy.dat")
        self._entry = entry

    def get_manem_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        if self._entry and self._entry.get("name") == name:
            return self._entry
        return None


class MockVrebembParser(VrebembParser):
    """Mock VrebembParser with a single per-reservoir spill cost."""

    def __init__(self, costs: Optional[Dict[str, float]] = None):
        super().__init__("dummy.dat")
        self._costs = costs or {}

    def get_cost(self, name: str) -> float | None:
        return self._costs.get(name)


class MockPlpmatParser:
    """Minimal stub exposing only ``vert_cost`` (the ``CVert`` field)."""

    def __init__(self, vert_cost: float = 0.0):
        self.vert_cost = vert_cost


# ─── Fixtures ─────────────────────────────────────────────────────────────


def _make_reservoir_central(
    name: str = "RsvA",
    *,
    vol_fin: float = 100.0,
    emin: float = 50.0,
    emax: float = 200.0,
) -> Dict[str, Any]:
    return {
        "name": name,
        "number": 10,
        "type": "embalse",
        "bus": 0,
        "pmin": 0,
        "pmax": 100,
        "vert_min": 0,
        "vert_max": 50,
        "efficiency": 0,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
        "vol_ini": 100.0,
        "vol_fin": vol_fin,
        "emin": emin,
        "emax": emax,
    }


def _make_manem_entry(name: str, num_stages: int) -> Dict[str, Any]:
    """Per-stage emin schedule: stage 1 → 60, stage 3 → 80, others → 0."""
    stages = np.array([1, 3], dtype=np.int32)
    emins = np.array([60.0, 80.0], dtype=np.float64)
    emax_arr = np.array([200.0, 200.0], dtype=np.float64)
    _ = num_stages
    return {
        "name": name,
        "stage": stages,
        "emin": emins,
        "emax": emax_arr,
    }


# ─── Tests ────────────────────────────────────────────────────────────────


def test_soft_storage_bounds_disabled_no_efin_cost_no_soft_routing():
    """Without the flag, neither efin_cost nor manem→soft_emin happens."""
    central = _make_reservoir_central()
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        manem_parser=MockManemParser(_make_manem_entry("RsvA", num_stages=4)),
        stage_parser=MockStageParser(num=4),
        vrebemb_parser=MockVrebembParser({"RsvA": 250.0}),
        options={"soft_storage_bounds": False},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert "efin_cost" not in reservoir
    # Manem-driven soft_emin is NOT set when the flag is off — even if
    # the manem schedule itself populates the hard emin field via the
    # parquet column.
    assert "soft_emin" not in reservoir
    assert "soft_emin_cost" not in reservoir


def test_soft_storage_bounds_emits_efin_cost_from_vrebemb():
    """vrebemb cost is the highest-priority source."""
    central = _make_reservoir_central(vol_fin=120.0)
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        stage_parser=MockStageParser(num=4),
        vrebemb_parser=MockVrebembParser({"RsvA": 250.0}),
        plpmat_parser=MockPlpmatParser(vert_cost=999.0),  # ignored
        options={"soft_storage_bounds": True},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert reservoir.get("efin") == 120.0
    assert reservoir.get("efin_cost") == 250.0


def test_soft_storage_bounds_falls_back_to_cvert_when_no_vrebemb():
    """When vrebemb is absent, plpmat.vert_cost wins."""
    central = _make_reservoir_central(vol_fin=120.0)
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        stage_parser=MockStageParser(num=4),
        plpmat_parser=MockPlpmatParser(vert_cost=42.0),
        options={"soft_storage_bounds": True},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert reservoir.get("efin_cost") == 42.0


def test_soft_storage_bounds_falls_back_to_cli_when_no_files():
    """No vrebemb, no CVert → CLI ``soft_emin_cost`` default wins."""
    central = _make_reservoir_central(vol_fin=120.0)
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        stage_parser=MockStageParser(num=4),
        options={"soft_storage_bounds": True, "soft_emin_cost": 7.5},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert reservoir.get("efin_cost") == 7.5


def test_soft_storage_bounds_uses_hard_fallback_when_all_sources_zero():
    """All sources zero → fixed 1000.0 fallback."""
    central = _make_reservoir_central(vol_fin=120.0)
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        stage_parser=MockStageParser(num=4),
        plpmat_parser=MockPlpmatParser(vert_cost=0.0),
        options={"soft_storage_bounds": True, "soft_emin_cost": 0.0},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert reservoir.get("efin_cost") == 1000.0


def test_soft_storage_bounds_skips_efin_cost_when_efin_zero():
    """No vol_fin → no efin_cost (avoids dead schedule entries)."""
    central = _make_reservoir_central(vol_fin=0.0)
    parser = MockCentralParser([central])
    writer = JunctionWriter(
        central_parser=parser,
        stage_parser=MockStageParser(num=4),
        vrebemb_parser=MockVrebembParser({"RsvA": 250.0}),
        options={"soft_storage_bounds": True},
    )
    [system] = writer.to_json_array()
    [reservoir] = system["reservoir_array"]

    assert "efin_cost" not in reservoir
