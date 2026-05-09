# SPDX-License-Identifier: BSD-3-Clause
r"""Per-fuel emission factor table — combustion (TTW) + upstream (WTT).

This module is the dispatch-independent companion to
:mod:`cen2gtopt._heatrate_sip`. It provides the per-fuel emission
factor that callers multiply by fuel consumption (GJ/h) to obtain
CO₂eq mass-flow (kg/h), or — when divided by power — by the
heat-rate fit ``HR(P)`` to obtain ε(P) in kg CO₂eq/MWh.

The factors are stored in **kg CO₂eq / GJ_LHV** (lower-heating-value
basis), the natural denominator for the
:func:`cen2gtopt._heatrate_sip.fit_linear_fuel` output.

For per-MWh ε at dispatch P:

    ε(P) = (a/P + b) · (EF_TTW + EF_WTT)        [kg CO₂eq / MWh]

with ``a, b`` from :class:`HeatRateFit` and ``(EF_TTW, EF_WTT)`` from
:func:`lookup`.

Lifecycle terminology
---------------------

The standard LCA decomposition for thermal power generation is::

    +--------+   +--------+   +--------+   +-------+   +-------+
    | well / |--→| extract|--→|process,|--→|transp.|--→|combust|
    | mine   |   |        |   |refine  |   | & LNG |   | (stack)|
    +--------+   +--------+   +--------+   +-------+   +-------+
       \____________ Well-to-Tank (WTT) ____________/   \_TTW _/
       \________________________ Well-to-Wire (WTW) ___________/

* **TTW** (tank-to-wheel / stack / direct / Scope 1): only the CO₂
  released by burning the fuel at the plant. This is what IPCC 2006
  guidelines and the Chile MMA national inventory tabulate.

* **WTT** (well-to-tank / upstream / Scope 3 cat. 3): everything
  before the fuel arrives at the plant — extraction methane, refinery
  heat, pipeline compression, LNG liquefaction + shipping. Highly
  variable for natural gas (5–20 kg CO₂eq/GJ depending on origin).

* **WTW** (well-to-wire / cradle-to-bus): TTW + WTT, the right total
  for a footprint that wants to credit the fuel-supply chain.

We expose both halves so the caller can choose: a pure dispatch-stack
audit (TTW only), a full footprint (WTW), or a sensitivity sweep
across upstream bases (e.g.\ to compare Argentina pipeline gas vs
US LNG for the same plant).

Sources
-------

TTW factors are IPCC 2006 Guidelines for National GHG Inventories,
Volume 2 Energy, Table 1.4 (default values, AR6 GWP100). These are
the values used by Chile MMA in the National GHG Inventory and by
SEC for emission audits.

WTT factors are harmonised from:

* NETL "Life Cycle Analysis of Natural Gas Extraction and Power
  Generation", DOE/NETL-2019/2042 (US production)
* NETL "Life Cycle Analysis of Bituminous Coal-Based Power Generation",
  DOE/NETL-2010/1429
* IEA Methane Tracker 2024 — country-resolved upstream NG methane
  intensity (used for Argentina pipeline gas)
* DEFRA 2024 GHG Conversion Factors — refined-product WTT (diesel,
  fuel oil)
* JEC Well-to-Wheels v5 (2020) — biomass + biogas pathway

All values harmonised to LHV basis and AR6 GWP100 (CH₄ = 29.8,
N₂O = 273).
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Final


class UpstreamBasis(str, Enum):
    """Upstream-EF selection for the natural-gas pathway.

    Ordered by typical SEN delivery footprint (lowest WTT first).
    """

    NONE = "none"  # combustion only (TTW), no upstream — Scope 1 audit
    AR_PIPELINE = "ar-pipeline"  # Argentina pipeline (Mejillones / Cuenca Neuquén)
    MIXED = "mixed"  # 50/50 weighted SEN-typical (LNG + AR pipeline)
    US_LNG = "us-lng"  # US Henry-Hub LNG cargo (Trinidad / Sabine Pass)

    @classmethod
    def default(cls) -> UpstreamBasis:
        """Recommended default — combustion-only stays Scope-1-clean."""
        return cls.NONE


@dataclass(slots=True, frozen=True)
class EmissionFactor:
    """One ``(TTW, WTT)`` pair for a canonical fuel + upstream basis.

    Both values in kg CO₂eq / GJ_LHV. The ``source`` and ``vintage``
    fields document provenance; ``notes`` carries any caveat the
    caller should propagate to a footnote.
    """

    fuel_key: str
    ttw_kg_per_gj: float
    wtt_kg_per_gj: float
    basis: UpstreamBasis
    source: str
    vintage: str
    notes: str = ""

    @property
    def total_kg_per_gj(self) -> float:
        """Well-to-wire total = TTW + WTT, kg CO₂eq / GJ_LHV."""
        return self.ttw_kg_per_gj + self.wtt_kg_per_gj


# ---------------------------------------------------------------------------
# Combustion (TTW) factors — IPCC 2006 GL Vol. 2 Table 1.4 default values.
# kg CO₂ / GJ_LHV.
# ---------------------------------------------------------------------------
TTW_KG_PER_GJ: Final[dict[str, float]] = {
    "coal_subbituminous": 96.1,
    "coal_bituminous": 94.6,
    "petcoke": 97.5,
    "diesel": 74.1,
    "fuel_oil": 77.4,
    "natural_gas": 56.1,
    "lpg": 63.1,
    "biomass_solid": 0.0,  # biogenic CO₂ — not counted (IPCC convention)
    "biogas": 0.0,
    "naphtha": 73.3,
}

# ---------------------------------------------------------------------------
# Upstream (WTT) factors — multi-basis where it matters (natural gas).
# kg CO₂eq / GJ_LHV, AR6 GWP100.
# ---------------------------------------------------------------------------
_WTT_TABLE: Final[dict[tuple[str, UpstreamBasis], EmissionFactor]] = {
    # Coal — Cerrejón Colombia + ocean shipping is the dominant SEN supply.
    ("coal_subbituminous", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="coal_subbituminous",
        ttw_kg_per_gj=96.1,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("coal_subbituminous", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="coal_subbituminous",
        ttw_kg_per_gj=96.1,
        wtt_kg_per_gj=6.5,
        basis=UpstreamBasis.AR_PIPELINE,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
        notes="Mining CMM + ocean shipping (Cerrejón → Mejillones)",
    ),
    ("coal_subbituminous", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="coal_subbituminous",
        ttw_kg_per_gj=96.1,
        wtt_kg_per_gj=6.5,
        basis=UpstreamBasis.MIXED,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
        notes="Coal supply chain is basis-insensitive — same as AR_PIPELINE",
    ),
    ("coal_subbituminous", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="coal_subbituminous",
        ttw_kg_per_gj=96.1,
        wtt_kg_per_gj=6.5,
        basis=UpstreamBasis.US_LNG,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
        notes="Coal supply chain is basis-insensitive — same as AR_PIPELINE",
    ),
    # Bituminous coal — small SEN footprint, value mirrors sub-bituminous
    # range. Provided for completeness.
    ("coal_bituminous", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="coal_bituminous",
        ttw_kg_per_gj=94.6,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("coal_bituminous", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="coal_bituminous",
        ttw_kg_per_gj=94.6,
        wtt_kg_per_gj=7.5,
        basis=UpstreamBasis.AR_PIPELINE,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
        notes="Bituminous mines have higher CMM than sub-bituminous",
    ),
    ("coal_bituminous", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="coal_bituminous",
        ttw_kg_per_gj=94.6,
        wtt_kg_per_gj=7.5,
        basis=UpstreamBasis.MIXED,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
    ),
    ("coal_bituminous", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="coal_bituminous",
        ttw_kg_per_gj=94.6,
        wtt_kg_per_gj=7.5,
        basis=UpstreamBasis.US_LNG,
        source="NETL DOE/NETL-2010/1429",
        vintage="2010",
    ),
    # Petroleum coke — companion to coal; modest WTT (refinery byproduct).
    ("petcoke", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="petcoke",
        ttw_kg_per_gj=97.5,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("petcoke", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="petcoke",
        ttw_kg_per_gj=97.5,
        wtt_kg_per_gj=4.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
        notes="Petcoke is a refinery byproduct — low upstream allocation",
    ),
    ("petcoke", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="petcoke",
        ttw_kg_per_gj=97.5,
        wtt_kg_per_gj=4.0,
        basis=UpstreamBasis.MIXED,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("petcoke", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="petcoke",
        ttw_kg_per_gj=97.5,
        wtt_kg_per_gj=4.0,
        basis=UpstreamBasis.US_LNG,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    # Diesel oil — refinery + ocean import.
    ("diesel", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="diesel",
        ttw_kg_per_gj=74.1,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("diesel", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="diesel",
        ttw_kg_per_gj=74.1,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
        notes="Refinery + transport, basis-insensitive",
    ),
    ("diesel", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="diesel",
        ttw_kg_per_gj=74.1,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.MIXED,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("diesel", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="diesel",
        ttw_kg_per_gj=74.1,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.US_LNG,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    # Heavy fuel oil (No. 6 / IFO380) — backup units only.
    ("fuel_oil", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="fuel_oil",
        ttw_kg_per_gj=77.4,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("fuel_oil", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="fuel_oil",
        ttw_kg_per_gj=77.4,
        wtt_kg_per_gj=12.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("fuel_oil", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="fuel_oil",
        ttw_kg_per_gj=77.4,
        wtt_kg_per_gj=12.0,
        basis=UpstreamBasis.MIXED,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("fuel_oil", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="fuel_oil",
        ttw_kg_per_gj=77.4,
        wtt_kg_per_gj=12.0,
        basis=UpstreamBasis.US_LNG,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    # Natural gas — the high-variance fuel where the basis flag earns its keep.
    ("natural_gas", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="natural_gas",
        ttw_kg_per_gj=56.1,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("natural_gas", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="natural_gas",
        ttw_kg_per_gj=56.1,
        wtt_kg_per_gj=12.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="IEA Methane Tracker 2024 (Argentina) + JEC WTW v5",
        vintage="2024",
        notes="Cuenca Neuquén production, ~1.0%-of-prod methane intensity",
    ),
    ("natural_gas", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="natural_gas",
        ttw_kg_per_gj=56.1,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.MIXED,
        source="IEA Methane Tracker 2024 + NETL DOE/NETL-2019/2042",
        vintage="2024",
        notes="50/50 weighted SEN-typical (Argentina pipeline + LNG)",
    ),
    ("natural_gas", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="natural_gas",
        ttw_kg_per_gj=56.1,
        wtt_kg_per_gj=16.5,
        basis=UpstreamBasis.US_LNG,
        source="NETL DOE/NETL-2019/2042 (US Henry-Hub) + IEA LNG shipping",
        vintage="2024",
        notes="US production + liquefaction + shipping (Trinidad / Sabine Pass)",
    ),
    # LPG (propane) — peaking units only.
    ("lpg", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="lpg",
        ttw_kg_per_gj=63.1,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("lpg", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="lpg",
        ttw_kg_per_gj=63.1,
        wtt_kg_per_gj=10.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("lpg", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="lpg",
        ttw_kg_per_gj=63.1,
        wtt_kg_per_gj=10.0,
        basis=UpstreamBasis.MIXED,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("lpg", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="lpg",
        ttw_kg_per_gj=63.1,
        wtt_kg_per_gj=10.0,
        basis=UpstreamBasis.US_LNG,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    # Solid biomass — biogenic-zero TTW (IPCC convention) + harvest WTT.
    ("biomass_solid", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="biomass_solid",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Ch.2 §2.3.3.4 (biogenic CO₂ excluded)",
        vintage="2006/AR6",
    ),
    ("biomass_solid", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="biomass_solid",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=6.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="JEC Well-to-Wheels v5 (2020), forest-residue pathway",
        vintage="2020",
        notes="Harvest + chipping + transport; excludes land-use carbon debt",
    ),
    ("biomass_solid", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="biomass_solid",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=6.0,
        basis=UpstreamBasis.MIXED,
        source="JEC Well-to-Wheels v5 (2020)",
        vintage="2020",
    ),
    ("biomass_solid", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="biomass_solid",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=6.0,
        basis=UpstreamBasis.US_LNG,
        source="JEC Well-to-Wheels v5 (2020)",
        vintage="2020",
    ),
    # Biogas — WWTP / landfill / agricultural digestion.
    ("biogas", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="biogas",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Ch.2 (biogenic CO₂ excluded)",
        vintage="2006/AR6",
    ),
    ("biogas", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="biogas",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=2.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="JEC Well-to-Wheels v5 (2020), biogas pathway",
        vintage="2020",
        notes="Digester electricity + upgrading losses",
    ),
    ("biogas", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="biogas",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=2.0,
        basis=UpstreamBasis.MIXED,
        source="JEC Well-to-Wheels v5 (2020)",
        vintage="2020",
    ),
    ("biogas", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="biogas",
        ttw_kg_per_gj=0.0,
        wtt_kg_per_gj=2.0,
        basis=UpstreamBasis.US_LNG,
        source="JEC Well-to-Wheels v5 (2020)",
        vintage="2020",
    ),
    # Naphtha — emerging units (e.g. Tarapacá run on imported naphtha
    # for a few campaigns). Same WTT as diesel.
    ("naphtha", UpstreamBasis.NONE): EmissionFactor(
        fuel_key="naphtha",
        ttw_kg_per_gj=73.3,
        wtt_kg_per_gj=0.0,
        basis=UpstreamBasis.NONE,
        source="IPCC 2006 GL Vol.2 Tab.1.4",
        vintage="2006/AR6",
    ),
    ("naphtha", UpstreamBasis.AR_PIPELINE): EmissionFactor(
        fuel_key="naphtha",
        ttw_kg_per_gj=73.3,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.AR_PIPELINE,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("naphtha", UpstreamBasis.MIXED): EmissionFactor(
        fuel_key="naphtha",
        ttw_kg_per_gj=73.3,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.MIXED,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
    ("naphtha", UpstreamBasis.US_LNG): EmissionFactor(
        fuel_key="naphtha",
        ttw_kg_per_gj=73.3,
        wtt_kg_per_gj=14.0,
        basis=UpstreamBasis.US_LNG,
        source="DEFRA 2024 GHG Conversion Factors",
        vintage="2024",
    ),
}

# ---------------------------------------------------------------------------
# SCVIC fuel name → canonical fuel_key — accent-tolerant, case-tolerant.
# ---------------------------------------------------------------------------
SCVIC_FUEL_ALIASES: Final[dict[str, str]] = {
    # SCVIC writes "Carbón" (with accent) for sub-bituminous; "Carbón
    # Bituminoso" appears occasionally in unit-master data.
    "carbon": "coal_subbituminous",
    "carbon subbituminoso": "coal_subbituminous",
    "carbon sub-bituminoso": "coal_subbituminous",
    "carbon bituminoso": "coal_bituminous",
    "carbon + petcoke": "coal_subbituminous",  # blended — dominant component
    "petcoke": "petcoke",
    "diesel": "diesel",
    "petroleo diesel": "diesel",
    "petroleo": "diesel",
    "fuel oil": "fuel_oil",
    "fuel oil nro 6": "fuel_oil",
    "fuel oil nro. 6": "fuel_oil",
    "fuel oil n6": "fuel_oil",
    "ifo380": "fuel_oil",
    "gas natural": "natural_gas",
    "gnl": "natural_gas",
    "lng": "natural_gas",
    "glp": "lpg",
    "gas propano": "lpg",
    "propano": "lpg",
    "biogas": "biogas",
    "biomasa": "biomass_solid",
    "biomass": "biomass_solid",
    "lena": "biomass_solid",
    "naphtha": "naphtha",
    "nafta": "naphtha",
}


def _normalize_scvic_fuel(raw: str) -> str:
    """Lowercase + strip accents + collapse whitespace, for alias lookup."""
    import unicodedata

    nfd = unicodedata.normalize("NFD", raw or "")
    no_marks = "".join(ch for ch in nfd if not unicodedata.combining(ch))
    return " ".join(no_marks.lower().split())


def canonical_fuel_key(scvic_fuel_name: str | None) -> str | None:
    """Map a SCVIC ``fuel`` string to a canonical fuel_key, or ``None``."""
    if not scvic_fuel_name:
        return None
    return SCVIC_FUEL_ALIASES.get(_normalize_scvic_fuel(scvic_fuel_name))


def lookup(
    fuel_key: str,
    basis: UpstreamBasis = UpstreamBasis.NONE,
) -> EmissionFactor:
    """Return the ``EmissionFactor`` for a canonical fuel + upstream basis.

    Raises :class:`KeyError` for an unknown ``fuel_key`` so callers
    don't silently miscompute on a typo. The ``UpstreamBasis`` is
    enum-typed so a typo on the basis is a static error.
    """
    try:
        return _WTT_TABLE[(fuel_key, basis)]
    except KeyError as exc:
        msg = (
            f"unknown fuel/basis combination: {fuel_key!r} × {basis.value!r}; "
            f"known fuels: {sorted({k for k, _ in _WTT_TABLE})}"
        )
        raise KeyError(msg) from exc


def lookup_scvic(
    scvic_fuel_name: str | None,
    basis: UpstreamBasis = UpstreamBasis.NONE,
) -> EmissionFactor | None:
    """Convenience: SCVIC fuel name → ``EmissionFactor`` or ``None``.

    Returns ``None`` for unknown / blank / non-fossil SCVIC labels
    (Hidráulica, Solar, Eólica, Geotérmica, …) — matching the
    behaviour of the legacy ``FUEL_EF.get(name, 0.0)`` lookup but
    without silently collapsing typos to zero.
    """
    fuel_key = canonical_fuel_key(scvic_fuel_name)
    if fuel_key is None:
        return None
    return lookup(fuel_key, basis)


# ---------------------------------------------------------------------------
# CLI helper — opt-in plumbing for callers that want to expose the flag.
# ---------------------------------------------------------------------------


def add_upstream_basis_arg(parser, *, dest: str = "upstream_basis") -> None:
    """Add ``--upstream-basis`` to an argparse parser.

    Usage::

        from cen2gtopt._emission_factors import (
            add_upstream_basis_arg, parse_upstream_basis,
        )
        parser = argparse.ArgumentParser(...)
        add_upstream_basis_arg(parser)
        args = parser.parse_args()
        basis = parse_upstream_basis(args.upstream_basis)

    The default is :class:`UpstreamBasis.NONE` — a Scope-1 (combustion-
    only) audit, which matches the behaviour of the pre-existing
    ``FUEL_EF`` table. Switch to ``mixed`` or ``us-lng`` for a Scope-3-
    inclusive footprint.
    """
    parser.add_argument(
        f"--{dest.replace('_', '-')}",
        dest=dest,
        choices=[b.value for b in UpstreamBasis],
        default=UpstreamBasis.default().value,
        help=(
            "Upstream-emission basis to add to combustion (TTW). "
            "'none' = Scope-1 audit only; 'ar-pipeline' = Argentina "
            "pipeline gas; 'us-lng' = US Henry-Hub LNG; "
            "'mixed' = 50/50 SEN-typical."
        ),
    )


def parse_upstream_basis(value: str | UpstreamBasis | None) -> UpstreamBasis:
    """Coerce a string flag value to :class:`UpstreamBasis`.

    Accepts the enum value (``"ar-pipeline"``), the enum member
    itself, or ``None`` (returns the default). Raises ``ValueError``
    for unrecognised strings.
    """
    if value is None:
        return UpstreamBasis.default()
    if isinstance(value, UpstreamBasis):
        return value
    try:
        return UpstreamBasis(value)
    except ValueError as exc:
        valid = ", ".join(b.value for b in UpstreamBasis)
        msg = f"unknown upstream basis {value!r}; choose from {{{valid}}}"
        raise ValueError(msg) from exc
