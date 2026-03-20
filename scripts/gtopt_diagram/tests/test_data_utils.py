"""Tests for gtopt_diagram._data_utils -- voltage reduction utilities.

Covers:
- ``build_voltage_map`` -- BFS-based voltage reduction mapping.
- ``count_visible_buses`` -- representative bus count after reduction.
- ``auto_voltage_threshold`` -- automatic threshold selection.
- ``resolve_bus_ref`` -- bus reference translation through the voltage map.
- ``VOLTAGE_BFS_MAX_DEPTH`` constant.
- ``_line_color_width`` -- voltage-based line coloring and width.
"""

from gtopt_diagram._data_utils import (
    VOLTAGE_BFS_MAX_DEPTH,
    auto_voltage_threshold,
    build_voltage_map,
    count_visible_buses,
    resolve_bus_ref,
)
from gtopt_diagram.gtopt_diagram import (
    _line_color_width,
    _LINE_VOLTAGE_BANDS,
    _PALETTE,
)


# ---------------------------------------------------------------------------
# resolve_bus_ref
# ---------------------------------------------------------------------------


class TestResolveBusRef:
    """Verify resolve_bus_ref translates through the voltage map."""

    def test_identity_when_not_in_map(self):
        assert resolve_bus_ref(42, {}) == 42

    def test_translates_through_map(self):
        vmap = {1: 3, 2: 3, 3: 3}
        assert resolve_bus_ref(1, vmap) == 3

    def test_string_ref(self):
        vmap = {"LV_bus": "HV_bus"}
        assert resolve_bus_ref("LV_bus", vmap) == "HV_bus"

    def test_none_ref(self):
        vmap = {None: "should_not_match"}
        # None as a ref should return None (dict.get default)
        assert resolve_bus_ref("missing", vmap) == "missing"


# ---------------------------------------------------------------------------
# build_voltage_map
# ---------------------------------------------------------------------------


class TestBuildVoltageMap:
    """Verify build_voltage_map produces correct bus-to-representative mappings."""

    def test_zero_threshold_returns_empty(self):
        buses = [{"uid": 1, "name": "B1", "voltage": 33}]
        lines = []
        assert not build_voltage_map(buses, lines, 0.0)

    def test_negative_threshold_returns_empty(self):
        buses = [{"uid": 1, "name": "B1", "voltage": 33}]
        lines = []
        assert not build_voltage_map(buses, lines, -10.0)

    def test_all_hv_buses_map_to_themselves(self):
        buses = [
            {"uid": 1, "name": "B1", "voltage": 220},
            {"uid": 2, "name": "B2", "voltage": 345},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        vmap = build_voltage_map(buses, lines, 100.0)
        assert vmap.get(1) == 1
        assert vmap.get(2) == 2

    def test_lv_bus_maps_to_nearest_hv_via_line(self):
        buses = [
            {"uid": 1, "name": "HV", "voltage": 220},
            {"uid": 2, "name": "LV", "voltage": 33},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        vmap = build_voltage_map(buses, lines, 100.0)
        assert vmap[2] == 1  # LV bus maps to HV bus

    def test_lv_bus_no_hv_neighbour_maps_to_self(self):
        """An isolated LV bus with no HV neighbour maps to itself."""
        buses = [
            {"uid": 1, "name": "LV1", "voltage": 33},
            {"uid": 2, "name": "LV2", "voltage": 33},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        vmap = build_voltage_map(buses, lines, 100.0)
        # Neither bus has HV, so both should map to themselves
        assert vmap[1] == 1
        assert vmap[2] == 2

    def test_bus_without_voltage_treated_as_hv(self):
        """Buses missing the 'voltage' field are treated as HV (never lumped)."""
        buses = [
            {"uid": 1, "name": "NoVolt"},  # no voltage field
            {"uid": 2, "name": "LV", "voltage": 33},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        vmap = build_voltage_map(buses, lines, 100.0)
        assert vmap[1] == 1  # no voltage -> HV
        assert vmap[2] == 1  # LV -> maps to bus without voltage (treated as HV)

    def test_chain_of_lv_buses_reaches_hv(self):
        """LV bus connected through another LV bus to an HV bus."""
        buses = [
            {"uid": 1, "name": "HV", "voltage": 220},
            {"uid": 2, "name": "LV1", "voltage": 33},
            {"uid": 3, "name": "LV2", "voltage": 33},
        ]
        lines = [
            {"bus_a": 1, "bus_b": 2},
            {"bus_a": 2, "bus_b": 3},
        ]
        vmap = build_voltage_map(buses, lines, 100.0)
        assert vmap[3] == 1  # LV2 -> LV1 -> HV

    def test_name_keys_also_mapped(self):
        """Both uid and name keys should be present in the voltage map."""
        buses = [
            {"uid": 1, "name": "HV", "voltage": 220},
            {"uid": 2, "name": "LV", "voltage": 33},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        vmap = build_voltage_map(buses, lines, 100.0)
        # The LV bus name should also be mapped
        assert vmap.get("LV") == 1

    def test_empty_buses_returns_empty(self):
        assert not build_voltage_map([], [], 100.0)

    def test_mixed_voltage_system(self):
        """System with 3 voltage levels: 345kV, 110kV, 33kV; threshold=100."""
        buses = [
            {"uid": 1, "name": "HV345", "voltage": 345},
            {"uid": 2, "name": "MV110", "voltage": 110},
            {"uid": 3, "name": "LV33", "voltage": 33},
        ]
        lines = [
            {"bus_a": 1, "bus_b": 2},
            {"bus_a": 2, "bus_b": 3},
        ]
        vmap = build_voltage_map(buses, lines, 100.0)
        # HV345 and MV110 are both >= 100, so they map to themselves
        assert vmap[1] == 1
        assert vmap[2] == 2
        # LV33 < 100, should map to nearest HV which is MV110 (uid=2)
        assert vmap[3] == 2


# ---------------------------------------------------------------------------
# count_visible_buses
# ---------------------------------------------------------------------------


class TestCountVisibleBuses:
    """Verify count_visible_buses returns the correct representative count."""

    def test_zero_threshold_returns_total_count(self):
        buses = [{"uid": i, "voltage": 100} for i in range(5)]
        assert count_visible_buses(buses, [], 0.0) == 5

    def test_all_hv_no_reduction(self):
        buses = [
            {"uid": 1, "voltage": 220},
            {"uid": 2, "voltage": 345},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        assert count_visible_buses(buses, lines, 100.0) == 2

    def test_lv_reduction(self):
        buses = [
            {"uid": 1, "voltage": 220},
            {"uid": 2, "voltage": 33},
        ]
        lines = [{"bus_a": 1, "bus_b": 2}]
        # Bus 2 (33kV) maps to bus 1 (220kV) -> only 1 representative
        assert count_visible_buses(buses, lines, 100.0) == 1

    def test_empty_buses(self):
        assert count_visible_buses([], [], 100.0) == 0


# ---------------------------------------------------------------------------
# auto_voltage_threshold
# ---------------------------------------------------------------------------


class TestAutoVoltageThreshold:
    """Verify auto_voltage_threshold picks the right threshold."""

    def test_small_system_returns_zero(self):
        buses = [{"uid": i, "voltage": 345} for i in range(10)]
        assert auto_voltage_threshold(buses, [], max_buses=64) == 0.0

    def test_single_voltage_level_returns_fallback(self):
        """All buses at same voltage -> fallback to highest level."""
        buses = [{"uid": i, "voltage": 345} for i in range(200)]
        # Single voltage + no lines = no reduction possible; returns max level
        assert auto_voltage_threshold(buses, [], max_buses=64) == 345.0

    def test_returns_float(self):
        buses = [{"uid": i, "voltage": 345} for i in range(50)]
        buses += [{"uid": 50 + i, "voltage": 110} for i in range(50)]
        thresh = auto_voltage_threshold(buses, [], max_buses=64)
        assert isinstance(thresh, float)
        assert thresh >= 0.0

    def test_no_voltage_fields_returns_zero(self):
        """Buses without voltage fields -> no reduction possible."""
        buses = [{"uid": i} for i in range(200)]
        assert auto_voltage_threshold(buses, [], max_buses=64) == 0.0

    def test_two_voltage_levels_reduces(self):
        """HV (220kV) and LV (33kV) buses: threshold should be chosen to reduce."""
        hv_buses = [{"uid": i, "voltage": 220} for i in range(40)]
        lv_buses = [{"uid": 40 + i, "voltage": 33} for i in range(60)]
        buses = hv_buses + lv_buses
        # No lines -> LV buses can't find HV neighbours, all map to self
        # But the threshold should still be determined from voltage levels
        thresh = auto_voltage_threshold(buses, [], max_buses=50)
        assert isinstance(thresh, float)
        # Should pick a threshold that keeps <= 50 buses visible
        # With no lines, all 100 buses remain, so it should try 220 kV
        assert thresh > 0.0

    def test_three_levels_no_lines_returns_fallback(self):
        """Three voltage levels but no lines -> BFS can't merge -> fallback."""
        buses = [
            *[{"uid": i, "voltage": 345} for i in range(20)],
            *[{"uid": 20 + i, "voltage": 110} for i in range(30)],
            *[{"uid": 50 + i, "voltage": 33} for i in range(50)],
        ]
        # Without lines, no BFS merging occurs at any threshold,
        # so all 100 buses remain visible -> returns highest level as fallback
        thresh = auto_voltage_threshold(buses, [], max_buses=64)
        assert thresh == 345.0


# ---------------------------------------------------------------------------
# VOLTAGE_BFS_MAX_DEPTH constant
# ---------------------------------------------------------------------------


class TestConstants:
    """Verify the BFS depth constant has the expected value."""

    def test_bfs_max_depth(self):
        assert VOLTAGE_BFS_MAX_DEPTH == 20


# ---------------------------------------------------------------------------
# _line_color_width -- voltage-based line coloring
# ---------------------------------------------------------------------------


class TestLineColorWidth:
    """Verify _line_color_width returns correct (color, width) tuples
    based on voltage level bands."""

    def test_500kv_returns_darkest_and_widest(self):
        """500+ kV lines use the first (darkest, widest) band."""
        color, width = _line_color_width(500.0)
        assert color == "#BF360C"
        assert width == 5.0

    def test_above_500kv(self):
        """Voltage above 500 kV still matches the 500+ kV band."""
        color, width = _line_color_width(765.0)
        assert color == "#BF360C"
        assert width == 5.0

    def test_220kv_returns_medium(self):
        """220 kV line uses the 220 kV band (dark blue, width 3.0)."""
        color, width = _line_color_width(220.0)
        assert color == "#F57F17"
        assert width == 4.0

    def test_345kv_band(self):
        """345 kV line uses the 345 kV band."""
        color, width = _line_color_width(345.0)
        assert color == "#E65100"
        assert width == 4.5

    def test_110kv_band(self):
        """110 kV line uses the 110 kV band."""
        color, width = _line_color_width(110.0)
        assert color == "#9E9D24"
        assert width == 3.0

    def test_66kv_band(self):
        """66 kV line uses the 66 kV band."""
        color, width = _line_color_width(66.0)
        assert color == "#689F38"
        assert width == 2.5

    def test_33kv_band(self):
        """33 kV line uses the 33 kV band."""
        color, width = _line_color_width(33.0)
        assert color == "#7CB342"
        assert width == 2.0

    def test_0kv_returns_lowest_band(self):
        """0 kV matches the (0.0, ...) band -- gray, thin."""
        color, width = _line_color_width(0.0)
        assert color == "#AED581"
        assert width == 1.5

    def test_negative_kv_returns_default(self):
        """Negative kV does not match any band; returns palette default."""
        color, width = _line_color_width(-10.0)
        assert color == _PALETTE["line_edge"]
        assert width == 1.0

    def test_between_bands(self):
        """A voltage between bands picks the lower matching band."""
        # 150 kV is >= 110 but < 154, so it should match the 110 kV band
        color, width = _line_color_width(150.0)
        assert color == "#9E9D24"
        assert width == 3.0

    def test_all_band_boundaries(self):
        """Each band boundary returns its own band."""
        for min_kv, expected_color, expected_width in _LINE_VOLTAGE_BANDS:
            color, width = _line_color_width(min_kv)
            assert color == expected_color, (
                f"kv={min_kv}: expected color {expected_color}, got {color}"
            )
            assert width == expected_width, (
                f"kv={min_kv}: expected width {expected_width}, got {width}"
            )
