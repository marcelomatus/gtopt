# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plp2gtopt.tech_classify module."""

from __future__ import annotations

from plp2gtopt.tech_classify import (
    ALL_KNOWN_TYPES,
    CURTAILMENT_TYPES,
    HYDRO_TYPES,
    PLP_CATEGORY_MAP,
    RENEWABLE_TYPES,
    STORAGE_TYPES,
    THERMAL_TYPES,
    classify_type,
    type_color,
    type_label,
)


class TestClassifyType:
    """Tests for classify_type()."""

    def test_hydro_types(self) -> None:
        for t in (
            "embalse",
            "serie",
            "pasada",
            "hydro_reservoir",
            "hydro_ror",
            "hydro_small",
            "hydro_pumped",
        ):
            assert classify_type(t) == "hydro", f"{t} should be hydro"

    def test_thermal_types(self) -> None:
        for t in (
            "termica",
            "thermal",
            "gas",
            "coal",
            "diesel",
            "nuclear",
            "biomass",
            "geothermal",
        ):
            assert classify_type(t) == "thermal", f"{t} should be thermal"

    def test_renewable_types(self) -> None:
        for t in ("solar", "wind", "csp", "renewable"):
            assert classify_type(t) == "renewable", f"{t} should be renewable"

    def test_storage_types(self) -> None:
        for t in ("bateria", "battery"):
            assert classify_type(t) == "storage", f"{t} should be storage"

    def test_curtailment_types(self) -> None:
        for t in ("falla", "curtailment"):
            assert classify_type(t) == "curtailment", f"{t} should be curtailment"

    def test_other_type(self) -> None:
        assert classify_type("unknown") == "other"
        assert classify_type("modular") == "other"
        assert classify_type("notavalidtype") == "other"

    def test_uppercase_input(self) -> None:
        assert classify_type("SOLAR") == "renewable"
        assert classify_type("Embalse") == "hydro"
        assert classify_type("THERMAL") == "thermal"
        assert classify_type("Battery") == "storage"
        assert classify_type("FALLA") == "curtailment"


class TestTypeLabel:
    """Tests for type_label()."""

    def test_known_labels(self) -> None:
        assert type_label("solar") == "Solar PV"
        assert type_label("hydro_reservoir") == "Hydro (reservoir)"
        assert type_label("wind") == "Wind"
        assert type_label("battery") == "Battery storage"
        assert type_label("embalse") == "Hydro (reservoir)"

    def test_unknown_falls_back_to_raw(self) -> None:
        assert type_label("notavalidtype") == "notavalidtype"
        assert type_label("custom_gen") == "custom_gen"

    def test_case_insensitive(self) -> None:
        assert type_label("SOLAR") == "Solar PV"


class TestTypeColor:
    """Tests for type_color()."""

    def test_known_colors(self) -> None:
        assert type_color("solar") == "#f7b731"
        assert type_color("wind") == "#20bf6b"
        assert type_color("battery") == "#e377c2"
        assert type_color("embalse") == "#1f77b4"

    def test_unknown_falls_back(self) -> None:
        assert type_color("notavalidtype") == "#c7c7c7"
        assert type_color("custom_gen") == "#c7c7c7"

    def test_case_insensitive(self) -> None:
        assert type_color("SOLAR") == "#f7b731"


class TestPlpCategoryMap:
    """Tests for PLP_CATEGORY_MAP."""

    def test_solar_maps_to_termica(self) -> None:
        assert PLP_CATEGORY_MAP["solar"] == "termica"

    def test_hydro_reservoir_maps_to_embalse(self) -> None:
        assert PLP_CATEGORY_MAP["hydro_reservoir"] == "embalse"

    def test_hydro_ror_maps_to_serie(self) -> None:
        assert PLP_CATEGORY_MAP["hydro_ror"] == "serie"

    def test_wind_maps_to_termica(self) -> None:
        assert PLP_CATEGORY_MAP["wind"] == "termica"

    def test_embalse_maps_to_embalse(self) -> None:
        assert PLP_CATEGORY_MAP["embalse"] == "embalse"


class TestAllKnownTypes:
    """Tests for ALL_KNOWN_TYPES."""

    def test_contains_all_category_types(self) -> None:
        for t in HYDRO_TYPES:
            assert t in ALL_KNOWN_TYPES, f"{t} missing from ALL_KNOWN_TYPES"
        for t in THERMAL_TYPES:
            assert t in ALL_KNOWN_TYPES, f"{t} missing from ALL_KNOWN_TYPES"
        for t in RENEWABLE_TYPES:
            assert t in ALL_KNOWN_TYPES, f"{t} missing from ALL_KNOWN_TYPES"
        for t in STORAGE_TYPES:
            assert t in ALL_KNOWN_TYPES, f"{t} missing from ALL_KNOWN_TYPES"
        for t in CURTAILMENT_TYPES:
            assert t in ALL_KNOWN_TYPES, f"{t} missing from ALL_KNOWN_TYPES"

    def test_contains_special_types(self) -> None:
        assert "unknown" in ALL_KNOWN_TYPES
        assert "modular" in ALL_KNOWN_TYPES
