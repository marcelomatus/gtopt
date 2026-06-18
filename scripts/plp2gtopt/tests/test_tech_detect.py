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


def test_load_centipo_csv_comma_cen_codes(tmp_path: Path):
    """Authoritative CEN ``centipo.csv`` is comma-separated and uses the
    ``Tipo_T`` codes (TCA/TGN/TDI/PFV/PE/TBM/CSP/HP/GEO/RSA).  Regression
    for the bug where the whitespace-only split silently dropped every row
    of a comma-delimited file (coal/gas/etc. never classified)."""
    centipo = tmp_path / "centipo.csv"
    centipo.write_text(
        "Central,Tipo_T\n"
        "GUACOLDA_1,TCA\n"  # coal
        "IE_MEJILLONES_GN_A,TGN\n"  # gas
        "IE_MEJILLONES_DIE,TDI\n"  # diesel
        "CERRO_DOMINADOR_CS,CSP\n"  # csp
        "ARAUCO_MAPA_BL1,TBM\n"  # biomass
        "ALTO_HOSPICIO,HP\n"  # run-of-river hydro
        "CERRO_PABELLON_U1,GEO\n"  # geothermal
        "ALENA_EO,PE\n"  # wind
        "BAT_ARENA_LOAD,RSA\n"  # battery charging side
    )
    from plp2gtopt.tech_detect import load_centipo_csv

    result = load_centipo_csv(tmp_path)
    assert result["GUACOLDA_1"] == "coal"
    assert result["IE_MEJILLONES_GN_A"] == "gas"
    assert result["IE_MEJILLONES_DIE"] == "diesel"
    assert result["CERRO_DOMINADOR_CS"] == "csp"
    assert result["ARAUCO_MAPA_BL1"] == "biomass"
    assert result["ALTO_HOSPICIO"] == "hydro_ror"
    assert result["CERRO_PABELLON_U1"] == "geothermal"
    assert result["ALENA_EO"] == "wind"
    assert result["BAT_ARENA_LOAD"] == "battery"


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


# ---------------------------------------------------------------------------
# T6 — load_centipo_csv fallback for unknown labels
# ---------------------------------------------------------------------------


def test_load_centipo_csv_unknown_label_fallback(tmp_path: Path):
    """Unknown centipo label falls back to ``label.lower()``."""
    from plp2gtopt.tech_detect import load_centipo_csv

    centipo = tmp_path / "centipo.csv"
    centipo.write_text(
        "name label\n"  # header (skipped)
        "PlantA XYZ\n"  # unknown label → falls back to "xyz"
        "PlantB SOL\n"  # known label → "solar"
    )
    result = load_centipo_csv(tmp_path)
    assert result["PlantA"] == "xyz"  # documents the fallback behaviour
    assert result["PlantB"] == "solar"  # known-label path still works


# ---------------------------------------------------------------------------
# T7 — load_overrides skips malformed tokens
# ---------------------------------------------------------------------------


def test_load_overrides_skips_malformed_token():
    """Tokens missing ``:`` are logged and silently skipped."""
    result = load_overrides("SolarA:solar,BadToken,WindB:wind")
    assert result == {"SolarA": "solar", "WindB": "wind"}
    assert "BadToken" not in result


# ---------------------------------------------------------------------------
# T8 — detect_technology pattern-order priority for overlapping names
# ---------------------------------------------------------------------------


def test_detect_technology_overlapping_pattern_priority():
    """``_NAME_PATTERNS`` is ordered; the first matching pattern wins."""
    # "SolarHidro" matches both "solar" (pattern 0) and "hidr" (pattern 11) —
    # solar wins because it comes first in _NAME_PATTERNS.
    assert detect_technology("pasada", "SolarHidro") == "solar"

    # "MiniHidroBomba" matches both "mini.*hidr" (pattern 9: hydro_small) and
    # "bomb" (pattern 10: hydro_pumped). hydro_small wins because it comes first.
    assert detect_technology("pasada", "MiniHidroBomba") == "hydro_small"

    # Sanity: a clean hydro name without overlaps still resolves to hydro_ror.
    assert detect_technology("pasada", "RioMaule") == "hydro_ror"
