# -*- coding: utf-8 -*-

"""Unit tests for the irrigation coupling resolution in the converter.

The couplings reference physical gtopt elements (turbines, waterways,
junctions, diversion FlowRights, ReservoirSeepage).  A missing match
must FAIL the conversion loudly — a silent fallback would leave the
agreement quietly un-anchored (floating rights, fictional deliveries).
"""

from typing import Any, Dict

import pytest

from plp2gtopt._writer_hydro import HydroMixin


class _Writer(HydroMixin):
    """Minimal harness exposing the coupling helpers."""

    def __init__(self, system: Dict[str, Any]):
        self.planning = {"system": system, "simulation": {}}
        self._water_rights_options: Dict[str, Any] = {}


class TestAnchorFlowRef:
    def test_flow_mode_turbine(self):
        w = _Writer({"turbine_array": [{"name": "ELTORO", "junction_a": "X"}]})
        assert w._anchor_flow_ref("ELTORO") == "turbine('ELTORO').flow"

    def test_waterway_connected_turbine(self):
        w = _Writer(
            {
                "turbine_array": [{"name": "COLBUN", "waterway": "COLBUN_ww"}],
                "waterway_array": [{"name": "COLBUN_ww", "uid": 7}],
            }
        )
        assert w._anchor_flow_ref("COLBUN") == "waterway('COLBUN_ww').flow"

    def test_gen_waterway_prefix_and_exact(self):
        w = _Writer({"waterway_array": [{"name": "LMAULE_gen_1_2"}]})
        assert w._anchor_flow_ref("LMAULE") == "waterway('LMAULE_gen_1_2').flow"
        w2 = _Writer({"waterway_array": [{"name": "LMAULE_gen"}]})
        assert w2._anchor_flow_ref("LMAULE") == "waterway('LMAULE_gen').flow"

    def test_case_insensitive(self):
        w = _Writer({"waterway_array": [{"name": "LMaule_gen_1_2"}]})
        assert w._anchor_flow_ref("LMAULE") == "waterway('LMaule_gen_1_2').flow"

    def test_spill_arcs_never_match(self):
        w = _Writer({"waterway_array": [{"name": "LMAULE_ver_1_2"}]})
        assert w._anchor_flow_ref("LMAULE") is None


class TestRequireIrrigationRefs:
    def test_missing_anchor_raises(self):
        w = _Writer({})
        cfg: Dict[str, Any] = {"districts": []}
        with pytest.raises(ValueError, match="ELTORO.*silently"):
            w._require_irrigation_refs(cfg, "laja", {"anchor_turbinado_ref": "ELTORO"})

    def test_missing_district_offtake_raises(self):
        w = _Writer({})
        cfg: Dict[str, Any] = {
            "anchor_turbinado_ref": "turbine('ELTORO').flow",
            "districts": [{"name": "RieTucapel"}],
        }
        with pytest.raises(ValueError, match="RieTucapel.*fictional"):
            w._require_irrigation_refs(cfg, "laja", {"anchor_turbinado_ref": "ELTORO"})

    def test_resolved_refs_pass(self):
        w = _Writer({})
        cfg: Dict[str, Any] = {
            "anchor_turbinado_ref": "turbine('ELTORO').flow",
            "districts": [
                {
                    "name": "RieSaltos",
                    "anchor_flow_right": "RieSaltos_irrigation_right",
                },
                {"name": "RieTucapel", "anchor_junction": "RieTucapel"},
            ],
        }
        w._require_irrigation_refs(cfg, "laja", {"anchor_turbinado_ref": "ELTORO"})

    def test_disabled_couplings_skip_validation(self):
        """--no-irrigation-couplings: no injections, no validation —
        the legacy uncoupled shape is an explicit user choice."""
        w = _Writer({})
        w._water_rights_options = {"irrigation_couplings": False}
        cfg: Dict[str, Any] = {"districts": [{"name": "RieTucapel"}]}
        assert w._irrigation_couplings_enabled(cfg) is False
        # All granular keys were forced off.
        assert cfg["enable_physical_anchoring"] is False


class TestDistrictAnchors:
    def test_diversion_preferred_over_junction(self):
        w = _Writer(
            {
                "flow_right_array": [{"name": "RieSaltos_irrigation_right"}],
                "junction_array": [{"name": "RieSaltos"}],
            }
        )
        cfg: Dict[str, Any] = {"districts": [{"name": "RieSaltos"}]}
        w._inject_district_anchors(cfg)
        d = cfg["districts"][0]
        assert d["anchor_flow_right"] == "RieSaltos_irrigation_right"
        assert "anchor_junction" not in d

    def test_junction_fallback(self):
        w = _Writer({"junction_array": [{"name": "RieTucapel"}]})
        cfg: Dict[str, Any] = {"districts": [{"name": "RieTucapel"}]}
        w._inject_district_anchors(cfg)
        assert cfg["districts"][0]["anchor_junction"] == "RieTucapel"

    def test_injection_switches_diversion_to_carry(self):
        entity: Dict[str, Any] = {"name": "RieSaltos_irrigation_right"}
        w = _Writer({"flow_right_array": [entity]})
        cfg: Dict[str, Any] = {
            "districts": [{"name": "RieSaltos", "injection": "LAJA_I"}]
        }
        w._inject_district_anchors(cfg)
        assert entity["junction_b"] == "LAJA_I"
        assert entity["consumptive"] is False
