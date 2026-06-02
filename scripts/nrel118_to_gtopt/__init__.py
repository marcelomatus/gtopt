"""nrel118_to_gtopt — converter for the NREL-118 test system.

Reads the public NREL-118 (Pena, Martinez-Anido, Hodge 2017) test-system
CSVs published by NREL-Sienna's `PowerSystemsTestData` repo and emits a
gtopt JSON suitable for the `--emissions` accounting pipeline.

The piecewise heat-rate (Base MMBTU/hr + 5 incremental BTU/kWh bands) is
collapsed to a single scalar heat_rate (GJ/MWh at pmax) using the
pmax-weighted segment average documented in
`scripts/nrel118_to_gtopt/__main__.py`.  Per-fuel IPCC AR6 combustion
factors (tCO2/GJ) are hard-coded inline rather than added to
`gtopt_shared/data/ipcc_emission_factors.json` because the NREL-118
unit-type → fuel mapping is study-specific.

Use as a module:

    python -m nrel118_to_gtopt --week 2 -o nrel118_week2.json
"""

from nrel118_to_gtopt._converter import (
    BTU_PER_KWH_TO_GJ_PER_MWH,
    IPCC_AR6_TCO2_PER_GJ,
    collapse_piecewise_heat_rate,
    convert,
    unit_type_to_fuel,
)

__all__ = [
    "BTU_PER_KWH_TO_GJ_PER_MWH",
    "IPCC_AR6_TCO2_PER_GJ",
    "collapse_piecewise_heat_rate",
    "convert",
    "unit_type_to_fuel",
]
