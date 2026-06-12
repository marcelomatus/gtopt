# SPDX-License-Identifier: BSD-3-Clause
"""CSV (de)serialisation for the three audit artefacts:

* ``busmap.csv`` — original_bus_uid → cluster_bus_uid
* ``linemap.csv`` — original_line_uid → equivalent_line_uid (+ rule)
* ``aggregator_table.csv`` — per-component bus rewrite with share

Plus a ``reducer_config.json`` writer for reproducibility.
"""

from __future__ import annotations

import csv
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable


@dataclass(slots=True, frozen=True)
class BusmapRow:
    original_bus_uid: int
    cluster_bus_uid: int


@dataclass(slots=True, frozen=True)
class LinemapRow:
    original_line_uid: int
    equivalent_line_uid: int | None  # None = intra-cluster, dropped
    rule: str  # "parallel-merge" | "series-merge" | "inter-cluster" | "intra-cluster"


@dataclass(slots=True, frozen=True)
class AggregatorRow:
    component_kind: str  # "generator" | "demand" | "battery" | "turbine"
    component_uid: int
    original_bus_uid: int
    cluster_bus_uid: int
    share: float


# --- Generic helpers -------------------------------------------------------


def _write_csv(path: Path, rows: Iterable[Any], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def _read_csv(path: Path, factory: Any) -> list[Any]:
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        return [factory(**_coerce(row, factory)) for row in reader]


def _coerce(row: dict[str, str], factory: Any) -> dict[str, Any]:
    """Coerce CSV string fields to the dataclass field types."""
    annotations = getattr(factory, "__annotations__", {})
    out: dict[str, Any] = {}
    for k, v in row.items():
        if k not in annotations:
            continue
        ann = annotations[k]
        if v == "" or v is None:
            out[k] = None
            continue
        if ann is int or ann == "int":
            out[k] = int(v)
        elif ann is float or ann == "float":
            out[k] = float(v)
        elif "int" in str(ann):
            out[k] = int(v)
        elif "float" in str(ann):
            out[k] = float(v)
        else:
            out[k] = v
    return out


# --- busmap ----------------------------------------------------------------


def save_busmap(rows: Iterable[BusmapRow], path: str | Path) -> None:
    _write_csv(Path(path), rows, ["original_bus_uid", "cluster_bus_uid"])


def load_busmap(path: str | Path) -> list[BusmapRow]:
    return _read_csv(Path(path), BusmapRow)


# --- linemap ---------------------------------------------------------------


def save_linemap(rows: Iterable[LinemapRow], path: str | Path) -> None:
    _write_csv(
        Path(path),
        rows,
        ["original_line_uid", "equivalent_line_uid", "rule"],
    )


def load_linemap(path: str | Path) -> list[LinemapRow]:
    return _read_csv(Path(path), LinemapRow)


# --- aggregator_table ------------------------------------------------------


def save_aggregator(rows: Iterable[AggregatorRow], path: str | Path) -> None:
    _write_csv(
        Path(path),
        rows,
        [
            "component_kind",
            "component_uid",
            "original_bus_uid",
            "cluster_bus_uid",
            "share",
        ],
    )


def load_aggregator(path: str | Path) -> list[AggregatorRow]:
    return _read_csv(Path(path), AggregatorRow)


# --- reducer_config.json ---------------------------------------------------


def save_reducer_config(config: dict[str, Any], path: str | Path) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, sort_keys=True)
        f.write("\n")


def load_reducer_config(path: str | Path) -> dict[str, Any]:
    with Path(path).open("r", encoding="utf-8") as f:
        return json.load(f)
