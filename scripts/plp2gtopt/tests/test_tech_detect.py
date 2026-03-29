# SPDX-License-Identifier: BSD-3-Clause
"""Tests for generator technology auto-detection."""

import json
from pathlib import Path

from plp2gtopt.tech_detect import (
    available_types,
    classify_generators,
    detect_technology,
    load_overrides,
    suspect_technology,
)


# ---------------------------------------------------------------------------
# detect_technology — PLP base type mapping
# ---------------------------------------------------------------------------


def test_embalse_maps_to_hydro_reservoir():
    assert detect_technology("embalse", "RAPEL") == "hydro_reservoir"


def test_serie_maps_to_hydro_ror():
    assert detect_technology("serie", "SomeRiver") == "hydro_ror"


def test_pasada_default_is_hydro_ror():
    assert detect_technology("pasada", "GenericName", auto_detect=False) == "hydro_ror"


def test_termica_default_is_thermal():
    assert detect_technology("termica", "GenericPlant", auto_detect=False) == "thermal"


def test_bateria_maps_to_battery():
    assert detect_technology("bateria", "BESS1") == "battery"


# ---------------------------------------------------------------------------
# detect_technology — name-based auto-detection
# ---------------------------------------------------------------------------


def test_detect_solar_from_name():
    assert detect_technology("termica", "SolarAlmeyda") == "solar"
    assert detect_technology("pasada", "FotovoltaicaPV") == "solar"
    assert detect_technology("termica", "PV_Santiago") == "solar"
    assert detect_technology("pasada", "Plant_FV") == "solar"
    assert detect_technology("pasada", "Plant_FV_Sur") == "solar"
    assert detect_technology("termica", "PlantaSOLAR") == "solar"


def test_detect_wind_from_name():
    assert detect_technology("termica", "EolicaCanela") == "wind"
    assert detect_technology("pasada", "ParqueEolico") == "wind"
    assert detect_technology("termica", "WindFarmNorth") == "wind"
    assert detect_technology("pasada", "Plant_EO") == "wind"
    assert detect_technology("pasada", "Plant_EO_Norte") == "wind"


def test_detect_geothermal_from_name():
    assert detect_technology("termica", "GeotermicaCerro") == "geothermal"


def test_detect_biomass_from_name():
    assert detect_technology("termica", "BiomasForestal") == "biomass"


def test_detect_gas_from_name():
    assert detect_technology("termica", "GNL_Quintero") == "gas"
    assert detect_technology("termica", "CicloCombinado") == "gas"
    assert detect_technology("termica", "TurbogasNorte") == "gas"


def test_detect_coal_from_name():
    assert detect_technology("termica", "CarbonBocamina") == "coal"


def test_detect_diesel_from_name():
    assert detect_technology("termica", "DieselEmergencia") == "diesel"


def test_detect_nuclear_from_name():
    assert detect_technology("termica", "NuclearSur") == "nuclear"


def test_detect_hydro_from_pasada_name():
    """Pasada with hydro keyword stays hydro_ror."""
    assert detect_technology("pasada", "MiniHidroRio") == "hydro_small"


def test_no_detection_for_embalse():
    """Embalse always maps to hydro_reservoir, name patterns ignored."""
    assert detect_technology("embalse", "SolarEmbalse") == "hydro_reservoir"


def test_no_detection_when_disabled():
    """auto_detect=False skips name patterns."""
    assert detect_technology("termica", "SolarAlmeyda", auto_detect=False) == "thermal"


# ---------------------------------------------------------------------------
# Overrides
# ---------------------------------------------------------------------------


def test_override_takes_priority():
    overrides = {"MyPlant": "custom_type"}
    assert detect_technology("termica", "MyPlant", overrides=overrides) == "custom_type"


def test_override_beats_auto_detect():
    overrides = {"SolarAlmeyda": "thermal_override"}
    assert (
        detect_technology("termica", "SolarAlmeyda", overrides=overrides)
        == "thermal_override"
    )


# ---------------------------------------------------------------------------
# Fallback to PLP base type
# ---------------------------------------------------------------------------


def test_unknown_termica_falls_back_to_thermal():
    """Termica with unrecognised name → thermal (PLP base type)."""
    result = detect_technology("termica", "UnknownPlant")
    assert result == "thermal"


# ---------------------------------------------------------------------------
# centipo.csv reader
# ---------------------------------------------------------------------------


def test_load_centipo_csv(tmp_path: Path):
    """Read centipo.csv with PLP labels."""
    centipo = tmp_path / "centipo.csv"
    centipo.write_text("# header\n'SolarX'  SOL\n'WindY'  EOL\n'GasZ'  GNL\n")
    from plp2gtopt.tech_detect import load_centipo_csv

    result = load_centipo_csv(tmp_path)
    assert result["SolarX"] == "solar"
    assert result["WindY"] == "wind"
    assert result["GasZ"] == "gas"


def test_load_centipo_csv_missing(tmp_path: Path):
    """Missing centipo.csv returns empty dict."""
    from plp2gtopt.tech_detect import load_centipo_csv

    assert not load_centipo_csv(tmp_path)


# ---------------------------------------------------------------------------
# load_overrides
# ---------------------------------------------------------------------------


def test_load_overrides_inline():
    result = load_overrides("A:solar,B:wind,C:gas")
    assert result == {"A": "solar", "B": "wind", "C": "gas"}


def test_load_overrides_empty():
    assert load_overrides(None) == {}
    assert load_overrides("") == {}


def test_load_overrides_json(tmp_path: Path):
    fp = tmp_path / "overrides.json"
    fp.write_text(json.dumps({"SolarX": "solar", "WindY": "wind"}))
    result = load_overrides(str(fp))
    assert result == {"SolarX": "solar", "WindY": "wind"}


def test_load_overrides_csv(tmp_path: Path):
    fp = tmp_path / "overrides.csv"
    fp.write_text("# comment\nSolarX,solar\nWindY,wind\n")
    result = load_overrides(str(fp))
    assert result == {"SolarX": "solar", "WindY": "wind"}


# ---------------------------------------------------------------------------
# classify_generators (bulk)
# ---------------------------------------------------------------------------


def test_classify_generators():
    centrals = [
        {"name": "SolarAlmeyda", "type": "termica"},
        {"name": "RAPEL", "type": "embalse"},
        {"name": "EolicaCanela", "type": "pasada"},
        {"name": "DieselNorte", "type": "termica"},
    ]
    result = classify_generators(centrals)
    assert result["SolarAlmeyda"] == "solar"
    assert result["RAPEL"] == "hydro_reservoir"
    assert result["EolicaCanela"] == "wind"
    assert result["DieselNorte"] == "diesel"


def test_classify_with_overrides():
    centrals = [
        {"name": "Ambiguous", "type": "termica"},
    ]
    overrides = {"Ambiguous": "geothermal"}
    result = classify_generators(centrals, overrides=overrides)
    assert result["Ambiguous"] == "geothermal"


# ---------------------------------------------------------------------------
# available_types
# ---------------------------------------------------------------------------


def test_available_types_not_empty():
    types = available_types()
    assert "solar" in types
    assert "wind" in types
    assert "thermal" in types
    assert "hydro_reservoir" in types


# ---------------------------------------------------------------------------
# suspect_technology
# ---------------------------------------------------------------------------


def test_suspect_solar():
    assert suspect_technology("termica", "SolarAlmeyda") == "solar"
    assert suspect_technology("pasada", "FotovoltaicaPV") == "solar"


def test_suspect_wind():
    assert suspect_technology("termica", "EolicaCanela") == "wind"
    assert suspect_technology("pasada", "ParqueEolico") == "wind"


def test_suspect_none_for_non_solar_wind():
    """Non-solar/wind patterns (gas, coal, etc.) return None."""
    assert suspect_technology("termica", "GNL_Quintero") is None
    assert suspect_technology("termica", "CarbonBocamina") is None
    assert suspect_technology("termica", "DieselEmergencia") is None


def test_suspect_none_for_generic_name():
    assert suspect_technology("termica", "GenericPlant") is None
    assert suspect_technology("pasada", "GenericName") is None


def test_suspect_none_for_non_ambiguous_types():
    """embalse, serie, bateria are never suspected."""
    assert suspect_technology("embalse", "SolarEmbalse") is None
    assert suspect_technology("serie", "EolicaSerie") is None
    assert suspect_technology("bateria", "BESS1") is None
