# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``plp2gtopt._emissions``.

Coverage targets:
* alias / canonical-name lookup with aggressive normalization
* missing-CO2-factor injection on an existing Fuel
* emission_array synthesis (single "co2" pollutant row) when any Fuel
  gains a factor
* no-overwrite when a Fuel already carries a non-zero CO2 row (PLEXOS /
  project data is authoritative)
* zero-stub CO2 row is treated as "no factor" — defaults fill it
* unknown-fuel reporting (Fuel name not in defaults file)
* loader rejects malformed file (non-object root / missing 'fuels')
* report file persisted to disk
* bundled IPCC defaults JSON loads and indexes correctly
* end-to-end via apply_emission_defaults_from_file
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from gtopt_shared.emissions import (
    DEFAULT_EMISSIONS_FILE,
    EmissionDefaults,
    EmissionFactor,
    EmissionReport,
    apply_emission_defaults,
    apply_emission_defaults_from_file,
    load_emission_defaults,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _planning(
    fuel_array: list[dict[str, Any]],
    emission_array: list[dict[str, Any]] | None = None,
    emission_zone_array: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    sys_d: dict[str, Any] = {"name": "test", "fuel_array": fuel_array}
    if emission_array is not None:
        sys_d["emission_array"] = emission_array
    if emission_zone_array is not None:
        sys_d["emission_zone_array"] = emission_zone_array
    return {"options": {}, "simulation": {}, "system": sys_d}


def _make_defaults(fuels: list[EmissionFactor]) -> EmissionDefaults:
    from gtopt_shared.emissions import _normalize  # noqa: PLC0415

    by_norm: dict[str, EmissionFactor] = {}
    for f in fuels:
        for key in (f.name, *f.aliases):
            by_norm[_normalize(key)] = f
    return EmissionDefaults(
        source_path="<test>",
        description="test fixture",
        units={"co2_combustion": "tCO2 / GJ"},
        fuels=tuple(fuels),
        _by_normalized=by_norm,
    )


_FIX_DIESEL = EmissionFactor(
    name="diesel",
    aliases=("diesel oil", "petroleo diesel"),
    co2_combustion=0.0741,
    co2_upstream=0.0,
    heat_content=43.0,
    ipcc_reference="Table 1.4 - diesel",
)
_FIX_COAL = EmissionFactor(
    name="coal_bituminous",
    aliases=("coal", "carbon"),
    co2_combustion=0.0946,
    co2_upstream=0.0,
    heat_content=25.8,
    ipcc_reference="Table 1.4 - coal",
)
_FIX_BIOMASS = EmissionFactor(
    name="biomass",
    aliases=("biomasa",),
    co2_combustion=0.0,  # biogenic-zero default
    co2_upstream=0.0,
    heat_content=15.6,
    ipcc_reference="Table 1.4 - biomass biogenic-zero",
)


# ---------------------------------------------------------------------------
# Lookup
# ---------------------------------------------------------------------------


def test_lookup_canonical_name() -> None:
    d = _make_defaults([_FIX_DIESEL, _FIX_COAL])
    factor = d.lookup("diesel")
    assert factor is not None
    assert factor.co2_combustion == pytest.approx(0.0741)


def test_lookup_alias_with_normalization() -> None:
    d = _make_defaults([_FIX_DIESEL])
    # All of these should hit the same alias entry
    for name in ("Diesel Oil", "diesel-oil", "DIESEL_OIL", "  petroleo  diesel  "):
        assert d.lookup(name) == _FIX_DIESEL


def test_lookup_unknown_fuel_returns_none() -> None:
    d = _make_defaults([_FIX_DIESEL])
    assert d.lookup("mystery_fuel") is None


def test_lookup_prefix_fallback_for_cen_naming() -> None:
    """CEN-Chile PLEXOS fuels ship as ``<family>_<plant>[_band]`` —
    e.g. ``Carbon_Andina``, ``Biomasa_Celco_B1``,
    ``Diesel_AguasBlancas``.  The lookup must fall back to the first
    underscore-separated token after the full-name miss.
    """
    coal = EmissionFactor(
        name="coal_bituminous",
        aliases=("carbon", "coal"),
        co2_combustion=0.0946,
        co2_upstream=0.0,
        heat_content=25.8,
    )
    d = _make_defaults([_FIX_DIESEL, coal])
    # Full name doesn't match (Carbon_Andina != CARBON / COAL_BITUMINOUS)
    # — prefix fallback fires on "Carbon" → CARBON → matches alias.
    assert d.lookup("Carbon_Andina") == coal
    assert d.lookup("Diesel_AguasBlancas") == _FIX_DIESEL
    # Multi-segment names: only the FIRST token is used as the fallback.
    assert d.lookup("Biomasa_Celco_B1") is None  # no biomass fixture
    # Exact match still wins when both could apply.
    diesel_alias = EmissionFactor(
        name="diesel",
        aliases=("Diesel_AguasBlancas",),  # exact-match override
        co2_combustion=0.10,  # bespoke
        co2_upstream=0.0,
        heat_content=43.0,
    )
    d2 = _make_defaults([diesel_alias, _FIX_DIESEL])
    # The exact alias wins; prefix fallback never runs.  Both fixtures
    # carry name="diesel" so prefix would land on _FIX_DIESEL too — the
    # invariant under test is that exact match returns immediately.
    hit = d2.lookup("Diesel_AguasBlancas")
    assert hit is not None
    assert hit.co2_combustion == pytest.approx(0.10)


def test_lookup_prefix_fallback_no_underscore_no_match() -> None:
    """A name without ``_`` doesn't trigger the prefix fallback; the
    full-name miss is the final answer.
    """
    d = _make_defaults([_FIX_DIESEL])
    assert d.lookup("DieselSomething") is None  # no _ → no fallback


def test_factor_has_factor_flag() -> None:
    assert _FIX_DIESEL.has_factor
    assert not _FIX_BIOMASS.has_factor  # zero on both paths


# ---------------------------------------------------------------------------
# Injection
# ---------------------------------------------------------------------------


def test_missing_factor_is_injected() -> None:
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel", "price": 80.0}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    fuel = planning["system"]["fuel_array"][0]
    assert "emission_factors" in fuel
    co2 = fuel["emission_factors"][0]
    assert co2["emission"] == "co2"
    assert co2["combustion"] == pytest.approx(0.0741)
    # NCV carried through when the Fuel did not already have one
    assert fuel["heat_content"] == pytest.approx(43.0)

    assert report.fuels_factor_added == ("diesel",)
    assert not report.fuels_factor_preserved
    assert not report.fuels_unknown
    assert report.emission_array_created is True


def test_emission_array_co2_row_synthesized() -> None:
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel"}])
    apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    pollutants = planning["system"]["emission_array"]
    assert len(pollutants) == 1
    assert pollutants[0]["name"] == "co2"
    assert pollutants[0]["uid"] >= 1


def test_existing_emission_array_co2_row_not_duplicated() -> None:
    planning = _planning(
        fuel_array=[{"uid": 1, "name": "diesel"}],
        emission_array=[{"uid": 1, "name": "co2"}, {"uid": 2, "name": "nox"}],
    )
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    pollutants = planning["system"]["emission_array"]
    co2_rows = [p for p in pollutants if p.get("name") == "co2"]
    assert len(co2_rows) == 1  # no duplicate
    assert report.emission_array_created is False
    assert report.emission_array_already_present is True


def test_existing_non_zero_co2_factor_is_preserved() -> None:
    """When PLEXOS / project data already shipped a factor, the IPCC
    default must not clobber it — PLEXOS is authoritative.
    """
    planning = _planning(
        fuel_array=[
            {
                "uid": 1,
                "name": "diesel",
                "emission_factors": [
                    {"emission": "co2", "combustion": 0.0800},
                ],
            }
        ]
    )
    other = EmissionFactor(
        name="diesel",
        aliases=(),
        co2_combustion=0.0741,  # IPCC value, different from the 0.0800 shipped
        co2_upstream=0.0,
        heat_content=43.0,
    )
    report = apply_emission_defaults(planning, _make_defaults([other]))

    fuel = planning["system"]["fuel_array"][0]
    factors = fuel["emission_factors"]
    assert len(factors) == 1
    assert factors[0]["combustion"] == pytest.approx(0.0800)  # preserved
    assert report.fuels_factor_preserved == ("diesel",)
    assert not report.fuels_factor_added


def test_zero_stub_co2_row_is_filled() -> None:
    """A stub CO2 row with both rates 0 is treated as 'no factor' —
    the defaults fill it so an upstream pipeline that always emits the
    row shape but leaves the values empty still gets enriched.
    """
    planning = _planning(
        fuel_array=[
            {
                "uid": 1,
                "name": "diesel",
                "emission_factors": [
                    {"emission": "co2", "combustion": 0.0, "upstream": 0.0},
                ],
            }
        ]
    )
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    fuel = planning["system"]["fuel_array"][0]
    factors = fuel["emission_factors"]
    assert len(factors) == 1  # stub replaced, not duplicated
    assert factors[0]["combustion"] == pytest.approx(0.0741)
    assert report.fuels_factor_added == ("diesel",)


def test_unknown_fuel_reported_not_modified() -> None:
    planning = _planning(
        fuel_array=[
            {"uid": 1, "name": "exotic_fuel", "price": 10.0},
            {"uid": 2, "name": "diesel"},
        ]
    )
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    exotic = planning["system"]["fuel_array"][0]
    assert "emission_factors" not in exotic
    assert report.fuels_unknown == ("exotic_fuel",)
    assert report.fuels_factor_added == ("diesel",)


def test_biomass_default_zero_factor_does_not_create_co2_row() -> None:
    """Biogenic-zero biomass (all pollutant rates = 0) is NOT injected —
    a CO2 row with both values 0 is indistinguishable from absent and
    would create a meaningless emission_array row.  Reported in the
    ``fuels_zero_emission`` bucket (distinct from ``fuels_unknown``:
    the defaults-file entry DID resolve, just to a zero-everywhere
    factor — the maintainer reviewed and confirmed zero GHG).
    """
    planning = _planning(fuel_array=[{"uid": 1, "name": "biomass"}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_BIOMASS]))

    fuel = planning["system"]["fuel_array"][0]
    assert "emission_factors" not in fuel
    # Resolved (entry exists, all-zero) → zero_emission bucket; NOT unknown.
    assert report.fuels_zero_emission == ("biomass",)
    assert not report.fuels_unknown
    # emission_array stays empty (no factor → no pollutant row)
    assert planning["system"].get("emission_array", []) == []
    assert report.emission_array_created is False


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------


def test_loader_rejects_non_dict_root(tmp_path: Path) -> None:
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(["not", "a", "dict"]))
    with pytest.raises(ValueError, match="expected a JSON object"):
        load_emission_defaults(bad)


def test_loader_rejects_missing_fuels_key(tmp_path: Path) -> None:
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"description": "no fuels here"}))
    with pytest.raises(ValueError, match="missing top-level 'fuels' list"):
        load_emission_defaults(bad)


def test_loader_rejects_missing_path(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_emission_defaults(tmp_path / "does_not_exist.json")


def test_loader_round_trip(tmp_path: Path) -> None:
    p = tmp_path / "defaults.json"
    p.write_text(
        json.dumps(
            {
                "description": "round trip",
                "units": {"co2_combustion": "tCO2 / GJ"},
                "fuels": [
                    {
                        "name": "diesel",
                        "aliases": ["gasoil"],
                        "co2_combustion": 0.0741,
                        "co2_upstream": 0.0,
                        "heat_content": 43.0,
                        "ipcc_reference": "Table 1.4",
                    }
                ],
            }
        )
    )
    d = load_emission_defaults(p)
    assert d.source_path == str(p)
    assert len(d.fuels) == 1
    assert d.lookup("diesel") is not None
    assert d.lookup("Gas Oil") is not None  # alias hit


# ---------------------------------------------------------------------------
# Bundled IPCC defaults file
# ---------------------------------------------------------------------------


def test_bundled_ipcc_defaults_file_exists() -> None:
    assert DEFAULT_EMISSIONS_FILE.exists()


def test_bundled_ipcc_defaults_load() -> None:
    d = load_emission_defaults()  # None → bundled file
    # Must include the workhorses used by the Chilean PLP / PLEXOS cases
    for fuel in ("diesel", "fuel_oil", "natural_gas", "coal_bituminous", "petcoke"):
        factor = d.lookup(fuel)
        assert factor is not None, f"missing IPCC default for {fuel}"
        assert factor.co2_combustion > 0.0
    # Alias matching: Spanish/CEN names
    assert d.lookup("Gas Natural") is not None
    assert d.lookup("carbon") is not None
    assert d.lookup("petroleo diesel") is not None
    assert d.lookup("GNL") is not None


def test_bundled_defaults_values_match_ipcc_2006() -> None:
    """Spot-check the canonical Table 1.4 values to catch accidental drift."""
    d = load_emission_defaults()
    # Reference values are IPCC 2006 Vol 2 Ch 1 Table 1.4 (tCO2/GJ).
    expected = {
        "diesel": 0.0741,
        "fuel_oil": 0.0774,
        "natural_gas": 0.0561,
        "lng": 0.0561,
        "coal_bituminous": 0.0946,
        "coal_sub_bituminous": 0.0961,
        "coal_anthracite": 0.0983,
        "coal_lignite": 0.1010,
        "petcoke": 0.0975,
        "lpg": 0.0631,
        "kerosene": 0.0719,
        "naphtha": 0.0733,
    }
    for fuel, ref in expected.items():
        factor = d.lookup(fuel)
        assert factor is not None
        assert factor.co2_combustion == pytest.approx(ref), (
            f"{fuel}: {factor.co2_combustion} != IPCC {ref}"
        )


# ---------------------------------------------------------------------------
# End-to-end with report on disk
# ---------------------------------------------------------------------------


def test_apply_emission_defaults_from_file_with_report(tmp_path: Path) -> None:
    planning = _planning(
        fuel_array=[
            {"uid": 1, "name": "diesel", "price": 80.0},
            {"uid": 2, "name": "exotic", "price": 0.0},
        ]
    )
    report_path = tmp_path / "out" / "report.json"
    report = apply_emission_defaults_from_file(
        planning, source_path=None, report_path=report_path
    )

    assert isinstance(report, EmissionReport)
    assert report.fuels_factor_added == ("diesel",)
    assert report.fuels_unknown == ("exotic",)
    assert report.emission_array_created is True

    assert report_path.exists()
    loaded = json.loads(report_path.read_text())
    assert loaded["summary"]["factor_added"] == 1
    assert loaded["summary"]["unknown_fuels"] == 1
    assert loaded["summary"]["emission_array_created"] is True
    assert loaded["fuels_factor_added"] == ["diesel"]
    assert loaded["fuels_unknown"] == ["exotic"]


# ---------------------------------------------------------------------------
# Idempotence
# ---------------------------------------------------------------------------


def test_apply_is_idempotent() -> None:
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel"}])
    d = _make_defaults([_FIX_DIESEL])
    apply_emission_defaults(planning, d)
    first = json.dumps(planning, sort_keys=True)
    apply_emission_defaults(planning, d)
    second = json.dumps(planning, sort_keys=True)
    assert first == second


# ---------------------------------------------------------------------------
# Multi-pollutant injection (CH4 + N2O alongside CO2)
# ---------------------------------------------------------------------------


_FIX_COAL_GHG = EmissionFactor(
    name="coal_bituminous",
    aliases=("carbon", "coal"),
    co2_combustion=0.0946,
    co2_upstream=0.0,
    ch4_combustion=1e-6,
    n2o_combustion=1.5e-6,
    heat_content=25.8,
)


def test_apply_injects_all_three_ghg_pollutants() -> None:
    """Coal carries CO2 + CH4 + N2O after the apply pass.  emission_array
    + global_ghg zone reflect all three.
    """
    planning = _planning(fuel_array=[{"uid": 1, "name": "Carbon_Andina"}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_COAL_GHG]))

    coal = planning["system"]["fuel_array"][0]
    rows_by_pollutant = {row["emission"]: row for row in coal["emission_factors"]}
    assert rows_by_pollutant["co2"]["combustion"] == pytest.approx(0.0946)
    assert rows_by_pollutant["ch4"]["combustion"] == pytest.approx(1e-6)
    assert rows_by_pollutant["n2o"]["combustion"] == pytest.approx(1.5e-6)

    # emission_array has one row per pollutant.
    pollutants = {p["name"] for p in planning["system"]["emission_array"]}
    assert pollutants == {"co2", "ch4", "n2o"}

    # The zone name promotes from "global_co2" to "global_ghg" once
    # more than one pollutant is in scope.
    zones = planning["system"]["emission_zone_array"]
    assert len(zones) == 1
    assert zones[0]["name"] == "global_ghg"
    zone_pollutants = {ef["emission"] for ef in zones[0]["emissions"]}
    assert zone_pollutants == {"co2", "ch4", "n2o"}

    assert report.fuels_factor_added == ("Carbon_Andina",)


def test_preexisting_co2_preserved_while_ch4_n2o_filled() -> None:
    """A fuel that ships its own CO2 (e.g. from PLEXOS XML) keeps that
    CO2 row exactly — but the apply pass still ADDS the missing
    CH4 / N2O rows from the defaults.
    """
    custom_co2 = 0.0540  # NOT the IPCC default 0.0561
    planning = _planning(
        fuel_array=[
            {
                "uid": 1,
                "name": "natural_gas",
                "emission_factors": [
                    {"emission": "co2", "combustion": custom_co2},
                ],
            },
        ]
    )
    ng = EmissionFactor(
        name="natural_gas",
        aliases=("gas natural",),
        co2_combustion=0.0561,  # IPCC default
        co2_upstream=0.0,
        ch4_combustion=1e-6,
        n2o_combustion=1e-7,
        heat_content=48.0,
    )
    report = apply_emission_defaults(planning, _make_defaults([ng]))

    fuel = planning["system"]["fuel_array"][0]
    rows = {row["emission"]: row for row in fuel["emission_factors"]}
    # CO2 preserved at the source value, NOT clobbered.
    assert rows["co2"]["combustion"] == pytest.approx(custom_co2)
    # CH4 + N2O filled in from defaults.
    assert rows["ch4"]["combustion"] == pytest.approx(1e-6)
    assert rows["n2o"]["combustion"] == pytest.approx(1e-7)
    # Fuel WAS touched (CH4/N2O added); landed in `added`.
    assert report.fuels_factor_added == ("natural_gas",)
    assert not report.fuels_factor_preserved


def test_biogenic_zero_biomass_still_gets_ch4_n2o() -> None:
    """Biomass has co2_combustion=0 (biogenic-zero per IPCC AFOLU) but
    CH4 + N2O are real combustion byproducts (NOT biogenic-zero).
    The apply step must inject CH4 / N2O even though CO2 is zero.
    """
    biomass = EmissionFactor(
        name="biomass",
        aliases=("biomasa", "wood"),
        co2_combustion=0.0,  # biogenic-zero
        co2_upstream=0.0,
        ch4_combustion=3e-5,  # IPCC Table 2.2 wood/wood waste
        n2o_combustion=4e-6,
        heat_content=15.6,
    )
    planning = _planning(fuel_array=[{"uid": 1, "name": "Biomasa_Celco_B1"}])
    apply_emission_defaults(planning, _make_defaults([biomass]))

    fuel = planning["system"]["fuel_array"][0]
    rows = {row["emission"]: row for row in fuel["emission_factors"]}
    # CO2 not injected (zero combustion + zero upstream → skipped)
    assert "co2" not in rows
    # CH4 + N2O DO land (they're not biogenic-zero for combustion byproducts)
    assert rows["ch4"]["combustion"] == pytest.approx(3e-5)
    assert rows["n2o"]["combustion"] == pytest.approx(4e-6)

    pollutants = {p["name"] for p in planning["system"]["emission_array"]}
    assert pollutants == {"ch4", "n2o"}


def test_bundled_ipcc_defaults_carry_ch4_and_n2o() -> None:
    """The bundled defaults file ships CH4 + N2O on every fuel after the
    multi-pollutant extension.  Spot-check a few canonical values.
    """
    d = load_emission_defaults()
    # IPCC 2006 Vol 2 Ch 2 Table 2.2 spot-checks:
    diesel = d.lookup("diesel")
    assert diesel is not None
    assert diesel.ch4_combustion == pytest.approx(3e-6)  # 3 kg/TJ
    assert diesel.n2o_combustion == pytest.approx(6e-7)  # 0.6 kg/TJ
    coal = d.lookup("coal_bituminous")
    assert coal is not None
    assert coal.ch4_combustion == pytest.approx(1e-6)
    assert coal.n2o_combustion == pytest.approx(1.5e-6)
    ng = d.lookup("natural_gas")
    assert ng is not None
    assert ng.ch4_combustion == pytest.approx(1e-6)
    assert ng.n2o_combustion == pytest.approx(1e-7)
    biomass = d.lookup("biomass")
    assert biomass is not None
    assert biomass.co2_combustion == 0.0  # still biogenic-zero
    assert biomass.ch4_combustion == pytest.approx(
        3e-5
    )  # non-zero combustion byproduct
    assert biomass.n2o_combustion == pytest.approx(4e-6)
    # Biomass is now has_factor = True (CH4 + N2O carry it)
    assert biomass.has_factor


# ---------------------------------------------------------------------------
# EmissionZone synthesis (gap #5 from the emission-computability audit)
# ---------------------------------------------------------------------------


def test_emission_zone_synthesized_when_factor_added() -> None:
    """Without an EmissionZone covering 'co2', gtopt's
    expand_fuel_emission_sources returns early and the LP gets no
    emission rows.  --emissions must synthesize a default inert zone
    so the per-generator sources land.
    """
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel"}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    zones = planning["system"]["emission_zone_array"]
    assert len(zones) == 1
    zone = zones[0]
    assert zone["name"] == "global_co2"
    assert zone["uid"] == 1
    # Inert by default: no cap / cap_cost / price / allowance_pool
    for inert_key in ("cap", "cap_cost", "price", "allowance_pool"):
        assert inert_key not in zone, f"default zone must be inert: '{inert_key}' set"
    # Covers exactly co2 with weight 1.0
    assert zone["emissions"] == [{"emission": "co2", "weight": 1.0}]

    assert report.emission_zone_created is True
    assert report.emission_zone_already_present is False


def test_emission_zone_not_duplicated_when_already_present() -> None:
    """A user-supplied zone covering 'co2' must not be clobbered or
    duplicated, even when its name is not the default 'global_co2'.
    """
    existing = {
        "uid": 7,
        "name": "my_carbon_zone",
        "emissions": [{"emission": "co2", "weight": 1.0}],
        "price": 25.0,  # user-supplied carbon price
    }
    planning = _planning(
        fuel_array=[{"uid": 1, "name": "diesel"}],
        emission_zone_array=[existing],
    )
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    zones = planning["system"]["emission_zone_array"]
    assert len(zones) == 1  # no duplicate
    assert zones[0]["name"] == "my_carbon_zone"
    assert zones[0]["price"] == 25.0  # preserved
    assert report.emission_zone_created is False
    assert report.emission_zone_already_present is True


def test_emission_zone_not_synthesized_when_no_factor() -> None:
    """If no Fuel ends up with a factor (all unknown / biogenic-zero),
    no zone is created — keeps the JSON lean.
    """
    planning = _planning(fuel_array=[{"uid": 1, "name": "exotic"}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    # No factor added → no zone synthesized
    assert planning["system"].get("emission_zone_array", []) == []
    assert report.emission_zone_created is False


def test_emission_zone_uid_non_colliding() -> None:
    """When zones already exist (covering OTHER pollutants), the
    synthesized co2 zone uses a fresh uid.
    """
    nox_zone = {
        "uid": 3,
        "name": "nox_zone",
        "emissions": [{"emission": "nox", "weight": 1.0}],
    }
    planning = _planning(
        fuel_array=[{"uid": 1, "name": "diesel"}],
        emission_zone_array=[nox_zone],
    )
    apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    zones = planning["system"]["emission_zone_array"]
    assert len(zones) == 2
    uids = sorted(z["uid"] for z in zones)
    assert len(uids) == len(set(uids))
    assert max(uids) >= 4  # new uid > existing max


def test_emission_zone_present_for_other_emission_does_not_block_co2_zone() -> None:
    """A zone that covers, say, only NOx must NOT count as already
    covering CO2 — the converter still needs to synthesize a co2 zone.
    """
    nox_only = {
        "uid": 1,
        "name": "nox_zone",
        "emissions": [{"emission": "nox", "weight": 1.0}],
    }
    planning = _planning(
        fuel_array=[{"uid": 1, "name": "diesel"}],
        emission_zone_array=[nox_only],
    )
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))

    zones = planning["system"]["emission_zone_array"]
    # NOx zone preserved, new co2 zone added
    names = {z["name"] for z in zones}
    assert "nox_zone" in names
    assert "global_co2" in names
    assert report.emission_zone_created is True
    assert report.emission_zone_already_present is False


def test_emission_zone_idempotent() -> None:
    """A second apply pass must not create a second co2 zone."""
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel"}])
    d = _make_defaults([_FIX_DIESEL])
    apply_emission_defaults(planning, d)
    apply_emission_defaults(planning, d)
    zones = planning["system"]["emission_zone_array"]
    assert sum(1 for z in zones if z["name"] == "global_co2") == 1


def test_emission_zone_report_in_to_dict() -> None:
    """The on-disk report carries the new emission_zone_* fields."""
    planning = _planning(fuel_array=[{"uid": 1, "name": "diesel"}])
    report = apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))
    d = report.to_dict()
    assert d["summary"]["emission_zone_created"] is True
    assert d["summary"]["emission_zone_already_present"] is False


# ---------------------------------------------------------------------------
# Fuel.subtype hint — project-specific IPCC sub-grade routing
# ---------------------------------------------------------------------------


def test_subtype_hint_overrides_name_lookup() -> None:
    """When a Fuel JSON carries a ``subtype`` field, the emissions
    engine consults it FIRST — so two fuels in the same family name
    can resolve to DIFFERENT IPCC sub-grades.

    Concrete case: ``Gas_Colbun_GN_A`` (pipeline → natural_gas, no
    upstream) vs ``Gas_GNLQuintero_A`` (LNG → lng with upstream).
    Both share the ``Gas`` family prefix; the subtype hint
    disambiguates.
    """
    ng = EmissionFactor(
        name="natural_gas",
        aliases=("gas natural",),
        co2_combustion=0.0561,
        co2_upstream=0.0,
        heat_content=48.0,
    )
    lng = EmissionFactor(
        name="lng",
        aliases=("gnl",),
        co2_combustion=0.0561,
        co2_upstream=0.01,  # CEN-Chile LNG upstream chain
        heat_content=48.0,
    )
    planning = _planning(
        fuel_array=[
            {"uid": 1, "name": "Gas_Colbun_GN_A", "subtype": "natural_gas"},
            {"uid": 2, "name": "Gas_GNLQuintero_A", "subtype": "lng"},
        ],
    )
    apply_emission_defaults(planning, _make_defaults([ng, lng]))

    fuels = planning["system"]["fuel_array"]
    by_name = {f["name"]: f for f in fuels}
    pipeline = by_name["Gas_Colbun_GN_A"]
    lng_fuel = by_name["Gas_GNLQuintero_A"]

    # Both got CO2 (same combustion factor — IPCC says so).
    pip_co2 = next(r for r in pipeline["emission_factors"] if r["emission"] == "co2")
    lng_co2 = next(r for r in lng_fuel["emission_factors"] if r["emission"] == "co2")
    assert pip_co2["combustion"] == pytest.approx(0.0561)
    assert lng_co2["combustion"] == pytest.approx(0.0561)
    # But LNG has a non-zero upstream rate (the project-specific
    # CEN-Chile overlay) while pipeline does not.
    assert "upstream" not in pip_co2
    assert lng_co2["upstream"] == pytest.approx(0.01)


def test_subtype_hint_unrecognized_falls_back_to_name_lookup() -> None:
    """A subtype hint that doesn't resolve in the defaults falls
    through to the regular name + prefix-fallback path.  Lets
    upstream tools attach hints without breaking when the defaults
    file doesn't know about a given sub-grade.
    """
    coal = EmissionFactor(
        name="coal_bituminous",
        aliases=("coal", "carbon"),
        co2_combustion=0.0946,
        co2_upstream=0.0,
        heat_content=25.8,
    )
    planning = _planning(
        fuel_array=[
            {"uid": 1, "name": "Carbon_Andina", "subtype": "fancy_unknown_subtype"}
        ],
    )
    apply_emission_defaults(planning, _make_defaults([coal]))

    fuel = planning["system"]["fuel_array"][0]
    rows = {r["emission"]: r for r in fuel["emission_factors"]}
    # subtype didn't resolve → falls back to prefix-match on "Carbon"
    # → resolves via the "carbon" alias → coal_bituminous.
    assert rows["co2"]["combustion"] == pytest.approx(0.0946)


def test_empty_subtype_ignored() -> None:
    """``subtype: ""`` (the converter-side default) is the same as
    omitting the field — no override path, just regular name lookup.
    """
    planning = _planning(
        fuel_array=[{"uid": 1, "name": "diesel", "subtype": ""}],
    )
    apply_emission_defaults(planning, _make_defaults([_FIX_DIESEL]))
    rows = {
        r["emission"]: r
        for r in planning["system"]["fuel_array"][0]["emission_factors"]
    }
    assert rows["co2"]["combustion"] == pytest.approx(0.0741)


def test_cen_chile_override_file_loads_and_overrides_coal() -> None:
    """The shipped ``share/gtopt/emissions/cen_chile.json`` override
    file resolves the ``coal``/``carbon`` aliases to
    ``coal_sub_bituminous`` (0.0961) — the Chilean-fleet reality —
    instead of the IPCC default ``coal_bituminous`` (0.0946).
    """
    # Repo-relative path (resolves from any cwd via __file__ anchor).
    overlay = (
        Path(__file__).resolve().parents[3]
        / "share"
        / "gtopt"
        / "emissions"
        / "cen_chile.json"
    )
    if not overlay.exists():
        pytest.skip(f"CEN-Chile overlay not present at {overlay}")
    d = load_emission_defaults(overlay)
    # The "carbon" alias points at coal_sub_bituminous in this file.
    coal_hit = d.lookup("Carbon_Andina")
    assert coal_hit is not None
    assert coal_hit.name == "coal_sub_bituminous"
    assert coal_hit.co2_combustion == pytest.approx(0.0961)
    # LNG entry carries a non-zero upstream factor (CEN override).
    lng = d.lookup("lng")
    assert lng is not None
    assert lng.co2_upstream == pytest.approx(0.01)
    # Pipeline NG keeps upstream = 0.
    ng = d.lookup("natural_gas")
    assert ng is not None
    assert ng.co2_upstream == pytest.approx(0.0)
