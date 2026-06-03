# SPDX-License-Identifier: BSD-3-Clause
"""Topology dataclasses — the static (per-feed) snapshot of buses,
generators and lines.

Frozen `@dataclass(slots=True)` per the python-reviewer P1 finding:
no pydantic, no attrs, just stdlib dataclasses. This matches the
existing scripts/sddp2gtopt/entities.py precedent.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import pandas as pd


COL_BUS_UID = "uid"
COL_BUS_NAME = "name"
COL_BUS_REGION = "region"

COL_GEN_UID = "uid"
COL_GEN_NAME = "name"
COL_GEN_BUS_UID = "bus_uid"
COL_GEN_PMIN = "pmin"
COL_GEN_PMAX = "pmax"
COL_GEN_DECLARED_MC = "declared_MC"
COL_GEN_KIND = "kind"
COL_GEN_EMISSION_FACTOR = "emission_rate"

COL_LINE_UID = "uid"
COL_LINE_BUS_A = "bus_a_uid"
COL_LINE_BUS_B = "bus_b_uid"
COL_LINE_TMAX_AB = "tmax_ab"
COL_LINE_TMAX_BA = "tmax_ba"
COL_LINE_REACTANCE = "reactance"
COL_LINE_ACTIVE = "active"


@dataclass(slots=True, frozen=True)
class Bus:
    uid: int
    name: str
    region: Optional[str] = None


@dataclass(slots=True, frozen=True)
class Generator:
    uid: int
    name: str
    bus_uid: int
    pmin: float
    pmax: float
    declared_MC: Optional[float] = None
    kind: str = "thermal"  # thermal | hydro | battery | profile
    emission_rate: Optional[float] = None  # kgCO2eq/MWh, §4.12
    # True for self-dispatching cogeneration units (biomass / biogas /
    # geothermal cogen, refinery / pulp-mill steam cycles, etc.).  These
    # are flagged ``kind="thermal"`` because they consume fuel and emit,
    # but in PLEXOS / CEN they're modeled as MustRun with ``declared_MC=0``
    # and never appear as backfill marginal units — consumers walking the
    # merit ladder must skip them.
    is_cogen: bool = False


@dataclass(slots=True, frozen=True)
class Line:
    uid: int
    bus_a_uid: int
    bus_b_uid: int
    tmax_ab: float
    tmax_ba: float
    reactance: Optional[float] = None
    active: bool = True


@dataclass(slots=True)
class Topology:
    """Static topology snapshot. Stored on disk as three parquet
    files under ``<feed>/topology/{bus,generator,line}.parquet``."""

    buses: list[Bus] = field(default_factory=list)
    generators: list[Generator] = field(default_factory=list)
    lines: list[Line] = field(default_factory=list)

    def bus_uids(self) -> list[int]:
        return [b.uid for b in self.buses]

    def gen_uids(self) -> list[int]:
        return [g.uid for g in self.generators]

    def line_uids(self) -> list[int]:
        return [ln.uid for ln in self.lines]

    def bus_by_uid(self, uid: int) -> Bus:
        for b in self.buses:
            if b.uid == uid:
                return b
        raise KeyError(f"bus uid {uid} not in topology")

    def gen_by_uid(self, uid: int) -> Generator:
        for g in self.generators:
            if g.uid == uid:
                return g
        raise KeyError(f"generator uid {uid} not in topology")

    def to_frames(self) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
        """Return (buses_df, generators_df, lines_df) for parquet I/O."""
        buses_df = pd.DataFrame(
            [
                {
                    COL_BUS_UID: b.uid,
                    COL_BUS_NAME: b.name,
                    COL_BUS_REGION: b.region,
                }
                for b in self.buses
            ]
        )
        generators_df = pd.DataFrame(
            [
                {
                    COL_GEN_UID: g.uid,
                    COL_GEN_NAME: g.name,
                    COL_GEN_BUS_UID: g.bus_uid,
                    COL_GEN_PMIN: g.pmin,
                    COL_GEN_PMAX: g.pmax,
                    COL_GEN_DECLARED_MC: g.declared_MC,
                    COL_GEN_KIND: g.kind,
                    COL_GEN_EMISSION_FACTOR: g.emission_rate,
                }
                for g in self.generators
            ]
        )
        lines_df = pd.DataFrame(
            [
                {
                    COL_LINE_UID: ln.uid,
                    COL_LINE_BUS_A: ln.bus_a_uid,
                    COL_LINE_BUS_B: ln.bus_b_uid,
                    COL_LINE_TMAX_AB: ln.tmax_ab,
                    COL_LINE_TMAX_BA: ln.tmax_ba,
                    COL_LINE_REACTANCE: ln.reactance,
                    COL_LINE_ACTIVE: ln.active,
                }
                for ln in self.lines
            ]
        )
        return buses_df, generators_df, lines_df

    @classmethod
    def from_frames(
        cls,
        buses_df: pd.DataFrame,
        generators_df: pd.DataFrame,
        lines_df: pd.DataFrame,
    ) -> "Topology":
        buses = [
            Bus(
                uid=int(row[COL_BUS_UID]),
                name=str(row[COL_BUS_NAME]),
                region=_opt_str(row.get(COL_BUS_REGION)),
            )
            for _, row in buses_df.iterrows()
        ]
        generators = [
            Generator(
                uid=int(row[COL_GEN_UID]),
                name=str(row[COL_GEN_NAME]),
                bus_uid=int(row[COL_GEN_BUS_UID]),
                pmin=float(row[COL_GEN_PMIN]),
                pmax=float(row[COL_GEN_PMAX]),
                declared_MC=_opt_float(row.get(COL_GEN_DECLARED_MC)),
                kind=str(row.get(COL_GEN_KIND, "thermal")),
                emission_rate=_opt_float(row.get(COL_GEN_EMISSION_FACTOR)),
            )
            for _, row in generators_df.iterrows()
        ]
        lines = [
            Line(
                uid=int(row[COL_LINE_UID]),
                bus_a_uid=int(row[COL_LINE_BUS_A]),
                bus_b_uid=int(row[COL_LINE_BUS_B]),
                tmax_ab=float(row[COL_LINE_TMAX_AB]),
                tmax_ba=float(row[COL_LINE_TMAX_BA]),
                reactance=_opt_float(row.get(COL_LINE_REACTANCE)),
                active=bool(row.get(COL_LINE_ACTIVE, True)),
            )
            for _, row in lines_df.iterrows()
        ]
        return cls(buses=buses, generators=generators, lines=lines)


def _opt_str(v: object) -> Optional[str]:
    if v is None or (isinstance(v, float) and pd.isna(v)):
        return None
    return str(v)


def _opt_float(v: object) -> Optional[float]:
    if v is None or (isinstance(v, float) and pd.isna(v)):
        return None
    try:
        return float(v)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
