# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``cen2gtopt._emission_factors`` — TTW + WTT factor table."""

from __future__ import annotations

import argparse

import pytest

from cen2gtopt._emission_factors import (
    EmissionFactor,
    UpstreamBasis,
    add_upstream_basis_arg,
    canonical_fuel_key,
    lookup,
    lookup_scvic,
    parse_upstream_basis,
)
from cen2gtopt._heatrate_sip import HeatRateFit


# -----------------------------------------------------------------------------
# UpstreamBasis enum
# -----------------------------------------------------------------------------


class TestUpstreamBasis:
    """Enum surface."""

    def test_default_is_none(self) -> None:
        assert UpstreamBasis.default() == UpstreamBasis.NONE

    def test_string_values_are_kebab_case(self) -> None:
        # Avoid surprise — flag values must match what the CLI presents.
        assert UpstreamBasis.NONE.value == "none"
        assert UpstreamBasis.AR_PIPELINE.value == "ar-pipeline"
        assert UpstreamBasis.MIXED.value == "mixed"
        assert UpstreamBasis.US_LNG.value == "us-lng"

    def test_round_trip_value(self) -> None:
        for b in UpstreamBasis:
            assert UpstreamBasis(b.value) is b


# -----------------------------------------------------------------------------
# canonical_fuel_key — SCVIC → canonical mapping
# -----------------------------------------------------------------------------


class TestCanonicalFuelKey:
    """SCVIC fuel name normalisation."""

    @pytest.mark.parametrize(
        ("scvic_name", "expected"),
        [
            # Accented form (live SCVIC output)
            ("Carbón", "coal_subbituminous"),
            ("Carbón Bituminoso", "coal_bituminous"),
            # Multi-fuel blends collapse to dominant component
            ("Carbón + Petcoke", "coal_subbituminous"),
            ("Petcoke", "petcoke"),
            # Diesel synonyms (SCVIC writes both)
            ("Diesel", "diesel"),
            ("Diésel", "diesel"),
            ("Petróleo Diésel", "diesel"),
            # Fuel oil variants
            ("Fuel Oil", "fuel_oil"),
            ("Fuel Oil Nro. 6", "fuel_oil"),
            ("IFO380", "fuel_oil"),
            # NG synonyms
            ("Gas Natural", "natural_gas"),
            ("GNL", "natural_gas"),
            # LPG / propane
            ("GLP", "lpg"),
            ("Gas Propano", "lpg"),
            # Biomass
            ("Biomasa", "biomass_solid"),
            ("Biogas", "biogas"),
            # Naphtha
            ("Nafta", "naphtha"),
            # Case + whitespace robustness
            ("  carbón  ", "coal_subbituminous"),
            ("CARBÓN", "coal_subbituminous"),
        ],
    )
    def test_scvic_aliases_map_to_canonical(
        self, scvic_name: str, expected: str
    ) -> None:
        assert canonical_fuel_key(scvic_name) == expected

    @pytest.mark.parametrize(
        "scvic_name",
        [None, "", "Hidráulica", "Solar", "Eólica", "Geotérmica", "Unknown"],
    )
    def test_non_fossil_returns_none(self, scvic_name: str | None) -> None:
        # Non-fossil and unknown labels return None — caller must
        # handle. Avoids silent collapse-to-zero on typos.
        assert canonical_fuel_key(scvic_name) is None


# -----------------------------------------------------------------------------
# lookup
# -----------------------------------------------------------------------------


class TestLookup:
    """Direct (fuel_key, basis) → EmissionFactor lookup."""

    def test_returns_emission_factor(self) -> None:
        ef = lookup("natural_gas", UpstreamBasis.US_LNG)
        assert isinstance(ef, EmissionFactor)
        assert ef.fuel_key == "natural_gas"
        assert ef.basis == UpstreamBasis.US_LNG

    def test_ttw_invariant_across_bases(self) -> None:
        # TTW depends only on the fuel — the basis must not change it.
        for fuel in (
            "coal_subbituminous",
            "diesel",
            "natural_gas",
            "fuel_oil",
        ):
            ttw_values = {lookup(fuel, b).ttw_kg_per_gj for b in UpstreamBasis}
            assert len(ttw_values) == 1, (
                f"TTW for {fuel} differs across bases: {ttw_values}"
            )

    def test_wtt_zero_for_basis_none(self) -> None:
        for fuel_key in ("coal_subbituminous", "diesel", "natural_gas"):
            ef = lookup(fuel_key, UpstreamBasis.NONE)
            assert ef.wtt_kg_per_gj == 0.0
            assert ef.total_kg_per_gj == ef.ttw_kg_per_gj

    def test_natural_gas_wtt_ordering(self) -> None:
        # For NG specifically, AR_PIPELINE < MIXED ≤ US_LNG (WTT only).
        wtt = {b: lookup("natural_gas", b).wtt_kg_per_gj for b in UpstreamBasis}
        assert wtt[UpstreamBasis.NONE] == 0.0
        assert wtt[UpstreamBasis.AR_PIPELINE] < wtt[UpstreamBasis.MIXED]
        assert wtt[UpstreamBasis.MIXED] <= wtt[UpstreamBasis.US_LNG]
        # Sanity range — should sit between 5 and 25 for any sensible
        # basis (literature range, rounded out).
        for b in (UpstreamBasis.AR_PIPELINE, UpstreamBasis.US_LNG):
            assert 5.0 < wtt[b] < 25.0

    def test_coal_wtt_basis_insensitive(self) -> None:
        # Coal WTT does not depend on the gas-supply basis flag —
        # we still record a value but it must be identical across
        # the three non-NONE bases.
        wtt = {
            b: lookup("coal_subbituminous", b).wtt_kg_per_gj
            for b in (
                UpstreamBasis.AR_PIPELINE,
                UpstreamBasis.MIXED,
                UpstreamBasis.US_LNG,
            )
        }
        assert len(set(wtt.values())) == 1

    def test_total_equals_ttw_plus_wtt(self) -> None:
        ef = lookup("natural_gas", UpstreamBasis.MIXED)
        assert ef.total_kg_per_gj == ef.ttw_kg_per_gj + ef.wtt_kg_per_gj

    def test_unknown_fuel_raises(self) -> None:
        with pytest.raises(KeyError, match="unknown fuel/basis"):
            lookup("uranium", UpstreamBasis.NONE)

    def test_biomass_ttw_is_zero(self) -> None:
        # IPCC biogenic-CO₂ exclusion convention.
        for b in UpstreamBasis:
            assert lookup("biomass_solid", b).ttw_kg_per_gj == 0.0
            assert lookup("biogas", b).ttw_kg_per_gj == 0.0

    def test_biomass_has_nonzero_wtt_when_basis_not_none(self) -> None:
        # Biogenic ≠ zero-footprint — supply chain still emits.
        ef = lookup("biomass_solid", UpstreamBasis.MIXED)
        assert ef.wtt_kg_per_gj > 0.0


# -----------------------------------------------------------------------------
# lookup_scvic
# -----------------------------------------------------------------------------


class TestLookupScvic:
    """SCVIC-name convenience wrapper."""

    def test_known_scvic_name(self) -> None:
        ef = lookup_scvic("Carbón", UpstreamBasis.MIXED)
        assert ef is not None
        assert ef.fuel_key == "coal_subbituminous"
        assert ef.basis == UpstreamBasis.MIXED

    def test_default_basis_is_none(self) -> None:
        ef = lookup_scvic("Diesel")
        assert ef is not None
        assert ef.basis == UpstreamBasis.NONE

    @pytest.mark.parametrize("name", [None, "", "Hidráulica", "Solar", "Unknown Fuel"])
    def test_non_fossil_or_unknown_returns_none(self, name: str | None) -> None:
        assert lookup_scvic(name) is None


# -----------------------------------------------------------------------------
# Coupling with the heat-rate fit (the actual end-use pattern)
# -----------------------------------------------------------------------------


class TestEpsilonComputation:
    """Compose the WTT/TTW factors with a HeatRateFit to recover ε(P)."""

    def test_tarapaca_ttw_only(self) -> None:
        # Tarapaca 3-point fit (from test_heatrate_sip).
        fit = HeatRateFit(
            a_gj_per_h=39.7,
            b_gj_per_mwh=9.31,
            rsq=0.999,
            n_points=3,
        )
        ef = lookup("coal_subbituminous", UpstreamBasis.NONE)
        # ε(150 MW) = (a/P + b) * EF_TTW
        eps_full = (fit.a_gj_per_h / 150.0 + fit.b_gj_per_mwh) * (
            ef.ttw_kg_per_gj + ef.wtt_kg_per_gj
        )
        eps_pmin = (fit.a_gj_per_h / 90.0 + fit.b_gj_per_mwh) * (
            ef.ttw_kg_per_gj + ef.wtt_kg_per_gj
        )
        assert eps_pmin > eps_full
        # Reference values: ε(150)≈920, ε(90)≈935 with EF_TTW=96.1
        assert 900.0 < eps_full < 960.0
        assert 920.0 < eps_pmin < 980.0

    def test_wtt_increases_epsilon(self) -> None:
        fit = HeatRateFit(
            a_gj_per_h=39.7,
            b_gj_per_mwh=9.31,
            rsq=0.999,
            n_points=3,
        )
        ef_ttw = lookup("coal_subbituminous", UpstreamBasis.NONE)
        ef_wtw = lookup("coal_subbituminous", UpstreamBasis.MIXED)
        eps_ttw = (fit.a_gj_per_h / 150.0 + fit.b_gj_per_mwh) * (
            ef_ttw.ttw_kg_per_gj + ef_ttw.wtt_kg_per_gj
        )
        eps_wtw = (fit.a_gj_per_h / 150.0 + fit.b_gj_per_mwh) * (
            ef_wtw.ttw_kg_per_gj + ef_wtw.wtt_kg_per_gj
        )
        assert eps_wtw > eps_ttw
        # Curvature ratio is preserved (both factors get the same
        # 1/(a/P + b) shape).
        ratio = eps_wtw / eps_ttw
        expected_ratio = ef_wtw.total_kg_per_gj / ef_ttw.total_kg_per_gj
        assert ratio == pytest.approx(expected_ratio, rel=1e-9)


# -----------------------------------------------------------------------------
# CLI helpers
# -----------------------------------------------------------------------------


class TestCli:
    """argparse hook + value coercion."""

    def test_add_upstream_basis_arg_default(self) -> None:
        parser = argparse.ArgumentParser()
        add_upstream_basis_arg(parser)
        args = parser.parse_args([])
        assert args.upstream_basis == UpstreamBasis.NONE.value

    def test_add_upstream_basis_arg_explicit(self) -> None:
        parser = argparse.ArgumentParser()
        add_upstream_basis_arg(parser)
        args = parser.parse_args(["--upstream-basis", "us-lng"])
        assert args.upstream_basis == "us-lng"

    def test_add_upstream_basis_arg_rejects_typo(self) -> None:
        parser = argparse.ArgumentParser()
        add_upstream_basis_arg(parser)
        with pytest.raises(SystemExit):
            parser.parse_args(["--upstream-basis", "argentina"])

    def test_add_upstream_basis_arg_custom_dest(self) -> None:
        parser = argparse.ArgumentParser()
        add_upstream_basis_arg(parser, dest="ef_basis")
        args = parser.parse_args(["--ef-basis", "mixed"])
        assert args.ef_basis == "mixed"

    def test_parse_upstream_basis_string(self) -> None:
        assert parse_upstream_basis("us-lng") == UpstreamBasis.US_LNG

    def test_parse_upstream_basis_enum(self) -> None:
        assert parse_upstream_basis(UpstreamBasis.MIXED) == UpstreamBasis.MIXED

    def test_parse_upstream_basis_none(self) -> None:
        assert parse_upstream_basis(None) == UpstreamBasis.default()

    def test_parse_upstream_basis_unknown_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown upstream basis"):
            parse_upstream_basis("argentina")
