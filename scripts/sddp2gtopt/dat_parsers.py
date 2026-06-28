"""Parsers for the raw PSR SDDP / NCP ``.dat`` files.

This is the second front-end for ``sddp2gtopt`` (the first being
``psrclasses.json``).  Real PSR SDDP / NCP studies — e.g. the Guatemalan
AMM weekly/daily cases — ship a flat collection of fixed-format ``.dat``
files instead of the GUI's JSON snapshot:

| file          | holds                                  | → entity        |
|---------------|----------------------------------------|-----------------|
| ``sddp.dat``  | study control (stages, deficit, rate)  | ``StudySpec``   |
| ``sistem.dat``| system list                            | ``SystemSpec``  |
| ``ccombu*.dat``| fuels (``Custo`` $/MWh)               | ``FuelSpec``    |
| ``ctermi*.dat``| thermal plants (``Comb``/``CEsp``/``CVaria``) | ``ThermalSpec`` |
| ``chidro*.dat``| hydro plants (``Pot``)                | ``HydroSpec``   |
| ``cpde*.dat`` | hourly demand over the dispatch week   | ``DemandSpec``  |

Each parser subclasses :class:`gtopt_shared.base_parser.BaseTextParser`,
reusing its file handling, indexing and scalar helpers (the same base
``plp2gtopt`` parsers use).  Files are Latin-1 with ``!`` comment lines.

The whitespace-delimited files (fuel/thermal/system) split cleanly; the
hydro file is fixed-width with blank columns, so the hydro parser picks
the plant name plus the first decimal value (``Pot``) rather than relying
on positional split — enough for the v0 zero-cost hydro flattening.
"""

from __future__ import annotations

import logging
import re
from pathlib import Path

from gtopt_shared.base_parser import BaseTextParser

from .entities import (
    BusSpec,
    CircuitSpec,
    FuelSpec,
    HydroSpec,
    StudySpec,
    SystemSpec,
    ThermalSpec,
)


# Bus base voltage from the name suffix (``AGU-230`` → 230, ``SID-22`` → 22).
_KV_SUFFIX = re.compile(r"-(\d+(?:\.\d+)?)$")


logger = logging.getLogger(__name__)


def _to_float(token: str) -> float:
    """Parse a PSR numeric token (trailing ``.`` like ``50.`` allowed)."""
    try:
        return float(token)
    except ValueError:
        return 0.0


def _is_number(token: str) -> bool:
    try:
        float(token)
        return True
    except ValueError:
        return False


class ControlParser(BaseTextParser):
    """Parse ``sddp.dat`` (study control) into a :class:`StudySpec`."""

    # PSR SDDP ``ETAPA`` cadence codes match the json ``Tipo_Etapa``:
    # 1 = weekly, 2 = monthly, 3 = trimester.
    def parse(self) -> StudySpec:
        self.validate_file()
        lines = self.read_data_lines(comment_prefixes=())

        def value_after(keyword: str, cast, default):
            """First numeric token appearing after ``keyword`` on its line."""
            for line in lines:
                pos = line.find(keyword)
                if pos < 0:
                    continue
                for tok in line[pos + len(keyword) :].split():
                    if _is_number(tok):
                        return cast(_to_float(tok))
            return default

        # ``ETAPA`` cadence: the standalone line (not "NUMERO DE ETAPAS").
        stage_type = 1
        for line in lines:
            if line.strip().startswith("ETAPA"):
                toks = line.split()
                if len(toks) > 1 and _is_number(toks[1]):
                    stage_type = int(_to_float(toks[1]))
                break

        # Deficit cost: first numeric row after the "Costo Deficit" header
        # is "<%> <c1> ..."; the cost is the 2nd token.
        deficit = 1000.0
        for i, line in enumerate(lines):
            if "Costo Deficit" in line:
                for nxt in lines[i + 1 :]:
                    toks = nxt.split()
                    if len(toks) >= 2 and all(_is_number(t) for t in toks[:2]):
                        deficit = _to_float(toks[1])
                        break
                break

        study = StudySpec(
            initial_year=value_after("NICIAL", int, 2000)
            if "A~NO INICIAL" in "\n".join(lines)
            else 2000,
            stage_type=stage_type,
            num_stages=value_after("NUMERO DE ETAPAS", int, 1),
            num_systems=value_after("NUMERO DE SISTEMAS", int, 1),
            num_blocks=value_after("NUMERO DE BLOQUES DEMANDA", int, 1),
            deficit_cost=deficit,
            discount_rate=value_after("TASA DE DESCUENTO", float, 0.0),
        )
        logger.info(
            "parsed %s: %d stages, cadence=%d, blocks=%d, deficit=%.2f, disc=%.3f",
            self.file_path.name,
            study.num_stages,
            study.stage_type,
            study.num_blocks,
            study.deficit_cost,
            study.discount_rate,
        )
        return study


class SystemParser(BaseTextParser):
    """Parse ``sistem.dat`` (``NUM NOMBRE ID``) into ``SystemSpec`` list."""

    def parse(self) -> list[SystemSpec]:
        self.validate_file()
        out: list[SystemSpec] = []
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 2 or not _is_number(toks[0]):
                continue  # header / separator
            code = int(_to_float(toks[0]))
            ident = toks[-1]
            name = " ".join(toks[1:-1]) if len(toks) > 2 else toks[1]
            out.append(
                SystemSpec(code=code, name=name, reference_id=code, currency=ident)
            )
        logger.info("parsed %s: %d system(s)", self.file_path.name, len(out))
        return out


class FuelParser(BaseTextParser):
    """Parse ``ccombu*.dat`` (``Num Nome UComb Custo …``) → ``FuelSpec`` list."""

    def parse(self) -> list[FuelSpec]:
        self.validate_file()
        out: list[FuelSpec] = []
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 4 or not _is_number(toks[0]):
                continue
            out.append(
                FuelSpec(
                    code=int(_to_float(toks[0])),
                    name=toks[1],
                    reference_id=int(_to_float(toks[0])),
                    unit=toks[2],
                    cost=_to_float(toks[3]),
                    co2=_to_float(toks[4]) if len(toks) > 4 else 0.0,
                )
            )
        logger.info("parsed %s: %d fuel(s)", self.file_path.name, len(out))
        return out


class ThermalParser(BaseTextParser):
    """Parse ``ctermi*.dat`` → ``ThermalSpec`` list (cost via fuels).

    Field order: ``Num Nombre #Uni Tipo PotIns GerMin GerMax Teif Ih
    CVaria MR Comb G1 CEsp1 G2 CEsp2 G3 CEsp3 …``.  ``G(i)`` are
    percentages of capacity (sum ≈ 100); the per-segment gtopt cost is
    ``CEsp(i) × fuel.Custo + CVaria``.
    """

    def parse(self, fuels: dict[int, float] | None = None) -> list[ThermalSpec]:
        self.validate_file()
        fuel_cost = fuels or {}
        out: list[ThermalSpec] = []
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 12 or not _is_number(toks[0]):
                continue
            code = int(_to_float(toks[0]))
            name = toks[1]
            pmin = _to_float(toks[5])
            pmax = _to_float(toks[6])
            cvaria = _to_float(toks[9])
            comb = int(_to_float(toks[11]))
            cost = fuel_cost.get(comb, 0.0)
            segs: list[tuple[float, float]] = []
            # Up to 3 (G%, CEsp) pairs at indices 12/13, 14/15, 16/17.
            for gi, ci in ((12, 13), (14, 15), (16, 17)):
                if len(toks) <= ci:
                    break
                g_pct = _to_float(toks[gi])
                cesp = _to_float(toks[ci])
                if g_pct > 0 and pmax > 0:
                    cap_mw = g_pct / 100.0 * pmax
                    segs.append((cap_mw, cesp * cost + cvaria))
            if not segs:
                segs = [(pmax, cvaria)]
            out.append(
                ThermalSpec(
                    code=code,
                    name=name,
                    reference_id=code,
                    pmin=pmin,
                    pmax=pmax,
                    fuel_refs=[comb] if comb else [],
                    g_segments=segs,
                )
            )
        logger.info("parsed %s: %d thermal unit(s)", self.file_path.name, len(out))
        return out


class HydroParser(BaseTextParser):
    """Parse ``chidro*.dat`` → ``HydroSpec`` list (name + ``Pot``).

    The hydro record is fixed-width with optional blank topology columns,
    so positional split is unreliable.  v0 only needs the installed power
    for the zero-cost hydro flattening: the name is the 2nd token and
    ``Pot`` is the first decimal value on the line.
    """

    def parse(self) -> list[HydroSpec]:
        self.validate_file()
        out: list[HydroSpec] = []
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 3 or not _is_number(toks[0]):
                continue
            code = int(_to_float(toks[0]))
            name = toks[1]
            # The first two decimal values after the id columns are
            # ``Pot`` (installed MW) then ``FPMed`` (MW per m³/s).
            decimals = [_to_float(t) for t in toks[2:] if "." in t and _is_number(t)]
            p_inst = decimals[0] if decimals else 0.0
            fp_med = decimals[1] if len(decimals) > 1 else 0.0
            out.append(
                HydroSpec(
                    code=code,
                    name=name,
                    reference_id=code,
                    p_inst=p_inst,
                    fp_med=fp_med,
                )
            )
        logger.info("parsed %s: %d hydro plant(s)", self.file_path.name, len(out))
        return out


class DemandParser(BaseTextParser):
    """Parse ``cpde*.dat`` hourly demand into a flat per-block MW series.

    Layout: an ``Infxbloque`` line, a ``dd/mm/aaaa <hours…>`` header, then
    one row per day (``dd/mm/aaaa`` + N hourly MW values).  All days are
    concatenated into a single dispatch-horizon block series.
    """

    def parse(self) -> list[float]:
        self.validate_file()
        values: list[float] = []
        for line in self.read_data_lines():
            toks = line.split()
            if not toks:
                continue
            # Day rows start with a dd/mm/aaaa date token.
            if "/" not in toks[0]:
                continue
            values.extend(_to_float(t) for t in toks[1:] if _is_number(t))
        logger.info("parsed %s: %d demand block(s)", self.file_path.name, len(values))
        return values


class BusParser(BaseTextParser):
    """Parse ``dbus.dat`` → bus list + a ``gen-name → bus number`` map.

    Header: ``NUM Tp NombreB Id  # tg Plnt Nombre Gener.Area …``.  A plain
    bus row has the number/type/name/area; a generator row additionally
    carries ``# tg <plnt-code> <gen-name>`` (so the same bus appears once
    per generator).  ``gen2bus`` maps each generator name to its bus so
    thermal/hydro plants (matched by name) land on the right node.
    """

    def __init__(self, file_path: str | Path) -> None:
        super().__init__(file_path)
        self.gen2bus: dict[str, int] = {}

    def parse(self) -> list[BusSpec]:
        self.validate_file()
        self.gen2bus = {}
        buses: dict[int, BusSpec] = {}
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 3 or not toks[0].isdigit():
                continue  # header / separator
            num = int(toks[0])
            name = toks[2]
            if num not in buses:
                m = _KV_SUFFIX.search(name)
                kv = float(m.group(1)) if m else 0.0
                area = toks[3] if len(toks) > 3 else ""
                buses[num] = BusSpec(number=num, name=name, base_kv=kv, area=area)
            # Generator row: "<#idx> <tg> <plnt-code> <gen-name>".
            if len(toks) >= 8 and toks[4].isdigit() and toks[6].isdigit():
                self.gen2bus[toks[7]] = num
        logger.info(
            "parsed %s: %d buses, %d gen→bus mappings",
            self.file_path.name,
            len(buses),
            len(self.gen2bus),
        )
        return list(buses.values())


class VoltParser(BaseTextParser):
    """Parse ``Volt.dat`` (``Bus Nome O kV``) → ``{bus: base_kV}``.

    Authoritative bus base voltages for the network buses (the
    bus-name suffix is the fallback; generator terminal buses absent
    here are resolved by neighbour inference in the loader).
    """

    def parse(self) -> dict[int, float]:
        self.validate_file()
        out: dict[int, float] = {}
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 3 or not toks[0].isdigit():
                continue  # header
            kv = _to_float(toks[-1].rstrip("."))
            if kv > 0:
                out[int(toks[0])] = kv
        logger.info("parsed %s: %d bus voltages", self.file_path.name, len(out))
        return out


def _split_reactance_region(region: str) -> tuple[float, float]:
    """Split the fixed-width ``RESIS REACT`` region into ``(R, X)``.

    Normally two whitespace-separated tokens; when the columns touch the
    text merges (e.g. ``0.100.25`` = R 0.10 + X 0.25).  Resistances have a
    single integer digit, so the merged value is split one character
    before its **second** decimal point.
    """
    parts = region.split()
    if len(parts) >= 2:
        return float(parts[0]), float(parts[1])
    if len(parts) == 1:
        s = parts[0]
        dots = [i for i, c in enumerate(s) if c == "."]
        if len(dots) >= 2:
            cut = dots[1] - 1
            return float(s[:cut]), float(s[cut:])
        return float(s), 0.0
    return 0.0, 0.0


class CircuitParser(BaseTextParser):
    """Parse the fixed-width ``dcirc.dat`` → ``CircuitSpec`` list.

    Columns: ``#BOR`` [0:8], ``#BDE`` [8:17], ``RESIS REACT`` [17:29]
    (ohms — converted to per-unit later by the loader), then the name and
    the MVA rating.  ``reactance_pu`` on the returned specs temporarily
    holds the **raw ohm** value; :func:`dat_loader.load_dat_case`
    normalises it to per-unit using the bus base voltages.
    """

    def parse(self) -> list[CircuitSpec]:
        self.validate_file()
        out: list[CircuitSpec] = []
        for line in self.read_data_lines():
            if len(line) < 17 or not line[:8].strip().isdigit():
                continue  # header / separator
            from_bus = int(line[:8])
            to_field = line[8:17].strip()
            if not to_field.isdigit():
                continue
            to_bus = int(to_field)
            r_ohm, x_ohm = _split_reactance_region(line[17:29])
            rest = line[29:].split()
            name = rest[0] if rest else ""
            rating = 0.0
            for tok in rest[1:]:
                if "." in tok:
                    rating = _to_float(tok)
                    break
            out.append(
                CircuitSpec(
                    from_bus=from_bus,
                    to_bus=to_bus,
                    name=name,
                    resistance=r_ohm,
                    reactance_pu=x_ohm,  # raw ohms; loader → per-unit
                    rating=rating,
                )
            )
        logger.info("parsed %s: %d circuit(s)", self.file_path.name, len(out))
        return out


class BusDemandParser(BaseTextParser):
    """Parse ``cpdexbus.dat`` (per-bus hourly demand) → ``{bus: [MW…]}``.

    Layout: a header row ``dd/mm/aaaa:hh <bus-numbers…>`` then one row per
    hour (``DD/MM/YYYY:HH <values…>``).  Returns a flat per-bus MW series
    over the whole horizon.
    """

    def parse(self) -> dict[int, list[float]]:
        self.validate_file()
        bus_ids: list[int] | None = None
        rows: list[list[float]] = []
        for line in self.read_data_lines():
            toks = line.split()
            if not toks:
                continue
            if "dd/mm" in toks[0].lower():
                bus_ids = [int(_to_float(t)) for t in toks[1:] if _is_number(t)]
            elif "/" in toks[0] and ":" in toks[0]:
                rows.append([_to_float(t) for t in toks[1:] if _is_number(t)])
        if not bus_ids:
            logger.warning("%s: no bus-id header found", self.file_path.name)
            return {}
        series: dict[int, list[float]] = {b: [] for b in bus_ids}
        for row in rows:
            for col, bus in enumerate(bus_ids):
                series[bus].append(row[col] if col < len(row) else 0.0)
        logger.info(
            "parsed %s: %d buses × %d hours",
            self.file_path.name,
            len(bus_ids),
            len(rows),
        )
        return series


def _parse_psr_block_csv_means(path: str | Path) -> dict[str, float]:
    """Mean per-column over the block rows of a PSR ``Stag,Seq,Blck,…`` CSV.

    Shared by the per-hydro water-value (``watervcp.csv``) and inflow
    (``inflow.csv``) readers: the agent names are columns 3+ of the
    ``Stag``-prefixed header row, the values one row per block.
    """
    path = Path(path)
    lines = path.read_text(encoding="latin-1").splitlines()
    header_idx = next(
        (i for i, ln in enumerate(lines) if ln.strip().lower().startswith("stag")),
        None,
    )
    if header_idx is None:
        logger.warning("%s: no 'Stag' header row", path.name)
        return {}
    names = [h.strip() for h in lines[header_idx].split(",")][3:]
    sums = [0.0] * len(names)
    count = 0
    for ln in lines[header_idx + 1 :]:
        cells = [c.strip() for c in ln.split(",")][3:]
        if not cells:
            continue
        for i in range(min(len(names), len(cells))):
            if cells[i]:
                sums[i] += _to_float(cells[i])
        count += 1
    if count == 0:
        return {}
    return {names[i]: sums[i] / count for i in range(len(names))}


def parse_water_values(path: str | Path) -> dict[str, float]:
    """Parse PSR ``watervcp.csv`` → ``{hydro_name: mean water value}``.

    The PSR water-value output is in **k$/hm³** per hydro per block,
    averaged over the horizon (clamped at 0 — tiny negatives are
    numerical noise on run-of-river plants).  The loader converts
    k$/hm³ → $/MWh via each plant's production factor ``FPMed``.
    """
    out = {n: max(0.0, v) for n, v in _parse_psr_block_csv_means(path).items()}
    logger.info("parsed %s: %d hydro water values", Path(path).name, len(out))
    return out


def parse_inflows(path: str | Path) -> dict[str, float]:
    """Parse PSR ``inflow.csv`` → ``{hydro_name: mean inflow [m³/s]}``.

    The dispatch-week inflow forecast (the NCP input, not an output),
    averaged over the horizon.  The loader caps run-of-river hydro at
    ``inflow · FPMed`` so free hydro can't exceed the available water.
    """
    out = {n: max(0.0, v) for n, v in _parse_psr_block_csv_means(path).items()}
    logger.info("parsed %s: %d hydro inflows", Path(path).name, len(out))
    return out


def find_dat(case_dir: Path, *patterns: str) -> Path | None:
    """Return the first existing ``.dat`` matching any glob in ``case_dir``."""
    for pat in patterns:
        for p in sorted(case_dir.glob(pat)):
            if p.is_file():
                return p
    return None
