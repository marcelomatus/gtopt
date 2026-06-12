# -*- coding: utf-8 -*-

"""Unit tests for :mod:`gtopt_expand.pumped_storage_expand`."""

from __future__ import annotations

import json

import pytest

from gtopt_expand.pumped_storage_expand import (
    GEN_PF_MINTEC,
    GEN_PF_NOMINAL,
    GEN_PNOM_MW,
    PUMP_FACTOR,
    PUMP_PMAX_MW,
    default_config,
    expand_pumped_storage,
    expand_pumped_storage_from_file,
)


def _colbun_reservoir(emin: float = 1000.0, emax: float = 10000.0) -> dict:
    return {
        "uid": 1,
        "name": "COLBUN",
        "junction": "COLBUN",
        "emin": emin,
        "emax": emax,
    }


def _hb_maule_cfg(**overrides):
    cfg = default_config(name="hb_maule", vmin=1000.0, vmax=10000.0)
    cfg.update(overrides)
    return cfg


class TestExpandPumpedStorage:
    def test_emits_all_entity_arrays(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
        )
        assert set(fragment) == {
            "generator_array",
            "demand_array",
            "waterway_array",
            "turbine_array",
            "pump_array",
            "reservoir_production_factor_array",
        }
        assert len(fragment["generator_array"]) == 1
        assert len(fragment["demand_array"]) == 1
        assert len(fragment["waterway_array"]) == 2
        assert len(fragment["turbine_array"]) == 1
        assert len(fragment["pump_array"]) == 1
        assert len(fragment["reservoir_production_factor_array"]) == 1

    def test_name_drives_all_element_names(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(name="other_ps"),
            reservoir_names={"MACHICURA"},
        )
        assert fragment["generator_array"][0]["name"] == "hydro_other_ps"
        assert fragment["demand_array"][0]["name"] == "pump_load_other_ps"
        assert fragment["waterway_array"][0]["name"] == "ww_other_ps_gen"
        assert fragment["waterway_array"][1]["name"] == "ww_other_ps_pump"
        assert fragment["turbine_array"][0]["name"] == "tur_other_ps"
        assert fragment["pump_array"][0]["name"] == "pump_other_ps"
        assert fragment["reservoir_production_factor_array"][0]["name"] == (
            "rpf_other_ps"
        )

    def test_name_arg_overrides_cfg_name(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(name="from_cfg"),
            name="from_arg",
            reservoir_names={"MACHICURA"},
        )
        assert fragment["turbine_array"][0]["name"] == "tur_from_arg"

    def test_missing_name_raises(self):
        cfg = _hb_maule_cfg()
        cfg.pop("name")
        with pytest.raises(ValueError, match="requires a 'name'"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_missing_upper_reservoir_raises(self):
        cfg = _hb_maule_cfg()
        cfg.pop("upper_reservoir")
        with pytest.raises(ValueError, match="upper_reservoir"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_missing_lower_reservoir_raises(self):
        cfg = _hb_maule_cfg()
        cfg.pop("lower_reservoir")
        with pytest.raises(ValueError, match="lower_reservoir"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_missing_pump_dict_raises(self):
        cfg = _hb_maule_cfg()
        cfg.pop("pump")
        with pytest.raises(ValueError, match="gen_nominal"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_template_values_are_used(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
        )
        gen = fragment["generator_array"][0]
        dem = fragment["demand_array"][0]
        pump = fragment["pump_array"][0]
        tur = fragment["turbine_array"][0]

        assert gen["capacity"] == GEN_PNOM_MW == 70.0
        assert dem["capacity"] == PUMP_PMAX_MW == 75.0
        assert pump["pump_factor"] == PUMP_FACTOR == 1.88
        assert tur["production_factor"] == GEN_PF_NOMINAL == 1.44

    def test_waterways_are_antiparallel(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
        )
        gen_ww, pump_ww = fragment["waterway_array"]
        assert gen_ww["junction_a"] == "COLBUN"
        assert gen_ww["junction_b"] == "MACHICURA"
        assert pump_ww["junction_a"] == "MACHICURA"
        assert pump_ww["junction_b"] == "COLBUN"
        assert gen_ww["fmax"] == 49.0
        assert pump_ww["fmax"] == 40.0

    def test_production_factor_segments(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(vmin=1000.0, vmax=10000.0),
            reservoir_names={"MACHICURA"},
        )
        rpf = fragment["reservoir_production_factor_array"][0]
        assert rpf["turbine"] == "tur_hb_maule"
        assert rpf["reservoir"] == "COLBUN"
        assert rpf["mean_production_factor"] == GEN_PF_NOMINAL
        segs = rpf["segments"]
        assert len(segs) == 2
        assert segs[0]["volume"] == 1000.0
        assert segs[0]["constant"] == GEN_PF_MINTEC == 1.36
        assert segs[1]["volume"] == 10000.0
        assert segs[1]["constant"] == GEN_PF_NOMINAL == 1.44
        assert segs[1]["slope"] == 0.0
        expected_slope = (GEN_PF_NOMINAL - GEN_PF_MINTEC) / (10000.0 - 1000.0)
        assert segs[0]["slope"] == pytest.approx(expected_slope)

    def test_lower_reservoir_missing_raises(self):
        with pytest.raises(ValueError, match="MACHICURA"):
            expand_pumped_storage(_hb_maule_cfg(), reservoir_names=set())

    def test_lower_reservoir_from_reservoir_array(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoirs=[
                _colbun_reservoir(),
                {"uid": 2, "name": "MACHICURA"},
            ],
        )
        assert fragment["pump_array"][0]["name"] == "pump_hb_maule"

    def test_vmin_vmax_fall_back_to_reservoir_entry(self):
        cfg = _hb_maule_cfg(vmin=0.0, vmax=0.0)
        fragment = expand_pumped_storage(
            cfg,
            reservoirs=[_colbun_reservoir(emin=500.0, emax=5000.0)],
            reservoir_names={"MACHICURA"},
        )
        segs = fragment["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 500.0
        assert segs[1]["volume"] == 5000.0

    def test_missing_upper_entry_raises_without_vmin_vmax(self):
        cfg = _hb_maule_cfg(vmin=0.0, vmax=0.0)
        with pytest.raises(ValueError, match="COLBUN"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_bus_defaults_to_lowercased_upper_reservoir(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
        )
        assert fragment["generator_array"][0]["bus"] == "colbun"
        assert fragment["demand_array"][0]["bus"] == "colbun"

    def test_cfg_bus_overrides_default(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(bus="grid_bus"),
            reservoir_names={"MACHICURA"},
        )
        assert fragment["generator_array"][0]["bus"] == "grid_bus"

    def test_explicit_bus_name_is_honoured(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
            bus_name="sic_bus",
        )
        assert fragment["generator_array"][0]["bus"] == "sic_bus"

    def test_invalid_span_raises(self):
        cfg = _hb_maule_cfg(vmin=5000.0, vmax=5000.0)
        with pytest.raises(ValueError, match="invalid capacity span"):
            expand_pumped_storage(cfg, reservoir_names={"MACHICURA"})

    def test_uids_are_contiguous_from_uid_start(self):
        fragment = expand_pumped_storage(
            _hb_maule_cfg(),
            reservoir_names={"MACHICURA"},
            uid_start=42,
        )
        uids = [
            fragment["generator_array"][0]["uid"],
            fragment["demand_array"][0]["uid"],
            fragment["waterway_array"][0]["uid"],
            fragment["waterway_array"][1]["uid"],
            fragment["turbine_array"][0]["uid"],
            fragment["pump_array"][0]["uid"],
            fragment["reservoir_production_factor_array"][0]["uid"],
        ]
        assert uids == [42, 43, 44, 45, 46, 47, 48]

    def test_generic_plant_naming(self):
        """A completely different plant — names, reservoirs, values."""
        cfg = {
            "name": "chapiquina",
            "upper_reservoir": "UPPER_LAKE",
            "lower_reservoir": "LOWER_LAKE",
            "vmin": 100.0,
            "vmax": 500.0,
            "gen_nominal": {
                "pmax_mw": 50.0,
                "qmax_m3s": 30.0,
                "production_factor": 1.67,
            },
            "gen_mintec": {
                "pmin_mw": 20.0,
                "qmin_m3s": 15.0,
                "production_factor": 1.5,
            },
            "pump": {"pmax_mw": 55.0, "qmax_m3s": 25.0, "pump_factor": 2.2},
            "pump_efficiency": 0.9,
            "bus": "north_bus",
        }
        fragment = expand_pumped_storage(
            cfg, reservoir_names={"UPPER_LAKE", "LOWER_LAKE"}
        )
        assert fragment["turbine_array"][0]["name"] == "tur_chapiquina"
        assert fragment["turbine_array"][0]["main_reservoir"] == "UPPER_LAKE"
        assert fragment["waterway_array"][0]["junction_a"] == "UPPER_LAKE"
        assert fragment["waterway_array"][1]["junction_a"] == "LOWER_LAKE"
        assert fragment["generator_array"][0]["bus"] == "north_bus"
        assert fragment["pump_array"][0]["efficiency"] == 0.9


class TestDefaultConfig:
    def test_mirrors_pump_pdf_table(self):
        cfg = default_config(name="hb_maule", vmin=1000.0, vmax=10000.0)
        assert cfg["name"] == "hb_maule"
        assert cfg["upper_reservoir"] == "COLBUN"
        assert cfg["lower_reservoir"] == "MACHICURA"
        assert cfg["vmin"] == 1000.0
        assert cfg["vmax"] == 10000.0
        assert cfg["gen_nominal"] == {
            "pmax_mw": 70.0,
            "qmax_m3s": 49.0,
            "production_factor": 1.44,
        }
        assert cfg["gen_mintec"] == {
            "pmin_mw": 34.0,
            "qmin_m3s": 25.0,
            "production_factor": 1.36,
        }
        assert cfg["pump"] == {
            "pmax_mw": 75.0,
            "qmax_m3s": 40.0,
            "pump_factor": 1.88,
        }
        assert cfg["pump_efficiency"] == 0.85

    def test_default_name_is_generic(self):
        cfg = default_config()
        assert cfg["name"] == "pumped_storage"


class TestExpandPumpedStorageFromFile:
    def test_reads_canonical_config(self, tmp_path):
        cfg = default_config(name="hb_maule", vmin=2000.0, vmax=8000.0)
        path = tmp_path / "hb_maule.json"
        path.write_text(json.dumps(cfg), encoding="utf-8")

        fragment = expand_pumped_storage_from_file(path, reservoir_names={"MACHICURA"})
        segs = fragment["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 2000.0
        assert segs[1]["volume"] == 8000.0
        assert fragment["turbine_array"][0]["name"] == "tur_hb_maule"

    def test_name_falls_back_to_filename_stem(self, tmp_path):
        cfg = default_config(vmin=1000.0, vmax=10000.0)
        cfg.pop("name")
        path = tmp_path / "my_pumped_unit.json"
        path.write_text(json.dumps(cfg), encoding="utf-8")

        fragment = expand_pumped_storage_from_file(path, reservoir_names={"MACHICURA"})
        assert fragment["turbine_array"][0]["name"] == "tur_my_pumped_unit"

    def test_non_object_config_raises(self, tmp_path):
        path = tmp_path / "bad.json"
        path.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
        with pytest.raises(ValueError, match="expected a JSON object"):
            expand_pumped_storage_from_file(path, reservoir_names={"MACHICURA"})
