# -*- coding: utf-8 -*-

"""Unit tests for :mod:`gtopt_expand.hb_maule_expand`."""

from __future__ import annotations

import pytest

import json

from gtopt_expand.hb_maule_expand import (
    GEN_PF_MINTEC,
    GEN_PF_NOMINAL,
    GEN_PNOM_MW,
    PUMP_FACTOR,
    PUMP_PMAX_MW,
    default_config,
    expand_hb_maule,
    expand_hb_maule_from_file,
)


def _colbun_reservoir(emin: float = 1000.0, emax: float = 10000.0) -> dict:
    return {
        "uid": 1,
        "name": "COLBUN",
        "junction": "COLBUN",
        "emin": emin,
        "emax": emax,
    }


class TestExpandHbMaule:
    def test_emits_all_entity_arrays(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
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

    def test_pdf_table_values_are_used(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
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
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
            reservoir_names={"MACHICURA"},
        )
        gen_ww, pump_ww = fragment["waterway_array"]
        assert gen_ww["junction_a"] == "COLBUN"
        assert gen_ww["junction_b"] == "MACHICURA"
        assert pump_ww["junction_a"] == "MACHICURA"
        assert pump_ww["junction_b"] == "COLBUN"
        assert gen_ww["fmax"] == 49.0
        assert pump_ww["fmax"] == 40.0

    def test_production_factor_segments_match_pdf_table(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir(emin=1000.0, emax=10000.0)],
            reservoir_names={"MACHICURA"},
        )
        rpf = fragment["reservoir_production_factor_array"][0]
        assert rpf["turbine"] == "tur_hb_maule"
        assert rpf["reservoir"] == "COLBUN"
        assert rpf["mean_production_factor"] == GEN_PF_NOMINAL
        segs = rpf["segments"]
        assert len(segs) == 2
        # Low-head anchor: at V=emin, PF=1.36
        assert segs[0]["volume"] == 1000.0
        assert segs[0]["constant"] == GEN_PF_MINTEC == 1.36
        # High-head cap: at V=emax, PF=1.44 (flat)
        assert segs[1]["volume"] == 10000.0
        assert segs[1]["constant"] == GEN_PF_NOMINAL == 1.44
        assert segs[1]["slope"] == 0.0
        # Rising slope: (1.44 - 1.36) / (emax - emin)
        expected_slope = (GEN_PF_NOMINAL - GEN_PF_MINTEC) / (10000.0 - 1000.0)
        assert segs[0]["slope"] == pytest.approx(expected_slope)

    def test_machicura_missing_raises(self):
        with pytest.raises(ValueError, match="MACHICURA"):
            expand_hb_maule(
                reservoirs=[_colbun_reservoir()],
                reservoir_names=set(),
            )

    def test_machicura_from_ror_promoted_names_only(self):
        # Machicura may be RoR-promoted, so appears in reservoir_names
        # without being in the reservoirs list.
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
            reservoir_names={"MACHICURA"},
        )
        assert fragment["pump_array"][0]["name"] == "pump_hb_maule"

    def test_colbun_emin_emax_overridable_via_config(self):
        fragment = expand_hb_maule(
            config={"emin": 500.0, "emax": 5000.0},
            reservoir_names={"MACHICURA"},
        )
        segs = fragment["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 500.0
        assert segs[1]["volume"] == 5000.0

    def test_missing_colbun_raises_without_config_override(self):
        with pytest.raises(ValueError, match="COLBUN"):
            expand_hb_maule(
                reservoirs=[],
                reservoir_names={"MACHICURA"},
            )

    def test_bus_defaults_to_lowercased_upper_reservoir(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
            reservoir_names={"MACHICURA"},
        )
        assert fragment["generator_array"][0]["bus"] == "colbun"
        assert fragment["demand_array"][0]["bus"] == "colbun"

    def test_explicit_bus_name_is_honoured(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
            reservoir_names={"MACHICURA"},
            bus_name="sic_bus",
        )
        assert fragment["generator_array"][0]["bus"] == "sic_bus"

    def test_invalid_colbun_span_raises(self):
        with pytest.raises(ValueError, match="invalid capacity span"):
            expand_hb_maule(
                config={"emin": 5000.0, "emax": 5000.0},
                reservoir_names={"MACHICURA"},
            )

    def test_colbun_vmin_vmax_override_via_config(self):
        """``vmin`` / ``vmax`` (canonical schema) take priority."""
        fragment = expand_hb_maule(
            config={"vmin": 500.0, "vmax": 5000.0},
            reservoir_names={"MACHICURA"},
        )
        segs = fragment["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 500.0
        assert segs[1]["volume"] == 5000.0

    def test_uids_are_contiguous_from_uid_start(self):
        fragment = expand_hb_maule(
            reservoirs=[_colbun_reservoir()],
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


class TestDefaultConfig:
    def test_mirrors_pump_pdf_table(self):
        cfg = default_config(vmin=1000.0, vmax=10000.0)
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

    def test_round_trip_via_canonical_dict(self):
        """``expand_hb_maule(default_config(...))`` == default expansion."""
        cfg = default_config(vmin=1000.0, vmax=10000.0)
        via_cfg = expand_hb_maule(
            config=cfg,
            reservoir_names={"MACHICURA"},
        )
        via_reservoirs = expand_hb_maule(
            reservoirs=[_colbun_reservoir(emin=1000.0, emax=10000.0)],
            reservoir_names={"MACHICURA"},
        )
        assert via_cfg == via_reservoirs


class TestExpandHbMauleFromFile:
    def test_reads_canonical_dat_file(self, tmp_path):
        cfg = default_config(vmin=2000.0, vmax=8000.0)
        dat_path = tmp_path / "hb_maule_dat.json"
        dat_path.write_text(json.dumps(cfg), encoding="utf-8")

        fragment = expand_hb_maule_from_file(
            dat_path,
            reservoir_names={"MACHICURA"},
        )
        segs = fragment["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 2000.0
        assert segs[1]["volume"] == 8000.0

    def test_partial_override_merges_with_defaults(self, tmp_path):
        """Unspecified sub-dicts fall back to pump.pdf values."""
        partial = {
            "vmin": 1000.0,
            "vmax": 10000.0,
            "pump": {"pump_factor": 2.0},  # override only pump factor
        }
        dat_path = tmp_path / "hb_maule_dat.json"
        dat_path.write_text(json.dumps(partial), encoding="utf-8")

        fragment = expand_hb_maule_from_file(
            dat_path,
            reservoir_names={"MACHICURA"},
        )
        pump = fragment["pump_array"][0]
        assert pump["pump_factor"] == 2.0
        # Gen-side still uses PDF defaults
        tur = fragment["turbine_array"][0]
        assert tur["production_factor"] == 1.44
