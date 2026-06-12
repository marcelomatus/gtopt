"""rts_gmlc_to_gtopt — converter for the RTS-GMLC (GridMod 2019) test system.

Reads the public RTS-GMLC ``RTS_Data/SourceData`` CSVs and emits a gtopt
JSON suitable for the ``--emissions`` accounting pipeline.

Unlike NREL-118 (per-fuel IPCC overlay), RTS-GMLC ships per-generator
combustion rates in lbs CO2 / MMBTU directly on each row, so each Fuel
synthesised here carries the unit-specific combustion factor.  Multi-
pollutant CO2 / SO2 / NOX / CH4 / N2O / CO / VOCs handling is wired
through ``EmissionSource.emission_factors[*]`` so a downstream caller can
register additional ``Emission`` rows and zones.

Use as a module:

    python -m rts_gmlc_to_gtopt --day 1 -o rts_gmlc_day1.json
"""

from rts_gmlc_to_gtopt._converter import (
    LBS_PER_MMBTU_TO_TCO2_PER_GJ,
    POLLUTANTS,
    RtsGen,
    convert,
    to_gtopt_json,
)

__all__ = [
    "LBS_PER_MMBTU_TO_TCO2_PER_GJ",
    "POLLUTANTS",
    "RtsGen",
    "convert",
    "to_gtopt_json",
]
