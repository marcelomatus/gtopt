"""``sddp2gtopt --info`` renderer.

Prints a compact summary of an SDDP case so users can sanity-check
what the converter will see without needing to crack open
``psrclasses.json`` by hand. Mirrors the ``plp2gtopt --info`` flow.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .psrclasses_loader import PsrClassesLoader, load_psrclasses


logger = logging.getLogger(__name__)


def _study_summary(study: dict[str, Any]) -> list[tuple[str, str]]:
    """Extract the headline study parameters as ``(label, value)`` pairs."""
    rows: list[tuple[str, str]] = []

    def _add(label: str, key: str, default: str = "-") -> None:
        val = study.get(key)
        if val is None or (isinstance(val, list) and not val):
            rows.append((label, default))
        elif isinstance(val, list):
            rows.append((label, str(val[0])))
        else:
            rows.append((label, str(val)))

    _add("Initial year", "Ano_inicial")
    _add("Initial month/week", "Etapa_inicial")
    _add("Initial hydro year", "Ano_Inicial_Hidro")
    _add("Number of stages", "NumeroEtapas")
    _add("Number of systems", "NumeroSistemas")
    _add("Series (forward)", "Series_Forward")
    _add("Series (backward)", "Series_Backward")
    _add("Number of blocks", "NumeroBlocosDemanda")
    _add("Discount rate", "TaxaDesconto")
    _add("Deficit cost (k$)", "DeficitCost")
    _add("Currency", "CurrencyReference")
    _add("Max iterations", "MaximoIteracoes")
    return rows


def _named_table(
    entities: list[dict[str, Any]], extra_keys: tuple[str, ...] = ()
) -> list[tuple[str, ...]]:
    """Build a small table of ``(code, name, *extra)`` tuples for display."""
    rows: list[tuple[str, ...]] = []
    for ent in entities:
        code = ent.get("code", "?")
        name = ent.get("name", "?")
        extras = tuple(_format_value(ent.get(k)) for k in extra_keys)
        rows.append((str(code), str(name), *extras))
    return rows


def _format_value(val: Any) -> str:
    """Render a value compactly for the --info table output."""
    if val is None:
        return "-"
    if isinstance(val, list):
        if not val:
            return "-"
        if len(val) == 1:
            return _format_value(val[0])
        return f"[{len(val)} items]"
    if isinstance(val, float):
        return f"{val:g}"
    return str(val)


def display_sddp_info(options: dict[str, Any]) -> None:
    """Print a human-readable summary of the SDDP case at ``options['input_dir']``.

    Raises:
        FileNotFoundError: If ``psrclasses.json`` is missing.
        ValueError: If the JSON is malformed.
    """
    input_dir = Path(options["input_dir"])
    loader = load_psrclasses(input_dir)
    print(f"SDDP case: {input_dir}")
    print(f"Manifest:  {loader.path.name}")
    print()
    _print_collection_overview(loader)
    print()
    _print_study(loader)
    print()
    _print_systems(loader)
    print()
    _print_demands(loader)
    print()
    _print_fuels(loader)
    print()
    _print_thermal(loader)
    print()
    _print_hydro(loader)
    print()
    _print_gauging(loader)


def _print_collection_overview(loader: PsrClassesLoader) -> None:
    """Print a one-line-per-collection summary of entity counts."""
    print("Collections:")
    width = max((len(c) for c in loader.collections()), default=0)
    for name in loader.collections():
        count = loader.count(name)
        print(f"  {name.ljust(width)}  {count:>4d}")


def _print_table(
    title: str, headers: tuple[str, ...], rows: list[tuple[str, ...]]
) -> None:
    """Print a small text-mode table; no-op when ``rows`` is empty."""
    print(title)
    if not rows:
        print("  (none)")
        return
    cols = list(zip(headers, *rows, strict=False))
    widths = [max(len(str(c)) for c in col) for col in cols]
    line = "  " + "  ".join(h.ljust(w) for h, w in zip(headers, widths, strict=False))
    print(line)
    print("  " + "  ".join("-" * w for w in widths))
    for row in rows:
        print(
            "  " + "  ".join(str(c).ljust(w) for c, w in zip(row, widths, strict=False))
        )


def _print_study(loader: PsrClassesLoader) -> None:
    """Print the headline parameters of the (single) ``PSRStudy`` entity."""
    study = loader.first("PSRStudy")
    if study is None:
        print("Study: (no PSRStudy entity)")
        return
    print("Study:")
    for label, val in _study_summary(study):
        print(f"  {label:<22}  {val}")


def _print_systems(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRSystem`` entity."""
    rows = _named_table(loader.entities("PSRSystem"), extra_keys=("UnM",))
    _print_table("Systems:", ("code", "name", "currency"), rows)


def _print_demands(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRDemand`` entity."""
    rows = _named_table(
        loader.entities("PSRDemand"), extra_keys=("Duracao(1)", "system")
    )
    _print_table("Demands:", ("code", "name", "duracao(1)", "system_ref"), rows)


def _print_fuels(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRFuel`` entity."""
    rows = _named_table(
        loader.entities("PSRFuel"), extra_keys=("Custo", "UE", "EmiCO2")
    )
    _print_table("Fuels:", ("code", "name", "cost", "unit", "co2"), rows)


def _print_thermal(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRThermalPlant`` entity."""
    rows = _named_table(
        loader.entities("PSRThermalPlant"),
        extra_keys=("GerMin", "GerMax", "CTransp"),
    )
    _print_table(
        "Thermal plants:",
        ("code", "name", "Pmin", "Pmax", "Ctrans"),
        rows,
    )


def _print_hydro(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRHydroPlant`` entity."""
    rows = _named_table(
        loader.entities("PSRHydroPlant"),
        extra_keys=("PotInst", "Vmin", "Vmax", "Qmax", "FPMed"),
    )
    _print_table(
        "Hydro plants:",
        ("code", "name", "Pinst", "Vmin", "Vmax", "Qmax", "FPmed"),
        rows,
    )


def _print_gauging(loader: PsrClassesLoader) -> None:
    """Print one row per ``PSRGaugingStation`` entity (inflow stations)."""
    rows: list[tuple[str, ...]] = []
    for ent in loader.entities("PSRGaugingStation"):
        code = str(ent.get("code", "?"))
        name = str(ent.get("name", "?"))
        vazao = ent.get("Vazao") or []
        rows.append((code, name, str(len(vazao))))
    _print_table("Gauging stations:", ("code", "name", "Vazao series len"), rows)
