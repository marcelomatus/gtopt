# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt.pcp_inputs`` — read CEN PCP LP-input CSV bundles.

The ``DATOS{YYYYMMDD}.zip`` payload of every ``PLEXOS{date}.zip``
bundle (see :mod:`cen2gtopt.pcp_archive`) contains the **complete LP
inputs** of CEN's day-ahead unit-commitment model: per-unit heat
rates, per-fuel prices, per-reservoir water values, line limits,
per-bus loads, take-or-pay constraints, and reserve requirements.

This module provides a typed, lazy-loaded reader for those CSVs plus
a few derived quantities (merit-order marginal cost per generator
per period, expected thermal marginal CV per hour).

Per-unit names in the PCP CSVs (``ENAP_ACONCAGUA``,
``ANGAMOS_1``, ``ANDES_U1-U2_DIE``) are bridged to CEN's canonical
``id_central`` via :mod:`cen2gtopt._id_bridge`, with PCP-specific
suffix strippers (``_DIE``, ``_GN``, ``_GNL``, ``_GP``, ``_BL[N]``).

Examples
--------

::

    # Load + summary print
    python -m cen2gtopt.pcp_inputs summary --date 2026-04-07

    # Library use
    from cen2gtopt.pcp_inputs import load_pcp_inputs
    inputs = load_pcp_inputs("2026-04-07")
    mc = inputs.marginal_cost_per_unit(period=18)   # 18:00
    print(mc.sort_values('mc_usd_mwh').head(20))

    # Per-fuel emission factor (kg CO₂ / MWh)
    eps = inputs.epsilon_per_unit()
    print(eps.head())

    # Compare PCP-derived merit with our pipeline:
    python -m cen2gtopt.pcp_validate --date 2026-04-07
"""

from __future__ import annotations

import argparse
import logging
import re
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from typing import Any

import pandas as pd

from cen2gtopt._id_bridge import norm as _norm, resolve as bridge_resolve
from cen2gtopt.pcp_archive import (
    DEFAULT_DOWNLOAD_ROOT,
    download_one,
    fetch_index,
)

_LOG = logging.getLogger("cen2gtopt.pcp_inputs")


#: Per-fuel CO₂ emission factor (kg CO₂ / GJ).  IPCC 2006 GL Vol. 2
#: default values × Chilean SEN's typical fuel mix mapping.  Used in
#: combination with the per-unit ``HeatRate (GJ/MWh)`` from the PCP
#: CSVs to derive a per-unit ε in kg CO₂ / MWh.
FUEL_CO2_KG_PER_GJ: dict[str, float] = {
    # PCP fuel-name patterns (substring-matched, case-insensitive)
    "Carbon": 94.6,  # bituminous coal
    "Petcoke": 97.5,
    "Diesel": 74.1,
    "DIE": 74.1,
    "Fuel": 77.4,  # fuel oil
    "Gas": 56.1,  # natural gas (covers Gas, GN, GNL)
    "GLP": 63.1,  # propane / LPG
    "Biogas": 0.0,  # IPCC biogenic-zero
    "Biomasa": 0.0,
    "Eolica": 0.0,
    "Solar": 0.0,
    "Hydro": 0.0,
}


def _co2_factor(fuel_name: str) -> float:
    """Resolve a fuel string to kg CO₂ / GJ.  Returns 0 on unknown."""
    if not fuel_name:
        return 0.0
    upper = str(fuel_name).upper()
    for needle, val in FUEL_CO2_KG_PER_GJ.items():
        if needle.upper() in upper:
            return val
    return 0.0


# ---------------------------------------------------------------------------
# PCP unit-name → id_central bridge with PCP-specific suffix strippers
# ---------------------------------------------------------------------------


_PCP_SUFFIXES = (
    "_die",
    "_dies",
    "_diesel",  # diesel sub-mode
    "_gn",
    "_gnl",
    "_gp",  # gas sub-modes
    "_b1",
    "_b2",
    "_b3",
    "_b4",  # block id
    "_bl1",
    "_bl2",
    "_bl3",
    "_bl4",
    "_bl5",
    "_u1",
    "_u2",
    "_u3",
    "_u4",
    "_u5",
    "_u6",
    "_u7",
    "_u8",
    "_u9",
    "_a",
    "_b",
    "_c",
    "_1",
    "_2",
    "_3",
    "_4",
    "_5",
    "_6",
)


#: CEN catalogue prefixes that PCP names omit.  E.g. PID lists
#: ``ABANICO`` while ``unidades-generadoras`` registers it as
#: "HP ABANICO" (Hidroeléctrica de Pasada) → normalises to
#: ``hpabanico``.  We try every prepended variant so a PID name
#: matches the alias index.
_CEN_TYPE_PREFIXES = (
    "hp",
    "he",
    "ter",
    "terhp",
    "terhe",
    "fv",
    "terfv",
    "eo",
    "tereo",
    "cs",
    "tercs",
    "bess",
    "terbess",
    "bio",
    "terbio",
    "geo",
    "tergeo",
)


def _pcp_aliases(plexos_name: str) -> list[str]:
    """Generate alias keys for a PCP unit name, in addition to the
    standard :func:`cen2gtopt._id_bridge.norm` outputs."""
    out: set[str] = set()
    raw = str(plexos_name)
    out.add(_norm(raw))

    # Strip PCP-style trailing modifiers and numeric tails.
    low = raw.lower()
    for _ in range(3):
        for suf in _PCP_SUFFIXES:
            if low.endswith(suf):
                low = low[: -len(suf)]
                out.add(_norm(low))

    # Variants: replace separators
    for sep_in in ("_", "-"):
        for sep_out in ("", " "):
            out.add(_norm(raw.replace(sep_in, sep_out)))

    # First word only (e.g. "ANDES_U1-U2_DIE" → "ANDES")
    head = re.split(r"[_\- ]", raw, maxsplit=1)[0]
    if head:
        out.add(_norm(head))

    # Prepend CEN-catalogue type prefixes to every alias generated so
    # far (PCP names omit the technology prefix).
    prepended: set[str] = set()
    for a in list(out):
        for pfx in _CEN_TYPE_PREFIXES:
            prepended.add(pfx + a)
    out |= prepended

    return [a for a in out if a and len(a) >= 2]


def resolve_pcp_unit(name: str, alias_index: dict[str, int]) -> int | None:
    """Bridge a PCP unit name to ``id_central``.  Tries the standard
    aliases first, then PCP-specific variants."""
    ic = bridge_resolve(name, alias_index)
    if ic is not None:
        return ic
    for a in _pcp_aliases(name):
        ic = alias_index.get(a)
        if ic is not None:
            return ic
    return None


# ---------------------------------------------------------------------------
# Per-day inputs container
# ---------------------------------------------------------------------------


@dataclass  # NOTE: cannot use slots=True; @cached_property needs __dict__
class PcpInputs:
    """Lazy-loaded view of one day's PCP / PID LP inputs.

    Handles both layouts transparently:

    * **PCP**: ``DATOS{YYYYMMDD}.zip`` unpacked into ``datos_dir``;
      flat CSV layout.
    * **PID**: ``PID_{YYYYMMDD}_{PP}.zip`` unpacked into a directory
      with a ``Modelos/`` subfolder.  The ``Acople_PCP/`` sub-sub-
      folder carries PID-only artefacts (the critical
      ``Gen_MarginalLossFactor.csv``, per-bus losses, etc.) accessed
      via :attr:`marginal_loss_factor` and :attr:`nod_loss`.

    The selected layout is detected from the file system — if
    ``datos_dir / "Gen_HeatRate.csv"`` exists it's PCP; if
    ``datos_dir / "Modelos" / "Gen_HeatRate.csv"`` exists it's PID.
    """

    date_iso: str
    datos_dir: Path
    period: int | None = None  # PID re-solve period (1..24); None for PCP

    def _model_root(self) -> Path:
        """The folder that actually contains ``Gen_HeatRate.csv``.
        Either ``datos_dir`` (PCP) or ``datos_dir / Modelos`` (PID)."""
        if (self.datos_dir / "Gen_HeatRate.csv").exists():
            return self.datos_dir
        candidate = self.datos_dir / "Modelos"
        if (candidate / "Gen_HeatRate.csv").exists():
            return candidate
        # Last resort: walk one level deeper (some PID outer ZIPs nest
        # the period folder)
        for sub in self.datos_dir.iterdir():
            if sub.is_dir() and (sub / "Modelos" / "Gen_HeatRate.csv").exists():
                return sub / "Modelos"
            if sub.is_dir() and (sub / "Gen_HeatRate.csv").exists():
                return sub
        return self.datos_dir

    @property
    def is_pid(self) -> bool:
        """True when this bundle is a PID (intra-day re-solve)."""
        return (self._model_root() / "Acople_PCP").exists()

    def _read_csv(self, name: str, *, subdir: str | None = None) -> pd.DataFrame:
        root = self._model_root()
        path = (root / subdir / name) if subdir else (root / name)
        if not path.exists():
            return pd.DataFrame()
        return pd.read_csv(path)

    # ---- Generators ---------------------------------------------------------

    @cached_property
    def heat_rate(self) -> pd.DataFrame:
        """Per-unit HeatRate (GJ/MWh) per period."""
        return self._read_csv("Gen_HeatRate.csv")

    @cached_property
    def gen_rating(self) -> pd.DataFrame:
        """Per-unit pmax (MW) per period."""
        return self._read_csv("Gen_Rating.csv")

    @cached_property
    def gen_min_stable_level(self) -> pd.DataFrame:
        """Per-unit pmin (MW) per period."""
        return self._read_csv("Gen_MinStableLevel.csv")

    @cached_property
    def gen_vom_charge(self) -> pd.DataFrame:
        """Per-unit variable O&M (USD/MWh) per period."""
        return self._read_csv("Gen_VOMCharge.csv")

    @cached_property
    def gen_fuel_transport_charge(self) -> pd.DataFrame:
        """Per-unit fuel-transport adder (USD/MWh) per period."""
        return self._read_csv("Gen_FuelTransportCharge.csv")

    @cached_property
    def gen_start_cost(self) -> pd.DataFrame:
        """Per-unit start-up cost (USD)."""
        return self._read_csv("Gen_StartCost.csv")

    @cached_property
    def gen_units_out(self) -> pd.DataFrame:
        """Forced-outage signal."""
        return self._read_csv("Gen_UnitsOut.csv")

    # ---- Fuels --------------------------------------------------------------

    @cached_property
    def fuel_price(self) -> pd.DataFrame:
        """Per-fuel-source price (USD/GJ) per period."""
        return self._read_csv("Fuel_Price.csv")

    @cached_property
    def fuel_max_offtake_week(self) -> pd.DataFrame:
        """TOP / take-or-pay constraint per fuel-source per week."""
        return self._read_csv("Fuel_MaxOfftakeWeek.csv")

    # ---- Hydro --------------------------------------------------------------

    @cached_property
    def hydro_water_values(self) -> pd.DataFrame:
        """Per-reservoir water value (USD/Hm³) per period — the
        revealed marginal water value PLP/PCP solves for."""
        return self._read_csv("Hydro_StoWaterValues.csv")

    @cached_property
    def hydro_water_flows(self) -> pd.DataFrame:
        return self._read_csv("Hydro_WaterFlows.csv")

    @cached_property
    def hydro_max_volume(self) -> pd.DataFrame:
        return self._read_csv("Hydro_MaxVolume.csv")

    @cached_property
    def hydro_min_volume(self) -> pd.DataFrame:
        return self._read_csv("Hydro_MinVolume.csv")

    @cached_property
    def hydro_initial_volume(self) -> pd.DataFrame:
        return self._read_csv("Hydro_InitialVolume.csv")

    # ---- Network ------------------------------------------------------------

    @cached_property
    def lin_max_rating(self) -> pd.DataFrame:
        """Per-line max flow (MW) per period."""
        return self._read_csv("Lin_MaxRating.csv")

    @cached_property
    def lin_min_rating(self) -> pd.DataFrame:
        return self._read_csv("Lin_MinRating.csv")

    @cached_property
    def lin_units(self) -> pd.DataFrame:
        return self._read_csv("Lin_Units.csv")

    # ---- Demand -------------------------------------------------------------

    @cached_property
    def nod_load(self) -> pd.DataFrame:
        """Per-bus load (MW), wide format: rows are (year, month,
        day, period); columns are bus names."""
        return self._read_csv("Nod_Load.csv")

    @cached_property
    def nod_load_long(self) -> pd.DataFrame:
        """Tidy view of ``nod_load``: one row per (period, bus)."""
        wide = self.nod_load
        if wide.empty:
            return wide
        keys = ["YEAR", "MONTH", "DAY", "PERIOD"]
        long = wide.melt(id_vars=keys, var_name="bus_name", value_name="load_mw")
        long["load_mw"] = pd.to_numeric(long["load_mw"], errors="coerce")
        return long.dropna(subset=["load_mw"])

    # ---- Reserves -----------------------------------------------------------

    @cached_property
    def res_requirement(self) -> pd.DataFrame:
        return self._read_csv("Res_Requirement.csv")

    @cached_property
    def sscc_activation_bess(self) -> pd.DataFrame:
        return self._read_csv("SSCC_Activation_BESS.csv")

    @cached_property
    def bess_ini_value(self) -> pd.DataFrame:
        return self._read_csv("BESS_IniValue.csv")

    # ---- PID-only: Acople_PCP/ subfolder -----------------------------------

    @cached_property
    def marginal_loss_factor(self) -> pd.DataFrame:
        """Per-unit per-period **marginal** loss factor (PID only).

        Columns: ``NAME``, ``YEAR``, ``MONTH``, ``DAY``, ``PERIOD``,
        ``VALUE``.  ``VALUE`` is the dimensionless loss-adjusted factor
        applied to that unit's MWh when computing nodal contributions
        (typically 0.85-1.05; values < 1 mean injection is penalised
        by transmission losses, > 1 means the unit is "near load").

        The replacement for the empirical ``λ_bus / λ_ref`` ratio in
        the marginal-emission pipeline (critical-review §1.5 / F8).

        Returns an empty DataFrame for PCP bundles.
        """
        return self._read_csv("Gen_MarginalLossFactor.csv", subdir="Acople_PCP")

    @cached_property
    def nod_loss(self) -> pd.DataFrame:
        """Per-bus per-period transmission losses (MW), PID only."""
        return self._read_csv("Nod_Loss.csv", subdir="Acople_PCP")

    @cached_property
    def hydro_water_values_pcp(self) -> pd.DataFrame:
        """Water values inherited from the upstream PCP solution
        (PID only).  Distinct from ``hydro_water_values`` which is
        the PID's own re-solved values."""
        return self._read_csv("Hydro_StoWaterValuesPCP.csv", subdir="Acople_PCP")

    @cached_property
    def fuel_max_offtake_48h(self) -> pd.DataFrame:
        """48-hour rolling fuel-offtake constraint (PID only)."""
        return self._read_csv("Fuel_MaxOfftake48h.csv", subdir="Acople_PCP")

    @cached_property
    def fuel_penalty_price(self) -> pd.DataFrame:
        return self._read_csv("Fuel_PenaltyPrice.csv", subdir="Acople_PCP")

    @cached_property
    def fuel_penalty_quantity(self) -> pd.DataFrame:
        return self._read_csv("Fuel_PenaltyQuantity.csv", subdir="Acople_PCP")

    # =========================================================================
    # Derived quantities
    # =========================================================================

    @cached_property
    def units(self) -> pd.DataFrame:
        """One row per generator unit; aggregates over periods.

        Columns: ``unit_name``, ``pmax``, ``pmin``, ``heat_rate_gj_per_mwh``,
        ``vom_usd_per_mwh``, ``fuel_transport_usd_per_mwh``,
        ``fuel_name`` (best-effort, may be empty).
        """
        # Use the first period as canonical (PCP heat-rates are usually
        # constant within a day).
        hr = self.heat_rate
        if hr.empty:
            return pd.DataFrame()
        first_hr = hr.sort_values("PERIOD").groupby("NAME").first().reset_index()
        out = first_hr[["NAME", "VALUE"]].rename(
            columns={"NAME": "unit_name", "VALUE": "heat_rate_gj_per_mwh"}
        )

        for src, col in (
            (self.gen_rating, "pmax"),
            (self.gen_min_stable_level, "pmin"),
            (self.gen_vom_charge, "vom_usd_per_mwh"),
            (self.gen_fuel_transport_charge, "fuel_transport_usd_per_mwh"),
        ):
            if src.empty:
                out[col] = float("nan")
                continue
            agg = src.groupby("NAME")["VALUE"].first().rename(col)
            out = out.merge(
                agg,
                left_on="unit_name",
                right_index=True,
                how="left",
            )
        # Authoritative unit→fuel mapping from the PLEXOS XML.
        ufm = self.unit_fuel_map
        if ufm:
            out["fuel_name"] = out["unit_name"].map(ufm).fillna("")
        else:
            out["fuel_name"] = ""
        return out

    @cached_property
    def unit_fuel_map(self) -> dict[str, str]:
        """``{generator_name → primary fuel_name}`` parsed from the
        PLEXOS XML object database (``DBSEN_PRGDIARIO*.xml``).
        Empty when the XML is missing.
        """
        # Local import to avoid heavy module load on simple property
        # access.
        # pylint: disable=import-outside-toplevel
        from cen2gtopt._plexos_xml import parse_unit_fuel_map

        root = self._model_root()
        for cand in (
            "DBSEN_PRGDIARIO.xml",
            "DBSEN_PRGDIARIO_PID.xml",
        ):
            xml_path = root / cand
            if xml_path.exists():
                try:
                    return parse_unit_fuel_map(xml_path)
                except Exception:  # noqa: BLE001  # pylint: disable=broad-except
                    return {}
        return {}

    @cached_property
    def unit_node_map(self) -> dict[str, str]:
        """``{generator_name → bus / node_name}`` parsed from the
        PLEXOS XML."""
        # pylint: disable=import-outside-toplevel
        from cen2gtopt._plexos_xml import parse_unit_node_map

        root = self._model_root()
        for cand in (
            "DBSEN_PRGDIARIO.xml",
            "DBSEN_PRGDIARIO_PID.xml",
        ):
            xml_path = root / cand
            if xml_path.exists():
                try:
                    return parse_unit_node_map(xml_path)
                except Exception:  # noqa: BLE001  # pylint: disable=broad-except
                    return {}
        return {}

    @cached_property
    def fuels(self) -> pd.DataFrame:
        """One row per fuel-source; aggregates fuel_price over periods.

        Columns: ``fuel_name``, ``price_usd_per_gj``,
        ``co2_kg_per_gj``."""
        fp = self.fuel_price
        if fp.empty:
            return pd.DataFrame()
        first = fp.sort_values("PERIOD").groupby("NAME").first().reset_index()
        out = first[["NAME", "VALUE"]].rename(
            columns={"NAME": "fuel_name", "VALUE": "price_usd_per_gj"}
        )
        out["co2_kg_per_gj"] = out["fuel_name"].map(_co2_factor)
        return out

    def marginal_cost_per_unit(
        self,
        *,
        period: int = 18,
    ) -> pd.DataFrame:
        """Compute per-unit short-run marginal cost (USD/MWh) at one
        period.

        ``MC = HeatRate(GJ/MWh) × FuelPrice(USD/GJ)
              + VOM(USD/MWh)
              + FuelTransport(USD/MWh)``

        Returns a DataFrame with columns ``unit_name``,
        ``mc_usd_mwh``, ``heat_rate``, ``fuel_price``, and the
        components of the sum.

        We don't have the unit→fuel association in the input CSVs
        (it lives in the PLEXOS XML); instead this method returns
        ``fuel_price=NaN`` and ``mc_usd_mwh=NaN`` for units whose
        fuel can't be inferred.  Use :meth:`epsilon_per_unit` for an
        ε-only view that doesn't need fuel resolution.
        """
        units = self.units.copy()
        if units.empty:
            return units
        # Period-specific heat-rate override
        hr_p = self.heat_rate
        if not hr_p.empty:
            hr_p = hr_p[hr_p["PERIOD"] == period]
            if not hr_p.empty:
                units = units.drop(columns=["heat_rate_gj_per_mwh"]).merge(
                    hr_p[["NAME", "VALUE"]].rename(
                        columns={"NAME": "unit_name", "VALUE": "heat_rate_gj_per_mwh"}
                    ),
                    on="unit_name",
                    how="left",
                )
        fuels = self.fuels
        if fuels.empty:
            units["fuel_name"] = ""
            units["fuel_price_usd_per_gj"] = float("nan")
            units["mc_usd_mwh"] = float("nan")
            return units
        # Authoritative XML mapping (fast path).  Falls back to a
        # substring-match heuristic for units missing from the XML.
        ufm = self.unit_fuel_map
        fuel_keys = sorted(
            fuels["fuel_name"].astype(str).tolist(), key=len, reverse=True
        )

        def _resolve_fuel(unit_name: str) -> str:
            if ufm:
                hit = ufm.get(str(unit_name))
                if hit:
                    return hit
            up = str(unit_name).upper()
            for k in fuel_keys:
                if k.upper() in up:
                    return k
            return ""

        units["fuel_name"] = units["unit_name"].map(_resolve_fuel)
        fuel_price_lookup = dict(zip(fuels["fuel_name"], fuels["price_usd_per_gj"]))
        units["fuel_price_usd_per_gj"] = units["fuel_name"].map(
            lambda f: fuel_price_lookup.get(f, float("nan"))
        )
        units["mc_usd_mwh"] = (
            units["heat_rate_gj_per_mwh"] * units["fuel_price_usd_per_gj"]
            + units["vom_usd_per_mwh"].fillna(0.0)
            + units["fuel_transport_usd_per_mwh"].fillna(0.0)
        )
        return units

    def epsilon_per_unit(self) -> pd.DataFrame:
        """Per-unit ε (kg CO₂ / MWh) = HeatRate × CO₂_per_GJ.

        Returns ``unit_name``, ``heat_rate_gj_per_mwh``,
        ``fuel_name``, ``co2_kg_per_gj``, ``epsilon_kg_per_mwh``.
        """
        u = self.marginal_cost_per_unit()
        if u.empty:
            return u
        u = u.copy()
        u["co2_kg_per_gj"] = u["fuel_name"].map(_co2_factor)
        u["epsilon_kg_per_mwh"] = u["heat_rate_gj_per_mwh"] * u["co2_kg_per_gj"]
        return u[
            [
                "unit_name",
                "fuel_name",
                "heat_rate_gj_per_mwh",
                "co2_kg_per_gj",
                "epsilon_kg_per_mwh",
            ]
        ]


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------


def _ymd(date_iso: str) -> str:
    return date_iso.replace("-", "")


def load_pcp_inputs(
    date_iso: str,
    *,
    period: int | None = None,
    source: str = "auto",
    download_root: Path | None = None,
    auto_download: bool = True,
    extract_root: Path | None = None,
) -> PcpInputs:
    """Load the LP inputs for ``date_iso``.

    Args:
        date_iso: ``YYYY-MM-DD``.
        period: PID re-solve period (1..24).  If supplied, prefers
            ``PID_{date}_{period}.zip`` over PCP.
        source: ``"pcp"``, ``"pid"``, or ``"auto"``.  In ``"auto"``
            mode prefers PID (more recent + carries marginal loss
            factors) and falls back to PCP.
        auto_download: When True, fetch the matching ZIP via
            :mod:`cen2gtopt.pcp_archive` if it is not already cached.

    PCP and PID archives unpack into the same ``extract_root`` with
    distinct sub-directories so they can coexist.
    """
    if source not in ("auto", "pcp", "pid"):
        raise ValueError(f"source must be 'auto'|'pcp'|'pid', got {source!r}")
    download_root = download_root or DEFAULT_DOWNLOAD_ROOT
    extract_root = extract_root or (download_root / "_unpacked")
    ymd = _ymd(date_iso)

    # ----- Resolve which archive to load ------------------------------------
    pid_name: str | None = None
    if source in ("auto", "pid"):
        # First check the local cache (no network needed).
        local_pid_dir = download_root / "PID"
        local_unpacked = list(extract_root.glob(f"PID_{ymd}_*"))
        local_zips = (
            sorted(local_pid_dir.glob(f"PID_{ymd}_*.zip"))
            if local_pid_dir.exists()
            else []
        )
        local_names = sorted(
            {p.name for p in local_zips} | {p.name + ".zip" for p in local_unpacked},
            reverse=True,
        )
        if period is not None:
            wanted = f"PID_{ymd}_{period:02d}.zip"
            if wanted in local_names:
                pid_name = wanted
        elif local_names:
            pid_name = local_names[0]  # most recent locally available

        # Fall back to remote index when nothing local AND auto_download
        if pid_name is None and auto_download:
            files = fetch_index()
            pid_candidates = sorted(
                [f for f in files if f.nombre.startswith(f"PID_{ymd}_")],
                key=lambda f: f.nombre,
                reverse=True,
            )
            if period is not None:
                wanted = f"PID_{ymd}_{period:02d}.zip"
                hit = [f for f in pid_candidates if f.nombre == wanted]
                if hit:
                    pid_name = hit[0].nombre
            elif pid_candidates:
                pid_name = pid_candidates[0].nombre

    if pid_name:
        return _load_pid(
            date_iso=date_iso,
            ymd=ymd,
            pid_name=pid_name,
            period=period,
            download_root=download_root,
            extract_root=extract_root,
            auto_download=auto_download,
        )

    if source == "pid":
        raise FileNotFoundError(
            f"no PID file found for {date_iso}"
            + (f" period {period}" if period else "")
        )

    return _load_pcp(
        date_iso=date_iso,
        ymd=ymd,
        download_root=download_root,
        extract_root=extract_root,
        auto_download=auto_download,
    )


def _load_pcp(
    *,
    date_iso: str,
    ymd: str,
    download_root: Path,
    extract_root: Path,
    auto_download: bool,
) -> PcpInputs:
    """Load a PCP (day-ahead) bundle."""
    plexos_zip = download_root / "PCP" / f"PLEXOS{ymd}.zip"
    datos_zip = download_root / "PCP" / f"DATOS{ymd}.zip"

    if auto_download and not plexos_zip.exists() and not datos_zip.exists():
        _LOG.info("PLEXOS bundle for %s not cached — downloading", date_iso)
        files = fetch_index()
        match = [f for f in files if f.nombre == f"PLEXOS{ymd}.zip"]
        if not match:
            raise FileNotFoundError(
                f"no PLEXOS{ymd}.zip in CEN index for date {date_iso}"
            )
        download_one(match[0], output_dir=download_root)

    datos_dir = extract_root / f"PCP_{ymd}"
    if datos_dir.exists() and (datos_dir / "Gen_HeatRate.csv").exists():
        return PcpInputs(date_iso=date_iso, datos_dir=datos_dir)

    if plexos_zip.exists():
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            with zipfile.ZipFile(plexos_zip) as z:
                z.extractall(tmp)
            inner = tmp / f"DATOS{ymd}.zip"
            if not inner.exists():
                raise FileNotFoundError(f"DATOS{ymd}.zip missing inside {plexos_zip}")
            datos_dir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(inner) as z:
                z.extractall(datos_dir)
    elif datos_zip.exists():
        datos_dir.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(datos_zip) as z:
            z.extractall(datos_dir)
    else:
        raise FileNotFoundError(
            f"no PCP archive found for {date_iso} under {download_root}"
        )
    return PcpInputs(date_iso=date_iso, datos_dir=datos_dir)


def _load_pid(
    *,
    date_iso: str,
    ymd: str,
    pid_name: str,
    period: int | None,
    download_root: Path,
    extract_root: Path,
    auto_download: bool,
) -> PcpInputs:
    """Load a PID (intra-day re-solve) bundle.

    PID outer ZIP unpacks to ``PID_{ymd}_{pp}/Modelos/...`` so we
    pass that subdirectory as ``datos_dir``.
    """
    pid_zip = download_root / "PID" / pid_name
    if auto_download and not pid_zip.exists():
        _LOG.info("PID bundle %s not cached — downloading", pid_name)
        files = fetch_index(force=True)  # need fresh URL
        match = [f for f in files if f.nombre == pid_name]
        if not match:
            raise FileNotFoundError(f"no {pid_name} in CEN index")
        download_one(match[0], output_dir=download_root)

    extract_dir = extract_root / pid_name.removesuffix(".zip")
    inner_dir = extract_dir / pid_name.removesuffix(".zip")
    if inner_dir.exists() and (inner_dir / "Modelos" / "Gen_HeatRate.csv").exists():
        eff_period = period
        if eff_period is None:
            # Parse from filename like PID_20260506_20.zip
            m = re.search(r"PID_\d{8}_(\d{2})\.zip$", pid_name)
            if m:
                eff_period = int(m.group(1))
        return PcpInputs(
            date_iso=date_iso,
            datos_dir=inner_dir,
            period=eff_period,
        )

    if not pid_zip.exists():
        raise FileNotFoundError(f"{pid_zip} not found")
    extract_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(pid_zip) as z:
        z.extractall(extract_dir)

    # The PID ZIP unpacks as PID_{ymd}_{pp}/{everything}
    inner = extract_dir / pid_name.removesuffix(".zip")
    if not (inner / "Modelos" / "Gen_HeatRate.csv").exists():
        raise FileNotFoundError(
            f"unexpected PID layout — Modelos/Gen_HeatRate.csv missing in {inner}"
        )
    eff_period = period
    if eff_period is None:
        m = re.search(r"PID_\d{8}_(\d{2})\.zip$", pid_name)
        if m:
            eff_period = int(m.group(1))
    return PcpInputs(
        date_iso=date_iso,
        datos_dir=inner,
        period=eff_period,
    )


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _summary(inputs: PcpInputs) -> dict[str, Any]:
    units = inputs.units
    fuels = inputs.fuels
    hwv = inputs.hydro_water_values
    hr_n = len(units)
    fuel_n = len(fuels)
    n_hours = inputs.heat_rate["PERIOD"].nunique() if not inputs.heat_rate.empty else 0
    n_lines = (
        inputs.lin_max_rating["NAME"].nunique()
        if not inputs.lin_max_rating.empty
        else 0
    )
    n_buses = (
        len(
            [
                c
                for c in inputs.nod_load.columns
                if c not in ("YEAR", "MONTH", "DAY", "PERIOD")
            ]
        )
        if not inputs.nod_load.empty
        else 0
    )
    return {
        "date": inputs.date_iso,
        "n_units": hr_n,
        "n_fuels": fuel_n,
        "n_lines": n_lines,
        "n_buses": n_buses,
        "n_hours": n_hours,
        "n_reservoirs": len(hwv),
        "datos_dir": str(inputs.datos_dir),
    }


def _cmd_summary(args: argparse.Namespace) -> int:
    inputs = load_pcp_inputs(
        args.date,
        source=args.source,
        period=args.period,
    )
    s = _summary(inputs)
    label = "PID" if inputs.is_pid else "PCP"
    period_tag = f" period={inputs.period:02d}" if inputs.period else ""
    print(f"=== {label} inputs summary — {args.date}{period_tag} ===")
    for k, v in s.items():
        print(f"  {k:15s} = {v}")
    print()
    units = inputs.units
    if not units.empty:
        print("--- top 10 units by pmax ---")
        cols = [
            "unit_name",
            "pmax",
            "pmin",
            "heat_rate_gj_per_mwh",
            "vom_usd_per_mwh",
        ]
        print(
            units.sort_values("pmax", ascending=False)[cols]
            .head(10)
            .round(3)
            .to_string(index=False)
        )
    print()
    fuels = inputs.fuels
    if not fuels.empty:
        print("--- fuel prices (top 10 by price) ---")
        print(
            fuels.sort_values("price_usd_per_gj", ascending=False)
            .head(10)
            .round(3)
            .to_string(index=False)
        )
    print()
    hwv = inputs.hydro_water_values
    if not hwv.empty:
        print("--- water values per reservoir ---")
        print(hwv.head(20).round(2).to_string(index=False))
    if inputs.is_pid:
        print()
        mlf = inputs.marginal_loss_factor
        if not mlf.empty:
            print("--- marginal loss factor (PID Acople_PCP) ---")
            print(f"  rows: {len(mlf)}, units: {mlf['NAME'].nunique()}")
            print(f"  range: [{mlf['VALUE'].min():.3f}, {mlf['VALUE'].max():.3f}]")
            print(f"  median: {mlf['VALUE'].median():.3f}")
    return 0


def _cmd_merit(args: argparse.Namespace) -> int:
    inputs = load_pcp_inputs(
        args.date,
        source=args.source,
        period=args.period,
    )
    mc = inputs.marginal_cost_per_unit(period=args.period)
    if mc.empty:
        print("no input data found")
        return 1
    mc = mc.dropna(subset=["mc_usd_mwh"])
    mc = mc.sort_values("mc_usd_mwh")
    cols = [
        "unit_name",
        "fuel_name",
        "heat_rate_gj_per_mwh",
        "fuel_price_usd_per_gj",
        "vom_usd_per_mwh",
        "mc_usd_mwh",
        "pmax",
    ]
    print(f"=== merit order at period={args.period} ({args.date}) ===")
    print(mc[cols].head(args.limit).round(3).to_string(index=False))
    print()
    print(f"  total units with derived MC: {len(mc)}")
    print(f"  median MC: ${mc['mc_usd_mwh'].median():.2f}/MWh")
    print(f"  P75 MC:    ${mc['mc_usd_mwh'].quantile(0.75):.2f}/MWh")
    return 0


def _cmd_epsilon(args: argparse.Namespace) -> int:
    inputs = load_pcp_inputs(
        args.date,
        source=args.source,
        period=args.period,
    )
    eps = inputs.epsilon_per_unit()
    if eps.empty:
        print("no input data found")
        return 1
    eps = eps.dropna(subset=["epsilon_kg_per_mwh"])
    eps = eps[eps["epsilon_kg_per_mwh"] > 0]
    print(f"=== ε per unit ({args.date}) — non-zero only ===")
    print(
        eps.sort_values("epsilon_kg_per_mwh", ascending=False)
        .head(args.limit)
        .round(2)
        .to_string(index=False)
    )
    print()
    print(f"  total units with ε > 0: {len(eps)}")
    return 0


def _cmd_loss_factors(args: argparse.Namespace) -> int:
    inputs = load_pcp_inputs(
        args.date,
        source=args.source or "pid",
        period=args.period,
    )
    if not inputs.is_pid:
        print("loss-factors are only available in PID bundles (use --source pid)")
        return 2
    mlf = inputs.marginal_loss_factor
    if mlf.empty:
        print("Gen_MarginalLossFactor.csv missing")
        return 1
    # Aggregate per unit (mean across periods)
    agg = (
        mlf.groupby("NAME")["VALUE"]
        .agg(["mean", "min", "max", "std"])
        .round(4)
        .reset_index()
        .rename(columns={"NAME": "unit_name"})
    )
    agg = agg.sort_values("mean")
    label = f"PID period={inputs.period:02d}" if inputs.period else "PID"
    print(f"=== marginal loss factor — {args.date} ({label}) ===")
    print(f"  units total: {len(agg)}")
    print()
    print("--- bottom 10 (highest losses → most penalised injection) ---")
    print(agg.head(10).to_string(index=False))
    print()
    print("--- top 10 (lowest losses → near load) ---")
    print(agg.tail(10).to_string(index=False))
    print()
    print(f"  median: {agg['mean'].median():.4f}")
    print(f"  range:  [{agg['mean'].min():.4f}, {agg['mean'].max():.4f}]")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cen2gtopt.pcp_inputs",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.add_argument(
        "-V", "--version", action="version", version="cen2gtopt.pcp_inputs 0.1.0"
    )

    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    def _add_common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--date", required=True, help="YYYY-MM-DD")
        sp.add_argument(
            "--source",
            default="auto",
            choices=("auto", "pcp", "pid"),
            help="auto = prefer PID, fall back to PCP (default).",
        )
        sp.add_argument(
            "--period",
            type=int,
            default=None,
            help="PID re-solve period (1..24); ignored for PCP.",
        )

    ps = sub.add_parser("summary", help="High-level summary of one day")
    _add_common(ps)
    ps.set_defaults(func=_cmd_summary)

    pm = sub.add_parser("merit", help="Per-unit merit order at one period")
    _add_common(pm)
    pm.add_argument("--limit", type=int, default=30, help="Max rows (default 30)")
    pm.set_defaults(func=_cmd_merit)
    # Note: --period on `merit` is reused both as the PID re-solve
    # selector AND as the merit-snapshot period.  Default 18 = 18:00.

    pe = sub.add_parser("epsilon", help="Per-unit ε (kg CO₂ / MWh)")
    _add_common(pe)
    pe.add_argument("--limit", type=int, default=30)
    pe.set_defaults(func=_cmd_epsilon)

    pl = sub.add_parser("loss-factors", help="Per-unit marginal loss factor (PID only)")
    _add_common(pl)
    pl.add_argument("--limit", type=int, default=30)
    pl.set_defaults(func=_cmd_loss_factors)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.quiet:
        log_level = logging.WARNING
    elif args.verbose:
        log_level = logging.DEBUG
    else:
        log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
