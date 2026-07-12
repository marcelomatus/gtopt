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


# Fixed-width column map for the PSR ``chidro`` record (0-based [start:end)
# half-open slices), derived from the header row
# ``NUM ...Nombre... .PV. .VAA .TAA #Uni Tipo ....Pot .FPMed. .QMin.. .QMax..
#  .VMin.. .VMax.. .VInic. … .Cota1. .Vol1.. .Cota2. .Vol2.. …``.
_CHIDRO_COLS = {
    "num": (0, 4),
    "name": (4, 17),
    "vaa": (23, 27),  # downstream plant NUM (Jusante)
    "pot": (43, 50),  # installed power [MW]
    "fpmed": (51, 58),  # production factor [MW/(m³/s)]
    "qmax": (67, 74),  # max turbine flow [m³/s]
    "vmin": (75, 82),  # min storage volume [hm³]
    "vmax": (83, 90),  # max storage volume [hm³]
    "vinic": (91, 98),  # initial elevation (cota) [m]
}
# Five (cota, volume) pairs of the reservoir curve: cota [m] → volume [hm³].
_CHIDRO_COTA_VOL = (
    (371, 378, 379, 386),
    (387, 394, 395, 402),
    (403, 410, 411, 418),
    (419, 426, 427, 434),
    (435, 442, 443, 450),
)


def _slice_float(line: str, start: int, end: int) -> float | None:
    """Parse a fixed-width numeric field; ``None`` if blank/short/non-numeric."""
    if len(line) < start:
        return None
    tok = line[start:end].strip()
    return _to_float(tok) if tok and _is_number(tok) else None


def _interp_cota_vol(
    cota: float, pairs: list[tuple[float | None, float | None]]
) -> float | None:
    """Linear-interpolate a volume [hm³] from an elevation ``cota`` [m].

    ``pairs`` is the reservoir's ascending (cota, volume) curve; values
    outside the range clamp to the endpoints.  Returns ``None`` when the
    curve has fewer than two points.
    """
    pts: list[tuple[float, float]] = sorted(
        (c, v) for c, v in pairs if c is not None and v is not None
    )
    if len(pts) < 2:
        return None
    if cota <= pts[0][0]:
        return pts[0][1]
    if cota >= pts[-1][0]:
        return pts[-1][1]
    for i in range(1, len(pts)):
        if cota <= pts[i][0]:
            (c0, v0), (c1, v1) = pts[i - 1], pts[i]
            span = c1 - c0
            return v0 + (v1 - v0) * (cota - c0) / span if span else v0
    return pts[-1][1]


class HydroParser(BaseTextParser):
    """Parse ``chidro*.dat`` → ``HydroSpec`` list.

    The hydro record is fixed-width.  Beyond the installed power (``Pot``)
    and production factor (``FPMed``) the parser now also recovers the
    cascade topology (``VAA`` = downstream plant) and the storage state
    (``VMin``/``VMax`` [hm³] and the initial volume, interpolated from the
    initial elevation ``VInic`` over the reservoir's embedded cota–vol
    curve) needed to build the full water network.

    Power/FPMed are read by fixed-width slice with a whitespace-token
    fallback, so narrow / hand-made fixtures (which omit the storage
    columns) keep parsing — those simply carry no reservoir.
    """

    def parse(self) -> list[HydroSpec]:
        self.validate_file()
        out: list[HydroSpec] = []
        for line in self.read_data_lines():
            toks = line.split()
            if len(toks) < 3 or not _is_number(toks[0]):
                continue
            code = int(_to_float(toks[0]))

            # The record is real PSR fixed-width only when the ``Pot``/``FPMed``
            # columns parse at their nominal offsets; narrow or hand-made
            # fixtures fall back to whitespace tokens (``Pot``/``FPMed`` = the
            # first two decimals) and carry no storage / cascade topology.
            pot_fw = _slice_float(line, *_CHIDRO_COLS["pot"])
            fp_fw = _slice_float(line, *_CHIDRO_COLS["fpmed"])

            qmax = vmin = vmax = vinic = eini = 0.0
            p_inst = fp_med = 0.0
            downstream: int | None = None
            if pot_fw is not None and fp_fw is not None and len(line) >= 58:
                name = line[slice(*_CHIDRO_COLS["name"])].strip() or toks[1]
                p_inst, fp_med = pot_fw, fp_fw
                qmax = _slice_float(line, *_CHIDRO_COLS["qmax"]) or 0.0
                vmin = _slice_float(line, *_CHIDRO_COLS["vmin"]) or 0.0
                vmax = _slice_float(line, *_CHIDRO_COLS["vmax"]) or 0.0
                vinic = _slice_float(line, *_CHIDRO_COLS["vinic"]) or 0.0
                vaa = line[slice(*_CHIDRO_COLS["vaa"])].strip()
                downstream = int(vaa) if vaa.isdigit() and int(vaa) != code else None
                if vmax > 0.0:
                    pairs = [
                        (_slice_float(line, a, b), _slice_float(line, c, d))
                        for a, b, c, d in _CHIDRO_COTA_VOL
                    ]
                    interp = _interp_cota_vol(vinic, pairs)
                    eini = interp if interp is not None else 0.5 * (vmin + vmax)
                    eini = min(max(eini, vmin), vmax)
            else:
                name = toks[1]
                decimals = [
                    _to_float(t) for t in toks[2:] if "." in t and _is_number(t)
                ]
                p_inst = decimals[0] if decimals else 0.0
                fp_med = decimals[1] if len(decimals) > 1 else 0.0

            out.append(
                HydroSpec(
                    code=code,
                    name=name,
                    reference_id=code,
                    p_inst=p_inst,
                    fp_med=fp_med or 0.0,
                    qmax=qmax,
                    vmin=vmin,
                    vmax=vmax,
                    vinic=vinic,
                    eini=eini,
                    downstream_code=downstream,
                )
            )
        n_res = sum(1 for h in out if h.vmax > 0.0)
        logger.info(
            "parsed %s: %d hydro plant(s) (%d with storage)",
            self.file_path.name,
            len(out),
            n_res,
        )
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


def _parse_psr_block_csv_profiles(path: str | Path) -> dict[str, list[float]]:
    """Per-column **full time series** over a PSR ``Stag,Seq,Blck,…`` CSV.

    The agent names are columns 3+ of the ``Stag``-prefixed header row; each
    data row is one stage/block.  Returns ``{agent: [v_stage1, v_stage2, …]}``
    preserving the per-stage values — callers decide whether to keep the whole
    profile (inflow, time-varying) or reduce it (water value).  Collapsing this
    to a single scalar throws away PSR's time resolution, so the profile form
    is the primitive and the mean is derived from it.
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    path = Path(path)
    lines = read_text(path, encoding="latin-1", errors="replace").splitlines()
    header_idx = next(
        (i for i, ln in enumerate(lines) if ln.strip().lower().startswith("stag")),
        None,
    )
    if header_idx is None:
        logger.warning("%s: no 'Stag' header row", path.name)
        return {}
    names = [h.strip() for h in lines[header_idx].split(",")][3:]
    cols: list[list[float]] = [[] for _ in names]
    for ln in lines[header_idx + 1 :]:
        cells = [c.strip() for c in ln.split(",")][3:]
        if not cells or not any(cells):
            continue
        for i in range(len(names)):
            if i < len(cells) and cells[i]:
                cols[i].append(_to_float(cells[i]))
    return {names[i]: cols[i] for i in range(len(names)) if cols[i]}


def _parse_psr_block_csv_means(path: str | Path) -> dict[str, float]:
    """Mean per-column over a PSR ``Stag,Seq,Blck,…`` CSV (derived from the
    full :func:`_parse_psr_block_csv_profiles`)."""
    return {
        n: (sum(v) / len(v)) for n, v in _parse_psr_block_csv_profiles(path).items()
    }


def parse_water_values(path: str | Path) -> dict[str, float]:
    """Parse PSR ``watervcp.csv`` → ``{hydro_name: water value}``.

    The PSR water-value output is in **k$/hm³** per hydro per stage.  Across the
    horizon this NCP marginal is ~flat (it is the seasonal FCF slope, not a
    sub-daily quantity), so the scalar mean is the right reduction here; the
    loader converts k$/hm³ → the boundary-cut coefficient.  Clamped at 0 —
    tiny negatives are numerical noise on run-of-river plants.
    """
    out = {n: max(0.0, v) for n, v in _parse_psr_block_csv_means(path).items()}
    logger.info("parsed %s: %d hydro water values", Path(path).name, len(out))
    return out


def parse_inflows(path: str | Path) -> dict[str, list[float]]:
    """Parse PSR ``inflow.csv`` → ``{hydro_name: [inflow per stage] [m³/s]}``.

    The inflow forecast is **time-varying** (a daily series over the forecast
    horizon), so it is returned as the full per-stage profile — NOT a scalar
    mean.  Collapsing it to the horizon mean over-states the dispatch-period
    water (the forecast extends past the dispatch and its later high-flow
    stages inflate the mean), which floods run-of-river hydro.  The loader
    maps this profile onto the model's day/block grid.
    """
    out = {
        n: [max(0.0, x) for x in v]
        for n, v in _parse_psr_block_csv_profiles(path).items()
    }
    logger.info("parsed %s: %d hydro inflow profiles", Path(path).name, len(out))
    return out


def parse_renewables(path: str | Path) -> list[ThermalSpec]:
    """Parse PSR ``cgndgu.dat`` → non-dispatchable renewables as generators.

    Wind (``-E``) and solar (``-F``) plants; columns
    ``Num Name Bus Tipo #Uni PotIns FatOpe ProbFal SFal Stat O&M CurtCos
    TechTyp`` (some optional fields blank, so ``PotIns`` is read front-anchored
    at index 5 and ``O&M`` back-anchored at ``-3``).  Each becomes a
    :class:`ThermalSpec` with installed ``PotIns`` and the small O&M cost, so
    the writer's generator path emits it; the hourly availability cap
    (``max_gen``) is attached by the loader from
    :func:`parse_renewable_profiles`.
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    path = Path(path)
    out: list[ThermalSpec] = []
    for ln in read_text(path, encoding="latin-1", errors="replace").splitlines():
        s = ln.strip()
        if not s or s[0] in "$!":
            continue
        toks = s.split()
        if len(toks) < 9 or not toks[0].isdigit():
            continue
        code = int(toks[0])
        pot = _to_float(toks[5])
        om = max(0.0, _to_float(toks[-3]))
        out.append(
            ThermalSpec(
                code=code,
                name=toks[1],
                reference_id=code,
                pmin=0.0,
                pmax=pot,
                bus_number=int(_to_float(toks[2])),
                g_segments=[(pot, om)],
                is_import=False,
            )
        )
    logger.info("parsed %s: %d renewable plants", path.name, len(out))
    return out


def parse_renewable_profiles(path: str | Path) -> dict[int, list[float]]:
    """Parse PSR ``cpgndgu.dat`` → ``{code: [hourly availability MW]}``.

    Blocks ``**** COD: N TP: …`` followed by a ``dd/mm/aaaa`` header and one
    row per day of 24 hourly values; the days are concatenated into the
    plant's full hourly generation forecast (its time-varying ``pmax`` cap).
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    path = Path(path)
    out: dict[int, list[float]] = {}
    cur: int | None = None
    for ln in read_text(path, encoding="latin-1", errors="replace").splitlines():
        if "COD:" in ln:
            try:
                cur = int(ln.split("COD:")[1].split()[0])
                out[cur] = []
            except (ValueError, IndexError):
                cur = None
            continue
        if cur is None or "dd/mm" in ln.lower():
            continue
        toks = ln.split()
        if toks and "/" in toks[0]:
            out[cur].extend(_to_float(x) for x in toks[1:])
    return {k: v for k, v in out.items() if v}


def parse_unit_prices(path: str | Path) -> dict[str, list[float]]:
    """Parse PSR ``PRECIOSMEX.csv`` → ``{unit_name: [hourly bid $/MWh]}``.

    The AMM's per-unit hourly offer price (one column per unit, one row per
    hour).  This is the authoritative dispatch cost — notably it prices the
    interconnection imports (``MEX-I`` free, the other ties 250 $/MWh on the
    billed day, ~0 on look-ahead days), which the $0 fuel table does not.
    Blank cells become 0.
    """
    import csv  # pylint: disable=import-outside-toplevel
    import io  # pylint: disable=import-outside-toplevel

    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    text = read_text(Path(path), encoding="latin-1", errors="replace")
    rows = list(csv.reader(io.StringIO(text)))
    if not rows:
        return {}
    names = [h.strip().strip("'\"") for h in rows[0]]
    data = [r for r in rows[1:] if r and r[0].strip()]
    out: dict[str, list[float]] = {}
    for j in range(1, len(names)):
        prof = [_to_float(r[j]) if j < len(r) and r[j].strip() else 0.0 for r in data]
        if prof:
            out[names[j]] = prof
    logger.info("parsed %s: %d unit price series", Path(path).name, len(out))
    return out


def parse_gen_constraints(
    path: str | Path,
) -> dict[str, tuple[str, list[float | None]]]:
    """Parse PSR ``RESTMEX.csv`` → ``{unit: (type, [hourly bound | None])}``.

    The AMM operational constraints: a ``Tipo`` row of ``< > =`` over named
    units (``2-ORT-G DG``, ``61-EDC-I`` …) with hourly right-hand-sides; blank
    cells mean the constraint is inactive that hour.  ``<`` caps the unit's
    generation (pmax), ``>`` floors it (pmin), ``=`` fixes it.  Unit labels are
    normalised by stripping the leading ``N-`` index and trailing tag word.
    """
    import csv  # pylint: disable=import-outside-toplevel
    import io  # pylint: disable=import-outside-toplevel

    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    rows = list(csv.reader(io.StringIO(read_text(Path(path), encoding="latin-1"))))
    ti = next((i for i, r in enumerate(rows) if r and r[0].strip() == "Tipo"), None)
    if ti is None or ti + 1 >= len(rows):
        return {}
    tipos = [t.strip() for t in rows[ti][1:]]
    units = [re.sub(r"^\d+-", "", u).split()[0].strip() for u in rows[ti + 1][1:]]
    data = [r for r in rows[ti + 2 :] if r and r[0].strip()]
    out: dict[str, tuple[str, list[float | None]]] = {}
    for j, (tp, unit) in enumerate(zip(tipos, units)):
        vals: list[float | None] = [
            _to_float(r[j + 1]) if j + 1 < len(r) and r[j + 1].strip() else None
            for r in data
        ]
        if tp in "<>=" and any(v is not None for v in vals):
            out[unit] = (tp, vals)
    logger.info("parsed %s: %d AMM generation constraints", Path(path).name, len(out))
    return out


def parse_final_volumes(path: str | Path) -> dict[str, float]:
    """Parse PSR ``volfincp.csv`` → ``{hydro_name: end-of-horizon volume [hm³]}``.

    PSR's solved end-of-stage storage (a ``Stag,Seq,Blck,<res…>`` block CSV);
    the **last** data row is the volume at the end of the horizon — the
    reservoir's expected final volume ``efin``, the point at which the
    boundary-cut future-cost function is linearised.
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    path = Path(path)
    lines = read_text(path, encoding="latin-1", errors="replace").splitlines()
    header_idx = next(
        (i for i, ln in enumerate(lines) if ln.strip().lower().startswith("stag")),
        None,
    )
    if header_idx is None:
        logger.warning("%s: no 'Stag' header row", path.name)
        return {}
    names = [h.strip() for h in lines[header_idx].split(",")][3:]
    last_cells: list[str] | None = None
    for ln in lines[header_idx + 1 :]:
        cells = [c.strip() for c in ln.split(",")][3:]
        if any(cells):
            last_cells = cells
    if last_cells is None:
        return {}
    out: dict[str, float] = {}
    for i in range(min(len(names), len(last_cells))):
        if last_cells[i]:
            out[names[i]] = _to_float(last_cells[i])
    logger.info("parsed %s: %d hydro end-volumes", path.name, len(out))
    return out


def parse_max_generation(path: str | Path) -> tuple[dict[str, list[float]], bool]:
    """Parse PSR ``cprmxhgu`` / ``cprmxtgu`` → ``{plant: [hourly max gen]}``.

    Per-plant, per-hour maximum generation limit (operational derating /
    availability), the PSR cap that holds plants below their installed
    capacity.  Layout: a ``Unidades : N (1=MW, 2=%G)`` line, then per plant a
    ``****  <code> <name>`` header, a ``dd/mm/aaaa <hours…>`` header and one
    row per day of hourly values; days are concatenated into the horizon
    series.  Returns ``(profiles, is_percent)`` — ``is_percent`` flags ``%G``
    units (value is a percentage of installed capacity).  ``-1`` sentinels
    (no cap) are passed through for the loader to resolve against installed.
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    lines = read_text(Path(path), encoding="latin-1", errors="replace").splitlines()
    is_percent = False
    out: dict[str, list[float]] = {}
    cur: str | None = None
    for ln in lines:
        unit_m = re.search(r"Unidades\s*:\s*(\d)", ln)
        if unit_m:
            is_percent = unit_m.group(1) == "2"
            continue
        head_m = re.match(r"\*+\s+\d+\s+(\S+)", ln)
        if head_m:
            cur = head_m.group(1)
            out.setdefault(cur, [])
            continue
        if cur is not None and re.match(r"\s*\d{2}/\d{2}/\d{4}", ln):
            out[cur].extend(_to_float(t) for t in ln.split()[1:] if _is_number(t))
    out = {k: v for k, v in out.items() if v}
    logger.info(
        "parsed %s: %d plant max-gen profiles (%s)",
        Path(path).name,
        len(out),
        "%G" if is_percent else "MW",
    )
    return out, is_percent


def parse_commitment(path: str | Path) -> dict[str, bool]:
    """Parse PSR ``commith.dat`` → ``{hydro_name: committed}``.

    Per-hydro on/off commitment flag (``1`` = committed, ``0`` = off for the
    dispatch).  Layout: one line per unit, ``<flag>! <code>,!<name>``.  Units
    committed off must not generate (their generator ``pmax`` is pinned to 0).
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        read_text,
    )

    lines = read_text(Path(path), encoding="latin-1", errors="replace").splitlines()
    out: dict[str, bool] = {}
    for ln in lines:
        m = re.match(r"\s*([01])\s*!\s*(\d+)\s*,?\s*!?\s*(\S+)", ln)
        if m and m.group(3):
            out[m.group(3).strip()] = m.group(1) == "1"
    n_off = sum(1 for v in out.values() if not v)
    logger.info(
        "parsed %s: %d hydro commitments (%d off)", Path(path).name, len(out), n_off
    )
    return out


# Compression suffixes a ``.dat`` may carry (in glob-probe order).
_COMPRESS_EXT = ("", ".xz", ".gz", ".zst", ".lz4", ".bz2")


def find_dat(case_dir: Path, *patterns: str) -> Path | None:
    """Return the first matching ``.dat`` (or compressed ``.dat.<codec>``).

    Each glob ``pat`` is also tried with the compression suffixes, so a
    case shipping ``sddp.dat.xz`` resolves transparently.
    """
    for pat in patterns:
        for ext in _COMPRESS_EXT:
            for p in sorted(case_dir.glob(pat + ext)):
                if p.is_file():
                    return p
    return None
