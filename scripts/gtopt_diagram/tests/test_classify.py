# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_diagram._classify -- pure-logic helpers.

Covers:
- Generator classification (classify_gen)
- Turbine/efficiency reference collectors
- Element naming (elem_name)
- Scalar formatting (scalar)
- Array lookup (resolve)
- Icon key mapping (icon_key_for_type)
- Dominant kind (dominant_kind)
- Generator pmax extraction (gen_pmax)
- GEN_TYPE_META constant
"""

from __future__ import annotations

import pytest

from gtopt_diagram._classify import (
    GEN_TYPE_META,
    classify_gen,
    dominant_kind,
    efficiency_turbine_pairs,
    elem_name,
    gen_pmax,
    icon_key_for_type,
    resolve,
    scalar,
    turbine_gen_refs,
    turbine_waterway_refs,
)


# ---------------------------------------------------------------------------
# classify_gen
# ---------------------------------------------------------------------------


class TestClassifyGen:
    """Generator type classification."""

    def test_hydro_by_turbine_uid(self):
        gen = {"uid": 1, "name": "G1"}
        assert classify_gen(gen, turb_refs={1}) == "hydro"

    def test_hydro_by_turbine_name(self):
        gen = {"uid": 99, "name": "MyGen"}
        assert classify_gen(gen, turb_refs={"MyGen"}) == "hydro"

    def test_solar_by_name_solar(self):
        assert classify_gen({"name": "Solar_PV_1"}, set()) == "solar"

    def test_solar_by_name_pv(self):
        assert classify_gen({"name": "pv_farm_3"}, set()) == "solar"

    def test_solar_by_name_foto(self):
        assert classify_gen({"name": "FOTOVOLTAICA"}, set()) == "solar"

    def test_solar_by_name_cspv(self):
        assert classify_gen({"name": "CSPV_PLANT"}, set()) == "solar"

    def test_wind_by_name_wind(self):
        assert classify_gen({"name": "Wind_Farm_1"}, set()) == "wind"

    def test_wind_by_name_eol(self):
        assert classify_gen({"name": "EOLICA_SUR"}, set()) == "wind"

    def test_wind_by_name_eolico(self):
        assert classify_gen({"name": "eolico_norte"}, set()) == "wind"

    def test_wind_by_name_eolico_accent(self):
        assert classify_gen({"name": "E\u00f3lico_1"}, set()) == "wind"

    def test_wind_by_name_aerog(self):
        assert classify_gen({"name": "AEROG_3"}, set()) == "wind"

    def test_battery_by_name_bat(self):
        assert classify_gen({"name": "BAT_STORAGE"}, set()) == "battery"

    def test_battery_by_name_bess(self):
        assert classify_gen({"name": "BESS_01"}, set()) == "battery"

    def test_battery_by_name_ess(self):
        assert classify_gen({"name": "ESS_FACILITY"}, set()) == "battery"

    def test_battery_by_name_storage(self):
        assert classify_gen({"name": "grid_storage"}, set()) == "battery"

    def test_battery_by_name_almac(self):
        assert classify_gen({"name": "ALMACENAMIENTO"}, set()) == "battery"

    def test_thermal_default(self):
        assert classify_gen({"name": "CC_NORTE", "uid": 5}, set()) == "thermal"

    def test_missing_name_defaults_thermal(self):
        assert classify_gen({"uid": 10}, set()) == "thermal"

    def test_empty_dict_defaults_thermal(self):
        assert classify_gen({}, set()) == "thermal"

    def test_turbine_ref_takes_priority_over_name(self):
        """A generator named 'solar' but referenced by a turbine is hydro."""
        gen = {"uid": 7, "name": "solar_gen"}
        assert classify_gen(gen, turb_refs={7}) == "hydro"


# ---------------------------------------------------------------------------
# gen_pmax
# ---------------------------------------------------------------------------


class TestGenPmax:
    """Extract effective pmax from generator dicts."""

    def test_scalar_pmax(self):
        assert gen_pmax({"pmax": 250}) == 250.0

    def test_scalar_capacity_fallback(self):
        assert gen_pmax({"capacity": 100.5}) == 100.5

    def test_pmax_takes_priority_over_capacity(self):
        assert gen_pmax({"pmax": 200, "capacity": 100}) == 200.0

    def test_list_pmax(self):
        assert gen_pmax({"pmax": [10, 20, 30]}) == 30.0

    def test_nested_list_pmax(self):
        assert gen_pmax({"pmax": [[5, 10], [15, 20]]}) == 20.0

    def test_empty_list_returns_zero(self):
        assert gen_pmax({"pmax": []}) == 0.0

    def test_none_pmax_returns_zero(self):
        assert gen_pmax({"pmax": None}) == 0.0

    def test_missing_pmax_returns_zero(self):
        assert gen_pmax({}) == 0.0

    def test_zero_pmax_falls_through_to_capacity(self):
        """pmax=0 is falsy, so capacity is used as fallback."""
        assert gen_pmax({"pmax": 0, "capacity": 50}) == 50.0

    def test_string_pmax_returns_zero(self):
        assert gen_pmax({"pmax": "unknown"}) == 0.0


# ---------------------------------------------------------------------------
# elem_name
# ---------------------------------------------------------------------------


class TestElemName:
    """Element naming with NAME:UID format."""

    def test_name_and_uid(self):
        assert elem_name({"name": "ELTORO", "uid": 2}) == "ELTORO:2"

    def test_name_equals_uid_string(self):
        """When name and uid stringify the same, no duplication."""
        assert elem_name({"name": "2", "uid": 2}) == "2"

    def test_name_only(self):
        assert elem_name({"name": "B1"}) == "B1"

    def test_uid_only(self):
        assert elem_name({"uid": 3}) == "3"

    def test_empty_dict(self):
        assert elem_name({}) == "?"

    def test_none_name_and_uid(self):
        assert elem_name({"name": None, "uid": None}) == "?"

    def test_integer_name(self):
        assert elem_name({"name": 42, "uid": 42}) == "42"

    def test_integer_name_different_uid(self):
        assert elem_name({"name": 42, "uid": 7}) == "42:7"


# ---------------------------------------------------------------------------
# scalar
# ---------------------------------------------------------------------------


class TestScalar:
    """Scalar formatting for display."""

    def test_none_returns_em_dash(self):
        assert scalar(None) == "\u2014"

    def test_int(self):
        assert scalar(100) == "100"
        assert scalar(0) == "0"

    def test_whole_float(self):
        assert scalar(100.0) == "100"
        assert scalar(500.0) == "500"

    def test_float_two_decimals(self):
        assert scalar(100.5) == "100.50"
        assert scalar(25.123456) == "25.12"

    def test_float_rounding(self):
        assert scalar(0.005) == "0.01"

    def test_very_small_float(self):
        assert scalar(0.0025) == "0.00"

    def test_list_range(self):
        result = scalar([10.0, 100.12345])
        assert result == "10\u2026100.12"
        assert "100.12345" not in result

    def test_list_identical_values(self):
        assert scalar([42.0, 42.0]) == "42"

    def test_list_empty(self):
        assert scalar([]) == "\u2014"

    def test_nested_list(self):
        result = scalar([[1, 2], [3, 4]])
        assert result == "1\u20264"

    def test_string_quoted(self):
        assert scalar("parquet") == '"parquet"'

    def test_other_type(self):
        """Non-standard types fall through to str()."""
        assert scalar({"key": 1}) == "{'key': 1}"


# ---------------------------------------------------------------------------
# resolve
# ---------------------------------------------------------------------------


class TestResolve:
    """Array lookup by uid or name."""

    def test_find_by_uid(self):
        arr = [{"uid": 1, "name": "A"}, {"uid": 2, "name": "B"}]
        assert resolve(arr, 2) == {"uid": 2, "name": "B"}

    def test_find_by_name(self):
        arr = [{"uid": 1, "name": "A"}, {"uid": 2, "name": "B"}]
        assert resolve(arr, "A") == {"uid": 1, "name": "A"}

    def test_not_found(self):
        arr = [{"uid": 1, "name": "A"}]
        assert resolve(arr, 99) is None

    def test_empty_array(self):
        assert resolve([], "anything") is None

    def test_first_match_returned(self):
        arr = [{"uid": 1, "name": "X"}, {"uid": 1, "name": "Y"}]
        assert resolve(arr, 1)["name"] == "X"


# ---------------------------------------------------------------------------
# turbine_gen_refs / turbine_waterway_refs / efficiency_turbine_pairs
# ---------------------------------------------------------------------------


class TestTurbineRefs:
    """Turbine reference extraction from system dicts."""

    _SYS = {
        "turbine_array": [
            {"uid": 1, "generator": "G1", "waterway": "W1"},
            {"uid": 2, "generator": "G2", "waterway": "W2"},
            {"uid": 3, "waterway": "W3"},  # no generator
        ],
        "reservoir_production_factor_array": [
            {"uid": 1, "turbine": 1},
            {"uid": 2, "turbine": 2},
        ],
    }

    def test_turbine_gen_refs(self):
        refs = turbine_gen_refs(self._SYS)
        assert refs == {"G1", "G2"}

    def test_turbine_gen_refs_empty(self):
        assert turbine_gen_refs({}) == set()

    def test_turbine_waterway_refs(self):
        refs = turbine_waterway_refs(self._SYS)
        assert refs == {"W1", "W2", "W3"}

    def test_turbine_waterway_refs_empty(self):
        assert turbine_waterway_refs({}) == set()

    def test_efficiency_turbine_pairs(self):
        refs = efficiency_turbine_pairs(self._SYS)
        assert refs == {1, 2}

    def test_efficiency_turbine_pairs_empty(self):
        assert efficiency_turbine_pairs({}) == set()

    def test_efficiency_turbine_pairs_no_key(self):
        assert efficiency_turbine_pairs({"turbine_array": []}) == set()


# ---------------------------------------------------------------------------
# icon_key_for_type
# ---------------------------------------------------------------------------


class TestIconKeyForType:
    """Map generator types to palette/icon keys."""

    @pytest.mark.parametrize(
        "gen_type,expected",
        [
            ("hydro", "gen_hydro"),
            ("solar", "gen_solar"),
            ("wind", "gen_solar"),
            ("battery", "battery"),
            ("nuclear", "gen"),
            ("gas", "gen"),
            ("thermal", "gen"),
        ],
    )
    def test_known_types(self, gen_type, expected):
        assert icon_key_for_type(gen_type) == expected

    def test_unknown_type_defaults_to_gen(self):
        assert icon_key_for_type("geothermal") == "gen"

    def test_empty_string_defaults_to_gen(self):
        assert icon_key_for_type("") == "gen"


# ---------------------------------------------------------------------------
# dominant_kind
# ---------------------------------------------------------------------------


class TestDominantKind:
    """Find the dominant palette-key from a list of generator types."""

    def test_single_hydro(self):
        assert dominant_kind(["hydro"]) == "gen_hydro"

    def test_single_solar(self):
        assert dominant_kind(["solar"]) == "gen_solar"

    def test_hydro_beats_solar(self):
        """Hydro has higher priority than solar."""
        assert dominant_kind(["solar", "hydro"]) == "gen_hydro"

    def test_all_thermal(self):
        assert dominant_kind(["thermal", "thermal"]) == "gen"

    def test_battery(self):
        assert dominant_kind(["battery"]) == "battery"

    def test_wind(self):
        assert dominant_kind(["wind"]) == "gen_solar"

    def test_empty_list(self):
        assert dominant_kind([]) == "gen"

    def test_unknown_types_only(self):
        assert dominant_kind(["geothermal", "tidal"]) == "gen"

    def test_mixed_with_unknown(self):
        assert dominant_kind(["unknown", "solar", "unknown"]) == "gen_solar"


# ---------------------------------------------------------------------------
# GEN_TYPE_META constant
# ---------------------------------------------------------------------------


class TestGenTypeMeta:
    """Verify the GEN_TYPE_META constant structure."""

    def test_all_known_types_present(self):
        for key in ("hydro", "solar", "wind", "battery", "thermal"):
            assert key in GEN_TYPE_META

    def test_tuple_structure(self):
        for key, val in GEN_TYPE_META.items():
            assert isinstance(val, tuple), f"{key} value is not a tuple"
            assert len(val) == 3, f"{key} tuple has {len(val)} elements, expected 3"
            label, icon, palette_key = val
            assert isinstance(label, str)
            assert isinstance(icon, str)
            assert isinstance(palette_key, str)

    def test_hydro_values(self):
        label, _icon, palette_key = GEN_TYPE_META["hydro"]
        assert label == "Hydro"
        assert palette_key == "gen_hydro"

    def test_thermal_values(self):
        label, _icon, palette_key = GEN_TYPE_META["thermal"]
        assert label == "Thermal"
        assert palette_key == "gen"
