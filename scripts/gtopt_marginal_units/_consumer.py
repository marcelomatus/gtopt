# SPDX-License-Identifier: BSD-3-Clause
"""MarginalUnitDataset — read-only consumer of the parquet dataset
produced by gtopt_marginal_units. Master plan §4.10.

Provides:
* ``bus_lmp(stage=None, carbon_price=None)`` — per-bus zone_lmp time
  series; with a positive carbon price emits ``lmp_with_carbon``.
* ``recompute_lmp(unit_costs, carbon_price=None, unit_emissions=None)``
  — λ_b under an alternative cost catalogue, computed
  deterministically from the saved bus_price_recipe table; composes
  with the carbon-price knob.
* ``bus_emission_intensity(stage=None)`` — per-bus ε_b.
* ``recompute_emission(unit_emissions={uid: ef})`` — ε_b under an
  alternative emission catalogue.
* ``bus_lmp_and_emission(stage=None, carbon_price=None)`` — combined
  view; with a positive carbon price emits ``lmp_with_carbon``.
* ``carbon_price_usd_per_ton()`` — the carbon price persisted by the
  writer in ``manifest.extras`` (0 when unset).
* ``merit_ladder(cell_key=None, zone_id=None, depth=None)`` — read
  the merit-ladder rows.
* ``outage_sensitivity(gen_uid)`` — for each cell where ``gen_uid``
  was rank-0 marginal, return rank+1 hypothetical_lmp as the
  one-step takeover price.
* ``marginal_units(bus_uid=None, cell_key=None)`` — per-bus
  attribution.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

import pandas as pd


_CELL_KEY_COLS = ("scenario", "stage", "block", "date_utc", "hour", "data_source")


class MarginalUnitDataset:
    """Read-only view of a marginal_units.parquet dataset (the directory
    written by ``gtopt-marginal-units``)."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        if not (self.root / "manifest.json").exists():
            raise FileNotFoundError(
                f"manifest.json not found at {self.root}; "
                "this does not look like a gtopt-marginal-units output directory."
            )
        self._manifest_cache: Optional[dict] = None

    @classmethod
    def open(cls, root: str | Path) -> "MarginalUnitDataset":
        return cls(Path(root))

    def __repr__(self) -> str:
        return f"MarginalUnitDataset(root={self.root!r})"

    # ---------------------------------------------------------------
    # Public queries
    # ---------------------------------------------------------------

    def manifest(self) -> dict:
        if self._manifest_cache is None:
            self._manifest_cache = json.loads(
                (self.root / "manifest.json").read_text(encoding="utf-8")
            )
        return self._manifest_cache

    def carbon_price_usd_per_ton(self) -> float:
        """Carbon price persisted by the writer in ``manifest.extras``.

        Returns 0.0 when the field is absent (no carbon price was set
        at write time).  The unit is USD per metric ton CO2eq.
        """
        extras = self.manifest().get("extras") or {}
        try:
            return float(extras.get("carbon_price_usd_per_ton", 0.0) or 0.0)
        except (TypeError, ValueError):
            return 0.0

    def _resolve_carbon_price(self, override: float | None) -> float:
        if override is None:
            return self.carbon_price_usd_per_ton()
        if override < 0.0:
            raise ValueError(f"carbon_price must be non-negative; got {override}")
        return float(override)

    def per_bus(self) -> pd.DataFrame:
        """Read attribution/per_bus.parquet."""
        return pd.read_parquet(self.root / "attribution/per_bus.parquet")

    def per_zone(self) -> pd.DataFrame:
        """Read attribution/per_zone.parquet."""
        return pd.read_parquet(self.root / "attribution/per_zone.parquet")

    def bus_lmp(
        self,
        stage: int | None = None,
        carbon_price: float | None = None,
    ) -> pd.DataFrame:
        """Per-zone LMP time series.

        When ``carbon_price`` (or the manifest value) is positive, also
        emits a per-bus ``lmp_with_carbon`` column equal to
        ``zone_lmp + carbon_price/1000 * emission_intensity_kg_per_mwh``.
        The ``carbon_price`` parameter is in USD per metric ton CO2eq.
        """
        cp = self._resolve_carbon_price(carbon_price)
        df = self.per_zone()
        cols = [c for c in _CELL_KEY_COLS if c in df.columns] + ["zone_id", "zone_lmp"]
        out = df[cols].copy()
        if stage is not None and "stage" in out.columns:
            out = out[out["stage"] == stage]
        if cp > 0.0:
            em = self.bus_emission_intensity(stage=stage)
            if not em.empty:
                em = em.copy()
                em["carbon_adder_usd_per_mwh"] = (cp / 1000.0) * em[
                    "emission_intensity_kg_per_mwh"
                ].fillna(0.0)
                join_cols = [
                    c for c in _CELL_KEY_COLS if c in out.columns and c in em.columns
                ]
                merged = em.merge(out, on=join_cols, how="inner")
                merged["lmp_with_carbon"] = (
                    merged["zone_lmp"] + merged["carbon_adder_usd_per_mwh"]
                )
                merged["carbon_price_usd_per_ton"] = cp
                return merged.reset_index(drop=True)
        return out.reset_index(drop=True)

    def marginal_units(
        self,
        bus_uid: int | None = None,
        cell_key: tuple | None = None,
    ) -> pd.DataFrame:
        """Filter the per-bus attribution to one bus and/or one cell."""
        df = self.per_bus()
        if bus_uid is not None:
            df = df[df["bus_uid"] == bus_uid]
        if cell_key is not None:
            scenario, stage, block = (cell_key + (None, None, None))[:3]
            if scenario is not None and "scenario" in df.columns:
                df = df[df["scenario"] == scenario]
            if stage is not None and "stage" in df.columns:
                df = df[df["stage"] == stage]
            if block is not None and "block" in df.columns:
                df = df[df["block"] == block]
        return df.reset_index(drop=True)

    def merit_ladder(
        self,
        cell_key: tuple | None = None,
        zone_id: int | None = None,
        depth: int | None = None,
    ) -> pd.DataFrame:
        path = self.root / "merit_ladder.parquet"
        if not path.exists():
            return pd.DataFrame()
        df = pd.read_parquet(path)
        if cell_key is not None:
            scenario, stage, block = (cell_key + (None, None, None))[:3]
            if scenario is not None:
                df = df[df["scenario"] == scenario]
            if stage is not None:
                df = df[df["stage"] == stage]
            if block is not None:
                df = df[df["block"] == block]
        if zone_id is not None:
            df = df[df["zone_id"] == zone_id]
        if depth is not None:
            df = df[df["rank"].abs() <= depth]
        return df.sort_values(_present_cols(df, "rank")).reset_index(drop=True)

    # ---------------------------------------------------------------
    # Recompute APIs (master §4.10)
    # ---------------------------------------------------------------

    def recompute_lmp(
        self,
        unit_costs: dict[int, float],
        carbon_price: float | None = None,
        unit_emissions: dict[int, float] | None = None,
    ) -> pd.DataFrame:
        """Recompute λ_b for every (cell, bus) under an alternative
        cost catalogue. Uses the saved bus_price_recipe verbatim.

        When ``carbon_price > 0`` (or set in the manifest), also adds
        an ``lmp_with_carbon`` column equal to
        ``zone_lmp_recomputed + carbon_price/1000 * ε_b``.  The ε_b
        used is recomputed from the saved emission recipe under the
        optional ``unit_emissions`` override (otherwise the captured
        catalogue is used)."""
        out = self._recompute(
            recipe_path=self.root / "bus_price_recipe.parquet",
            data_col="marginal_costs",
            output_col="zone_lmp_recomputed",
            cap_col="formula_constant",
            user_data=unit_costs,
        )
        cp = self._resolve_carbon_price(carbon_price)
        if cp <= 0.0 or out.empty:
            return out
        em_recipe = self.root / "bus_emission_intensity_recipe.parquet"
        if not em_recipe.exists():
            return out
        em_df = pd.read_parquet(em_recipe)
        if unit_emissions is not None:
            em_df = self.recompute_emission(unit_emissions)
            em_col = "emission_intensity_recomputed"
        else:
            em_col = "recomputed_emission_intensity"
        if em_col not in em_df.columns:
            return out
        join_cols = [
            c for c in _CELL_KEY_COLS if c in out.columns and c in em_df.columns
        ]
        if "bus_uid" in out.columns and "bus_uid" in em_df.columns:
            join_cols.append("bus_uid")
        em_slim = em_df[join_cols + [em_col]].copy()
        merged = out.merge(em_slim, on=join_cols, how="left")
        merged["carbon_adder_usd_per_mwh"] = (cp / 1000.0) * merged[em_col].fillna(0.0)
        merged["lmp_with_carbon"] = (
            merged["zone_lmp_recomputed"] + merged["carbon_adder_usd_per_mwh"]
        )
        merged["carbon_price_usd_per_ton"] = cp
        return merged

    def recompute_emission(self, unit_emissions: dict[int, float]) -> pd.DataFrame:
        return self._recompute(
            recipe_path=self.root / "bus_emission_intensity_recipe.parquet",
            data_col="marginal_emission_factors",
            output_col="emission_intensity_recomputed",
            cap_col="formula_constant",
            user_data=unit_emissions,
        )

    def _recompute(
        self,
        recipe_path: Path,
        data_col: str,
        output_col: str,
        cap_col: str,
        user_data: dict[int, float],
    ) -> pd.DataFrame:
        if not recipe_path.exists():
            raise FileNotFoundError(f"recipe table missing: {recipe_path}")
        df = pd.read_parquet(recipe_path)
        recomputed: list[float] = []
        original_col = (
            "recomputed_lmp"
            if "recomputed_lmp" in df.columns
            else "recomputed_emission_intensity"
        )
        for _, row in df.iterrows():
            uids = (
                list(row["marginal_gen_uids"])
                if row["marginal_gen_uids"] is not None
                else []
            )
            weights = (
                list(row["marginal_weights"])
                if row["marginal_weights"] is not None
                else []
            )
            if not uids:
                # Constant-driven (demand_fail / renewable_curtailment / unattributed).
                recomputed.append(float(row[cap_col]))
                continue
            total = 0.0
            missing = False
            for u, w in zip(uids, weights):
                if int(u) not in user_data:
                    missing = True
                    break
                total += float(w) * float(user_data[int(u)])
            recomputed.append(
                total + float(row[cap_col]) if not missing else float("nan")
            )
        out = df.copy()
        out[output_col] = recomputed
        if original_col in out.columns:
            out[output_col.replace("_recomputed", "_original")] = out[original_col]
        # Delta column for convenience.
        if original_col in out.columns:
            out[("lmp_delta" if data_col == "marginal_costs" else "emission_delta")] = (
                out[output_col] - out[original_col]
            )
        return out

    def bus_emission_intensity(self, stage: int | None = None) -> pd.DataFrame:
        path = self.root / "bus_emission_intensity_recipe.parquet"
        if not path.exists():
            return pd.DataFrame()
        df = pd.read_parquet(path)
        cols = [c for c in _CELL_KEY_COLS if c in df.columns] + [
            "bus_uid",
            "recomputed_emission_intensity",
        ]
        df = df[cols].rename(
            columns={"recomputed_emission_intensity": "emission_intensity_kg_per_mwh"}
        )
        if stage is not None and "stage" in df.columns:
            df = df[df["stage"] == stage]
        return df.reset_index(drop=True)

    def bus_lmp_and_emission(
        self,
        stage: int | None = None,
        carbon_price: float | None = None,
    ) -> pd.DataFrame:
        """Combined dual-currency view — λ_b and ε_b in one frame.

        When ``carbon_price`` (or the manifest value) is positive, also
        emits ``carbon_adder_usd_per_mwh`` and ``lmp_with_carbon``
        columns."""
        cp = self._resolve_carbon_price(carbon_price)
        per_bus = self.per_bus()
        if "zone_lmp" in per_bus.columns:
            lmp_df = per_bus[
                [c for c in _CELL_KEY_COLS if c in per_bus.columns]
                + ["bus_uid", "zone_lmp"]
            ].drop_duplicates()
        else:
            lmp_df = pd.DataFrame()
        em_df = self.bus_emission_intensity(stage=stage)
        if lmp_df.empty or em_df.empty:
            return lmp_df if not lmp_df.empty else em_df
        if stage is not None and "stage" in lmp_df.columns:
            lmp_df = lmp_df[lmp_df["stage"] == stage]
        join_cols = [
            c for c in _CELL_KEY_COLS if c in lmp_df.columns and c in em_df.columns
        ]
        join_cols.append("bus_uid")
        out = lmp_df.merge(em_df, on=join_cols, how="inner").reset_index(drop=True)
        if cp > 0.0 and "emission_intensity_kg_per_mwh" in out.columns:
            out["carbon_adder_usd_per_mwh"] = (cp / 1000.0) * out[
                "emission_intensity_kg_per_mwh"
            ].fillna(0.0)
            out["lmp_with_carbon"] = out["zone_lmp"] + out["carbon_adder_usd_per_mwh"]
            out["carbon_price_usd_per_ton"] = cp
        return out

    def outage_sensitivity(self, gen_uid: int) -> pd.DataFrame:
        """One-step outage sensitivity. For each (cell, zone) where
        ``gen_uid`` was the rank-0 marginal, return the rank+1
        hypothetical_lmp as the post-outage price."""
        ladder_path = self.root / "merit_ladder.parquet"
        if not ladder_path.exists():
            return pd.DataFrame()
        ladder = pd.read_parquet(ladder_path)
        # Cells where gen_uid was rank-0:
        anchored = ladder[(ladder["rank"] == 0) & (ladder["gen_uid"] == gen_uid)][
            _present_cols(ladder, *_CELL_KEY_COLS, "zone_id")
        ].drop_duplicates()
        if anchored.empty:
            return pd.DataFrame()
        # rank+1 rows:
        plus_one = ladder[ladder["rank"] == 1][
            _present_cols(
                ladder,
                *_CELL_KEY_COLS,
                "zone_id",
                "gen_uid",
                "gen_name",
                "hypothetical_lmp",
            )
        ].rename(
            columns={
                "gen_uid": "replacing_gen_uid",
                "gen_name": "replacing_gen_name",
                "hypothetical_lmp": "lmp_post_outage",
            }
        )
        join_cols = [
            c for c in _CELL_KEY_COLS if c in anchored.columns and c in plus_one.columns
        ]
        join_cols.append("zone_id")
        return anchored.merge(plus_one, on=join_cols, how="left").reset_index(drop=True)


def _present_cols(df: pd.DataFrame, *names: str) -> list[str]:
    return [n for n in names if n in df.columns]
